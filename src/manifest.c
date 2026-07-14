#include "manifest.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "cJSON.h"
#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --------------------------------------------------------------------- */
/* Writer                                                                 */
/* --------------------------------------------------------------------- */

/* mkdir_p: creates each path component in turn (like `mkdir -p`). Returns
 * 0 on success (including "already exists"), -1 on failure (errno set). */
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

/* set_json_path: adds either a "path" (UTF-8 string) or "path_b64" (base64
 * of the raw bytes) field named by key_base to obj, per the path encoding
 * rule in the spec. */
static int set_json_path_field(cJSON *obj, const char *key_base, const char *raw, size_t raw_len) {
    if (tp_utf8_valid((const uint8_t *)raw, raw_len)) {
        /* cJSON_CreateString() takes a NUL-terminated C string with no
         * explicit length, but callers (e.g. the scanner's readlinkat()
         * target buffer) may pass a raw buffer that is exactly raw_len
         * valid bytes WITHOUT a guaranteed NUL terminator at that offset.
         * Build an explicitly NUL-terminated copy first; using raw
         * directly here previously read past the intended length into
         * whatever stack garbage followed (masked by Debug builds'
         * zeroed/poisoned memory, but visible under Release). */
        char *nul_terminated = malloc(raw_len + 1);
        if (nul_terminated == NULL) {
            return -1;
        }
        if (raw_len > 0) {
            memcpy(nul_terminated, raw, raw_len);
        }
        nul_terminated[raw_len] = '\0';

        cJSON *item = cJSON_CreateString(nul_terminated);
        free(nul_terminated);
        if (item == NULL) {
            return -1;
        }
        if (!cJSON_AddItemToObject(obj, key_base, item)) {
            cJSON_Delete(item);
            return -1;
        }
        return 0;
    }

    char *b64 = tp_base64_encode((const uint8_t *)raw, raw_len);
    if (b64 == NULL) {
        return -1;
    }

    char key[64];
    snprintf(key, sizeof(key), "%s_b64", key_base);

    cJSON *item = cJSON_CreateString(b64);
    free(b64);
    if (item == NULL) {
        return -1;
    }
    if (!cJSON_AddItemToObject(obj, key, item)) {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

/* write_json_line: serializes obj with cJSON_PrintUnformatted, writes it
 * followed by '\n', deletes obj, and frees the printed buffer. Returns 0
 * on success, -1 on failure. Always takes ownership of obj (deletes it). */
static int write_json_line(struct tp_snapshot_writer *w, cJSON *obj) {
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        w->error = -1;
        return -1;
    }

    int rc = 0;
    if (fputs(text, w->f) == EOF || fputc('\n', w->f) == EOF) {
        w->error = errno != 0 ? errno : -1;
        rc = -1;
    }
    free(text);
    return rc;
}

int tp_snapshot_writer_open(struct tp_snapshot_writer *w, const char *repo, const char *label,
                             const char *root) {
    memset(w, 0, sizeof(*w));

    char snapshots_dir[PATH_MAX];
    int n = snprintf(snapshots_dir, sizeof(snapshots_dir), "%s/snapshots", repo);
    if (n < 0 || (size_t)n >= sizeof(snapshots_dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir_p(snapshots_dir) != 0) {
        return -1;
    }

    char *path = malloc(strlen(snapshots_dir) + 1 + strlen(label) + strlen(".jsonl") + 1);
    if (path == NULL) {
        return -1;
    }
    sprintf(path, "%s/%s.jsonl", snapshots_dir, label);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        int saved_errno = errno;
        free(path);
        errno = saved_errno;
        return -1;
    }

    w->f = f;
    w->path = path;
    w->error = 0;

    cJSON *header = cJSON_CreateObject();
    if (header == NULL) {
        w->error = -1;
        return -1;
    }
    cJSON_AddItemToObject(header, "format", cJSON_CreateString("tarpack-snapshot-v1"));
    cJSON_AddItemToObject(header, "created", cJSON_CreateNumber((double)time(NULL)));
    if (set_json_path_field(header, "root", root, strlen(root)) != 0) {
        /* root should always be valid UTF-8 in practice; fall back to plain string on
         * any unexpected failure path rather than losing the header entirely. */
        cJSON_AddItemToObject(header, "root", cJSON_CreateString(root));
    }

    if (write_json_line(w, header) != 0) {
        return -1;
    }

    return 0;
}

static void add_time_pair(cJSON *obj, const char *sec_key, const char *nsec_key, int64_t sec,
                          long nsec) {
    if (sec == TP_TIME_ABSENT) {
        return;
    }
    cJSON_AddItemToObject(obj, sec_key, cJSON_CreateNumber((double)sec));
    cJSON_AddItemToObject(obj, nsec_key, cJSON_CreateNumber((double)nsec));
}

static cJSON *make_common_entry(const char *type, const char *path, uid_t uid, gid_t gid,
                                 int64_t mtime_sec, long mtime_nsec, dev_t dev, ino_t ino,
                                 const struct tp_extra_times *extra) {
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddItemToObject(obj, "type", cJSON_CreateString(type));
    if (set_json_path_field(obj, "path", path, strlen(path)) != 0) {
        cJSON_Delete(obj);
        return NULL;
    }
    cJSON_AddItemToObject(obj, "uid", cJSON_CreateNumber((double)uid));
    cJSON_AddItemToObject(obj, "gid", cJSON_CreateNumber((double)gid));
    cJSON_AddItemToObject(obj, "mtime_sec", cJSON_CreateNumber((double)mtime_sec));
    cJSON_AddItemToObject(obj, "mtime_nsec", cJSON_CreateNumber((double)mtime_nsec));
    if (extra != NULL) {
        add_time_pair(obj, "atime_sec", "atime_nsec", extra->atime_sec, extra->atime_nsec);
        add_time_pair(obj, "btime_sec", "btime_nsec", extra->btime_sec, extra->btime_nsec);
        add_time_pair(obj, "ctime_sec", "ctime_nsec", extra->ctime_sec, extra->ctime_nsec);
    }
    cJSON_AddItemToObject(obj, "dev", cJSON_CreateNumber((double)dev));
    cJSON_AddItemToObject(obj, "ino", cJSON_CreateNumber((double)ino));
    return obj;
}

static void add_mode_field(cJSON *obj, mode_t mode_perm) {
    char mode_str[8];
    snprintf(mode_str, sizeof(mode_str), "%04o", (unsigned int)(mode_perm & 07777));
    cJSON_AddItemToObject(obj, "mode", cJSON_CreateString(mode_str));
}

int tp_snapshot_write_file(struct tp_snapshot_writer *w, const char *path, const char *object_id,
                            nlink_t nlink, uid_t uid, gid_t gid, mode_t mode_perm, off_t size,
                            int64_t mtime_sec, long mtime_nsec, dev_t dev, ino_t ino,
                            const char *sha256_hex, const struct tp_extra_times *extra) {
    if (w->error != 0) {
        return -1;
    }

    cJSON *obj = make_common_entry("file", path, uid, gid, mtime_sec, mtime_nsec, dev, ino, extra);
    if (obj == NULL) {
        w->error = -1;
        return -1;
    }
    cJSON_AddItemToObject(obj, "object_id", cJSON_CreateString(object_id));
    cJSON_AddItemToObject(obj, "nlink", cJSON_CreateNumber((double)nlink));
    add_mode_field(obj, mode_perm);
    cJSON_AddItemToObject(obj, "size", cJSON_CreateNumber((double)size));
    if (sha256_hex != NULL && sha256_hex[0] != '\0') {
        cJSON_AddItemToObject(obj, "sha256", cJSON_CreateString(sha256_hex));
    }

    return write_json_line(w, obj);
}

int tp_snapshot_write_dir(struct tp_snapshot_writer *w, const char *path, uid_t uid, gid_t gid,
                           mode_t mode_perm, int64_t mtime_sec, long mtime_nsec, dev_t dev,
                           ino_t ino, const struct tp_extra_times *extra) {
    if (w->error != 0) {
        return -1;
    }

    cJSON *obj = make_common_entry("dir", path, uid, gid, mtime_sec, mtime_nsec, dev, ino, extra);
    if (obj == NULL) {
        w->error = -1;
        return -1;
    }
    add_mode_field(obj, mode_perm);

    return write_json_line(w, obj);
}

int tp_snapshot_write_symlink(struct tp_snapshot_writer *w, const char *path, const char *target,
                               size_t target_len, uid_t uid, gid_t gid, int64_t mtime_sec,
                               long mtime_nsec, dev_t dev, ino_t ino,
                               const struct tp_extra_times *extra) {
    if (w->error != 0) {
        return -1;
    }

    cJSON *obj =
        make_common_entry("symlink", path, uid, gid, mtime_sec, mtime_nsec, dev, ino, extra);
    if (obj == NULL) {
        w->error = -1;
        return -1;
    }
    if (set_json_path_field(obj, "target", target, target_len) != 0) {
        cJSON_Delete(obj);
        w->error = -1;
        return -1;
    }

    return write_json_line(w, obj);
}

int tp_snapshot_writer_close(struct tp_snapshot_writer *w) {
    int rc = w->error != 0 ? -1 : 0;

    if (w->f != NULL) {
        if (fflush(w->f) != 0) {
            rc = -1;
        }
        if (fclose(w->f) != 0) {
            rc = -1;
        }
        w->f = NULL;
    }

    free(w->path);
    w->path = NULL;

    return rc;
}

/* --------------------------------------------------------------------- */
/* Reader                                                                 */
/* --------------------------------------------------------------------- */

void tp_manifest_entry_free(struct tp_manifest_entry *e) {
    free(e->format);
    free(e->root);
    free(e->path);
    free(e->object_id);
    free(e->target);
    memset(e, 0, sizeof(*e));
}

/* get_path_field: reads either "<key_base>" (UTF-8 text) or
 * "<key_base>_b64" (base64 of raw bytes) from obj, decoding as needed.
 * Sets *out to a malloc'd NUL-terminated buffer holding the raw bytes and
 * *out_len to the raw byte length (not counting the extra NUL). Returns 0
 * on success, -1 if neither key is present or decoding fails. */
static int base64_decode(const char *in, uint8_t **out, size_t *out_len);

static int get_path_field(const cJSON *obj, const char *key_base, char **out, size_t *out_len) {
    cJSON *plain = cJSON_GetObjectItemCaseSensitive(obj, key_base);
    if (cJSON_IsString(plain) && plain->valuestring != NULL) {
        size_t len = strlen(plain->valuestring);
        char *buf = malloc(len + 1);
        if (buf == NULL) {
            return -1;
        }
        memcpy(buf, plain->valuestring, len + 1);
        *out = buf;
        *out_len = len;
        return 0;
    }

    char b64_key[64];
    snprintf(b64_key, sizeof(b64_key), "%s_b64", key_base);
    cJSON *b64_item = cJSON_GetObjectItemCaseSensitive(obj, b64_key);
    if (cJSON_IsString(b64_item) && b64_item->valuestring != NULL) {
        uint8_t *raw = NULL;
        size_t raw_len = 0;
        if (base64_decode(b64_item->valuestring, &raw, &raw_len) != 0) {
            return -1;
        }
        char *buf = malloc(raw_len + 1);
        if (buf == NULL) {
            free(raw);
            return -1;
        }
        memcpy(buf, raw, raw_len);
        buf[raw_len] = '\0';
        free(raw);
        *out = buf;
        *out_len = raw_len;
        return 0;
    }

    return -1;
}

static int base64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode(const char *in, uint8_t **out, size_t *out_len) {
    size_t len = strlen(in);
    if (len == 0) {
        uint8_t *buf = malloc(1);
        if (buf == NULL) {
            return -1;
        }
        *out = buf;
        *out_len = 0;
        return 0;
    }
    if (len % 4 != 0) {
        return -1;
    }

    size_t pad = 0;
    if (len >= 1 && in[len - 1] == '=') pad++;
    if (len >= 2 && in[len - 2] == '=') pad++;

    size_t out_cap = (len / 4) * 3;
    uint8_t *buf = malloc(out_cap > 0 ? out_cap : 1);
    if (buf == NULL) {
        return -1;
    }

    size_t oi = 0;
    for (size_t i = 0; i < len; i += 4) {
        int c0 = base64_decode_char(in[i]);
        int c1 = base64_decode_char(in[i + 1]);
        int c2 = (in[i + 2] == '=') ? 0 : base64_decode_char(in[i + 2]);
        int c3 = (in[i + 3] == '=') ? 0 : base64_decode_char(in[i + 3]);

        if (c0 < 0 || c1 < 0 || (in[i + 2] != '=' && c2 < 0) || (in[i + 3] != '=' && c3 < 0)) {
            free(buf);
            return -1;
        }

        uint32_t v = ((uint32_t)c0 << 18) | ((uint32_t)c1 << 12) | ((uint32_t)c2 << 6) | (uint32_t)c3;

        buf[oi++] = (uint8_t)((v >> 16) & 0xFF);
        if (in[i + 2] != '=') {
            buf[oi++] = (uint8_t)((v >> 8) & 0xFF);
        }
        if (in[i + 3] != '=') {
            buf[oi++] = (uint8_t)(v & 0xFF);
        }
    }
    (void)pad;

    *out = buf;
    *out_len = oi;
    return 0;
}

static uid_t get_uid(const cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? (uid_t)item->valuedouble : 0;
}

static int64_t get_i64(const cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? (int64_t)item->valuedouble : 0;
}

/* like get_i64, but a caller-chosen default distinguishes "field missing"
 * (pre-v1.3 snapshots omit the secondary timestamps) from a real 0 value */
static int64_t get_i64_default(const cJSON *obj, const char *key, int64_t dflt) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? (int64_t)item->valuedouble : dflt;
}

static char *get_string_dup(const cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return NULL;
    }
    size_t len = strlen(item->valuestring);
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }
    memcpy(buf, item->valuestring, len + 1);
    return buf;
}

int tp_manifest_parse_line(const char *line, struct tp_manifest_entry *out) {
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        return -1;
    }

    cJSON *format_item = cJSON_GetObjectItemCaseSensitive(root, "format");
    if (cJSON_IsString(format_item) && format_item->valuestring != NULL) {
        out->type = TP_ENTRY_HEADER;
        out->format = get_string_dup(root, "format");
        out->created = get_i64(root, "created");
        out->root = get_string_dup(root, "root");
        cJSON_Delete(root);
        return 0;
    }

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_item) || type_item->valuestring == NULL) {
        cJSON_Delete(root);
        return -1;
    }

    if (get_path_field(root, "path", &out->path, &out->path_len) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    out->uid = get_uid(root, "uid");
    out->gid = (gid_t)get_i64(root, "gid");
    out->mtime_sec = get_i64(root, "mtime_sec");
    out->mtime_nsec = (long)get_i64(root, "mtime_nsec");
    out->extra.atime_sec = get_i64_default(root, "atime_sec", TP_TIME_ABSENT);
    out->extra.atime_nsec = (long)get_i64(root, "atime_nsec");
    out->extra.btime_sec = get_i64_default(root, "btime_sec", TP_TIME_ABSENT);
    out->extra.btime_nsec = (long)get_i64(root, "btime_nsec");
    out->extra.ctime_sec = get_i64_default(root, "ctime_sec", TP_TIME_ABSENT);
    out->extra.ctime_nsec = (long)get_i64(root, "ctime_nsec");
    out->dev = (dev_t)get_i64(root, "dev");
    out->ino = (ino_t)get_i64(root, "ino");

    if (strcmp(type_item->valuestring, "file") == 0) {
        out->type = TP_ENTRY_FILE;
        out->object_id = get_string_dup(root, "object_id");
        out->nlink = (nlink_t)get_i64(root, "nlink");
        out->size = (off_t)get_i64(root, "size");

        char *mode_str = get_string_dup(root, "mode");
        if (mode_str != NULL) {
            out->mode_perm = (mode_t)strtol(mode_str, NULL, 8);
            free(mode_str);
        }

        out->sha256_hex[0] = '\0';
        cJSON *sha_item = cJSON_GetObjectItemCaseSensitive(root, "sha256");
        if (cJSON_IsString(sha_item) && sha_item->valuestring != NULL) {
            snprintf(out->sha256_hex, sizeof(out->sha256_hex), "%s", sha_item->valuestring);
        }
    } else if (strcmp(type_item->valuestring, "dir") == 0) {
        out->type = TP_ENTRY_DIR;

        char *mode_str = get_string_dup(root, "mode");
        if (mode_str != NULL) {
            out->mode_perm = (mode_t)strtol(mode_str, NULL, 8);
            free(mode_str);
        }
    } else if (strcmp(type_item->valuestring, "symlink") == 0) {
        out->type = TP_ENTRY_SYMLINK;
        if (get_path_field(root, "target", &out->target, &out->target_len) != 0) {
            cJSON_Delete(root);
            tp_manifest_entry_free(out);
            return -1;
        }
    } else {
        cJSON_Delete(root);
        tp_manifest_entry_free(out);
        return -1;
    }

    cJSON_Delete(root);
    return 0;
}

int tp_manifest_read_file(const char *path, tp_manifest_line_cb cb, void *user_data) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }

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

        struct tp_manifest_entry entry;
        if (tp_manifest_parse_line(line, &entry) != 0) {
            rc = -1;
            break;
        }

        int cb_rc = cb(&entry, user_data);
        tp_manifest_entry_free(&entry);

        if (cb_rc != 0) {
            rc = -1;
            break;
        }
    }

    free(line);
    fclose(f);
    return rc;
}
