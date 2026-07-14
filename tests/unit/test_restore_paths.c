/*
 * Unit test for restore path-safety (tp_restore_path_safe) and hardlink
 * grouping order (tp_restore_collect_links). No framework; mirrors
 * tests/unit/test_manifest.c / test_packer_select.c.
 *
 * Path safety: absolute paths and any path containing a ".." component are
 * rejected; weird-but-legal names (spaces, quotes, embedded dots that are not
 * a lone "..", multibyte UTF-8, dotfiles) are accepted.
 *
 * Hardlink grouping: given a snapshot with a 3-name hardlink group plus other
 * objects, tp_restore_collect_links returns one group per object_id in
 * first-seen order, and refs[0] of the hardlink group is the FIRST path listed
 * (the link target), with the remaining names in snapshot order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "manifest.h"
#include "restore.h"

static int g_fail_count = 0;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail_count++;
        return;
    }
    printf("PASS: %s\n", label);
}

static void expect_safe(const char *p, const char *label) {
    check(tp_restore_path_safe(p, strlen(p)) == 1, label);
}

static void expect_unsafe(const char *p, const char *label) {
    check(tp_restore_path_safe(p, strlen(p)) == 0, label);
}

int main(void) {
    /* --- path safety: rejections --- */
    expect_unsafe("", "empty path rejected");
    expect_unsafe("/etc/passwd", "absolute path rejected");
    expect_unsafe("..", "bare .. rejected");
    expect_unsafe("../secret", "leading ../ rejected");
    expect_unsafe("a/../b", "middle .. component rejected");
    expect_unsafe("a/b/..", "trailing /.. rejected");
    expect_unsafe("../../x", "multiple .. rejected");

    /* --- path safety: weird-but-legal names accepted --- */
    expect_safe("a.txt", "plain relative name accepted");
    expect_safe("sub/dir/file", "nested relative name accepted");
    expect_safe("...", "triple-dot name accepted (not a .. component)");
    expect_safe("a..b", "embedded double-dot in a segment accepted");
    expect_safe("..a", "segment starting with .. but longer accepted");
    expect_safe("a../b", "segment ending with .. but longer accepted");
    expect_safe(".hidden", "dotfile accepted");
    expect_safe("has space.txt", "name with space accepted");
    expect_safe("quote'\"name", "name with quotes accepted");
    expect_safe("caf\xC3\xA9_\xF0\x9F\x98\x80.txt", "multibyte UTF-8 name accepted");
    expect_safe("./ok", "note: leading ./ is a legal '.' component, accepted");

    /* --- hardlink grouping order --- */
    char tmpl[] = "/tmp/tarpack_test_restore_paths.XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    if (tmpdir == NULL) {
        fprintf(stderr, "FAIL: mkdtemp failed\n");
        return 1;
    }

    char repo[512];
    snprintf(repo, sizeof(repo), "%s/repo", tmpdir);

    struct tp_snapshot_writer w;
    int rc = tp_snapshot_writer_open(&w, repo, "snap", "/some/root");
    check(rc == 0, "writer_open succeeds");

    /* O1: a plain first object */
    tp_snapshot_write_file(&w, "first.txt", "O1", 1, 501, 20, 0644, 10, 1700000000, 0, 10, 100, NULL, NULL);
    /* O2: a 3-name hardlink group. "link_a" is first -> the link target. */
    tp_snapshot_write_file(&w, "link_a", "O2", 3, 501, 20, 0644, 20, 1700000001, 0, 10, 101, NULL, NULL);
    tp_snapshot_write_dir(&w, "sub", 501, 20, 0755, 1700000002, 0, 10, 200, NULL);
    tp_snapshot_write_file(&w, "sub/link_b", "O2", 3, 501, 20, 0644, 20, 1700000001, 0, 10, 101,
                            NULL, NULL);
    tp_snapshot_write_file(&w, "link_c", "O2", 3, 501, 20, 0644, 20, 1700000001, 0, 10, 101, NULL, NULL);
    /* symlink entry (must be ignored by link grouping) */
    tp_snapshot_write_symlink(&w, "slink", "first.txt", strlen("first.txt"), 501, 20, 1700000003, 0,
                               10, 300, NULL);
    /* O3: another plain object, seen after the hardlink group */
    tp_snapshot_write_file(&w, "last.txt", "O3", 1, 501, 20, 0600, 30, 1700000004, 0, 10, 102, NULL, NULL);
    check(w.error == 0, "all snapshot writes succeed");
    check(tp_snapshot_writer_close(&w) == 0, "writer_close succeeds");

    char snap_path[600];
    snprintf(snap_path, sizeof(snap_path), "%s/snapshots/snap.jsonl", repo);

    struct tp_restore_link_set set;
    memset(&set, 0, sizeof(set));
    rc = tp_restore_collect_links(snap_path, &set);
    check(rc == 0, "collect_links succeeds");
    check(set.count == 3, "exactly 3 object groups (O1, O2, O3)");

    if (set.count >= 1) {
        struct tp_restore_link_group *g = &set.groups[0];
        check(strcmp(g->object_id, "O1") == 0, "group[0] is O1 (first seen)");
        check(g->count == 1, "group[0] has 1 path");
        check(strcmp(g->refs[0].path, "first.txt") == 0, "group[0] target is first.txt");
    }
    if (set.count >= 2) {
        struct tp_restore_link_group *g = &set.groups[1];
        check(strcmp(g->object_id, "O2") == 0, "group[1] is O2");
        check(g->count == 3, "group[1] has 3 paths (hardlink group)");
        check(strcmp(g->refs[0].path, "link_a") == 0,
              "group[1] refs[0] is link_a (first listed = link target)");
        check(strcmp(g->refs[1].path, "sub/link_b") == 0, "group[1] refs[1] is sub/link_b");
        check(strcmp(g->refs[2].path, "link_c") == 0, "group[1] refs[2] is link_c");
    }
    if (set.count >= 3) {
        struct tp_restore_link_group *g = &set.groups[2];
        check(strcmp(g->object_id, "O3") == 0, "group[2] is O3 (after hardlink group)");
        check(g->count == 1, "group[2] has 1 path");
        check(strcmp(g->refs[0].path, "last.txt") == 0, "group[2] target is last.txt");
    }

    tp_restore_link_set_free(&set);

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
