#include "verify.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <archive.h>
#include <archive_entry.h>

#include "checksum.h"
#include "packer.h"
#include "sha256.h"
#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---------------------------------------------------------------------- */
/* Small helpers                                                           */
/* ---------------------------------------------------------------------- */

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* list_dir_names: returns a malloc'd array of malloc'd names (just the
 * dirent name, not the full path) for every regular-ish entry in dir whose
 * name ends with `suffix` (pass "" for no filter). *count_out set. Returns
 * NULL (and *count_out = 0) if the directory does not exist or on OOM (the
 * two cases are distinguished by errno: ENOENT vs other). */
static char **list_dir_names(const char *dir, const char *suffix, size_t *count_out) {
    *count_out = 0;
    DIR *d = opendir(dir);
    if (d == NULL) {
        return NULL;
    }

    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t suffix_len = strlen(suffix);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        size_t len = strlen(name);
        if (suffix_len > 0) {
            if (len < suffix_len || strcmp(name + len - suffix_len, suffix) != 0) {
                continue;
            }
        }
        if (count == capacity) {
            size_t new_cap = capacity == 0 ? 8 : capacity * 2;
            char **p = realloc(names, new_cap * sizeof(*p));
            if (p == NULL) {
                closedir(d);
                for (size_t i = 0; i < count; i++) free(names[i]);
                free(names);
                *count_out = 0;
                return NULL;
            }
            names = p;
            capacity = new_cap;
        }
        names[count] = strdup(name);
        if (names[count] == NULL) {
            closedir(d);
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            *count_out = 0;
            return NULL;
        }
        count++;
    }
    closedir(d);
    *count_out = count;
    return names;
}

static void free_names(char **names, size_t count) {
    if (names == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

static int str_in_array(const char *s, char **arr, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(s, arr[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Base mode: SHA256SUMS verification                                      */
/* ---------------------------------------------------------------------- */

/* verify_checksums_file: checks every entry in SHA256SUMS against the file
 * on disk; prints OK/FAILED per entry (shasum -c style) and notes missing
 * files. Returns 0 if everything matched and every SHA256SUMS-listed file
 * exists, 1 otherwise. Populates *listed_relpaths (array of relpaths, for
 * the caller's "packs missing from SHA256SUMS" cross-check) -- caller
 * frees with free_names. */
static int verify_checksums_file(const char *repo, char ***listed_relpaths_out,
                                  size_t *listed_count_out) {
    struct tp_checksums_line_entry *entries = NULL;
    size_t count = 0;
    *listed_relpaths_out = NULL;
    *listed_count_out = 0;

    if (tp_checksums_read(repo, &entries, &count) != 0) {
        tp_warnx("verify: failed to read checksums/SHA256SUMS");
        return 1;
    }

    int failed = 0;

    char **relpaths = count > 0 ? calloc(count, sizeof(*relpaths)) : NULL;
    if (count > 0 && relpaths == NULL) {
        tp_warnx("verify: out of memory");
        tp_checksums_entries_free(entries, count);
        return 1;
    }

    for (size_t i = 0; i < count; i++) {
        relpaths[i] = strdup(entries[i].relpath);

        char full[PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", repo, entries[i].relpath);
        if (n < 0 || (size_t)n >= sizeof(full)) {
            printf("%s: FAILED (path too long)\n", entries[i].relpath);
            failed = 1;
            continue;
        }

        if (!file_exists(full)) {
            printf("%s: FAILED open or read\n", entries[i].relpath);
            failed = 1;
            continue;
        }

        char hex[65];
        if (tp_sha256_file(full, hex) != 0) {
            printf("%s: FAILED open or read\n", entries[i].relpath);
            failed = 1;
            continue;
        }

        if (strcmp(hex, entries[i].sha256_hex) == 0) {
            printf("%s: OK\n", entries[i].relpath);
        } else {
            printf("%s: FAILED\n", entries[i].relpath);
            failed = 1;
        }
    }

    tp_checksums_entries_free(entries, count);
    *listed_relpaths_out = relpaths;
    *listed_count_out = count;
    return failed;
}

/* verify_packs_vs_checksums: flags pack archives (.tar.zst) and pack
 * manifests (.json) on disk under packs/ that are missing from the
 * SHA256SUMS listing. Returns 1 if any are missing, 0 otherwise. */
static int verify_packs_vs_checksums(const char *repo, char **listed_relpaths,
                                      size_t listed_count) {
    char packs_dir[PATH_MAX];
    snprintf(packs_dir, sizeof(packs_dir), "%s/packs", repo);

    int failed = 0;
    static const char *const suffixes[] = {".tar.zst", ".json"};
    for (size_t s = 0; s < sizeof(suffixes) / sizeof(suffixes[0]); s++) {
        /* skip .part scratch files entirely -- they are never "packs" */
        size_t n = 0;
        char **names = list_dir_names(packs_dir, suffixes[s], &n);
        for (size_t i = 0; i < n; i++) {
            char rel[PATH_MAX];
            snprintf(rel, sizeof(rel), "packs/%s", names[i]);
            if (!str_in_array(rel, listed_relpaths, listed_count)) {
                printf("%s: MISSING FROM SHA256SUMS\n", rel);
                failed = 1;
            }
        }
        free_names(names, n);
    }
    return failed;
}

/* verify_index_vs_packs: flags objects.jsonl entries whose referenced pack
 * doesn't exist on disk. Returns 1 if any are missing, 0 otherwise (also 0
 * if objects.jsonl doesn't exist -- nothing to check). */
static int verify_index_vs_packs(const char *repo) {
    char index_path[PATH_MAX];
    snprintf(index_path, sizeof(index_path), "%s/objects/objects.jsonl", repo);

    FILE *f = fopen(index_path, "r");
    if (f == NULL) {
        return 0; /* nothing indexed yet is not a failure */
    }

    int failed = 0;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    /* very small local JSON field extraction, avoiding a cJSON dependency
     * here: reuse tp_index's on-disk format by scanning for "pack":"...". */
    while ((line_len = getline(&line, &line_cap, f)) != -1) {
        (void)line_len;
        char *key = strstr(line, "\"pack\":\"");
        if (key == NULL) {
            continue;
        }
        key += strlen("\"pack\":\"");
        char *end = strchr(key, '"');
        if (end == NULL) {
            continue;
        }
        size_t pack_len = (size_t)(end - key);
        char pack_name[64];
        if (pack_len >= sizeof(pack_name)) {
            continue;
        }
        memcpy(pack_name, key, pack_len);
        pack_name[pack_len] = '\0';

        char pack_path[PATH_MAX];
        snprintf(pack_path, sizeof(pack_path), "%s/packs/%s.tar.zst", repo, pack_name);
        if (!file_exists(pack_path)) {
            printf("objects.jsonl: entry references missing pack '%s'\n", pack_name);
            failed = 1;
        }
    }

    free(line);
    fclose(f);
    return failed;
}

/* ---------------------------------------------------------------------- */
/* --objects deep mode                                                     */
/* ---------------------------------------------------------------------- */

static void bytes_to_hex(const unsigned char *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

static const struct tp_pack_manifest_entry *find_manifest_entry(
    const struct tp_pack_manifest_entry *entries, size_t count, const char *pathname) {
    for (size_t i = 0; i < count; i++) {
        if (entries[i].path_len == strlen(pathname) &&
            memcmp(entries[i].path, pathname, entries[i].path_len) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

/* verify_one_pack_objects: deep-verifies one pack against its manifest.
 * Returns 0 clean, 1 if any mismatch/missing/extra entry found. */
static int verify_one_pack_objects(const char *repo, const char *pack_name) {
    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof(manifest_path), "%s/packs/%s.json", repo, pack_name);

    char *format = NULL;
    char *pack_field = NULL;
    int64_t created = 0;
    struct tp_pack_manifest_entry *entries = NULL;
    size_t count = 0;
    if (tp_pack_manifest_read(manifest_path, &format, &pack_field, &created, &entries, &count) !=
        0) {
        printf("%s: FAILED (cannot read manifest)\n", pack_name);
        return 1;
    }
    free(format);
    free(pack_field);

    char archive_path[PATH_MAX];
    snprintf(archive_path, sizeof(archive_path), "%s/packs/%s.tar.zst", repo, pack_name);

    struct archive *a = archive_read_new();
    if (a == NULL) {
        printf("%s: FAILED (cannot allocate archive reader)\n", pack_name);
        tp_pack_manifest_entries_free(entries, count);
        return 1;
    }
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, archive_path, 64 * 1024) != ARCHIVE_OK) {
        printf("%s: FAILED (cannot open archive: %s)\n", pack_name, archive_error_string(a));
        archive_read_free(a);
        tp_pack_manifest_entries_free(entries, count);
        return 1;
    }

    int failed = 0;
    size_t seen_count = 0;
    char **seen_paths = count > 0 ? calloc(count, sizeof(*seen_paths)) : NULL;

    struct archive_entry *entry;
    int rc;
    while ((rc = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char *pathname = archive_entry_pathname(entry);
        const struct tp_pack_manifest_entry *me = find_manifest_entry(entries, count, pathname);
        if (me == NULL) {
            printf("%s: %s: FAILED (present in archive, not in manifest)\n", pack_name, pathname);
            failed = 1;
            archive_read_data_skip(a);
            continue;
        }

        if (seen_paths != NULL && seen_count < count) {
            seen_paths[seen_count++] = strdup(pathname);
        }

        int64_t entry_size = archive_entry_size(entry);
        SHA256_CTX sha;
        sha256_init(&sha);
        char chunk[64 * 1024];
        la_ssize_t r;
        int64_t total = 0;
        while ((r = archive_read_data(a, chunk, sizeof(chunk))) > 0) {
            sha256_update(&sha, (const BYTE *)chunk, (size_t)r);
            total += r;
        }
        if (r < 0) {
            printf("%s: %s: FAILED (read error: %s)\n", pack_name, pathname,
                   archive_error_string(a));
            failed = 1;
            continue;
        }

        unsigned char digest[SHA256_BLOCK_SIZE];
        sha256_final(&sha, digest);
        char hex[65];
        bytes_to_hex(digest, SHA256_BLOCK_SIZE, hex);

        int ok = 1;
        if (total != (int64_t)me->size || entry_size != (int64_t)me->size) {
            printf("%s: %s: FAILED (size mismatch: manifest=%lld archive_header=%lld read=%lld)\n",
                   pack_name, pathname, (long long)me->size, (long long)entry_size,
                   (long long)total);
            ok = 0;
        }
        if (strcmp(hex, me->sha256_hex) != 0) {
            printf("%s: %s: FAILED (sha256 mismatch)\n", pack_name, pathname);
            ok = 0;
        }
        if (ok) {
            printf("%s: %s: OK\n", pack_name, pathname);
        } else {
            failed = 1;
        }
    }
    if (rc != ARCHIVE_EOF) {
        printf("%s: FAILED (archive read error: %s)\n", pack_name, archive_error_string(a));
        failed = 1;
    }

    archive_read_close(a);
    archive_read_free(a);

    /* manifest entries never seen in the archive */
    for (size_t i = 0; i < count; i++) {
        int seen = 0;
        for (size_t j = 0; j < seen_count; j++) {
            if (seen_paths[j] != NULL && entries[i].path_len == strlen(seen_paths[j]) &&
                memcmp(entries[i].path, seen_paths[j], entries[i].path_len) == 0) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            printf("%s: %s: FAILED (in manifest, missing from archive)\n", pack_name,
                   entries[i].path);
            failed = 1;
        }
    }

    if (seen_paths != NULL) {
        for (size_t j = 0; j < seen_count; j++) {
            free(seen_paths[j]);
        }
        free(seen_paths);
    }
    tp_pack_manifest_entries_free(entries, count);

    printf("%s: %s\n", pack_name, failed ? "FAILED" : "OK (objects)");
    return failed;
}

static int verify_all_packs_objects(const char *repo) {
    char packs_dir[PATH_MAX];
    snprintf(packs_dir, sizeof(packs_dir), "%s/packs", repo);

    size_t n = 0;
    char **names = list_dir_names(packs_dir, ".tar.zst", &n);

    int failed = 0;
    for (size_t i = 0; i < n; i++) {
        /* strip ".tar.zst" to get the bare pack name */
        size_t len = strlen(names[i]);
        size_t suffix_len = strlen(".tar.zst");
        if (len <= suffix_len) {
            continue;
        }
        char pack_name[64];
        size_t base_len = len - suffix_len;
        if (base_len >= sizeof(pack_name)) {
            continue;
        }
        memcpy(pack_name, names[i], base_len);
        pack_name[base_len] = '\0';

        if (verify_one_pack_objects(repo, pack_name) != 0) {
            failed = 1;
        }
    }

    free_names(names, n);
    return failed;
}

/* ---------------------------------------------------------------------- */
/* Top level                                                                */
/* ---------------------------------------------------------------------- */

int tp_verify(const char *repo, int deep_objects) {
    if (repo == NULL) {
        tp_warnx("verify: repo is required");
        return 1;
    }

    struct stat st;
    if (stat(repo, &st) != 0 || !S_ISDIR(st.st_mode)) {
        tp_warnx("verify: repo '%s' does not exist", repo);
        return 1;
    }

    int failed = 0;

    char **listed_relpaths = NULL;
    size_t listed_count = 0;
    if (verify_checksums_file(repo, &listed_relpaths, &listed_count) != 0) {
        failed = 1;
    }

    if (verify_packs_vs_checksums(repo, listed_relpaths, listed_count) != 0) {
        failed = 1;
    }

    if (verify_index_vs_packs(repo) != 0) {
        failed = 1;
    }

    free_names(listed_relpaths, listed_count);

    if (deep_objects) {
        if (verify_all_packs_objects(repo) != 0) {
            failed = 1;
        }
    }

    return failed;
}
