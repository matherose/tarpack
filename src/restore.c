#include "restore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "index.h"
#include "manifest.h"
#include "sha256.h"
#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---------------------------------------------------------------------- */
/* Path safety                                                             */
/* ---------------------------------------------------------------------- */

int tp_restore_path_safe(const char *path, size_t len) {
    if (path == NULL || len == 0) {
        return 0; /* empty */
    }
    if (path[0] == '/') {
        return 0; /* absolute */
    }
    /* Reject any ".." component: a "/"-delimited segment equal exactly to
     * "..". Segments that merely start/end with ".." (e.g. "..a", "a..") or
     * are "..." are fine. */
    size_t seg_start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || path[i] == '/') {
            size_t seg_len = i - seg_start;
            if (seg_len == 2 && path[seg_start] == '.' && path[seg_start + 1] == '.') {
                return 0;
            }
            seg_start = i + 1;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------- */
/* Hardlink grouping                                                       */
/* ---------------------------------------------------------------------- */

static struct tp_restore_link_group *link_set_find(struct tp_restore_link_set *s,
                                                    const char *object_id) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->groups[i].object_id, object_id) == 0) {
            return &s->groups[i];
        }
    }
    return NULL;
}

static int link_set_grow(struct tp_restore_link_set *s) {
    if (s->count < s->capacity) {
        return 0;
    }
    size_t newcap = s->capacity == 0 ? 8 : s->capacity * 2;
    struct tp_restore_link_group *ng = realloc(s->groups, newcap * sizeof(*ng));
    if (ng == NULL) {
        return -1;
    }
    s->groups = ng;
    s->capacity = newcap;
    return 0;
}

static int group_grow(struct tp_restore_link_group *g) {
    if (g->count < g->capacity) {
        return 0;
    }
    size_t newcap = g->capacity == 0 ? 4 : g->capacity * 2;
    struct tp_restore_file_ref *nr = realloc(g->refs, newcap * sizeof(*nr));
    if (nr == NULL) {
        return -1;
    }
    g->refs = nr;
    g->capacity = newcap;
    return 0;
}

struct collect_links_ctx {
    struct tp_restore_link_set *set;
    int oom;
};

static int collect_links_cb(const struct tp_manifest_entry *entry, void *user_data) {
    struct collect_links_ctx *ctx = (struct collect_links_ctx *)user_data;

    if (entry->type != TP_ENTRY_FILE || entry->object_id == NULL) {
        return 0; /* header/dir/symlink ignored */
    }

    struct tp_restore_link_group *g = link_set_find(ctx->set, entry->object_id);
    if (g == NULL) {
        if (link_set_grow(ctx->set) != 0) {
            ctx->oom = 1;
            return -1;
        }
        g = &ctx->set->groups[ctx->set->count];
        memset(g, 0, sizeof(*g));
        g->object_id = strdup(entry->object_id);
        if (g->object_id == NULL) {
            ctx->oom = 1;
            return -1;
        }
        g->uid = entry->uid;
        g->gid = entry->gid;
        g->mode_perm = entry->mode_perm;
        g->size = entry->size;
        g->mtime_sec = entry->mtime_sec;
        g->mtime_nsec = entry->mtime_nsec;
        ctx->set->count++;
    }

    if (group_grow(g) != 0) {
        ctx->oom = 1;
        return -1;
    }
    struct tp_restore_file_ref *r = &g->refs[g->count];
    r->object_id = strdup(entry->object_id);
    r->path = malloc(entry->path_len + 1);
    if (r->object_id == NULL || r->path == NULL) {
        free(r->object_id);
        free(r->path);
        ctx->oom = 1;
        return -1;
    }
    memcpy(r->path, entry->path, entry->path_len);
    r->path[entry->path_len] = '\0';
    r->path_len = entry->path_len;
    g->count++;
    return 0;
}

int tp_restore_collect_links(const char *snapshot_path, struct tp_restore_link_set *out) {
    struct collect_links_ctx ctx;
    ctx.set = out;
    ctx.oom = 0;

    int rc = tp_manifest_read_file(snapshot_path, collect_links_cb, &ctx);
    if (rc != 0 || ctx.oom) {
        tp_restore_link_set_free(out);
        return -1;
    }
    return 0;
}

void tp_restore_link_set_free(struct tp_restore_link_set *s) {
    if (s == NULL || s->groups == NULL) {
        return;
    }
    for (size_t i = 0; i < s->count; i++) {
        struct tp_restore_link_group *g = &s->groups[i];
        for (size_t j = 0; j < g->count; j++) {
            free(g->refs[j].object_id);
            free(g->refs[j].path);
        }
        free(g->refs);
        free(g->object_id);
    }
    free(s->groups);
    s->groups = NULL;
    s->count = 0;
    s->capacity = 0;
}

/* ---------------------------------------------------------------------- */
/* Top-level restore                                                       */
/* ---------------------------------------------------------------------- */

/*
 * Read-side charset note: packs are written with hdrcharset=UTF-8, so pax
 * headers carry raw UTF-8 pathnames. On read, libarchive converts them to
 * the current locale charset. We deliberately DO NOT nudge LC_CTYPE to a
 * UTF-8 locale here (unlike the write side in packer.c): on macOS that
 * conversion normalizes to NFD, silently changing the pathname bytes and
 * breaking byte-exact matching against the index. Instead the process stays
 * in the startup "C" locale, where the conversion of non-ASCII names fails
 * with ARCHIVE_WARN and libarchive falls back to storing the RAW UTF-8
 * bytes -- exactly the bytes recorded in the index. The header-iteration
 * loop therefore accepts ARCHIVE_WARN alongside ARCHIVE_OK.
 */

/* pick_latest_snapshot: returns a malloc'd label (without .jsonl) of the
 * lexicographically greatest snapshot in <repo>/snapshots, or NULL if none /
 * on error. Same rule as `tarpack pack` (mirrors the static helper in
 * packer.c). */
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

/* join_path: malloc'd "<dest>/<rel>". Caller frees. */
static char *join_path(const char *dest, const char *rel, size_t rel_len) {
    size_t dest_len = strlen(dest);
    char *out = malloc(dest_len + 1 + rel_len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, dest, dest_len);
    out[dest_len] = '/';
    memcpy(out + dest_len + 1, rel, rel_len);
    out[dest_len + 1 + rel_len] = '\0';
    return out;
}

/* mkdir_p: creates each component of path (like `mkdir -p`), mode 0700 for
 * every created directory (fixed up later from snapshot metadata). Returns 0
 * on success (including "already exists"), -1 on failure. */
static int mkdir_p(const char *path) {
    size_t len = strlen(path);
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return -1;
    }
    memcpy(buf, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
                free(buf);
                return -1;
            }
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
        free(buf);
        return -1;
    }
    free(buf);
    return 0;
}

/* mkdir_parents: mkdir -p every component of `path` EXCEPT the last. */
static int mkdir_parents(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) {
        return 0;
    }
    size_t len = (size_t)(slash - path);
    char *parent = malloc(len + 1);
    if (parent == NULL) {
        return -1;
    }
    memcpy(parent, path, len);
    parent[len] = '\0';
    int rc = mkdir_p(parent);
    free(parent);
    return rc;
}

static void bytes_to_hex(const unsigned char *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

/* set_mtime: utimensat with AT_SYMLINK_NOFOLLOW (works for symlinks too);
 * atime is left untouched (UTIME_OMIT). Returns 0/-1. */
static int set_mtime(const char *path, int64_t mtime_sec, long mtime_nsec) {
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = (time_t)mtime_sec;
    times[1].tv_nsec = mtime_nsec;
    return utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW);
}

/* --- snapshot dir/symlink collection ------------------------------------ */

struct dir_meta {
    char *path;
    size_t path_len;
    uid_t uid;
    gid_t gid;
    mode_t mode_perm;
    int64_t mtime_sec;
    long mtime_nsec;
};

struct sym_meta {
    char *path;
    size_t path_len;
    char *target; /* raw bytes, NUL-terminated */
    size_t target_len;
    uid_t uid;
    gid_t gid;
    int64_t mtime_sec;
    long mtime_nsec;
};

struct tree_ctx {
    struct dir_meta *dirs;
    size_t dir_count;
    size_t dir_cap;
    struct sym_meta *syms;
    size_t sym_count;
    size_t sym_cap;
    int oom;
};

static int tree_cb(const struct tp_manifest_entry *entry, void *user_data) {
    struct tree_ctx *ctx = (struct tree_ctx *)user_data;

    if (entry->type == TP_ENTRY_DIR) {
        if (ctx->dir_count == ctx->dir_cap) {
            size_t newcap = ctx->dir_cap == 0 ? 8 : ctx->dir_cap * 2;
            struct dir_meta *nd = realloc(ctx->dirs, newcap * sizeof(*nd));
            if (nd == NULL) {
                ctx->oom = 1;
                return -1;
            }
            ctx->dirs = nd;
            ctx->dir_cap = newcap;
        }
        struct dir_meta *d = &ctx->dirs[ctx->dir_count];
        d->path = malloc(entry->path_len + 1);
        if (d->path == NULL) {
            ctx->oom = 1;
            return -1;
        }
        memcpy(d->path, entry->path, entry->path_len);
        d->path[entry->path_len] = '\0';
        d->path_len = entry->path_len;
        d->uid = entry->uid;
        d->gid = entry->gid;
        d->mode_perm = entry->mode_perm;
        d->mtime_sec = entry->mtime_sec;
        d->mtime_nsec = entry->mtime_nsec;
        ctx->dir_count++;
        return 0;
    }

    if (entry->type == TP_ENTRY_SYMLINK) {
        if (ctx->sym_count == ctx->sym_cap) {
            size_t newcap = ctx->sym_cap == 0 ? 8 : ctx->sym_cap * 2;
            struct sym_meta *ns = realloc(ctx->syms, newcap * sizeof(*ns));
            if (ns == NULL) {
                ctx->oom = 1;
                return -1;
            }
            ctx->syms = ns;
            ctx->sym_cap = newcap;
        }
        struct sym_meta *s = &ctx->syms[ctx->sym_count];
        s->path = malloc(entry->path_len + 1);
        s->target = malloc(entry->target_len + 1);
        if (s->path == NULL || s->target == NULL) {
            free(s->path);
            free(s->target);
            ctx->oom = 1;
            return -1;
        }
        memcpy(s->path, entry->path, entry->path_len);
        s->path[entry->path_len] = '\0';
        s->path_len = entry->path_len;
        memcpy(s->target, entry->target, entry->target_len);
        s->target[entry->target_len] = '\0';
        s->target_len = entry->target_len;
        s->uid = entry->uid;
        s->gid = entry->gid;
        s->mtime_sec = entry->mtime_sec;
        s->mtime_nsec = entry->mtime_nsec;
        ctx->sym_count++;
        return 0;
    }

    return 0; /* header/file handled elsewhere */
}

static void tree_ctx_free(struct tree_ctx *ctx) {
    for (size_t i = 0; i < ctx->dir_count; i++) {
        free(ctx->dirs[i].path);
    }
    free(ctx->dirs);
    for (size_t i = 0; i < ctx->sym_count; i++) {
        free(ctx->syms[i].path);
        free(ctx->syms[i].target);
    }
    free(ctx->syms);
    memset(ctx, 0, sizeof(*ctx));
}

static int dir_meta_cmp(const void *a, const void *b) {
    const struct dir_meta *da = (const struct dir_meta *)a;
    const struct dir_meta *db = (const struct dir_meta *)b;
    return strcmp(da->path, db->path);
}

/* --- per-pack extraction ------------------------------------------------- */

/* needed_obj: one object to extract -- the link group it belongs to plus the
 * index entry resolving it to a pack. */
struct needed_obj {
    struct tp_restore_link_group *group;
    const struct tp_index_entry *ie;
    int written;
};

static int needed_obj_cmp(const void *a, const void *b) {
    const struct needed_obj *na = (const struct needed_obj *)a;
    const struct needed_obj *nb = (const struct needed_obj *)b;
    int c = strcmp(na->ie->pack, nb->ie->pack);
    if (c != 0) {
        return c;
    }
    /* within a pack, order does not matter (sequential scan matches by
     * pathname); keep object_id order for determinism */
    return strcmp(na->group->object_id, nb->group->object_id);
}

/* write_all: loops write() until all n bytes are out. Returns 0/-1. */
static int write_all(int fd, const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* stream_one_entry: copies the current archive entry's data to the file at
 * full (created/truncated), verifying byte count against the index size and,
 * when the index has a sha256, the content hash. Returns 0 clean, 1 written
 * with warnings, -1 fatal (could not create/write the file). */
static int stream_one_entry(struct archive *a, const char *full, const struct needed_obj *no) {
    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 && errno == ENOENT) {
        if (mkdir_parents(full) == 0) {
            fd = open(full, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        }
    }
    if (fd < 0) {
        tp_warn("restore: cannot create '%s'", full);
        return -1;
    }

    SHA256_CTX sha;
    sha256_init(&sha);

    char buf[64 * 1024];
    int64_t total = 0;
    int warned = 0;

    for (;;) {
        la_ssize_t n = archive_read_data(a, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            tp_warnx("restore: read error in pack for '%s': %s", full, archive_error_string(a));
            close(fd);
            return -1;
        }
        sha256_update(&sha, (const BYTE *)buf, (size_t)n);
        if (write_all(fd, buf, (size_t)n) != 0) {
            tp_warn("restore: write error on '%s'", full);
            close(fd);
            return -1;
        }
        total += (int64_t)n;
    }
    if (close(fd) != 0) {
        tp_warn("restore: close error on '%s'", full);
        return -1;
    }

    if (total != (int64_t)no->ie->size) {
        tp_warnx("restore: '%s': size mismatch (index %lld, extracted %lld)", full,
                 (long long)no->ie->size, (long long)total);
        warned = 1;
    }

    if (no->ie->sha256_hex[0] != '\0') {
        BYTE digest[32];
        char hexdigest[65];
        sha256_final(&sha, digest);
        bytes_to_hex(digest, sizeof(digest), hexdigest);
        if (strcmp(hexdigest, no->ie->sha256_hex) != 0) {
            tp_warnx("restore: '%s': sha256 mismatch (index %s, extracted %s)", full,
                     no->ie->sha256_hex, hexdigest);
            warned = 1;
        }
    }

    tp_verbosex("restore: file %s", full);
    return warned;
}

/* restore_one_pack: one sequential pass over <repo>/packs/<pack>.tar.zst,
 * extracting the objects in items[0..count) (all of which live in this pack)
 * to the FIRST snapshot path of their link group under dest. Entries are
 * matched by pathname against the representative path recorded in the index.
 * Returns 0 clean, 1 completed with warnings, -1 fatal. */
static int restore_one_pack(const char *repo, const char *dest, const char *pack,
                             struct needed_obj *items, size_t count) {
    char archive_path[PATH_MAX];
    if (snprintf(archive_path, sizeof(archive_path), "%s/packs/%s.tar.zst", repo, pack) >=
        (int)sizeof(archive_path)) {
        tp_warnx("restore: pack path too long for '%s'", pack);
        return -1;
    }

    struct archive *a = archive_read_new();
    if (a == NULL) {
        tp_warnx("restore: out of memory creating archive reader");
        return -1;
    }
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, archive_path, 64 * 1024) != ARCHIVE_OK) {
        tp_warnx("restore: cannot open pack '%s': %s", archive_path, archive_error_string(a));
        archive_read_free(a);
        return -1;
    }

    int result = 0;
    size_t remaining = count;

    struct archive_entry *entry;
    int rc;
    while (remaining > 0 &&
           ((rc = archive_read_next_header(a, &entry)) == ARCHIVE_OK || rc == ARCHIVE_WARN)) {
        const char *pathname = archive_entry_pathname(entry);
        if (pathname == NULL) {
            archive_read_data_skip(a);
            continue;
        }

        struct needed_obj *match = NULL;
        for (size_t i = 0; i < count; i++) {
            if (!items[i].written && strcmp(items[i].ie->path, pathname) == 0) {
                match = &items[i];
                break;
            }
        }
        if (match == NULL) {
            archive_read_data_skip(a);
            continue;
        }

        char *full = join_path(dest, match->group->refs[0].path, match->group->refs[0].path_len);
        if (full == NULL) {
            archive_read_free(a);
            return -1;
        }
        int src = stream_one_entry(a, full, match);
        free(full);
        if (src < 0) {
            archive_read_free(a);
            return -1;
        }
        if (src > 0) {
            result = 1;
        }
        match->written = 1;
        remaining--;
    }

    archive_read_free(a);
    return result;
}

int tp_restore(const char *repo, const char *dest, const char *snapshot_label) {
    /* --- resolve the snapshot --- */
    char *chosen_label = NULL;
    if (snapshot_label == NULL) {
        chosen_label = pick_latest_snapshot(repo);
        if (chosen_label == NULL) {
            tp_warnx("restore: no snapshots found in '%s/snapshots'", repo);
            return -1;
        }
    }
    const char *label = snapshot_label != NULL ? snapshot_label : chosen_label;

    char snapshot_path[PATH_MAX];
    if (snprintf(snapshot_path, sizeof(snapshot_path), "%s/snapshots/%s.jsonl", repo, label) >=
        (int)sizeof(snapshot_path)) {
        tp_warnx("restore: snapshot path too long");
        free(chosen_label);
        return -1;
    }
    free(chosen_label);

    /* --- collect files (grouped by object), dirs, symlinks --- */
    struct tp_restore_link_set links;
    memset(&links, 0, sizeof(links));
    if (tp_restore_collect_links(snapshot_path, &links) != 0) {
        tp_warnx("restore: cannot read snapshot '%s'", snapshot_path);
        return -1;
    }

    struct tree_ctx tree;
    memset(&tree, 0, sizeof(tree));
    if (tp_manifest_read_file(snapshot_path, tree_cb, &tree) != 0 || tree.oom) {
        tp_warnx("restore: cannot read snapshot '%s'", snapshot_path);
        tree_ctx_free(&tree);
        tp_restore_link_set_free(&links);
        return -1;
    }

    int result = 0;
    struct tp_index idx;
    int idx_loaded = 0;
    struct needed_obj *needed = NULL;

    /* --- path safety: every path we will create must be safe --- */
    {
        size_t unsafe = 0;
        for (size_t i = 0; i < links.count; i++) {
            for (size_t j = 0; j < links.groups[i].count; j++) {
                if (!tp_restore_path_safe(links.groups[i].refs[j].path,
                                           links.groups[i].refs[j].path_len)) {
                    tp_warnx("restore: unsafe path in snapshot: '%s'",
                             links.groups[i].refs[j].path);
                    unsafe++;
                }
            }
        }
        for (size_t i = 0; i < tree.dir_count; i++) {
            if (!tp_restore_path_safe(tree.dirs[i].path, tree.dirs[i].path_len)) {
                tp_warnx("restore: unsafe dir path in snapshot: '%s'", tree.dirs[i].path);
                unsafe++;
            }
        }
        for (size_t i = 0; i < tree.sym_count; i++) {
            if (!tp_restore_path_safe(tree.syms[i].path, tree.syms[i].path_len)) {
                tp_warnx("restore: unsafe symlink path in snapshot: '%s'", tree.syms[i].path);
                unsafe++;
            }
        }
        if (unsafe > 0) {
            tp_warnx("restore: %zu unsafe path(s); refusing to extract", unsafe);
            goto fatal;
        }
    }

    /* --- resolve every object through the index (missing = fatal) --- */
    if (tp_index_load(repo, &idx) != 0) {
        tp_warnx("restore: cannot load object index for '%s'", repo);
        goto fatal;
    }
    idx_loaded = 1;

    if (links.count > 0) {
        needed = calloc(links.count, sizeof(*needed));
        if (needed == NULL) {
            goto fatal;
        }
    }
    {
        size_t missing = 0;
        for (size_t i = 0; i < links.count; i++) {
            const struct tp_index_entry *ie =
                tp_index_find_by_object_id(&idx, links.groups[i].object_id);
            if (ie == NULL) {
                tp_warnx("restore: missing object %s (needed for '%s')",
                         links.groups[i].object_id, links.groups[i].refs[0].path);
                missing++;
                continue;
            }
            needed[i].group = &links.groups[i];
            needed[i].ie = ie;
            needed[i].written = 0;
        }
        if (missing > 0) {
            tp_warnx("restore: %zu object(s) missing from the index; aborting", missing);
            goto fatal;
        }
    }

    /* --- create the directory tree first (mode|0700, fixed up at the end) --- */
    if (mkdir_p(dest) != 0) {
        tp_warn("restore: cannot create destination '%s'", dest);
        goto fatal;
    }
    for (size_t i = 0; i < tree.dir_count; i++) {
        char *full = join_path(dest, tree.dirs[i].path, tree.dirs[i].path_len);
        if (full == NULL) {
            goto fatal;
        }
        int rc = mkdir(full, tree.dirs[i].mode_perm | 0700);
        if (rc != 0 && errno == ENOENT) {
            if (mkdir_parents(full) == 0) {
                rc = mkdir(full, tree.dirs[i].mode_perm | 0700);
            }
        }
        if (rc != 0 && errno != EEXIST) {
            tp_warn("restore: cannot create directory '%s'", full);
            free(full);
            goto fatal;
        }
        tp_verbosex("restore: dir %s", full);
        free(full);
    }

    /* --- extract, one sequential pass per pack (ascending) --- */
    if (links.count > 0) {
        qsort(needed, links.count, sizeof(*needed), needed_obj_cmp);
        size_t start = 0;
        while (start < links.count) {
            size_t end = start + 1;
            while (end < links.count && strcmp(needed[end].ie->pack, needed[start].ie->pack) == 0) {
                end++;
            }
            int prc = restore_one_pack(repo, dest, needed[start].ie->pack, needed + start,
                                        end - start);
            if (prc < 0) {
                goto fatal;
            }
            if (prc > 0) {
                result = 1;
            }
            start = end;
        }

        /* Objects not found in their pack: a zero-size object legitimately
         * reduces to "create an empty file"; anything else is a warning. */
        for (size_t i = 0; i < links.count; i++) {
            if (needed[i].written) {
                continue;
            }
            char *full = join_path(dest, needed[i].group->refs[0].path,
                                    needed[i].group->refs[0].path_len);
            if (full == NULL) {
                goto fatal;
            }
            if (needed[i].ie->size == 0) {
                int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                if (fd < 0) {
                    tp_warn("restore: cannot create empty file '%s'", full);
                    free(full);
                    goto fatal;
                }
                close(fd);
                needed[i].written = 1;
            } else {
                tp_warnx("restore: object %s ('%s') not found in pack %s",
                         needed[i].group->object_id, full, needed[i].ie->pack);
                result = 1;
            }
            free(full);
        }
    }

    /* --- remaining hardlink names --- */
    for (size_t i = 0; i < links.count; i++) {
        struct tp_restore_link_group *g = &links.groups[i];
        if (g->count < 2) {
            continue;
        }
        char *target = join_path(dest, g->refs[0].path, g->refs[0].path_len);
        if (target == NULL) {
            goto fatal;
        }
        for (size_t j = 1; j < g->count; j++) {
            char *linkname = join_path(dest, g->refs[j].path, g->refs[j].path_len);
            if (linkname == NULL) {
                free(target);
                goto fatal;
            }
            if (link(target, linkname) != 0) {
                tp_warn("restore: cannot hardlink '%s' -> '%s'", linkname, target);
                result = 1;
            } else {
                tp_verbosex("restore: hardlink %s => %s", linkname, target);
            }
            free(linkname);
        }
        free(target);
    }

    /* --- symlinks (target bytes verbatim; never resolved or validated) --- */
    for (size_t i = 0; i < tree.sym_count; i++) {
        struct sym_meta *s = &tree.syms[i];
        char *full = join_path(dest, s->path, s->path_len);
        if (full == NULL) {
            goto fatal;
        }
        if (symlink(s->target, full) != 0) {
            tp_warn("restore: cannot symlink '%s' -> '%s'", full, s->target);
            result = 1;
            free(full);
            continue;
        }
        tp_verbosex("restore: symlink %s -> %s", full, s->target);
        if (set_mtime(full, s->mtime_sec, s->mtime_nsec) != 0) {
            tp_warn("restore: cannot set mtime on symlink '%s'", full);
            result = 1;
        }
        if (geteuid() == 0) {
            if (fchownat(AT_FDCWD, full, s->uid, s->gid, AT_SYMLINK_NOFOLLOW) != 0) {
                tp_warn("restore: cannot chown symlink '%s'", full);
                result = 1;
            }
        }
        free(full);
    }

    /* --- file metadata (one name per inode is enough for hardlink groups) --- */
    for (size_t i = 0; i < links.count; i++) {
        struct tp_restore_link_group *g = &links.groups[i];
        char *full = join_path(dest, g->refs[0].path, g->refs[0].path_len);
        if (full == NULL) {
            goto fatal;
        }
        if (chmod(full, g->mode_perm) != 0) {
            tp_warn("restore: cannot chmod '%s'", full);
            result = 1;
        }
        if (geteuid() == 0) {
            if (chown(full, g->uid, g->gid) != 0) {
                tp_warn("restore: cannot chown '%s'", full);
                result = 1;
            }
        }
        if (set_mtime(full, g->mtime_sec, g->mtime_nsec) != 0) {
            tp_warn("restore: cannot set mtime on '%s'", full);
            result = 1;
        }
        free(full);
    }

    /* --- directory metadata LAST, deepest-first, so file/link creation does
     * not clobber the restored mtimes ("a/b" sorts after "a", so walking the
     * sorted array backwards visits children before their parents) --- */
    if (tree.dir_count > 0) {
        qsort(tree.dirs, tree.dir_count, sizeof(*tree.dirs), dir_meta_cmp);
        for (size_t k = tree.dir_count; k > 0; k--) {
            struct dir_meta *d = &tree.dirs[k - 1];
            char *full = join_path(dest, d->path, d->path_len);
            if (full == NULL) {
                goto fatal;
            }
            if (chmod(full, d->mode_perm) != 0) {
                tp_warn("restore: cannot chmod directory '%s'", full);
                result = 1;
            }
            if (geteuid() == 0) {
                if (chown(full, d->uid, d->gid) != 0) {
                    tp_warn("restore: cannot chown directory '%s'", full);
                    result = 1;
                }
            }
            if (set_mtime(full, d->mtime_sec, d->mtime_nsec) != 0) {
                tp_warn("restore: cannot set mtime on directory '%s'", full);
                result = 1;
            }
            free(full);
        }
    }

    free(needed);
    if (idx_loaded) {
        tp_index_free(&idx);
    }
    tree_ctx_free(&tree);
    tp_restore_link_set_free(&links);
    return result;

fatal:
    free(needed);
    if (idx_loaded) {
        tp_index_free(&idx);
    }
    tree_ctx_free(&tree);
    tp_restore_link_set_free(&links);
    return -1;
}
