/*
 * Unit tests for src/checksum.h: tp_sha256_file, tp_checksums_append,
 * tp_checksums_read. No framework; mirrors the other unit test files in
 * this repo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "checksum.h"

static int g_fail_count = 0;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail_count++;
        return;
    }
    printf("PASS: %s\n", label);
}

static char g_tmpdir[] = "/tmp/tarpack_test_checksum.XXXXXX";

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    fputs(content, f);
    fclose(f);
}

int main(void) {
    char *tmpdir = mkdtemp(g_tmpdir);
    if (tmpdir == NULL) {
        fprintf(stderr, "FAIL: mkdtemp failed\n");
        return 1;
    }

    char repo[512];
    snprintf(repo, sizeof(repo), "%s/repo", tmpdir);
    mkdir(repo, 0777);

    /* --- tp_sha256_file: known vector for "abc" --- */
    char filepath[600];
    snprintf(filepath, sizeof(filepath), "%s/abc.txt", repo);
    write_file(filepath, "abc");

    char hex[65];
    int rc = tp_sha256_file(filepath, hex);
    check(rc == 0, "tp_sha256_file succeeds");
    check(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "tp_sha256_file computes the correct sha256 for 'abc'");

    check(tp_sha256_file("/nonexistent/path/xyz", hex) != 0,
          "tp_sha256_file fails cleanly for a missing file");

    /* --- tp_checksums_append / tp_checksums_read round trip --- */
    const char sha_pack[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char sha_json[] = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

    rc = tp_checksums_append(repo, sha_pack, "packs/pack-000001.tar.zst");
    check(rc == 0, "tp_checksums_append (pack) succeeds");
    rc = tp_checksums_append(repo, sha_json, "packs/pack-000001.json");
    check(rc == 0, "tp_checksums_append (json) succeeds");

    char checksums_path[700];
    snprintf(checksums_path, sizeof(checksums_path), "%s/checksums/SHA256SUMS", repo);
    check(access(checksums_path, F_OK) == 0, "checksums/SHA256SUMS exists after append");

    struct tp_checksums_line_entry *entries = NULL;
    size_t count = 0;
    rc = tp_checksums_read(repo, &entries, &count);
    check(rc == 0, "tp_checksums_read succeeds");
    check(count == 2, "two lines read back");

    if (count >= 1) {
        check(strcmp(entries[0].sha256_hex, sha_pack) == 0, "entry[0] sha256 round-trips");
        check(strcmp(entries[0].relpath, "packs/pack-000001.tar.zst") == 0,
              "entry[0] relpath round-trips");
    }
    if (count >= 2) {
        check(strcmp(entries[1].sha256_hex, sha_json) == 0, "entry[1] sha256 round-trips");
        check(strcmp(entries[1].relpath, "packs/pack-000001.json") == 0,
              "entry[1] relpath round-trips");
    }

    /* wire format sanity: two-space separator, like shasum output */
    {
        FILE *f = fopen(checksums_path, "r");
        check(f != NULL, "reopen SHA256SUMS");
        if (f != NULL) {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            fclose(f);
            char needle[128];
            snprintf(needle, sizeof(needle), "%s  packs/pack-000001.tar.zst", sha_pack);
            check(strstr(buf, needle) != NULL, "line uses two-space hex/path separator");
        }
    }

    tp_checksums_entries_free(entries, count);

    /* --- reading a missing SHA256SUMS is not an error --- */
    char repo2[560];
    snprintf(repo2, sizeof(repo2), "%s/repo_missing", tmpdir);
    mkdir(repo2, 0777);
    struct tp_checksums_line_entry *entries2 = NULL;
    size_t count2 = 999;
    rc = tp_checksums_read(repo2, &entries2, &count2);
    check(rc == 0, "tp_checksums_read on missing file returns success");
    check(count2 == 0, "tp_checksums_read on missing file yields zero entries");
    tp_checksums_entries_free(entries2, count2);

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
