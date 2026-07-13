#ifndef TARPACK_INDEX_H
#define TARPACK_INDEX_H

#include <stdint.h>
#include <sys/types.h>

/*
 * tp_index: the cross-run object index, <repo>/objects/objects.jsonl.
 *
 * This is an append-only JSONL log, one line per packed object, appended by
 * `tarpack pack` ONLY after the pack file containing that object has been
 * successfully renamed into place. Each line looks like:
 *   {"object_id":"O7","pack":"pack-000002","offset":1536,"size":123,
 *    "sha256":"<hex or empty>","path":"<representative path>"|"path_b64":"...",
 *    "mtime_sec":...,"mtime_nsec":...}
 *
 * Cross-run object identity intentionally does NOT rely on (dev,inode):
 * inodes get recycled across runs/filesystems. Instead, on `scan`, the
 * index is consulted to detect files that are already packed (by
 * path+size+mtime, or by content hash in --hash mode) so their existing
 * object_id can be reused; brand-new content gets a fresh, globally unique
 * id derived from the max id seen in the index so far.
 */

/* tp_index_entry: one decoded line of objects.jsonl. */
struct tp_index_entry {
    char *object_id;   /* malloc'd, e.g. "O7" */
    char *pack;        /* malloc'd pack name, e.g. "pack-000002" */
    int64_t offset;
    off_t size;
    char sha256_hex[65]; /* may be empty string if not computed */
    char *path;         /* malloc'd representative path, raw bytes, NUL-terminated */
    size_t path_len;
    int64_t mtime_sec;
    long mtime_nsec;
};

/* Opaque hash maps; internal representation lives in index.c. */
struct tp_index {
    void *by_path_impl; /* keyed by (path,size,mtime_sec,mtime_nsec) */
    void *by_sha_impl;  /* keyed by sha256 hex, only entries with non-empty sha256 */
    void *by_id_impl;   /* keyed by object_id, used by tp_index_object_id_present */
    uint64_t next_id;    /* (max numeric suffix of existing "O<n>") + 1, or 1 if empty */
};

/*
 * tp_index_load: loads <repo>/objects/objects.jsonl (if present) into *idx.
 * A missing file is not an error: *idx is initialized empty with next_id=1.
 * Returns 0 on success, -1 on a genuine I/O/parse failure (file exists but
 * is unreadable/corrupt).
 */
int tp_index_load(const char *repo, struct tp_index *idx);

/*
 * tp_index_free: releases all internal storage. Safe to call once after a
 * successful or failed tp_index_load.
 */
void tp_index_free(struct tp_index *idx);

/*
 * tp_index_find_by_path: looks up an entry by exact (relative path, size,
 * mtime_sec, mtime_nsec) match (the v0.3 "fast mode" dedup key). Returns
 * NULL if not present. The returned pointer is owned by *idx.
 */
const struct tp_index_entry *tp_index_find_by_path(const struct tp_index *idx, const char *path,
                                                     size_t path_len, off_t size,
                                                     int64_t mtime_sec, long mtime_nsec);

/*
 * tp_index_find_by_sha256: looks up an entry by sha256 hex digest ("hash
 * mode" dedup key). Returns NULL if not present or sha256_hex is empty.
 * The returned pointer is owned by *idx.
 */
const struct tp_index_entry *tp_index_find_by_sha256(const struct tp_index *idx,
                                                       const char *sha256_hex);

/*
 * tp_index_object_id_present: returns 1 if object_id already appears in the
 * index (i.e. it was packed in a previous run and `tarpack pack` should
 * skip storing it again), 0 otherwise.
 */
int tp_index_object_id_present(const struct tp_index *idx, const char *object_id);

/*
 * tp_index_append: appends `count` entries to <repo>/objects/objects.jsonl,
 * creating the file (and the objects/ directory) if needed. Opens in append
 * mode, writes each entry as one JSON line, then fflush+fsync. Does NOT
 * update *idx in memory (callers reload or insert separately if they need
 * the appended entries visible via find_by_*). Returns 0 on success, -1 on
 * failure.
 */
int tp_index_append(const char *repo, const struct tp_index_entry *entries, size_t count);

#endif /* TARPACK_INDEX_H */
