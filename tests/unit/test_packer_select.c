/*
 * Unit test for the packer's object-collection step (tp_pack_collect).
 * Builds a synthetic snapshot via the manifest writer API: two paths sharing
 * one object_id (a hardlink pair), two additional distinct file objects, plus
 * a directory entry and a symlink entry. Asserts tp_pack_collect returns
 * exactly the 3 unique file objects, in first-seen order, with the correct
 * representative (first-listed) paths and metadata.
 *
 * No framework; mirrors tests/unit/test_manifest.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "manifest.h"
#include "packer.h"

static int g_fail_count = 0;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail_count++;
        return;
    }
    printf("PASS: %s\n", label);
}

static char g_tmpdir[] = "/tmp/tarpack_test_packer_select.XXXXXX";

int main(void) {
    char *tmpdir = mkdtemp(g_tmpdir);
    if (tmpdir == NULL) {
        fprintf(stderr, "FAIL: mkdtemp failed\n");
        return 1;
    }

    char repo[512];
    snprintf(repo, sizeof(repo), "%s/repo", tmpdir);

    struct tp_snapshot_writer w;
    int rc = tp_snapshot_writer_open(&w, repo, "snap", "/some/root");
    check(rc == 0, "writer_open succeeds");

    /* dir entry (must be ignored) */
    rc = tp_snapshot_write_dir(&w, "sub", 501, 20, 0755, 1700000000, 0, 10, 1);
    check(rc == 0, "write_dir");

    /* first object: a hardlink pair -- two paths, same object_id O1.
     * The FIRST path listed ("a.txt") is the representative. */
    rc = tp_snapshot_write_file(&w, "a.txt", "O1", 2, 501, 20, 0644, 100, 1700000001, 11, 10, 100,
                                 NULL);
    check(rc == 0, "write_file a.txt (O1, first of hardlink pair)");
    rc = tp_snapshot_write_file(&w, "sub/a_link.txt", "O1", 2, 501, 20, 0644, 100, 1700000001, 11,
                                 10, 100, NULL);
    check(rc == 0, "write_file sub/a_link.txt (O1, second of hardlink pair)");

    /* second distinct object */
    rc = tp_snapshot_write_file(&w, "b.txt", "O2", 1, 501, 20, 0600, 200, 1700000002, 22, 10, 101,
                                 NULL);
    check(rc == 0, "write_file b.txt (O2)");

    /* symlink entry (must be ignored) */
    rc = tp_snapshot_write_symlink(&w, "link", "a.txt", strlen("a.txt"), 501, 20, 1700000003, 0, 10,
                                    102);
    check(rc == 0, "write_symlink");

    /* third distinct object */
    rc = tp_snapshot_write_file(&w, "c.txt", "O3", 1, 501, 20, 0640, 0, 1700000004, 44, 10, 103,
                                 NULL);
    check(rc == 0, "write_file c.txt (O3, zero size)");

    rc = tp_snapshot_writer_close(&w);
    check(rc == 0, "writer_close");

    char snapshot_path[600];
    snprintf(snapshot_path, sizeof(snapshot_path), "%s/snapshots/snap.jsonl", repo);

    /* --- collect --- */
    struct tp_pack_object_set set;
    memset(&set, 0, sizeof(set));
    char *root = NULL;
    rc = tp_pack_collect(snapshot_path, &set, &root);
    check(rc == 0, "tp_pack_collect succeeds");
    check(root != NULL && strcmp(root, "/some/root") == 0, "root captured from header");

    check(set.count == 3, "exactly 3 unique file objects collected (hardlink counted once)");

    if (set.count >= 1) {
        struct tp_packed_object *o = &set.objects[0];
        check(strcmp(o->object_id, "O1") == 0, "object[0] id is O1 (first seen)");
        check(strcmp(o->path, "a.txt") == 0, "object[0] representative path is a.txt (first listed)");
        check(o->mode_perm == 0644, "object[0] mode");
        check(o->size == 100, "object[0] size");
        check(o->mtime_sec == 1700000001, "object[0] mtime_sec");
        check(o->mtime_nsec == 11, "object[0] mtime_nsec");
    }
    if (set.count >= 2) {
        struct tp_packed_object *o = &set.objects[1];
        check(strcmp(o->object_id, "O2") == 0, "object[1] id is O2");
        check(strcmp(o->path, "b.txt") == 0, "object[1] representative path is b.txt");
        check(o->size == 200, "object[1] size");
    }
    if (set.count >= 3) {
        struct tp_packed_object *o = &set.objects[2];
        check(strcmp(o->object_id, "O3") == 0, "object[2] id is O3");
        check(strcmp(o->path, "c.txt") == 0, "object[2] representative path is c.txt");
        check(o->size == 0, "object[2] size is 0");
    }

    free(root);
    tp_pack_object_set_free(&set);

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
