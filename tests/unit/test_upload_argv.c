/*
 * Unit test for tp_upload_build_argv_set: verifies the argv arrays built
 * for a sample repo/remote are correct, in order (packs, checksums,
 * snapshots, objects), execvp-ready ({"rclone","copy",src,dst,NULL}), and
 * that the set frees cleanly. No process is spawned by this test -- that
 * is exercised only via the (untested-here, by spec) integration path.
 *
 * No framework; mirrors the other unit test files in this repo.
 */
#include <stdio.h>
#include <string.h>

#include "upload.h"

static int g_fail_count = 0;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail_count++;
        return;
    }
    printf("PASS: %s\n", label);
}

static void check_step(char **argv, const char *expected_src, const char *expected_dst,
                        const char *label_prefix) {
    char label[256];

    snprintf(label, sizeof(label), "%s: argv[0] is rclone", label_prefix);
    check(argv != NULL && argv[0] != NULL && strcmp(argv[0], "rclone") == 0, label);

    snprintf(label, sizeof(label), "%s: argv[1] is copy", label_prefix);
    check(argv != NULL && argv[1] != NULL && strcmp(argv[1], "copy") == 0, label);

    snprintf(label, sizeof(label), "%s: argv[2] is source path", label_prefix);
    check(argv != NULL && argv[2] != NULL && strcmp(argv[2], expected_src) == 0, label);

    snprintf(label, sizeof(label), "%s: argv[3] is destination path", label_prefix);
    check(argv != NULL && argv[3] != NULL && strcmp(argv[3], expected_dst) == 0, label);

    snprintf(label, sizeof(label), "%s: argv[4] is NULL-terminated", label_prefix);
    check(argv != NULL && argv[4] == NULL, label);
}

int main(void) {
    struct tp_upload_argv_set set;
    memset(&set, 0, sizeof(set));

    int rc = tp_upload_build_argv_set("/data/myrepo", "myremote:backups", &set);
    check(rc == 0, "tp_upload_build_argv_set succeeds");

    check_step(set.argv[0], "/data/myrepo/packs", "myremote:backups/packs", "step0 (packs)");
    check_step(set.argv[1], "/data/myrepo/checksums", "myremote:backups/checksums",
               "step1 (checksums)");
    check_step(set.argv[2], "/data/myrepo/snapshots", "myremote:backups/snapshots",
               "step2 (snapshots)");
    check_step(set.argv[3], "/data/myrepo/objects", "myremote:backups/objects",
               "step3 (objects)");

    tp_upload_argv_set_free(&set);

    /* invalid input rejected */
    struct tp_upload_argv_set bad;
    memset(&bad, 0, sizeof(bad));
    check(tp_upload_build_argv_set(NULL, "remote:x", &bad) != 0, "NULL repo rejected");
    check(tp_upload_build_argv_set("/data/myrepo", NULL, &bad) != 0, "NULL remote rejected");
    check(tp_upload_build_argv_set("", "remote:x", &bad) != 0, "empty repo rejected");
    check(tp_upload_build_argv_set("/data/myrepo", "", &bad) != 0, "empty remote rejected");

    /* trailing slash on remote should not produce a double slash */
    struct tp_upload_argv_set slash;
    memset(&slash, 0, sizeof(slash));
    rc = tp_upload_build_argv_set("/data/myrepo", "myremote:backups/", &slash);
    check(rc == 0, "trailing-slash remote accepted");
    check(strcmp(slash.argv[0][3], "myremote:backups/packs") == 0,
          "trailing slash on remote does not produce a double slash");
    tp_upload_argv_set_free(&slash);

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
