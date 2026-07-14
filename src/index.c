#include "index.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cJSON.h"
#include "manifest.h"
#include "uthash.h"
#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --------------------------------------------------------------------- */
/* Internal node types                                                    */
/* --------------------------------------------------------------------- */

/* by-path key: composite of path bytes + size + mtime. Since path is
 * variable-length, we cannot use a fixed-size struct key with uthash's
 * simple HASH_ADD/HASH_FIND (those hash raw bytes of a fixed struct). We
 * instead build a single flat malloc'd key buffer per node:
 *   [size:8][mtime_sec:8][mtime_nsec:8][path bytes...]
 * and hash/compare over that whole buffer via HASH_ADD_KEYPTR / HASH_FIND
 * with an explicit key length. */
struct path_key_prefix {
    off_t size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
};

struct by_path_node {
    char *key;     /* malloc'd: prefix bytes followed by raw path bytes */
    size_t key_len;
    struct tp_index_entry entry;
    UT_hash_handle hh;
};

struct by_sha_node {
    char sha256_hex[65]; /* key */
    struct tp_index_entry *entry; /* points into the owning by_path_node (or a dedicated copy) */
    UT_hash_handle hh;
};

struct by_id_node {
    char *object_id; /* key, borrowed from the owning by_path_node's entry */
    struct tp_index_entry *entry; /* points into the owning by_path_node */
    UT_hash_handle hh;
};

/* We store the authoritative entry data once, in a singly linked list of
 * "owner" nodes, and have by_path_node/by_sha_node reference the same
 * tp_index_entry storage where applicable. Simplest correct approach:
 * by_path_node owns the tp_index_entry; by_sha_node just points at it. Since
 * every appended line has both a path and (optionally) a sha256, every
 * entry gets a by_path_node; entries with non-empty sha256 additionally get
 * a by_sha_node pointing at the same tp_index_entry. */

static char *make_path_key(off_t size, int64_t mtime_sec, long mtime_nsec, const char *path,
                            size_t path_len, size_t *out_len) {
    struct path_key_prefix prefix;
    memset(&prefix, 0, sizeof(prefix));
    prefix.size = size;
    prefix.mtime_sec = mtime_sec;
    prefix.mtime_nsec = (int64_t)mtime_nsec;

    size_t total = sizeof(prefix) + path_len;
    char *buf = malloc(total > 0 ? total : 1);
    if (buf == NULL) {
        return NULL;
    }
    memcpy(buf, &prefix, sizeof(prefix));
    if (path_len > 0) {
        memcpy(buf + sizeof(prefix), path, path_len);
    }
    *out_len = total;
    return buf;
}

static void entry_free_contents(struct tp_index_entry *e) {
    free(e->object_id);
    free(e->pack);
    free(e->path);
}

void tp_index_free(struct tp_index *idx) {
    struct by_sha_node *sha_head = (struct by_sha_node *)idx->by_sha_impl;
    struct by_sha_node *scur;
    struct by_sha_node *stmp;
    HASH_ITER(hh, sha_head, scur, stmp) {
        HASH_DEL(sha_head, scur);
        free(scur); /* entry pointer not owned here */
    }

    struct by_id_node *id_head = (struct by_id_node *)idx->by_id_impl;
    struct by_id_node *icur;
    struct by_id_node *itmp;
    HASH_ITER(hh, id_head, icur, itmp) {
        HASH_DEL(id_head, icur);
        free(icur); /* object_id pointer not owned here */
    }

    struct by_path_node *path_head = (struct by_path_node *)idx->by_path_impl;
    struct by_path_node *pcur;
    struct by_path_node *ptmp;
    HASH_ITER(hh, path_head, pcur, ptmp) {
        HASH_DEL(path_head, pcur);
        entry_free_contents(&pcur->entry);
        free(pcur->key);
        free(pcur);
    }

    idx->by_path_impl = NULL;
    idx->by_sha_impl = NULL;
    idx->by_id_impl = NULL;
    idx->next_id = 1;
}

const struct tp_index_entry *tp_index_find_by_path(const struct tp_index *idx, const char *path,
                                                     size_t path_len, off_t size,
                                                     int64_t mtime_sec, long mtime_nsec) {
    struct by_path_node *head = (struct by_path_node *)idx->by_path_impl;
    size_t key_len;
    char *key = make_path_key(size, mtime_sec, mtime_nsec, path, path_len, &key_len);
    if (key == NULL) {
        return NULL;
    }

    struct by_path_node *found = NULL;
    HASH_FIND(hh, head, key, key_len, found);
    free(key);

    return found != NULL ? &found->entry : NULL;
}

const struct tp_index_entry *tp_index_find_by_sha256(const struct tp_index *idx,
                                                       const char *sha256_hex) {
    if (sha256_hex == NULL || sha256_hex[0] == '\0') {
        return NULL;
    }
    struct by_sha_node *head = (struct by_sha_node *)idx->by_sha_impl;
    struct by_sha_node *found = NULL;
    char key[65];
    snprintf(key, sizeof(key), "%s", sha256_hex);
    HASH_FIND(hh, head, key, strlen(key), found);
    return found != NULL ? found->entry : NULL;
}

int tp_index_object_id_present(const struct tp_index *idx, const char *object_id) {
    if (object_id == NULL) {
        return 0;
    }
    struct by_id_node *head = (struct by_id_node *)idx->by_id_impl;
    struct by_id_node *found = NULL;
    HASH_FIND_STR(head, object_id, found);
    return found != NULL;
}

const struct tp_index_entry *tp_index_find_by_object_id(const struct tp_index *idx,
                                                          const char *object_id) {
    if (object_id == NULL) {
        return NULL;
    }
    struct by_id_node *head = (struct by_id_node *)idx->by_id_impl;
    struct by_id_node *found = NULL;
    HASH_FIND_STR(head, object_id, found);
    return found != NULL ? found->entry : NULL;
}

/* numeric_suffix: parses the trailing digits of an "O<n>" style id. Returns
 * 0 if the string does not match that shape (no digits, or non-'O' prefix). */
static uint64_t numeric_suffix(const char *id) {
    if (id == NULL || id[0] != 'O') {
        return 0;
    }
    const char *p = id + 1;
    if (*p == '\0') {
        return 0;
    }
    for (const char *q = p; *q != '\0'; q++) {
        if (*q < '0' || *q > '9') {
            return 0;
        }
    }
    return (uint64_t)strtoull(p, NULL, 10);
}

struct load_ctx {
    struct tp_index *idx;
    int error;
};

static int load_line_cb(const char *line, struct load_ctx *ctx) {
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        return -1;
    }

    cJSON *j_id = cJSON_GetObjectItemCaseSensitive(root, "object_id");
    cJSON *j_pack = cJSON_GetObjectItemCaseSensitive(root, "pack");
    cJSON *j_offset = cJSON_GetObjectItemCaseSensitive(root, "offset");
    cJSON *j_size = cJSON_GetObjectItemCaseSensitive(root, "size");
    cJSON *j_sha = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    cJSON *j_mtime_sec = cJSON_GetObjectItemCaseSensitive(root, "mtime_sec");
    cJSON *j_mtime_nsec = cJSON_GetObjectItemCaseSensitive(root, "mtime_nsec");
    cJSON *j_path = cJSON_GetObjectItemCaseSensitive(root, "path");
    cJSON *j_path_b64 = cJSON_GetObjectItemCaseSensitive(root, "path_b64");

    if (!cJSON_IsString(j_id) || !cJSON_IsString(j_pack) || !cJSON_IsNumber(j_offset) ||
        !cJSON_IsNumber(j_size)) {
        cJSON_Delete(root);
        return -1;
    }

    char *path = NULL;
    size_t path_len = 0;
    if (cJSON_IsString(j_path) && j_path->valuestring != NULL) {
        path_len = strlen(j_path->valuestring);
        path = malloc(path_len + 1);
        if (path == NULL) {
            cJSON_Delete(root);
            return -1;
        }
        memcpy(path, j_path->valuestring, path_len + 1);
    } else if (cJSON_IsString(j_path_b64) && j_path_b64->valuestring != NULL) {
        /* decode base64 locally (mirrors manifest.c's decoder) */
        const char *in = j_path_b64->valuestring;
        size_t in_len = strlen(in);
        if (in_len % 4 != 0) {
            cJSON_Delete(root);
            return -1;
        }
        size_t out_cap = (in_len / 4) * 3;
        path = malloc(out_cap + 1);
        if (path == NULL) {
            cJSON_Delete(root);
            return -1;
        }
        size_t oi = 0;
        int bad = 0;
        for (size_t i = 0; i < in_len && !bad; i += 4) {
            int v[4];
            for (int k = 0; k < 4; k++) {
                char c = in[i + (size_t)k];
                if (c == '=') {
                    v[k] = 0;
                } else if (c >= 'A' && c <= 'Z') {
                    v[k] = c - 'A';
                } else if (c >= 'a' && c <= 'z') {
                    v[k] = c - 'a' + 26;
                } else if (c >= '0' && c <= '9') {
                    v[k] = c - '0' + 52;
                } else if (c == '+') {
                    v[k] = 62;
                } else if (c == '/') {
                    v[k] = 63;
                } else {
                    bad = 1;
                    v[k] = 0;
                }
            }
            uint32_t triple = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                               ((uint32_t)v[2] << 6) | (uint32_t)v[3];
            path[oi++] = (char)((triple >> 16) & 0xFF);
            if (in[i + 2] != '=') {
                path[oi++] = (char)((triple >> 8) & 0xFF);
            }
            if (in[i + 3] != '=') {
                path[oi++] = (char)(triple & 0xFF);
            }
        }
        if (bad) {
            free(path);
            cJSON_Delete(root);
            return -1;
        }
        path[oi] = '\0';
        path_len = oi;
    } else {
        cJSON_Delete(root);
        return -1;
    }

    struct by_path_node *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        free(path);
        cJSON_Delete(root);
        return -1;
    }

    node->entry.object_id = strdup(j_id->valuestring);
    node->entry.pack = strdup(j_pack->valuestring);
    node->entry.offset = (int64_t)j_offset->valuedouble;
    node->entry.size = (off_t)j_size->valuedouble;
    node->entry.sha256_hex[0] = '\0';
    if (cJSON_IsString(j_sha) && j_sha->valuestring != NULL) {
        snprintf(node->entry.sha256_hex, sizeof(node->entry.sha256_hex), "%s", j_sha->valuestring);
    }
    node->entry.path = path;
    node->entry.path_len = path_len;
    node->entry.mtime_sec = cJSON_IsNumber(j_mtime_sec) ? (int64_t)j_mtime_sec->valuedouble : 0;
    node->entry.mtime_nsec = cJSON_IsNumber(j_mtime_nsec) ? (long)j_mtime_nsec->valuedouble : 0;

    if (node->entry.object_id == NULL || node->entry.pack == NULL) {
        entry_free_contents(&node->entry);
        free(node);
        cJSON_Delete(root);
        return -1;
    }

    node->key = make_path_key(node->entry.size, node->entry.mtime_sec, node->entry.mtime_nsec,
                               node->entry.path, node->entry.path_len, &node->key_len);
    if (node->key == NULL) {
        entry_free_contents(&node->entry);
        free(node);
        cJSON_Delete(root);
        return -1;
    }

    struct by_path_node *path_head = (struct by_path_node *)ctx->idx->by_path_impl;
    HASH_ADD_KEYPTR(hh, path_head, node->key, node->key_len, node);
    ctx->idx->by_path_impl = path_head;

    {
        struct by_id_node *idnode = calloc(1, sizeof(*idnode));
        if (idnode == NULL) {
            cJSON_Delete(root);
            return -1;
        }
        idnode->object_id = node->entry.object_id; /* borrowed */
        idnode->entry = &node->entry;               /* borrowed */

        struct by_id_node *id_head = (struct by_id_node *)ctx->idx->by_id_impl;
        HASH_ADD_KEYPTR(hh, id_head, idnode->object_id, strlen(idnode->object_id), idnode);
        ctx->idx->by_id_impl = id_head;
    }

    if (node->entry.sha256_hex[0] != '\0') {
        struct by_sha_node *snode = calloc(1, sizeof(*snode));
        if (snode == NULL) {
            cJSON_Delete(root);
            return -1;
        }
        snprintf(snode->sha256_hex, sizeof(snode->sha256_hex), "%s", node->entry.sha256_hex);
        snode->entry = &node->entry;

        struct by_sha_node *sha_head = (struct by_sha_node *)ctx->idx->by_sha_impl;
        HASH_ADD(hh, sha_head, sha256_hex, strlen(snode->sha256_hex), snode);
        ctx->idx->by_sha_impl = sha_head;
    }

    uint64_t suffix = numeric_suffix(node->entry.object_id);
    if (suffix + 1 > ctx->idx->next_id) {
        ctx->idx->next_id = suffix + 1;
    }

    cJSON_Delete(root);
    return 0;
}

int tp_index_load(const char *repo, struct tp_index *idx) {
    memset(idx, 0, sizeof(*idx));
    idx->next_id = 1;

    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/objects/objects.jsonl", repo);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        if (errno == ENOENT) {
            return 0; /* missing index is not an error */
        }
        return -1;
    }

    struct load_ctx ctx = {idx, 0};

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
        if (load_line_cb(line, &ctx) != 0) {
            rc = -1;
            break;
        }
    }

    free(line);
    fclose(f);

    if (rc != 0) {
        tp_index_free(idx);
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------- */
/* Append                                                                 */
/* --------------------------------------------------------------------- */

/* mkdir_p: creates each path component in turn (like `mkdir -p`). Returns
 * 0 on success (including "already exists"), -1 on failure (errno set).
 * Mirrors the static helper in manifest.c; duplicated here since that one
 * is not exported. */
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

static int add_index_path_field(cJSON *obj, const char *raw, size_t raw_len) {
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

int tp_index_append(const char *repo, const struct tp_index_entry *entries, size_t count) {
    char objects_dir[PATH_MAX];
    int n = snprintf(objects_dir, sizeof(objects_dir), "%s/objects", repo);
    if (n < 0 || (size_t)n >= sizeof(objects_dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir_p(objects_dir) != 0) {
        return -1;
    }

    char path[PATH_MAX];
    n = snprintf(path, sizeof(path), "%s/objects.jsonl", objects_dir);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *f = fopen(path, "a");
    if (f == NULL) {
        return -1;
    }

    int rc = 0;
    for (size_t i = 0; i < count; i++) {
        const struct tp_index_entry *e = &entries[i];

        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            rc = -1;
            break;
        }
        int ok = 1;
        ok = ok && cJSON_AddStringToObject(obj, "object_id", e->object_id) != NULL;
        ok = ok && cJSON_AddStringToObject(obj, "pack", e->pack) != NULL;
        ok = ok && cJSON_AddNumberToObject(obj, "offset", (double)e->offset) != NULL;
        ok = ok && cJSON_AddNumberToObject(obj, "size", (double)e->size) != NULL;
        ok = ok && cJSON_AddStringToObject(obj, "sha256", e->sha256_hex) != NULL;
        ok = ok && add_index_path_field(obj, e->path, e->path_len) == 0;
        ok = ok && cJSON_AddNumberToObject(obj, "mtime_sec", (double)e->mtime_sec) != NULL;
        ok = ok && cJSON_AddNumberToObject(obj, "mtime_nsec", (double)e->mtime_nsec) != NULL;

        if (!ok) {
            cJSON_Delete(obj);
            rc = -1;
            break;
        }

        char *text = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        if (text == NULL) {
            rc = -1;
            break;
        }
        if (fputs(text, f) == EOF || fputc('\n', f) == EOF) {
            free(text);
            rc = -1;
            break;
        }
        free(text);
    }

    if (rc == 0) {
        if (fflush(f) != 0) {
            rc = -1;
        }
    }
    int fd = fileno(f);
    if (rc == 0 && fd >= 0) {
        if (fsync(fd) != 0) {
            rc = -1;
        }
    }
    fclose(f);
    return rc;
}
