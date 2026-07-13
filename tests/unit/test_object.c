/*
 * Plain-C test runner for the object table (src/object.h).
 * No framework: checks conditions, counts failures, and returns
 * non-zero exit status if any check fails. Mirrors tests/unit/test_sha256.c.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "object.h"

static int g_fail_count = 0;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail_count++;
        return;
    }
    printf("PASS: %s\n", label);
}

static struct stat make_stat(dev_t dev, ino_t ino, nlink_t nlink, off_t size, mode_t mode,
                              uid_t uid, gid_t gid, int64_t mtime_sec, long mtime_nsec) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_dev = dev;
    st.st_ino = ino;
    st.st_nlink = nlink;
    st.st_size = size;
    st.st_mode = mode;
    st.st_uid = uid;
    st.st_gid = gid;
#ifdef __APPLE__
    st.st_mtimespec.tv_sec = (time_t)mtime_sec;
    st.st_mtimespec.tv_nsec = mtime_nsec;
#else
    st.st_mtim.tv_sec = (time_t)mtime_sec;
    st.st_mtim.tv_nsec = mtime_nsec;
#endif
    return st;
}

int main(void) {
    struct tp_object_table table;
    tp_object_table_init(&table);

    /* insert two distinct (dev,ino) -> different ids O1/O2 */
    struct stat st1 = make_stat(10, 100, 1, 4096, S_IFREG | 0644, 501, 20, 1700000000, 123456789);
    struct stat st2 = make_stat(10, 200, 1, 8192, S_IFREG | 0755, 501, 20, 1700000100, 987654321);

    struct tp_object *o1 = tp_object_table_insert(&table, &st1);
    struct tp_object *o2 = tp_object_table_insert(&table, &st2);

    check(o1 != NULL && o2 != NULL, "insert returns non-NULL objects");
    check(o1 != NULL && strcmp(o1->id, "O1") == 0, "first insert gets id O1");
    check(o2 != NULL && strcmp(o2->id, "O2") == 0, "second distinct insert gets id O2");
    check(o1 != NULL && o2 != NULL && strcmp(o1->id, o2->id) != 0, "distinct objects have distinct ids");

    /* find same (dev,ino) -> same pointer */
    struct tp_object *found1 = tp_object_table_find(&table, 10, 100);
    check(found1 == o1, "find returns same pointer as original insert for same (dev,ino)");

    struct tp_object *found_missing = tp_object_table_find(&table, 999, 999);
    check(found_missing == NULL, "find returns NULL for unknown (dev,ino)");

    /* verify metadata copied correctly from a fabricated struct stat */
    check(o1 != NULL && o1->dev == 10, "dev copied correctly");
    check(o1 != NULL && o1->ino == 100, "ino copied correctly");
    check(o1 != NULL && o1->nlink == 1, "nlink copied correctly");
    check(o1 != NULL && o1->size == 4096, "size copied correctly");
    check(o1 != NULL && o1->mode == (S_IFREG | 0644), "mode copied correctly");
    check(o1 != NULL && o1->uid == 501, "uid copied correctly");
    check(o1 != NULL && o1->gid == 20, "gid copied correctly");
    check(o1 != NULL && o1->mtime_sec == 1700000000, "mtime_sec copied correctly");
    check(o1 != NULL && o1->mtime_nsec == 123456789, "mtime_nsec copied correctly");
    check(o1 != NULL && o1->sha256_hex[0] == '\0', "sha256_hex starts empty");

    check(o2 != NULL && o2->size == 8192, "second object size copied correctly");
    check(o2 != NULL && o2->mode == (S_IFREG | 0755), "second object mode copied correctly");

    /* insert_with_id: caller-supplied id, does not disturb next_id sequence */
    uint64_t next_id_before = table.next_id;
    struct stat st3 = make_stat(10, 300, 1, 55, S_IFREG | 0644, 501, 20, 1700000200, 555);
    struct tp_object *o3 = tp_object_table_insert_with_id(&table, &st3, "O42");
    check(o3 != NULL && strcmp(o3->id, "O42") == 0, "insert_with_id assigns the caller-supplied id");
    check(table.next_id == next_id_before, "insert_with_id does not advance next_id");

    struct tp_object *found3 = tp_object_table_find(&table, 10, 300);
    check(found3 == o3, "find locates the insert_with_id object by (dev,ino)");

    struct tp_object *o4 = tp_object_table_insert(&table, &st1);
    check(o4 != NULL && strcmp(o4->id, "O3") == 0,
          "a subsequent plain insert still allocates the next sequential id (O3), unaffected by O42");

    tp_object_table_free(&table);

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }

    printf("\nall checks passed\n");
    return 0;
}
