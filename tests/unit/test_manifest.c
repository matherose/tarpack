/*
 * Plain-C test runner for the snapshot manifest writer/reader (src/manifest.h).
 * No framework: checks conditions, counts failures, and returns non-zero
 * exit status if any check fails. Mirrors tests/unit/test_sha256.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "manifest.h"

static int g_fail_count = 0;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail_count++;
        return;
    }
    printf("PASS: %s\n", label);
}

struct collected {
    struct tp_manifest_entry entries[16];
    int count;
};

static int collect_cb(const struct tp_manifest_entry *entry, void *user_data) {
    struct collected *c = (struct collected *)user_data;
    struct tp_manifest_entry copy = *entry;

    /* deep-copy the strings so they survive past the reader's per-line free */
    copy.format = entry->format ? strdup(entry->format) : NULL;
    copy.root = entry->root ? strdup(entry->root) : NULL;
    copy.path = entry->path ? strdup(entry->path) : NULL;
    copy.object_id = entry->object_id ? strdup(entry->object_id) : NULL;
    if (entry->target != NULL) {
        copy.target = malloc(entry->target_len + 1);
        memcpy(copy.target, entry->target, entry->target_len + 1);
    } else {
        copy.target = NULL;
    }

    c->entries[c->count++] = copy;
    return 0;
}

static char g_tmpdir[] = "/tmp/tarpack_test_manifest.XXXXXX";

int main(void) {
    char *tmpdir = mkdtemp(g_tmpdir);
    if (tmpdir == NULL) {
        fprintf(stderr, "FAIL: mkdtemp failed\n");
        return 1;
    }

    char repo[512];
    snprintf(repo, sizeof(repo), "%s/repo", tmpdir);

    /* --- write a snapshot with a file entry (ASCII path), a file entry with
     * non-UTF-8 path bytes, and a symlink entry --- */
    struct tp_snapshot_writer w;
    int rc = tp_snapshot_writer_open(&w, repo, "test-label", "/some/root");
    check(rc == 0, "writer_open succeeds and creates snapshots dir");

    rc = tp_snapshot_write_file(&w, "dir/hello.txt", "O1", 2, 501, 20, 0644, 1234,
                                 1700000000, 111, 10, 100, NULL);
    check(rc == 0, "write_file (ascii path) succeeds");

    const char bad_name[] = "bad\xFFname.txt";
    rc = tp_snapshot_write_file(&w, bad_name, "O2", 1, 501, 20, 0600, 42,
                                 1700000001, 222, 10, 101, "");
    check(rc == 0, "write_file (non-utf8 path) succeeds");

    const char sha_hex[] = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    rc = tp_snapshot_write_file(&w, "hashed.bin", "O3", 1, 501, 20, 0640, 999,
                                 1700000003, 444, 10, 103, sha_hex);
    check(rc == 0, "write_file (with sha256) succeeds");

    const char target[] = "../elsewhere";
    rc = tp_snapshot_write_symlink(&w, "dir/link", target, strlen(target), 501, 20,
                                    1700000002, 333, 10, 102);
    check(rc == 0, "write_symlink succeeds");

    rc = tp_snapshot_writer_close(&w);
    check(rc == 0, "writer_close succeeds");

    char manifest_path[600];
    snprintf(manifest_path, sizeof(manifest_path), "%s/snapshots/test-label.jsonl", repo);
    check(access(manifest_path, F_OK) == 0, "manifest file exists at <repo>/snapshots/<label>.jsonl");

    /* --- read it back line by line --- */
    struct collected c;
    memset(&c, 0, sizeof(c));
    rc = tp_manifest_read_file(manifest_path, collect_cb, &c);
    check(rc == 0, "read_file succeeds");
    check(c.count == 5, "read back header + 4 entries");

    if (c.count >= 1) {
        check(c.entries[0].type == TP_ENTRY_HEADER, "entry 0 is header");
        check(c.entries[0].format != NULL && strcmp(c.entries[0].format, "tarpack-snapshot-v1") == 0,
              "header format field correct");
        check(c.entries[0].root != NULL && strcmp(c.entries[0].root, "/some/root") == 0,
              "header root field correct");
        check(c.entries[0].created > 0, "header created field is populated");
    }

    if (c.count >= 2) {
        struct tp_manifest_entry *e = &c.entries[1];
        check(e->type == TP_ENTRY_FILE, "entry 1 is file");
        check(e->path != NULL && strcmp(e->path, "dir/hello.txt") == 0, "file path round-trips (ascii)");
        check(e->object_id != NULL && strcmp(e->object_id, "O1") == 0, "file object_id round-trips");
        check(e->nlink == 2, "file nlink round-trips");
        check(e->uid == 501, "file uid round-trips");
        check(e->gid == 20, "file gid round-trips");
        check(e->mode_perm == 0644, "file mode round-trips");
        check(e->size == 1234, "file size round-trips");
        check(e->mtime_sec == 1700000000, "file mtime_sec round-trips");
        check(e->mtime_nsec == 111, "file mtime_nsec round-trips");
        check(e->dev == 10, "file dev round-trips");
        check(e->ino == 100, "file ino round-trips");
        check(e->sha256_hex[0] == '\0', "file with NULL sha256 arg has empty sha256_hex on read-back");
    }

    if (c.count >= 3) {
        struct tp_manifest_entry *e = &c.entries[2];
        check(e->type == TP_ENTRY_FILE, "entry 2 is file");
        check(e->path != NULL && e->path_len == strlen(bad_name) && memcmp(e->path, bad_name, e->path_len) == 0,
              "non-utf8 path round-trips to identical raw bytes via path_b64");
        check(e->object_id != NULL && strcmp(e->object_id, "O2") == 0, "second file object_id round-trips");
        check(e->sha256_hex[0] == '\0', "file with \"\" sha256 arg has empty sha256_hex on read-back");
    }

    if (c.count >= 4) {
        struct tp_manifest_entry *e = &c.entries[3];
        check(e->type == TP_ENTRY_FILE, "entry 3 is file");
        check(e->path != NULL && strcmp(e->path, "hashed.bin") == 0, "hashed file path round-trips");
        check(e->object_id != NULL && strcmp(e->object_id, "O3") == 0, "hashed file object_id round-trips");
        check(strcmp(e->sha256_hex, sha_hex) == 0, "file sha256 round-trips when provided");
    }

    if (c.count >= 5) {
        struct tp_manifest_entry *e = &c.entries[4];
        check(e->type == TP_ENTRY_SYMLINK, "entry 4 is symlink");
        check(e->path != NULL && strcmp(e->path, "dir/link") == 0, "symlink path round-trips");
        check(e->target != NULL && e->target_len == strlen(target) && memcmp(e->target, target, e->target_len) == 0,
              "symlink target round-trips");
        check(e->uid == 501, "symlink uid round-trips");
        check(e->gid == 20, "symlink gid round-trips");
        check(e->mtime_sec == 1700000002, "symlink mtime_sec round-trips");
    }

    /* verify the raw file itself: header line must be valid JSON with the literal
     * "path_b64" key present for the non-UTF-8 entry (sanity check on wire format) */
    {
        FILE *f = fopen(manifest_path, "r");
        check(f != NULL, "can reopen manifest file directly");
        if (f != NULL) {
            char buf[4096];
            int found_path_b64 = 0;
            int line_no = 0;
            while (fgets(buf, sizeof(buf), f) != NULL) {
                line_no++;
                if (line_no == 3 && strstr(buf, "\"path_b64\"") != NULL) {
                    found_path_b64 = 1;
                }
            }
            check(found_path_b64, "non-utf8 entry line uses path_b64 key on the wire");
            fclose(f);
        }
    }

    /* cleanup deep copies */
    for (int i = 0; i < c.count; i++) {
        free(c.entries[i].format);
        free(c.entries[i].root);
        free(c.entries[i].path);
        free(c.entries[i].object_id);
        free(c.entries[i].target);
    }

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }

    printf("\nall checks passed\n");
    return 0;
}
