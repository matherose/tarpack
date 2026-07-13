/*
 * Unit test for the pack manifest writer/reader round-trip
 * (tp_pack_manifest_write / tp_pack_manifest_read). Verifies that
 * object_id/path/offset/size/sha256 survive a write->parse cycle, and that a
 * non-UTF-8 representative path uses the "path_b64" encoding on the wire and
 * still round-trips to identical raw bytes.
 *
 * No framework; mirrors tests/unit/test_manifest.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static char g_tmpdir[] = "/tmp/tarpack_test_pack_manifest.XXXXXX";

/* build a tp_packed_object with a given path/len (raw bytes) */
static void set_obj(struct tp_packed_object *o, const char *id, const char *path, size_t path_len,
                    int64_t offset, off_t size, const char *sha) {
    memset(o, 0, sizeof(*o));
    o->object_id = strdup(id);
    o->path = malloc(path_len + 1);
    memcpy(o->path, path, path_len);
    o->path[path_len] = '\0';
    o->path_len = path_len;
    o->offset = offset;
    o->size = size;
    snprintf(o->sha256_hex, sizeof(o->sha256_hex), "%s", sha);
}

int main(void) {
    char *tmpdir = mkdtemp(g_tmpdir);
    if (tmpdir == NULL) {
        fprintf(stderr, "FAIL: mkdtemp failed\n");
        return 1;
    }

    char manifest_path[600];
    snprintf(manifest_path, sizeof(manifest_path), "%s/pack-000001.json", tmpdir);

    const char sha_a[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char sha_b[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char bad_name[] = "bad\xFFname.txt";

    struct tp_pack_object_set set;
    memset(&set, 0, sizeof(set));
    set.objects = calloc(2, sizeof(*set.objects));
    set.count = 2;
    set.capacity = 2;
    set_obj(&set.objects[0], "O1", "dir/hello.txt", strlen("dir/hello.txt"), 512, 100, sha_a);
    set_obj(&set.objects[1], "O2", bad_name, strlen(bad_name), 2048, 0, sha_b);

    int rc = tp_pack_manifest_write(manifest_path, "pack-000001", 1700000000, &set);
    check(rc == 0, "tp_pack_manifest_write succeeds");
    check(access(manifest_path, F_OK) == 0, "pack manifest file exists");

    /* wire-format sanity: non-utf8 entry uses path_b64 key */
    {
        FILE *f = fopen(manifest_path, "r");
        check(f != NULL, "reopen manifest");
        if (f != NULL) {
            char buf[8192];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            fclose(f);
            check(strstr(buf, "\"path_b64\"") != NULL, "non-utf8 entry uses path_b64 on the wire");
            check(strstr(buf, "\"tarpack-pack-v1\"") != NULL, "format field present on the wire");
        }
    }

    /* --- read back --- */
    char *format = NULL;
    char *pack = NULL;
    int64_t created = 0;
    struct tp_pack_manifest_entry *entries = NULL;
    size_t count = 0;
    rc = tp_pack_manifest_read(manifest_path, &format, &pack, &created, &entries, &count);
    check(rc == 0, "tp_pack_manifest_read succeeds");
    check(format != NULL && strcmp(format, "tarpack-pack-v1") == 0, "format round-trips");
    check(pack != NULL && strcmp(pack, "pack-000001") == 0, "pack name round-trips");
    check(created == 1700000000, "created round-trips");
    check(count == 2, "two entries round-trip");

    if (count >= 1) {
        struct tp_pack_manifest_entry *e = &entries[0];
        check(strcmp(e->object_id, "O1") == 0, "entry[0] object_id");
        check(strcmp(e->path, "dir/hello.txt") == 0, "entry[0] path (ascii)");
        check(e->offset == 512, "entry[0] offset");
        check(e->size == 100, "entry[0] size");
        check(strcmp(e->sha256_hex, sha_a) == 0, "entry[0] sha256");
    }
    if (count >= 2) {
        struct tp_pack_manifest_entry *e = &entries[1];
        check(strcmp(e->object_id, "O2") == 0, "entry[1] object_id");
        check(e->path_len == strlen(bad_name) && memcmp(e->path, bad_name, e->path_len) == 0,
              "entry[1] non-utf8 path round-trips to identical raw bytes via path_b64");
        check(e->offset == 2048, "entry[1] offset");
        check(e->size == 0, "entry[1] size is 0");
        check(strcmp(e->sha256_hex, sha_b) == 0, "entry[1] sha256");
    }

    free(format);
    free(pack);
    tp_pack_manifest_entries_free(entries, count);
    tp_pack_object_set_free(&set);

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
