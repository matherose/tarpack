#include "object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uthash.h"

/* Packed composite key: padding bytes must be zeroed (memset) before use,
 * since uthash hashes/compares the key's raw bytes with memcmp/hash-over-bytes. */
struct tp_object_key {
    dev_t dev;
    ino_t ino;
};

struct tp_object_node {
    struct tp_object_key key;
    struct tp_object obj;
    UT_hash_handle hh;
};

void tp_object_table_init(struct tp_object_table *t) {
    t->impl = NULL;
    t->next_id = 1;
}

void tp_object_table_free(struct tp_object_table *t) {
    struct tp_object_node *head = (struct tp_object_node *)t->impl;
    struct tp_object_node *cur;
    struct tp_object_node *tmp;

    HASH_ITER(hh, head, cur, tmp) {
        HASH_DEL(head, cur);
        free(cur);
    }

    t->impl = NULL;
    t->next_id = 1;
}

struct tp_object *tp_object_table_find(struct tp_object_table *t, dev_t dev, ino_t ino) {
    struct tp_object_node *head = (struct tp_object_node *)t->impl;
    struct tp_object_key key;
    struct tp_object_node *found = NULL;

    memset(&key, 0, sizeof(key));
    key.dev = dev;
    key.ino = ino;

    HASH_FIND(hh, head, &key, sizeof(key), found);
    if (found == NULL) {
        return NULL;
    }
    return &found->obj;
}

/* insert_common: shared body for tp_object_table_insert / _with_id. Copies
 * metadata from *st into a freshly allocated node and hash-inserts it by
 * (dev,ino); the id itself is assigned by the caller after this returns
 * (see below). Returns the new object, or NULL on allocation failure. */
static struct tp_object *insert_common(struct tp_object_table *t, const struct stat *st) {
    struct tp_object_node *head = (struct tp_object_node *)t->impl;
    struct tp_object_node *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }

    memset(&node->key, 0, sizeof(node->key));
    node->key.dev = st->st_dev;
    node->key.ino = st->st_ino;

    struct tp_object *obj = &node->obj;
    memset(obj, 0, sizeof(*obj));

    obj->dev = st->st_dev;
    obj->ino = st->st_ino;
    obj->nlink = st->st_nlink;
    obj->size = st->st_size;
    obj->mode = st->st_mode;
    obj->uid = st->st_uid;
    obj->gid = st->st_gid;
#ifdef __APPLE__
    obj->mtime_sec = (int64_t)st->st_mtimespec.tv_sec;
    obj->mtime_nsec = (long)st->st_mtimespec.tv_nsec;
#else
    obj->mtime_sec = (int64_t)st->st_mtim.tv_sec;
    obj->mtime_nsec = (long)st->st_mtim.tv_nsec;
#endif
    obj->sha256_hex[0] = '\0';

    HASH_ADD(hh, head, key, sizeof(node->key), node);
    t->impl = head;

    return obj;
}

struct tp_object *tp_object_table_insert(struct tp_object_table *t, const struct stat *st) {
    struct tp_object *obj = insert_common(t, st);
    if (obj == NULL) {
        return NULL;
    }
    snprintf(obj->id, sizeof(obj->id), "O%llu", (unsigned long long)t->next_id);
    t->next_id++;
    return obj;
}

struct tp_object *tp_object_table_insert_with_id(struct tp_object_table *t, const struct stat *st,
                                                   const char *id) {
    struct tp_object *obj = insert_common(t, st);
    if (obj == NULL) {
        return NULL;
    }
    snprintf(obj->id, sizeof(obj->id), "%s", id);
    return obj;
}
