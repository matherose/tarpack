#include "packer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "cJSON.h"
#include "checksum.h"
#include "index.h"
#include "manifest.h"
#include "sha256.h"
#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---------------------------------------------------------------------- */
/* Object collection                                                       */
/* ---------------------------------------------------------------------- */

struct collect_ctx {
    struct tp_pack_object_set *set;
    char *root; /* copied from header */
    int oom;    /* set on allocation failure */
};

/* find_by_object_id: linear scan; object sets are small enough (unique
 * objects per snapshot) that this stays inexpensive, and it preserves
 * first-seen ordering trivially. */
static struct tp_packed_object *find_by_object_id(struct tp_pack_object_set *s, const char *id) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->objects[i].object_id, id) == 0) {
            return &s->objects[i];
        }
    }
    return NULL;
}

static int object_set_grow(struct tp_pack_object_set *s) {
    if (s->count < s->capacity) {
        return 0;
    }
    size_t new_cap = s->capacity == 0 ? 8 : s->capacity * 2;
    struct tp_packed_object *p = realloc(s->objects, new_cap * sizeof(*p));
    if (p == NULL) {
        return -1;
    }
    s->objects = p;
    s->capacity = new_cap;
    return 0;
}

static int collect_cb(const struct tp_manifest_entry *entry, void *user_data) {
    struct collect_ctx *ctx = (struct collect_ctx *)user_data;

    if (entry->type == TP_ENTRY_HEADER) {
        if (entry->root != NULL) {
            free(ctx->root);
            ctx->root = strdup(entry->root);
            if (ctx->root == NULL) {
                ctx->oom = 1;
                return -1;
            }
        }
        return 0;
    }

    if (entry->type != TP_ENTRY_FILE || entry->object_id == NULL) {
        return 0; /* dir/symlink ignored */
    }

    if (find_by_object_id(ctx->set, entry->object_id) != NULL) {
        return 0; /* already have this object; keep first-seen representative */
    }

    if (object_set_grow(ctx->set) != 0) {
        ctx->oom = 1;
        return -1;
    }

    struct tp_packed_object *o = &ctx->set->objects[ctx->set->count];
    memset(o, 0, sizeof(*o));
    o->object_id = strdup(entry->object_id);
    o->path = malloc(entry->path_len + 1);
    if (o->object_id == NULL || o->path == NULL) {
        free(o->object_id);
        free(o->path);
        ctx->oom = 1;
        return -1;
    }
    memcpy(o->path, entry->path, entry->path_len);
    o->path[entry->path_len] = '\0';
    o->path_len = entry->path_len;
    o->uid = entry->uid;
    o->gid = entry->gid;
    o->mode_perm = entry->mode_perm;
    o->size = entry->size;
    o->mtime_sec = entry->mtime_sec;
    o->mtime_nsec = entry->mtime_nsec;
    o->offset = 0;
    o->sha256_hex[0] = '\0';

    ctx->set->count++;
    return 0;
}

void tp_pack_object_set_free(struct tp_pack_object_set *s) {
    if (s == NULL || s->objects == NULL) {
        return;
    }
    for (size_t i = 0; i < s->count; i++) {
        free(s->objects[i].object_id);
        free(s->objects[i].path);
    }
    free(s->objects);
    s->objects = NULL;
    s->count = 0;
    s->capacity = 0;
}

int tp_pack_collect(const char *snapshot_path, struct tp_pack_object_set *out, char **root_out) {
    struct collect_ctx ctx;
    ctx.set = out;
    ctx.root = NULL;
    ctx.oom = 0;

    int rc = tp_manifest_read_file(snapshot_path, collect_cb, &ctx);
    if (rc != 0 || ctx.oom) {
        free(ctx.root);
        tp_pack_object_set_free(out);
        return -1;
    }

    if (root_out != NULL) {
        *root_out = ctx.root;
    } else {
        free(ctx.root);
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Size parsing / cost estimation / bin assignment (milestone v1.0)        */
/* ---------------------------------------------------------------------- */

int tp_parse_size(const char *s, int64_t *out_bytes) {
    if (s == NULL || out_bytes == NULL || s[0] == '\0') {
        return -1;
    }

    size_t len = strlen(s);
    char suffix = '\0';
    size_t digits_len = len;

    char last = s[len - 1];
    if ((last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z')) {
        suffix = (char)((last >= 'a' && last <= 'z') ? last - 'a' + 'A' : last);
        digits_len = len - 1;
    }

    if (digits_len == 0) {
        return -1; /* suffix with no digits */
    }
    for (size_t i = 0; i < digits_len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1; /* non-digit (also rejects leading '-') */
        }
    }

    /* parse digits manually to control overflow precisely */
    uint64_t val = 0;
    for (size_t i = 0; i < digits_len; i++) {
        int d = s[i] - '0';
        if (val > (UINT64_MAX - (uint64_t)d) / 10) {
            return -1; /* overflow */
        }
        val = val * 10 + (uint64_t)d;
    }

    uint64_t multiplier = 1;
    switch (suffix) {
        case '\0': multiplier = 1; break;
        case 'K': multiplier = 1024ULL; break;
        case 'M': multiplier = 1024ULL * 1024ULL; break;
        case 'G': multiplier = 1024ULL * 1024ULL * 1024ULL; break;
        default: return -1; /* unknown suffix */
    }

    if (val > (uint64_t)INT64_MAX / multiplier) {
        return -1; /* overflow */
    }
    uint64_t result = val * multiplier;
    if (result > (uint64_t)INT64_MAX) {
        return -1;
    }

    *out_bytes = (int64_t)result;
    return 0;
}

int64_t tp_estimate_entry_cost(off_t size) {
    int64_t sz = (int64_t)size;
    int64_t data_blocks = ((sz + 511) / 512) * 512;
    return 512 + data_blocks + 1536;
}

long tp_bin_assign_next_fit(const struct tp_bin_item *items, size_t count, int64_t target,
                             size_t *bin_out) {
    if (target <= 0) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (items == NULL || bin_out == NULL) {
        return -1;
    }

    long bin_count = 0;
    int have_current = 0;
    int64_t current_used = 0;

    for (size_t i = 0; i < count; i++) {
        int64_t cost = items[i].cost;
        if (cost > target) {
            /* oversized: always alone in a brand new bin; closes any
             * current bin (does not merge with it). */
            bin_out[i] = (size_t)bin_count;
            bin_count++;
            have_current = 0;
            current_used = 0;
            continue;
        }
        if (!have_current || current_used + cost > target) {
            /* start a fresh bin */
            bin_out[i] = (size_t)bin_count;
            bin_count++;
            have_current = 1;
            current_used = cost;
        } else {
            bin_out[i] = (size_t)(bin_count - 1);
            current_used += cost;
        }
    }

    return bin_count;
}

/* bin_rem: dynamic array of remaining capacity per open bin, used by FFD. */
struct bin_rem_array {
    int64_t *rem;
    size_t count;
    size_t capacity;
};

static int bin_rem_push(struct bin_rem_array *a, int64_t rem) {
    if (a->count == a->capacity) {
        size_t new_cap = a->capacity == 0 ? 8 : a->capacity * 2;
        int64_t *p = realloc(a->rem, new_cap * sizeof(*p));
        if (p == NULL) {
            return -1;
        }
        a->rem = p;
        a->capacity = new_cap;
    }
    a->rem[a->count++] = rem;
    return 0;
}

/* sort permutation: descending by cost, ties broken by ascending original
 * index (stable determinism requirement). */
static int ffd_cmp_ctx_cost_desc(const void *pa, const void *pb, void *ctx) {
    const struct tp_bin_item *items = (const struct tp_bin_item *)ctx;
    size_t ia = *(const size_t *)pa;
    size_t ib = *(const size_t *)pb;
    if (items[ia].cost != items[ib].cost) {
        return items[ia].cost > items[ib].cost ? -1 : 1;
    }
    if (ia != ib) {
        return ia < ib ? -1 : 1;
    }
    return 0;
}

/* Portable qsort_r shim: BSD/macOS qsort_r has signature
 * (base, nel, width, thunk, cmp(thunk, a, b)); glibc's differs. We avoid the
 * portability trap entirely by implementing a tiny insertion/merge sort by
 * hand over indices, since bin counts here are expected to stay modest and
 * correctness/portability matter more than micro-optimizing this path. */
static void ffd_sort_indices(size_t *order, size_t count, const struct tp_bin_item *items) {
    /* simple merge sort for O(n log n) determinism without qsort_r games */
    if (count < 2) {
        return;
    }
    size_t mid = count / 2;
    ffd_sort_indices(order, mid, items);
    ffd_sort_indices(order + mid, count - mid, items);

    size_t *tmp = malloc(count * sizeof(*tmp));
    if (tmp == NULL) {
        /* fall back to a simple insertion sort in place (still correct,
         * just O(n^2)); only reachable under OOM pressure. */
        for (size_t i = 1; i < count; i++) {
            size_t key = order[i];
            size_t j = i;
            while (j > 0 && ffd_cmp_ctx_cost_desc(&key, &order[j - 1], (void *)items) < 0) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = key;
        }
        return;
    }

    size_t i = 0, j = mid, k = 0;
    while (i < mid && j < count) {
        if (ffd_cmp_ctx_cost_desc(&order[i], &order[j], (void *)items) <= 0) {
            tmp[k++] = order[i++];
        } else {
            tmp[k++] = order[j++];
        }
    }
    while (i < mid) tmp[k++] = order[i++];
    while (j < count) tmp[k++] = order[j++];
    memcpy(order, tmp, count * sizeof(*tmp));
    free(tmp);
}

long tp_bin_assign_ffd(const struct tp_bin_item *items, size_t count, int64_t target,
                        size_t *bin_out) {
    if (target <= 0) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (items == NULL || bin_out == NULL) {
        return -1;
    }

    size_t *order = malloc(count * sizeof(*order));
    if (order == NULL) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        order[i] = i;
    }
    ffd_sort_indices(order, count, items);

    struct bin_rem_array bins;
    memset(&bins, 0, sizeof(bins));

    long rc = 0;
    for (size_t oi = 0; oi < count; oi++) {
        size_t idx = order[oi];
        int64_t cost = items[idx].cost;

        if (cost > target) {
            /* oversized: always a brand new, immediately-full bin. */
            if (bin_rem_push(&bins, 0) != 0) {
                rc = -1;
                break;
            }
            bin_out[idx] = bins.count - 1;
            continue;
        }

        size_t chosen = SIZE_MAX;
        for (size_t b = 0; b < bins.count; b++) {
            if (bins.rem[b] >= cost) {
                chosen = b;
                break;
            }
        }
        if (chosen == SIZE_MAX) {
            if (bin_rem_push(&bins, target - cost) != 0) {
                rc = -1;
                break;
            }
            chosen = bins.count - 1;
        } else {
            bins.rem[chosen] -= cost;
        }
        bin_out[idx] = chosen;
    }

    long bin_count = (long)bins.count;
    free(bins.rem);
    free(order);

    return rc == 0 ? bin_count : -1;
}

/* ---------------------------------------------------------------------- */
/* Pack manifest (JSON document) writer                                    */
/* ---------------------------------------------------------------------- */

/* add_path_field: writes "path" (UTF-8) or "path_b64" (base64 of raw bytes)
 * per the snapshot encoding rule. Returns 0 on success, -1 on failure. */
static int add_path_field(cJSON *obj, const char *raw, size_t raw_len) {
    if (tp_utf8_valid((const uint8_t *)raw, raw_len)) {
        cJSON *item = cJSON_CreateString(raw);
        if (item == NULL || !cJSON_AddItemToObject(obj, "path", item)) {
            cJSON_Delete(item);
            return -1;
        }
        return 0;
    }
    char *b64 = tp_base64_encode((const uint8_t *)raw, raw_len);
    if (b64 == NULL) {
        return -1;
    }
    cJSON *item = cJSON_CreateString(b64);
    free(b64);
    if (item == NULL || !cJSON_AddItemToObject(obj, "path_b64", item)) {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

/* write_file_atomic: writes len bytes to <path>.part, fsyncs it and the
 * containing directory, then renames to path. Returns 0 on success, -1 on
 * failure (errno set / warning emitted by caller). */
static int write_file_atomic(const char *path, const char *dir, const char *data, size_t len) {
    char part[PATH_MAX];
    int n = snprintf(part, sizeof(part), "%s.part", path);
    if (n < 0 || (size_t)n >= sizeof(part)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(part);
            return -1;
        }
        off += (size_t)w;
    }

    if (fsync(fd) != 0) {
        close(fd);
        unlink(part);
        return -1;
    }
    if (close(fd) != 0) {
        unlink(part);
        return -1;
    }

    if (rename(part, path) != 0) {
        unlink(part);
        return -1;
    }

    /* fsync the directory so the rename is durable */
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;
}

int tp_pack_manifest_write(const char *path, const char *pack_name, int64_t created,
                            const struct tp_pack_object_set *set) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }

    int ok = 1;
    ok = ok && cJSON_AddStringToObject(root, "format", "tarpack-pack-v1") != NULL;
    ok = ok && cJSON_AddStringToObject(root, "pack", pack_name) != NULL;
    ok = ok && cJSON_AddNumberToObject(root, "created", (double)created) != NULL;

    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    ok = ok && entries != NULL;

    for (size_t i = 0; ok && i < set->count; i++) {
        const struct tp_packed_object *o = &set->objects[i];
        cJSON *e = cJSON_CreateObject();
        if (e == NULL || !cJSON_AddItemToArray(entries, e)) {
            cJSON_Delete(e);
            ok = 0;
            break;
        }
        ok = ok && cJSON_AddStringToObject(e, "object_id", o->object_id) != NULL;
        ok = ok && add_path_field(e, o->path, o->path_len) == 0;
        ok = ok && cJSON_AddNumberToObject(e, "offset", (double)o->offset) != NULL;
        ok = ok && cJSON_AddNumberToObject(e, "size", (double)o->size) != NULL;
        ok = ok && cJSON_AddStringToObject(e, "sha256", o->sha256_hex) != NULL;
    }

    char *text = NULL;
    if (ok) {
        text = cJSON_PrintUnformatted(root);
        ok = text != NULL;
    }
    cJSON_Delete(root);
    if (!ok) {
        free(text);
        return -1;
    }

    /* determine the directory containing path for the durability fsync */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        dir[0] = '.';
        dir[1] = '\0';
    }

    int rc = write_file_atomic(path, dir, text, strlen(text));
    free(text);
    return rc;
}

/* ---------------------------------------------------------------------- */
/* Pack manifest reader                                                    */
/* ---------------------------------------------------------------------- */

/* base64 decode helper, mirrors the manifest reader's decoding. Returns a
 * malloc'd buffer (out_len set) or NULL on failure. */
static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char *b64_decode(const char *in, size_t *out_len) {
    size_t in_len = strlen(in);
    if (in_len % 4 != 0) {
        return NULL;
    }
    size_t pad = 0;
    if (in_len >= 1 && in[in_len - 1] == '=') pad++;
    if (in_len >= 2 && in[in_len - 2] == '=') pad++;

    size_t out_cap = in_len / 4 * 3;
    char *out = malloc(out_cap + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        int v0 = b64_val(in[i]);
        int v1 = b64_val(in[i + 1]);
        int v2 = in[i + 2] == '=' ? 0 : b64_val(in[i + 2]);
        int v3 = in[i + 3] == '=' ? 0 : b64_val(in[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            free(out);
            return NULL;
        }
        uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) |
                          (uint32_t)v3;
        out[o++] = (char)((triple >> 16) & 0xFF);
        out[o++] = (char)((triple >> 8) & 0xFF);
        out[o++] = (char)(triple & 0xFF);
    }
    o -= pad;
    out[o] = '\0';
    *out_len = o;
    return out;
}

void tp_pack_manifest_entries_free(struct tp_pack_manifest_entry *entries, size_t count) {
    if (entries == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(entries[i].object_id);
        free(entries[i].path);
    }
    free(entries);
}

int tp_pack_manifest_read(const char *path, char **format_out, char **pack_out,
                           int64_t *created_out, struct tp_pack_manifest_entry **entries_out,
                           size_t *count_out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return -1;
    }

    int rc = -1;
    struct tp_pack_manifest_entry *entries = NULL;
    size_t count = 0;

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(arr)) {
        goto done;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > 0) {
        entries = calloc((size_t)n, sizeof(*entries));
        if (entries == NULL) {
            goto done;
        }
    }

    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        struct tp_pack_manifest_entry *out = &entries[i];

        cJSON *oid = cJSON_GetObjectItemCaseSensitive(e, "object_id");
        cJSON *poff = cJSON_GetObjectItemCaseSensitive(e, "offset");
        cJSON *psize = cJSON_GetObjectItemCaseSensitive(e, "size");
        cJSON *psha = cJSON_GetObjectItemCaseSensitive(e, "sha256");
        cJSON *ppath = cJSON_GetObjectItemCaseSensitive(e, "path");
        cJSON *ppath_b64 = cJSON_GetObjectItemCaseSensitive(e, "path_b64");

        if (!cJSON_IsString(oid) || !cJSON_IsNumber(poff) || !cJSON_IsNumber(psize) ||
            !cJSON_IsString(psha)) {
            goto done;
        }

        out->object_id = strdup(oid->valuestring);
        if (out->object_id == NULL) {
            goto done;
        }
        out->offset = (int64_t)poff->valuedouble;
        out->size = (off_t)psize->valuedouble;
        snprintf(out->sha256_hex, sizeof(out->sha256_hex), "%s", psha->valuestring);

        if (cJSON_IsString(ppath)) {
            out->path = strdup(ppath->valuestring);
            if (out->path == NULL) {
                goto done;
            }
            out->path_len = strlen(out->path);
        } else if (cJSON_IsString(ppath_b64)) {
            out->path = b64_decode(ppath_b64->valuestring, &out->path_len);
            if (out->path == NULL) {
                goto done;
            }
        } else {
            goto done;
        }
        count = (size_t)(i + 1);
    }

    if (format_out != NULL) {
        cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
        *format_out = cJSON_IsString(fmt) ? strdup(fmt->valuestring) : NULL;
    }
    if (pack_out != NULL) {
        cJSON *pk = cJSON_GetObjectItemCaseSensitive(root, "pack");
        *pack_out = cJSON_IsString(pk) ? strdup(pk->valuestring) : NULL;
    }
    if (created_out != NULL) {
        cJSON *cr = cJSON_GetObjectItemCaseSensitive(root, "created");
        *created_out = cJSON_IsNumber(cr) ? (int64_t)cr->valuedouble : 0;
    }

    *entries_out = entries;
    *count_out = count;
    entries = NULL; /* transferred to caller */
    rc = 0;

done:
    cJSON_Delete(root);
    if (entries != NULL) {
        tp_pack_manifest_entries_free(entries, count);
    }
    return rc;
}

/* ---------------------------------------------------------------------- */
/* Pack writer                                                             */
/* ---------------------------------------------------------------------- */

/* ensure_utf8_ctype: libarchive translates entry pathnames through the
 * process LC_CTYPE charset. Our representative paths are stored as raw UTF-8
 * bytes, so if the inherited locale is C/POSIX (ASCII), libarchive rejects
 * multibyte names with "Can't translate pathname ... to UTF-8". Nudge
 * LC_CTYPE to a UTF-8 locale so the conversion is a no-op. Best effort: if no
 * UTF-8 locale is available we proceed and let libarchive report per-entry. */
static void ensure_utf8_ctype(void) {
    const char *cur = setlocale(LC_CTYPE, NULL);
    if (cur != NULL && (strstr(cur, "UTF-8") != NULL || strstr(cur, "utf8") != NULL)) {
        return; /* already UTF-8 */
    }
    /* Respect the environment first; if that yields UTF-8 we are done. */
    const char *env = setlocale(LC_CTYPE, "");
    if (env != NULL && (strstr(env, "UTF-8") != NULL || strstr(env, "utf8") != NULL)) {
        return;
    }
    static const char *const candidates[] = {"C.UTF-8", "en_US.UTF-8", "POSIX.UTF-8", NULL};
    for (int i = 0; candidates[i] != NULL; i++) {
        if (setlocale(LC_CTYPE, candidates[i]) != NULL) {
            return;
        }
    }
}

static void bytes_to_hex(const unsigned char *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

/* pick_latest_snapshot: returns a malloc'd label (without .jsonl) of the
 * lexicographically greatest snapshot in <repo>/snapshots, or NULL if none /
 * on error. */
static char *pick_latest_snapshot(const char *repo) {
    char dirpath[PATH_MAX];
    if (snprintf(dirpath, sizeof(dirpath), "%s/snapshots", repo) >= (int)sizeof(dirpath)) {
        return NULL;
    }
    DIR *d = opendir(dirpath);
    if (d == NULL) {
        return NULL;
    }
    char best[NAME_MAX + 1];
    best[0] = '\0';
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        size_t len = strlen(name);
        if (len < 7 || strcmp(name + len - 6, ".jsonl") != 0) {
            continue;
        }
        if (best[0] == '\0' || strcmp(name, best) > 0) {
            snprintf(best, sizeof(best), "%s", name);
        }
    }
    closedir(d);
    if (best[0] == '\0') {
        return NULL;
    }
    best[strlen(best) - 6] = '\0'; /* strip .jsonl */
    return strdup(best);
}

/* next_pack_number: scans <repo>/packs for files named "pack-NNNNNN.tar.zst"
 * and returns (max NNNNNN)+1, or 1 if none exist / the directory is
 * missing. Used instead of v0.2's "error if pack-000001 exists" guard, so
 * repeated packs land in fresh, sequentially numbered files. */
static unsigned long next_pack_number(const char *packs_dir) {
    DIR *d = opendir(packs_dir);
    if (d == NULL) {
        return 1;
    }
    unsigned long best = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        unsigned long n = 0;
        char suffix[16];
        if (sscanf(name, "pack-%6lu%15s", &n, suffix) == 2 && strcmp(suffix, ".tar.zst") == 0) {
            if (n > best) {
                best = n;
            }
        }
    }
    closedir(d);
    return best + 1;
}

/* pack_one_object: streams the file for object o from root_fd into the
 * archive, computing its sha256 and recording its data offset. Returns 0 on
 * success, 1 on a non-fatal warning (e.g. size mismatch handled), -1 fatal. */
static int pack_one_object(struct archive *a, int root_fd, struct tp_packed_object *o) {
    int fd = safe_openat(root_fd, o->path, O_RDONLY);
    if (fd < 0) {
        tp_warn("pack: cannot open '%s'", o->path);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        tp_warn("pack: cannot fstat '%s'", o->path);
        close(fd);
        return -1;
    }

    int warned = 0;
    off_t write_size = o->size;
    if (st.st_size != o->size) {
        tp_warnx("pack: '%s' changed size (snapshot %lld, now %lld); using current size",
                 o->path, (long long)o->size, (long long)st.st_size);
        write_size = st.st_size;
        warned = 1;
    }

    struct archive_entry *entry = archive_entry_new();
    if (entry == NULL) {
        close(fd);
        return -1;
    }
    archive_entry_set_pathname(entry, o->path);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, o->mode_perm);
    archive_entry_set_uid(entry, o->uid);
    archive_entry_set_gid(entry, o->gid);
    archive_entry_set_size(entry, write_size);
    archive_entry_set_mtime(entry, o->mtime_sec, o->mtime_nsec);

    if (archive_write_header(a, entry) != ARCHIVE_OK) {
        tp_warnx("pack: archive_write_header failed for '%s': %s", o->path, archive_error_string(a));
        archive_entry_free(entry);
        close(fd);
        return -1;
    }

    /* Offset of file DATA in the UNCOMPRESSED tar stream. Empirically (see the
     * probe programs / integration test), for a WRITE archive with filters
     * [0]=zstd, [1]=(client/output), archive_filter_bytes(a, 0) reports the
     * number of bytes handed to the compression filter -- i.e. the raw tar
     * stream position. Immediately after archive_write_header() returns, that
     * position is exactly where this entry's data begins (all header blocks,
     * including any pax extended header, have already been emitted). This is
     * validated against the real decompressed stream by tests/integration. */
    o->offset = archive_filter_bytes(a, 0);

    SHA256_CTX sha;
    sha256_init(&sha);

    char chunk[64 * 1024];
    off_t remaining = write_size;
    int rc = 0;
    while (remaining > 0) {
        size_t want = remaining > (off_t)sizeof(chunk) ? sizeof(chunk) : (size_t)remaining;
        ssize_t r = read(fd, chunk, want);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            tp_warn("pack: read error on '%s'", o->path);
            rc = -1;
            break;
        }
        if (r == 0) {
            /* file shrank mid-read; pad remainder with zeros to honor the
             * header size we already committed. */
            memset(chunk, 0, sizeof(chunk));
            while (remaining > 0) {
                size_t pad = remaining > (off_t)sizeof(chunk) ? sizeof(chunk) : (size_t)remaining;
                if (archive_write_data(a, chunk, pad) < 0) {
                    rc = -1;
                    break;
                }
                sha256_update(&sha, (const BYTE *)chunk, pad);
                remaining -= (off_t)pad;
            }
            tp_warnx("pack: '%s' shrank during read; padded with zeros", o->path);
            warned = 1;
            break;
        }
        if (archive_write_data(a, chunk, (size_t)r) < 0) {
            tp_warnx("pack: archive_write_data failed for '%s': %s", o->path,
                     archive_error_string(a));
            rc = -1;
            break;
        }
        sha256_update(&sha, (const BYTE *)chunk, (size_t)r);
        remaining -= r;
    }

    close(fd);
    archive_entry_free(entry);

    if (rc != 0) {
        return -1;
    }

    unsigned char digest[SHA256_BLOCK_SIZE];
    sha256_final(&sha, digest);
    bytes_to_hex(digest, SHA256_BLOCK_SIZE, o->sha256_hex);

    return warned ? 1 : 0;
}

/* ---------------------------------------------------------------------- */
/* Multi-pack splitting (milestone v1.0)                                   */
/* ---------------------------------------------------------------------- */

/* subset_ref: a lightweight, non-owning view over a contiguous run of
 * tp_packed_object entries borrowed from a larger tp_pack_object_set. Used
 * so write_one_pack_and_commit can operate on one bin's worth of objects
 * without any copying/reallocation. */
struct subset_ref {
    struct tp_packed_object *objects; /* points into the owning set's array */
    size_t count;
};

/* free_ordered_objects: frees the owned strings inside a plain array of
 * tp_packed_object (as produced by tp_pack_multi's stable partition, which
 * takes over ownership from the collected object set) plus the array
 * itself. Safe on NULL. */
static void free_ordered_objects(struct tp_packed_object *arr, size_t count) {
    if (arr == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(arr[i].object_id);
        free(arr[i].path);
    }
    free(arr);
}

/* ---------------------------------------------------------------------- */
/* Crash recovery (run at the start of every pack)                         */
/* ---------------------------------------------------------------------- */

/* repair_index_tail: a kill -9 during tp_index_append can leave a torn
 * (unterminated) final line in objects.jsonl. Left alone, the NEXT append
 * would concatenate onto that fragment and corrupt the index. This
 * truncates the file back to the last complete ('\n'-terminated) line.
 * Missing/empty file is fine. Returns 0 on success, -1 on I/O failure. */
static int repair_index_tail(const char *repo) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/objects/objects.jsonl", repo) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    if (st.st_size == 0) {
        close(fd);
        return 0;
    }

    char last;
    if (pread(fd, &last, 1, st.st_size - 1) != 1) {
        close(fd);
        return -1;
    }
    if (last == '\n') {
        close(fd);
        return 0; /* clean tail */
    }

    /* torn final line: scan backwards for the last '\n' */
    off_t good = 0; /* truncate target: byte AFTER the last '\n', or 0 */
    char chunk[4096];
    off_t pos = st.st_size;
    while (pos > 0 && good == 0) {
        size_t want = pos > (off_t)sizeof(chunk) ? sizeof(chunk) : (size_t)pos;
        off_t start = pos - (off_t)want;
        ssize_t r = pread(fd, chunk, want, start);
        if (r != (ssize_t)want) {
            close(fd);
            return -1;
        }
        for (ssize_t i = r - 1; i >= 0; i--) {
            if (chunk[i] == '\n') {
                good = start + i + 1;
                break;
            }
        }
        pos = start;
    }

    tp_warnx("pack: repairing torn objects.jsonl tail (truncating %lld -> %lld bytes)",
             (long long)st.st_size, (long long)good);
    if (ftruncate(fd, good) != 0) {
        close(fd);
        return -1;
    }
    if (fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* pack_name_set: tiny dynamic array of pack names for recovery. */
struct pack_name_set {
    char **names;
    size_t count;
    size_t capacity;
};

static int pack_name_set_add(struct pack_name_set *s, const char *name) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) {
            return 0;
        }
    }
    if (s->count == s->capacity) {
        size_t new_cap = s->capacity == 0 ? 8 : s->capacity * 2;
        char **p = realloc(s->names, new_cap * sizeof(*p));
        if (p == NULL) {
            return -1;
        }
        s->names = p;
        s->capacity = new_cap;
    }
    s->names[s->count] = strdup(name);
    if (s->names[s->count] == NULL) {
        return -1;
    }
    s->count++;
    return 0;
}

static int pack_name_set_has(const struct pack_name_set *s, const char *name) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void pack_name_set_free(struct pack_name_set *s) {
    for (size_t i = 0; i < s->count; i++) {
        free(s->names[i]);
    }
    free(s->names);
    s->names = NULL;
    s->count = 0;
    s->capacity = 0;
}

/* collect_indexed_packs: builds the set of pack names referenced by any
 * complete line of objects.jsonl. Must run AFTER repair_index_tail. A
 * missing index file yields an empty set. Returns 0/-1. */
static int collect_indexed_packs(const char *repo, struct pack_name_set *out) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/objects/objects.jsonl", repo) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    int rc = 0;
    char *line = NULL;
    size_t line_cap = 0;
    while (getline(&line, &line_cap, f) != -1) {
        const char *key = strstr(line, "\"pack\":\"");
        if (key == NULL) {
            continue;
        }
        key += strlen("\"pack\":\"");
        const char *end = strchr(key, '"');
        if (end == NULL) {
            continue;
        }
        char name[64];
        size_t len = (size_t)(end - key);
        if (len >= sizeof(name)) {
            continue;
        }
        memcpy(name, key, len);
        name[len] = '\0';
        if (pack_name_set_add(out, name) != 0) {
            rc = -1;
            break;
        }
    }
    free(line);
    fclose(f);
    return rc;
}

/* rewrite_checksums_filtered: drops SHA256SUMS lines whose file no longer
 * exists under repo (stale entries for packs deleted by recovery). Only
 * rewrites (atomically: .part + fsync + rename) when something was
 * dropped. Returns 0/-1. */
static int rewrite_checksums_filtered(const char *repo) {
    struct tp_checksums_line_entry *entries = NULL;
    size_t count = 0;
    if (tp_checksums_read(repo, &entries, &count) != 0) {
        return -1;
    }
    if (count == 0) {
        tp_checksums_entries_free(entries, count);
        return 0;
    }

    size_t kept = 0;
    int *keep = calloc(count, sizeof(*keep));
    if (keep == NULL) {
        tp_checksums_entries_free(entries, count);
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        char full[PATH_MAX];
        struct stat st;
        if (snprintf(full, sizeof(full), "%s/%s", repo, entries[i].relpath) <
                (int)sizeof(full) &&
            stat(full, &st) == 0) {
            keep[i] = 1;
            kept++;
        }
    }

    int rc = 0;
    if (kept != count) {
        tp_warnx("pack: dropping %zu stale checksum line(s) from SHA256SUMS",
                 count - kept);
        /* build the filtered content in memory (SHA256SUMS is small: two
         * lines per pack) */
        size_t buf_cap = kept * (64 + 2 + PATH_MAX / 4) + 1;
        char *buf = malloc(buf_cap);
        if (buf == NULL) {
            rc = -1;
        } else {
            size_t off = 0;
            for (size_t i = 0; i < count && rc == 0; i++) {
                if (!keep[i]) {
                    continue;
                }
                int n = snprintf(buf + off, buf_cap - off, "%s  %s\n", entries[i].sha256_hex,
                                 entries[i].relpath);
                if (n < 0 || (size_t)n >= buf_cap - off) {
                    rc = -1;
                    break;
                }
                off += (size_t)n;
            }
            if (rc == 0) {
                char sums_path[PATH_MAX];
                char sums_dir[PATH_MAX];
                if (snprintf(sums_dir, sizeof(sums_dir), "%s/checksums", repo) >=
                        (int)sizeof(sums_dir) ||
                    snprintf(sums_path, sizeof(sums_path), "%s/checksums/SHA256SUMS", repo) >=
                        (int)sizeof(sums_path)) {
                    rc = -1;
                } else {
                    rc = write_file_atomic(sums_path, sums_dir, buf, off);
                }
            }
            free(buf);
        }
    }

    free(keep);
    tp_checksums_entries_free(entries, count);
    return rc;
}

/* recover_crashed_state: brings the repo back to a fully consistent state
 * after a possible earlier crash, BEFORE any new pack is written:
 *   - deletes stale *.part scratch files under packs/ (documented behavior
 *     for orphans like pack-000042.tar.zst.part: they are removed here and
 *     are never counted as packs);
 *   - deletes committed pack files (and their .json manifests) that are
 *     not referenced by any complete objects.jsonl line -- by the commit
 *     ordering (index appended last), such packs never "happened", and
 *     their objects will simply be re-packed by this run;
 *   - drops SHA256SUMS lines whose file no longer exists.
 * Requires repair_index_tail to have run first. Returns 0/-1. */
static int recover_crashed_state(const char *repo, const char *packs_dir) {
    struct pack_name_set indexed;
    memset(&indexed, 0, sizeof(indexed));
    if (collect_indexed_packs(repo, &indexed) != 0) {
        pack_name_set_free(&indexed);
        return -1;
    }

    DIR *d = opendir(packs_dir);
    if (d != NULL) {
        /* collect names first; unlinking while iterating readdir is UB on
         * some platforms */
        struct pack_name_set to_delete;
        memset(&to_delete, 0, sizeof(to_delete));
        int oom = 0;

        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *name = de->d_name;
            size_t len = strlen(name);

            int is_part = len > 5 && strcmp(name + len - 5, ".part") == 0;
            int is_pack = 0;
            char base[64];
            if (!is_part && len > strlen(".tar.zst") &&
                strcmp(name + len - strlen(".tar.zst"), ".tar.zst") == 0) {
                size_t base_len = len - strlen(".tar.zst");
                if (base_len < sizeof(base)) {
                    memcpy(base, name, base_len);
                    base[base_len] = '\0';
                    unsigned long num = 0;
                    char rest = '\0';
                    if (sscanf(base, "pack-%6lu%c", &num, &rest) == 1) {
                        is_pack = 1;
                    }
                }
            }

            if (is_part) {
                tp_warnx("pack: removing stale scratch file '%s'", name);
                if (pack_name_set_add(&to_delete, name) != 0) {
                    oom = 1;
                    break;
                }
            } else if (is_pack && !pack_name_set_has(&indexed, base)) {
                tp_warnx("pack: removing uncommitted pack '%s' left by an earlier crash", base);
                if (pack_name_set_add(&to_delete, name) != 0) {
                    oom = 1;
                    break;
                }
                char json_name[80];
                snprintf(json_name, sizeof(json_name), "%s.json", base);
                if (pack_name_set_add(&to_delete, json_name) != 0) {
                    oom = 1;
                    break;
                }
            }
        }
        closedir(d);

        if (oom) {
            pack_name_set_free(&to_delete);
            pack_name_set_free(&indexed);
            return -1;
        }

        for (size_t i = 0; i < to_delete.count; i++) {
            char full[PATH_MAX];
            if (snprintf(full, sizeof(full), "%s/%s", packs_dir, to_delete.names[i]) <
                (int)sizeof(full)) {
                if (unlink(full) != 0 && errno != ENOENT) {
                    tp_warn("pack: cannot remove '%s'", full);
                }
            }
        }
        pack_name_set_free(&to_delete);
    }

    pack_name_set_free(&indexed);

    return rewrite_checksums_filtered(repo);
}

/* write_one_pack_and_commit: writes ONE pack archive containing exactly the
 * objects in `subset` (a contiguous slice of a larger object set, order
 * preserved), through the full sequence required for crash safety:
 *   1) write pack-NNNNNN.tar.zst.part, fsync file, fsync packs/ dir, rename
 *   2) write pack-NNNNNN.json (already atomic: .part+fsync+rename, done by
 *      tp_pack_manifest_write)
 *   3) append two lines to checksums/SHA256SUMS (pack .tar.zst, then .json)
 *   4) append this pack's objects to objects/objects.jsonl
 * Step (4) only runs after steps (1)-(3) succeeded, and each step only
 * starts after the previous one's on-disk artifact is durable, so a crash
 * at any point leaves a fully consistent repo: either this pack is fully
 * indexed+checksummed, or it doesn't count as done at all (the .part file,
 * if any, is not a finished pack).
 *
 * root_fd: open fd on the snapshot root, used to read source file data.
 * pack_num: the sequence number this pack will use (caller assigns,
 * monotonically increasing across calls for one tp_pack_multi run).
 * out_pack_name: buffer (>= 32 bytes) that receives "pack-NNNNNN" on
 * success, for logging/testing purposes.
 *
 * Returns 0 clean, 1 completed with warnings, -1 fatal.
 */
static int write_one_pack_and_commit(const char *repo, const char *packs_dir, int root_fd,
                                      unsigned long pack_num, struct subset_ref subset,
                                      char *out_pack_name, size_t out_pack_name_size) {
    int result = 0;

    char pack_name[32];
    snprintf(pack_name, sizeof(pack_name), "pack-%06lu", pack_num);
    if (out_pack_name != NULL) {
        snprintf(out_pack_name, out_pack_name_size, "%s", pack_name);
    }

    char pack_path[PATH_MAX];
    char pack_part[PATH_MAX];
    char manifest_path[PATH_MAX];
    snprintf(pack_path, sizeof(pack_path), "%s/%s.tar.zst", packs_dir, pack_name);
    snprintf(pack_part, sizeof(pack_part), "%s/%s.tar.zst.part", packs_dir, pack_name);
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s.json", packs_dir, pack_name);

    /* Any stale orphan <pack_name>.tar.zst.part left over from a previous
     * crashed run at this same pack number is not a "finished pack" (see
     * next_pack_number(), which only recognizes committed .tar.zst files),
     * so a fresh run is free to overwrite it: O_TRUNC below discards
     * whatever bytes it contained and we start writing from scratch. */

    ensure_utf8_ctype();
    struct archive *a = archive_write_new();
    if (a == NULL) {
        tp_warnx("pack: out of memory creating archive writer");
        return -1;
    }
    if (archive_write_add_filter_zstd(a) != ARCHIVE_OK ||
        archive_write_set_format_pax_restricted(a) != ARCHIVE_OK ||
        archive_write_set_options(a, "hdrcharset=UTF-8") != ARCHIVE_OK ||
        archive_write_open_filename(a, pack_part) != ARCHIVE_OK) {
        tp_warnx("pack: cannot open archive '%s': %s", pack_part, archive_error_string(a));
        archive_write_free(a);
        return -1;
    }

    for (size_t i = 0; i < subset.count; i++) {
        int prc = pack_one_object(a, root_fd, &subset.objects[i]);
        if (prc < 0) {
            archive_write_free(a);
            unlink(pack_part);
            return -1;
        }
        if (prc == 1) {
            result = 1;
        }
    }

    if (archive_write_close(a) != ARCHIVE_OK) {
        tp_warnx("pack: archive_write_close failed: %s", archive_error_string(a));
        archive_write_free(a);
        unlink(pack_part);
        return -1;
    }
    archive_write_free(a);

    /* fsync the .part file, then rename to final name, then fsync the dir */
    int pfd = open(pack_part, O_RDONLY | O_CLOEXEC);
    if (pfd < 0) {
        tp_warn("pack: cannot reopen '%s' for fsync", pack_part);
        unlink(pack_part);
        return -1;
    }
    if (fsync(pfd) != 0) {
        tp_warn("pack: fsync failed on '%s'", pack_part);
        close(pfd);
        unlink(pack_part);
        return -1;
    }
    close(pfd);

    if (rename(pack_part, pack_path) != 0) {
        tp_warn("pack: rename '%s' -> '%s' failed", pack_part, pack_path);
        unlink(pack_part);
        return -1;
    }

    int dfd = open(packs_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }

    /* pack manifest: atomic .part+fsync+rename handled inside the helper */
    struct tp_pack_object_set subset_set;
    subset_set.objects = subset.objects;
    subset_set.count = subset.count;
    subset_set.capacity = subset.count;
    if (tp_pack_manifest_write(manifest_path, pack_name, (int64_t)time(NULL), &subset_set) != 0) {
        tp_warn("pack: failed to write pack manifest '%s'", manifest_path);
        return -1;
    }

    /* checksums/SHA256SUMS: pack archive first, then its manifest, matching
     * the "packs first, metadata after" ordering used elsewhere (upload).
     * We re-read the finished, already-fsynced .tar.zst/.json files from
     * disk to compute their checksums rather than hashing while streaming
     * the compressed bytes out; see tp_pack_multi's doc comment / the
     * milestone report for the rationale (archive_write_open2 callback
     * plumbing vs. a simple, already-correct re-read of a file we just
     * fsynced is not worth the added complexity here). */
    {
        char hex[65];
        char rel_pack[64];
        char rel_json[64];
        snprintf(rel_pack, sizeof(rel_pack), "packs/%s.tar.zst", pack_name);
        snprintf(rel_json, sizeof(rel_json), "packs/%s.json", pack_name);

        if (tp_sha256_file(pack_path, hex) != 0) {
            tp_warn("pack: failed to hash '%s' for checksums", pack_path);
            return -1;
        }
        if (tp_checksums_append(repo, hex, rel_pack) != 0) {
            tp_warn("pack: failed to append checksum for '%s'", rel_pack);
            return -1;
        }
        if (tp_sha256_file(manifest_path, hex) != 0) {
            tp_warn("pack: failed to hash '%s' for checksums", manifest_path);
            return -1;
        }
        if (tp_checksums_append(repo, hex, rel_json) != 0) {
            tp_warn("pack: failed to append checksum for '%s'", rel_json);
            return -1;
        }
    }

    /* object index: MUST happen last, after the pack file, its manifest,
     * and both checksum lines are durable, so the index never references a
     * pack that doesn't fully exist yet. */
    {
        struct tp_index_entry *entries = calloc(subset.count, sizeof(*entries));
        if (entries == NULL) {
            tp_warnx("pack: out of memory building index entries");
            return -1;
        }
        for (size_t i = 0; i < subset.count; i++) {
            const struct tp_packed_object *o = &subset.objects[i];
            entries[i].object_id = o->object_id;
            entries[i].pack = pack_name;
            entries[i].offset = o->offset;
            entries[i].size = o->size;
            snprintf(entries[i].sha256_hex, sizeof(entries[i].sha256_hex), "%s", o->sha256_hex);
            entries[i].path = o->path;
            entries[i].path_len = o->path_len;
            entries[i].mtime_sec = o->mtime_sec;
            entries[i].mtime_nsec = o->mtime_nsec;
        }
        int append_rc = tp_index_append(repo, entries, subset.count);
        free(entries);
        if (append_rc != 0) {
            tp_warn("pack: failed to append to object index for repo '%s'", repo);
            return -1;
        }
    }

    return result;
}

int tp_pack_multi(const char *repo, const char *snapshot_label, int64_t target_size_bytes,
                   enum tp_pack_algo algo) {
    if (target_size_bytes < 1) {
        tp_warnx("pack: target size must be >= 1 byte");
        return -1;
    }

    /* resolve label */
    char *chosen_label = NULL;
    if (snapshot_label == NULL) {
        chosen_label = pick_latest_snapshot(repo);
        if (chosen_label == NULL) {
            tp_warnx("pack: no snapshots found in '%s/snapshots'", repo);
            return -1;
        }
    }
    const char *label = snapshot_label != NULL ? snapshot_label : chosen_label;

    char snapshot_path[PATH_MAX];
    if (snprintf(snapshot_path, sizeof(snapshot_path), "%s/snapshots/%s.jsonl", repo, label) >=
        (int)sizeof(snapshot_path)) {
        tp_warnx("pack: snapshot path too long");
        free(chosen_label);
        return -1;
    }

    struct tp_pack_object_set set;
    memset(&set, 0, sizeof(set));
    char *root = NULL;
    if (tp_pack_collect(snapshot_path, &set, &root) != 0) {
        tp_warnx("pack: failed to read snapshot '%s'", snapshot_path);
        free(chosen_label);
        return -1;
    }
    if (root == NULL) {
        tp_warnx("pack: snapshot '%s' has no root", snapshot_path);
        free(chosen_label);
        tp_pack_object_set_free(&set);
        return -1;
    }

    /* packs directory (needed by recovery below, and by the write loop) */
    char packs_dir[PATH_MAX];
    if (snprintf(packs_dir, sizeof(packs_dir), "%s/packs", repo) >= (int)sizeof(packs_dir)) {
        tp_warnx("pack: packs path too long");
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&set);
        return -1;
    }
    if (mkdir(packs_dir, 0777) != 0 && errno != EEXIST) {
        tp_warn("pack: cannot create '%s'", packs_dir);
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&set);
        return -1;
    }

    /* Crash recovery, BEFORE the index is loaded: repair a torn
     * objects.jsonl tail (a kill during a previous append), then remove
     * uncommitted packs / stale .part scratch files and stale checksum
     * lines. This makes every later step see a fully consistent repo. */
    if (repair_index_tail(repo) != 0) {
        tp_warn("pack: cannot repair object index tail for repo '%s'", repo);
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&set);
        return -1;
    }
    if (recover_crashed_state(repo, packs_dir) != 0) {
        tp_warn("pack: crash recovery failed for repo '%s'", repo);
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&set);
        return -1;
    }

    struct tp_index idx;
    if (tp_index_load(repo, &idx) != 0) {
        tp_warn("pack: cannot load object index for repo '%s'", repo);
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&set);
        return -1;
    }

    struct tp_pack_object_set new_set;
    memset(&new_set, 0, sizeof(new_set));
    for (size_t i = 0; i < set.count; i++) {
        if (tp_index_object_id_present(&idx, set.objects[i].object_id)) {
            continue;
        }
        if (object_set_grow(&new_set) != 0) {
            tp_warnx("pack: out of memory selecting new objects");
            free(root);
            free(chosen_label);
            tp_pack_object_set_free(&set);
            tp_pack_object_set_free(&new_set);
            tp_index_free(&idx);
            return -1;
        }
        new_set.objects[new_set.count++] = set.objects[i];
        memset(&set.objects[i], 0, sizeof(set.objects[i]));
    }
    tp_pack_object_set_free(&set);

    if (new_set.count == 0) {
        printf("nothing to pack\n");
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&new_set);
        tp_index_free(&idx);
        return 0;
    }

    /* Compute the estimated cost of each object and assign a bin (pack
     * number offset from the first pack written this run) via the chosen
     * pure algorithm. Both algorithms respect "oversized objects go alone
     * in their own pack, never split". */
    struct tp_bin_item *items = malloc(new_set.count * sizeof(*items));
    size_t *bin_of = malloc(new_set.count * sizeof(*bin_of));
    if (items == NULL || bin_of == NULL) {
        tp_warnx("pack: out of memory computing bin assignment");
        free(items);
        free(bin_of);
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&new_set);
        tp_index_free(&idx);
        return -1;
    }
    for (size_t i = 0; i < new_set.count; i++) {
        items[i].id = i;
        items[i].cost = tp_estimate_entry_cost(new_set.objects[i].size);
    }

    long bin_count = (algo == TP_PACK_ALGO_FFD)
                          ? tp_bin_assign_ffd(items, new_set.count, target_size_bytes, bin_of)
                          : tp_bin_assign_next_fit(items, new_set.count, target_size_bytes, bin_of);
    free(items);
    if (bin_count < 0) {
        tp_warnx("pack: bin assignment failed");
        free(bin_of);
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&new_set);
        tp_index_free(&idx);
        return -1;
    }

    /* Stable-partition new_set.objects into contiguous per-bin runs while
     * preserving each bin's internal relative order (both algorithms are
     * specified to keep the SAME relative order within a bin as the input;
     * FFD's descending-cost visiting order only affects WHICH bin an item
     * lands in, and ties are broken by ascending original index, so a
     * stable partition by bin_of[] reconstructs exactly the intended
     * per-pack member order). Counting sort by bin index, O(n). */
    struct tp_packed_object *ordered = malloc(new_set.count * sizeof(*ordered));
    size_t *bin_sizes = (bin_count > 0) ? calloc((size_t)bin_count, sizeof(*bin_sizes)) : NULL;
    size_t *bin_offsets = (bin_count > 0) ? calloc((size_t)bin_count, sizeof(*bin_offsets)) : NULL;
    if (ordered == NULL || (bin_count > 0 && (bin_sizes == NULL || bin_offsets == NULL))) {
        tp_warnx("pack: out of memory ordering bins");
        free(ordered);
        free(bin_sizes);
        free(bin_offsets);
        free(bin_of);
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&new_set);
        tp_index_free(&idx);
        return -1;
    }
    for (size_t i = 0; i < new_set.count; i++) {
        bin_sizes[bin_of[i]]++;
    }
    size_t running = 0;
    for (long b = 0; b < bin_count; b++) {
        bin_offsets[b] = running;
        running += bin_sizes[b];
    }
    size_t *cursor = malloc((size_t)(bin_count > 0 ? bin_count : 1) * sizeof(*cursor));
    if (cursor == NULL) {
        tp_warnx("pack: out of memory ordering bins");
        free(ordered);
        free(bin_sizes);
        free(bin_offsets);
        free(bin_of);
        free(root);
        free(chosen_label);
        tp_pack_object_set_free(&new_set);
        tp_index_free(&idx);
        return -1;
    }
    for (long b = 0; b < bin_count; b++) {
        cursor[b] = bin_offsets[b];
    }
    for (size_t i = 0; i < new_set.count; i++) {
        size_t b = bin_of[i];
        ordered[cursor[b]] = new_set.objects[i];
        cursor[b]++;
    }
    free(cursor);
    free(bin_of);
    free(bin_sizes);

    /* new_set.objects now only needs freeing as a shallow array (ownership
     * of every element moved into `ordered`). */
    free(new_set.objects);
    new_set.objects = NULL;
    new_set.count = 0;
    new_set.capacity = 0;

    int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0) {
        tp_warn("pack: cannot open snapshot root '%s'", root);
        free_ordered_objects(ordered, running);
        free(bin_offsets);
        free(root);
        free(chosen_label);
        tp_index_free(&idx);
        return -1;
    }

    /* Write each bin as its own fully-committed pack, in bin order. Pack
     * numbers are assigned sequentially starting at the next free number;
     * because each pack is durably committed (renamed + manifested +
     * checksummed + indexed) before the next one starts, a crash mid-run
     * leaves earlier packs fully consistent and later objects simply
     * unpacked (a subsequent run picks them up as new). */
    unsigned long next_num = next_pack_number(packs_dir);
    int result = 0;
    int fatal = 0;

    for (long b = 0; b < bin_count; b++) {
        size_t start = bin_offsets[b];
        size_t end = (b + 1 < bin_count) ? bin_offsets[b + 1] : running;

        struct subset_ref subset;
        subset.objects = ordered + start;
        subset.count = end - start;

        char pack_name[32];
        int prc = write_one_pack_and_commit(repo, packs_dir, root_fd,
                                            next_num + (unsigned long)b, subset, pack_name,
                                            sizeof(pack_name));
        if (prc < 0) {
            fatal = 1;
            break;
        }
        if (prc == 1) {
            result = 1;
        }
    }

    close(root_fd);
    free(bin_offsets);
    free_ordered_objects(ordered, running);
    free(root);
    free(chosen_label);
    tp_index_free(&idx);
    return fatal ? -1 : result;
}
