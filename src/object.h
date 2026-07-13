#ifndef TARPACK_OBJECT_H
#define TARPACK_OBJECT_H

#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
 * tp_object: metadata for one distinct on-disk object (a regular file
 * identified by its (dev,ino) pair). Multiple hardlinked directory
 * entries resolve to the same tp_object and share its id.
 */
struct tp_object {
    char id[32]; /* "O1", "O2", ... assigned in insertion order, per run */
    dev_t dev;
    ino_t ino;
    nlink_t nlink;
    off_t size;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    int64_t mtime_sec;
    long mtime_nsec;
    char sha256_hex[65]; /* empty string ("") if not yet computed */
};

/* Opaque handle; internal representation lives in object.c. */
struct tp_object_table {
    void *impl;
    uint64_t next_id;
};

/*
 * tp_object_table_init: initializes an empty table. Must be called before
 * any other tp_object_table_* function.
 */
void tp_object_table_init(struct tp_object_table *t);

/*
 * tp_object_table_free: releases all objects and internal bookkeeping.
 * The table must not be used again unless re-initialized.
 */
void tp_object_table_free(struct tp_object_table *t);

/*
 * tp_object_table_find: looks up an existing object by (dev,ino).
 * Returns NULL if not present. The returned pointer is owned by the
 * table and remains valid until tp_object_table_free().
 */
struct tp_object *tp_object_table_find(struct tp_object_table *t, dev_t dev, ino_t ino);

/*
 * tp_object_table_insert: inserts a new object described by *st, assigning
 * it the next "O<n>" id and copying dev/ino/nlink/size/mode/uid/gid/mtime
 * from *st. Does NOT check for an existing (dev,ino) entry first -- callers
 * (e.g. the scanner) should call tp_object_table_find() first and only
 * insert on a miss. Returns the new object, or NULL on allocation failure.
 */
struct tp_object *tp_object_table_insert(struct tp_object_table *t, const struct stat *st);

/*
 * tp_object_table_insert_with_id: like tp_object_table_insert, but assigns
 * the caller-supplied id (e.g. an id reused from the cross-run object index
 * after a v0.3 dedup hit) instead of allocating a fresh "O<n>" id, and does
 * NOT touch t->next_id. Used by the scanner when a scanned file matches an
 * already-packed object in the index. Does NOT check for an existing
 * (dev,ino) entry first, same caveat as tp_object_table_insert. Returns the
 * new object, or NULL on allocation failure.
 */
struct tp_object *tp_object_table_insert_with_id(struct tp_object_table *t, const struct stat *st,
                                                   const char *id);

#endif /* TARPACK_OBJECT_H */
