#include "checksum.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha256.h"
#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void bytes_to_hex(const unsigned char *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

int tp_sha256_file(const char *path, char out_hex[65]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    SHA256_CTX sha;
    sha256_init(&sha);

    char chunk[64 * 1024];
    for (;;) {
        ssize_t r = read(fd, chunk, sizeof(chunk));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (r == 0) {
            break;
        }
        sha256_update(&sha, (const BYTE *)chunk, (size_t)r);
    }
    close(fd);

    unsigned char digest[SHA256_BLOCK_SIZE];
    sha256_final(&sha, digest);
    bytes_to_hex(digest, SHA256_BLOCK_SIZE, out_hex);
    return 0;
}

/* mkdir_p: creates each path component in turn (like `mkdir -p`). Mirrors
 * the identical static helper in manifest.c/index.c; duplicated here since
 * those are not exported. */
static int mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
                return -1;
            }
            buf[i] = '/';
        }
    }

    if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int tp_checksums_append(const char *repo, const char *sha256_hex, const char *relpath) {
    if (repo == NULL || sha256_hex == NULL || relpath == NULL) {
        return -1;
    }

    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/checksums", repo);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir_p(dir) != 0) {
        return -1;
    }

    char path[PATH_MAX];
    n = snprintf(path, sizeof(path), "%s/SHA256SUMS", dir);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }

    char line[PATH_MAX + 128];
    n = snprintf(line, sizeof(line), "%s  %s\n", sha256_hex, relpath);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }

    size_t len = (size_t)n;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, line + off, len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }

    if (fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    if (close(fd) != 0) {
        return -1;
    }

    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }

    return 0;
}

void tp_checksums_entries_free(struct tp_checksums_line_entry *entries, size_t count) {
    if (entries == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(entries[i].relpath);
    }
    free(entries);
}

int tp_checksums_read(const char *repo, struct tp_checksums_line_entry **entries_out,
                       size_t *count_out) {
    if (repo == NULL || entries_out == NULL || count_out == NULL) {
        return -1;
    }
    *entries_out = NULL;
    *count_out = 0;

    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/checksums/SHA256SUMS", repo);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        if (errno == ENOENT) {
            return 0; /* missing file is not an error */
        }
        return -1;
    }

    struct tp_checksums_line_entry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    int rc = 0;

    while ((line_len = getline(&line, &line_cap, f)) != -1) {
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }
        if (line_len == 0) {
            continue;
        }

        /* format: "<64 hex chars>  <path>" (two spaces) */
        if ((size_t)line_len < 64 + 2 + 1 || line[64] != ' ' || line[65] != ' ') {
            rc = -1;
            break;
        }

        if (count == capacity) {
            size_t new_cap = capacity == 0 ? 8 : capacity * 2;
            struct tp_checksums_line_entry *p = realloc(entries, new_cap * sizeof(*p));
            if (p == NULL) {
                rc = -1;
                break;
            }
            entries = p;
            capacity = new_cap;
        }

        struct tp_checksums_line_entry *e = &entries[count];
        memset(e->sha256_hex, 0, sizeof(e->sha256_hex));
        memcpy(e->sha256_hex, line, 64);
        e->relpath = strdup(line + 66);
        if (e->relpath == NULL) {
            rc = -1;
            break;
        }
        count++;
    }

    free(line);
    fclose(f);

    if (rc != 0) {
        tp_checksums_entries_free(entries, count);
        return -1;
    }

    *entries_out = entries;
    *count_out = count;
    return 0;
}
