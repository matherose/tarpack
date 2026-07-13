/*
 * Plain-C test runner for the cross-run object index (src/index.h).
 * No framework: checks conditions, counts failures, and returns non-zero
 * exit status if any check fails. Mirrors tests/unit/test_manifest.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "index.h"

static int g_fail_count = 0;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail_count++;
        return;
    }
    printf("PASS: %s\n", label);
}

static char g_tmpdir[] = "/tmp/tarpack_test_index.XXXXXX";

static void set_entry(struct tp_index_entry *e, const char *id, const char *pack, int64_t offset,
                       off_t size, const char *sha, const char *path, int64_t mtime_sec,
                       long mtime_nsec) {
    memset(e, 0, sizeof(*e));
    e->object_id = strdup(id);
    e->pack = strdup(pack);
    e->offset = offset;
    e->size = size;
    snprintf(e->sha256_hex, sizeof(e->sha256_hex), "%s", sha);
    e->path = strdup(path);
    e->path_len = strlen(path);
    e->mtime_sec = mtime_sec;
    e->mtime_nsec = mtime_nsec;
}

static void free_entry(struct tp_index_entry *e) {
    free(e->object_id);
    free(e->pack);
    free(e->path);
}

int main(void) {
    char *tmpdir = mkdtemp(g_tmpdir);
    if (tmpdir == NULL) {
        fprintf(stderr, "FAIL: mkdtemp failed\n");
        return 1;
    }

    char repo[512];
    snprintf(repo, sizeof(repo), "%s/repo", tmpdir);

    /* --- missing objects.jsonl -> empty index, next id 1 --- */
    {
        struct tp_index idx;
        int rc = tp_index_load(repo, &idx);
        check(rc == 0, "load with no objects.jsonl succeeds");
        check(idx.next_id == 1, "next_id is 1 when index is empty/missing");
        check(tp_index_find_by_path(&idx, "a.txt", strlen("a.txt"), 10, 100, 0) == NULL,
              "lookup by path misses on empty index");
        check(tp_index_find_by_sha256(&idx, "deadbeef") == NULL,
              "lookup by sha256 misses on empty index");
        check(!tp_index_object_id_present(&idx, "O1"), "object_id_present misses on empty index");
        tp_index_free(&idx);
    }

    /* --- append two entries: O2 (with sha256) and O7 (no sha256) --- */
    {
        struct tp_index_entry entries[2];
        set_entry(&entries[0], "O2", "pack-000001", 0, 100,
                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "a.txt",
                   1700000000, 111);
        set_entry(&entries[1], "O7", "pack-000001", 512, 200, "", "sub/b.txt", 1700000100, 222);

        int rc = tp_index_append(repo, entries, 2);
        check(rc == 0, "append succeeds and creates objects/objects.jsonl");

        char index_path[600];
        snprintf(index_path, sizeof(index_path), "%s/objects/objects.jsonl", repo);
        check(access(index_path, F_OK) == 0, "objects.jsonl exists after append");

        free_entry(&entries[0]);
        free_entry(&entries[1]);
    }

    /* --- reload: max-id (O2, O7) -> next id 8; lookups hit/miss correctly --- */
    {
        struct tp_index idx;
        int rc = tp_index_load(repo, &idx);
        check(rc == 0, "load after append succeeds");
        check(idx.next_id == 8, "next_id is (max numeric suffix)+1 == 8 given O2,O7");

        const struct tp_index_entry *hit =
            tp_index_find_by_path(&idx, "a.txt", strlen("a.txt"), 100, 1700000000, 111);
        check(hit != NULL, "lookup by (path,size,mtime) hits for a.txt");
        check(hit != NULL && strcmp(hit->object_id, "O2") == 0, "hit resolves to object_id O2");

        check(tp_index_find_by_path(&idx, "a.txt", strlen("a.txt"), 999, 1700000000, 111) == NULL,
              "lookup by path misses when size differs");
        check(tp_index_find_by_path(&idx, "a.txt", strlen("a.txt"), 100, 1700000000, 999) == NULL,
              "lookup by path misses when mtime_nsec differs");
        check(tp_index_find_by_path(&idx, "nope.txt", strlen("nope.txt"), 100, 1700000000, 111) ==
                  NULL,
              "lookup by path misses for unknown path");

        const struct tp_index_entry *sha_hit = tp_index_find_by_sha256(
            &idx, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        check(sha_hit != NULL, "lookup by sha256 hits for the entry that has one");
        check(sha_hit != NULL && strcmp(sha_hit->object_id, "O2") == 0,
              "sha256 hit resolves to object_id O2");

        check(tp_index_find_by_sha256(&idx, "") == NULL,
              "lookup by empty sha256 never matches (O7's blank sha256 is not indexed)");
        check(tp_index_find_by_sha256(
                  &idx, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") ==
                  NULL,
              "lookup by sha256 misses for unknown digest");

        const struct tp_index_entry *o7 =
            tp_index_find_by_path(&idx, "sub/b.txt", strlen("sub/b.txt"), 200, 1700000100, 222);
        check(o7 != NULL && strcmp(o7->object_id, "O7") == 0, "O7 entry (no sha256) found by path");
        check(o7 != NULL && strcmp(o7->pack, "pack-000001") == 0, "O7 pack field round-trips");
        check(o7 != NULL && o7->offset == 512, "O7 offset field round-trips");

        check(tp_index_object_id_present(&idx, "O2"), "object_id_present hits for O2");
        check(tp_index_object_id_present(&idx, "O7"), "object_id_present hits for O7");
        check(!tp_index_object_id_present(&idx, "O99"), "object_id_present misses for unknown id");

        tp_index_free(&idx);
    }

    /* --- round-trip of a non-UTF-8 representative path via path_b64 --- */
    {
        char repo2[512];
        snprintf(repo2, sizeof(repo2), "%s/repo2", tmpdir);

        const char bad_path[] = "weird\xFFname.bin";
        struct tp_index_entry e;
        set_entry(&e, "O1", "pack-000001", 0, 5, "", bad_path, 1700000200, 333);
        /* set_entry used strdup which stops at NUL; rebuild path with raw bytes explicitly */
        free(e.path);
        e.path = malloc(sizeof(bad_path) - 1);
        memcpy(e.path, bad_path, sizeof(bad_path) - 1);
        e.path_len = sizeof(bad_path) - 1;

        int rc = tp_index_append(repo2, &e, 1);
        check(rc == 0, "append with non-utf8 path succeeds");
        free_entry(&e);

        struct tp_index idx2;
        rc = tp_index_load(repo2, &idx2);
        check(rc == 0, "load with non-utf8 path entry succeeds");

        const struct tp_index_entry *hit = tp_index_find_by_path(
            &idx2, bad_path, sizeof(bad_path) - 1, 5, 1700000200, 333);
        check(hit != NULL, "non-utf8 path round-trips and is found by lookup");
        check(hit != NULL && hit->path_len == sizeof(bad_path) - 1 &&
                  memcmp(hit->path, bad_path, hit->path_len) == 0,
              "non-utf8 path bytes are identical after round-trip");

        /* sanity: wire format used path_b64 for this entry */
        char index_path2[600];
        snprintf(index_path2, sizeof(index_path2), "%s/objects/objects.jsonl", repo2);
        FILE *f = fopen(index_path2, "r");
        check(f != NULL, "reopen repo2 objects.jsonl");
        if (f != NULL) {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            fclose(f);
            check(strstr(buf, "\"path_b64\"") != NULL, "non-utf8 path uses path_b64 on the wire");
        }

        tp_index_free(&idx2);
    }

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }

    printf("\nall checks passed\n");
    return 0;
}
