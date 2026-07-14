#include "scanner.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "index.h"
#include "manifest.h"
#include "object.h"
#include "sha256.h"
#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct scan_ctx {
    struct tp_object_table objects;
    struct tp_snapshot_writer writer;
    struct tp_index index;
    int hash_mode;
    dev_t repo_dev;
    ino_t repo_ino;
    int have_repo_stat;
    int had_error; /* set on any non-fatal warning (unreadable dir/file, etc.) */

    /* summary counters (count distinct objects, not hardlinked names) */
    uint64_t new_objects;
    uint64_t already_packed_objects;
};

static void bytes_to_hex(const unsigned char *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

/* hash_file: streams the regular file at dirfd/name (opened via
 * safe_openat semantics, 64 KiB chunks) and computes its SHA-256, written as
 * a lowercase hex string into out_hex (must be >= 65 bytes). Returns 0 on
 * success, -1 on failure (errno set by the failing syscall). */
static int hash_file(int dirfd, const char *name, char *out_hex) {
    int fd = safe_openat(dirfd, name, O_RDONLY);
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
            int saved_errno = errno;
            close(fd);
            errno = saved_errno;
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

/* dirent_name_cmp: byte-lexicographic comparator for qsort, used to make
 * traversal order (and therefore manifest output order) deterministic. */
static int dirent_name_cmp(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

/* list_sorted_names: reads all directory entries from dirfd (except "."
 * and ".."), returns a malloc'd, sorted (byte order) array of malloc'd
 * name strings, and sets *out_count. Returns 0 on success, -1 on failure
 * (errno set). Caller frees each name and the array. */
static int list_sorted_names(int dirfd, char ***out_names, size_t *out_count) {
    int fd_copy = dup(dirfd);
    if (fd_copy < 0) {
        return -1;
    }

    DIR *d = fdopendir(fd_copy);
    if (d == NULL) {
        int saved_errno = errno;
        close(fd_copy);
        errno = saved_errno;
        return -1;
    }

    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;

    errno = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (count == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            char **new_names = realloc(names, new_cap * sizeof(*names));
            if (new_names == NULL) {
                goto fail;
            }
            names = new_names;
            cap = new_cap;
        }
        char *dup_name = strdup(ent->d_name);
        if (dup_name == NULL) {
            goto fail;
        }
        names[count++] = dup_name;
        errno = 0;
    }
    if (errno != 0) {
        goto fail;
    }

    closedir(d); /* also closes fd_copy */

    qsort(names, count, sizeof(*names), dirent_name_cmp);

    *out_names = names;
    *out_count = count;
    return 0;

fail: {
    int saved_errno = errno != 0 ? errno : ENOMEM;
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
    closedir(d);
    errno = saved_errno;
    return -1;
}
}

static int join_rel_path(char *buf, size_t buf_size, const char *rel_dir, const char *name) {
    int n;
    if (rel_dir[0] == '\0') {
        n = snprintf(buf, buf_size, "%s", name);
    } else {
        n = snprintf(buf, buf_size, "%s/%s", rel_dir, name);
    }
    if (n < 0 || (size_t)n >= buf_size) {
        return -1;
    }
    return 0;
}

static void scan_dir(struct scan_ctx *ctx, int dirfd, const char *rel_dir);

static void handle_entry(struct scan_ctx *ctx, int dirfd, const char *rel_dir, const char *name) {
    struct stat st;

    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        tp_warn("cannot stat %s/%s", rel_dir, name);
        ctx->had_error = 1;
        return;
    }

    char rel_path[PATH_MAX];
    if (join_rel_path(rel_path, sizeof(rel_path), rel_dir, name) != 0) {
        tp_warnx("path too long, skipping: %s/%s", rel_dir, name);
        ctx->had_error = 1;
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        if (ctx->have_repo_stat && st.st_dev == ctx->repo_dev && st.st_ino == ctx->repo_ino) {
            /* skip the repo directory itself to avoid self-backup loops */
            return;
        }

        int sub_fd = safe_openat(dirfd, name, O_RDONLY | O_DIRECTORY);
        if (sub_fd < 0) {
            tp_warn("cannot open directory %s", rel_path);
            ctx->had_error = 1;
            return;
        }

        if (tp_snapshot_write_dir(&ctx->writer, rel_path, st.st_uid, st.st_gid,
                                   st.st_mode & 07777,
#ifdef __APPLE__
                                   (int64_t)st.st_mtimespec.tv_sec, (long)st.st_mtimespec.tv_nsec,
#else
                                   (int64_t)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec,
#endif
                                   st.st_dev, st.st_ino) != 0) {
            tp_warn("failed writing manifest entry for %s", rel_path);
            ctx->had_error = 1;
            close(sub_fd);
            return;
        }

        tp_verbosex("scan: dir %s", rel_path);
        scan_dir(ctx, sub_fd, rel_path);
        close(sub_fd);
        return;
    }

    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t len = readlinkat(dirfd, name, target, sizeof(target));
        if (len < 0) {
            tp_warn("cannot read symlink %s", rel_path);
            ctx->had_error = 1;
            return;
        }
        if ((size_t)len == sizeof(target)) {
            tp_warnx("symlink target too long, skipping: %s", rel_path);
            ctx->had_error = 1;
            return;
        }

        if (tp_snapshot_write_symlink(&ctx->writer, rel_path, target, (size_t)len, st.st_uid,
                                       st.st_gid,
#ifdef __APPLE__
                                       (int64_t)st.st_mtimespec.tv_sec, (long)st.st_mtimespec.tv_nsec,
#else
                                       (int64_t)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec,
#endif
                                       st.st_dev, st.st_ino) != 0) {
            tp_warn("failed writing manifest entry for %s", rel_path);
            ctx->had_error = 1;
        } else {
            tp_verbosex("scan: symlink %s -> %s", rel_path, target);
        }
        return;
    }

    if (S_ISREG(st.st_mode)) {
#ifdef __APPLE__
        int64_t mtime_sec = (int64_t)st.st_mtimespec.tv_sec;
        long mtime_nsec = (long)st.st_mtimespec.tv_nsec;
#else
        int64_t mtime_sec = (int64_t)st.st_mtim.tv_sec;
        long mtime_nsec = (long)st.st_mtim.tv_nsec;
#endif
        char sha256_hex[65];
        sha256_hex[0] = '\0';

        struct tp_object *obj = tp_object_table_find(&ctx->objects, st.st_dev, st.st_ino);
        if (obj == NULL) {
            /* first name seen for this inode this run: decide its id via
             * the cross-run index before inserting. */
            const struct tp_index_entry *hit = tp_index_find_by_path(
                &ctx->index, rel_path, strlen(rel_path), st.st_size, mtime_sec, mtime_nsec);

            if (hit == NULL && ctx->hash_mode) {
                if (hash_file(dirfd, name, sha256_hex) != 0) {
                    tp_warn("cannot hash %s", rel_path);
                    ctx->had_error = 1;
                    sha256_hex[0] = '\0';
                } else {
                    hit = tp_index_find_by_sha256(&ctx->index, sha256_hex);
                }
            }

            if (hit != NULL) {
                obj = tp_object_table_insert_with_id(&ctx->objects, &st, hit->object_id);
                if (obj != NULL) {
                    ctx->already_packed_objects++;
                }
            } else {
                obj = tp_object_table_insert(&ctx->objects, &st);
                if (obj != NULL) {
                    ctx->new_objects++;
                }
            }

            if (obj == NULL) {
                tp_warnx("out of memory recording object for %s", rel_path);
                ctx->had_error = 1;
                return;
            }
            if (sha256_hex[0] != '\0') {
                snprintf(obj->sha256_hex, sizeof(obj->sha256_hex), "%s", sha256_hex);
            }
        }

        if (tp_snapshot_write_file(&ctx->writer, rel_path, obj->id, st.st_nlink, st.st_uid,
                                    st.st_gid, st.st_mode & 07777, st.st_size, mtime_sec,
                                    mtime_nsec, st.st_dev, st.st_ino, obj->sha256_hex) != 0) {
            tp_warn("failed writing manifest entry for %s", rel_path);
            ctx->had_error = 1;
        } else {
            tp_verbosex("scan: file %s (object %s, %lld bytes)", rel_path, obj->id,
                        (long long)st.st_size);
        }
        return;
    }

    /* sockets, FIFOs, devices, etc: unsupported, skip with a notice */
    tp_warnx("skipping unsupported file type: %s", rel_path);
}

static void scan_dir(struct scan_ctx *ctx, int dirfd, const char *rel_dir) {
    char **names = NULL;
    size_t count = 0;

    if (list_sorted_names(dirfd, &names, &count) != 0) {
        tp_warn("cannot read directory %s", rel_dir[0] == '\0' ? "." : rel_dir);
        ctx->had_error = 1;
        return;
    }

    for (size_t i = 0; i < count; i++) {
        handle_entry(ctx, dirfd, rel_dir, names[i]);
        free(names[i]);
    }
    free(names);
}

int tp_scan(const char *root, const char *repo, const char *label, int hash_mode) {
    int root_fd = open(root, O_RDONLY | O_DIRECTORY);
    if (root_fd < 0) {
        tp_warn("cannot open root %s", root);
        return -1;
    }

    struct stat root_st;
    if (fstat(root_fd, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        tp_warnx("%s is not a directory", root);
        close(root_fd);
        return -1;
    }

    struct scan_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.hash_mode = hash_mode;

    if (tp_index_load(repo, &ctx.index) != 0) {
        tp_warn("cannot load object index for repo %s", repo);
        close(root_fd);
        return -1;
    }

    tp_object_table_init(&ctx.objects);
    ctx.objects.next_id = ctx.index.next_id;

    struct stat repo_st;
    if (stat(repo, &repo_st) == 0 && S_ISDIR(repo_st.st_mode)) {
        ctx.repo_dev = repo_st.st_dev;
        ctx.repo_ino = repo_st.st_ino;
        ctx.have_repo_stat = 1;
    }

    if (tp_snapshot_writer_open(&ctx.writer, repo, label, root) != 0) {
        tp_warn("cannot open snapshot writer for repo %s", repo);
        tp_object_table_free(&ctx.objects);
        tp_index_free(&ctx.index);
        close(root_fd);
        return -1;
    }

    scan_dir(&ctx, root_fd, "");

    int close_rc = tp_snapshot_writer_close(&ctx.writer);
    if (close_rc != 0) {
        tp_warn("failed to finalize snapshot manifest");
        ctx.had_error = 1;
    }

    fprintf(stderr, "tarpack: scan summary: %llu new object(s), %llu already-packed object(s)\n",
            (unsigned long long)ctx.new_objects, (unsigned long long)ctx.already_packed_objects);

    tp_object_table_free(&ctx.objects);
    tp_index_free(&ctx.index);
    close(root_fd);

    return ctx.had_error ? 1 : 0;
}
