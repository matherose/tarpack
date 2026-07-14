/*
 * Plain-C test runner for tp_sample_is_incompressible() (content-aware
 * packing, v1.2). No framework: checks conditions, counts failures, and
 * returns non-zero exit status if any check fails. Mirrors test_util.c.
 */
#include <fcntl.h>
#include <stdint.h>
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

/* xorshift64: deterministic high-entropy byte stream, so the "random" file
 * is identical on every run and platform (no /dev/urandom dependency). */
static uint64_t g_xs = 0x9E3779B97F4A7C15ull;

static unsigned char prng_byte(void) {
    g_xs ^= g_xs << 13;
    g_xs ^= g_xs >> 7;
    g_xs ^= g_xs << 17;
    return (unsigned char)(g_xs & 0xFF);
}

enum fill_mode { FILL_ZEROS, FILL_TEXT, FILL_RANDOM };

static int write_pattern(int dirfd, const char *name, size_t size, enum fill_mode mode) {
    unsigned char *buf = calloc(size ? size : 1, 1);
    if (buf == NULL) {
        return -1;
    }
    static const char text[] = "the quick brown fox jumps over the lazy dog. ";
    for (size_t i = 0; i < size; i++) {
        if (mode == FILL_TEXT) {
            buf[i] = (unsigned char)text[i % (sizeof(text) - 1)];
        } else if (mode == FILL_RANDOM) {
            buf[i] = prng_byte();
        }
    }
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(buf);
        return -1;
    }
    ssize_t w = write(fd, buf, size);
    free(buf);
    if (w < 0 || (size_t)w != size) {
        close(fd);
        return -1;
    }
    return close(fd);
}

int main(void) {
    char tmpl[] = "/tmp/tarpack_entropy_test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (dir == NULL) {
        fprintf(stderr, "FAIL: mkdtemp\n");
        return 1;
    }
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        fprintf(stderr, "FAIL: open tmpdir\n");
        return 1;
    }

    const size_t sample = TP_PACK_ENTROPY_SAMPLE;

    check(write_pattern(dirfd, "zeros.bin", sample, FILL_ZEROS) == 0, "fixture: zeros.bin");
    check(write_pattern(dirfd, "text.txt", sample, FILL_TEXT) == 0, "fixture: text.txt");
    check(write_pattern(dirfd, "random.bin", 2 * sample, FILL_RANDOM) == 0,
          "fixture: random.bin");
    check(write_pattern(dirfd, "small.bin", 4096, FILL_RANDOM) == 0, "fixture: small.bin");

    double bits = -1.0;
    check(tp_sample_is_incompressible(dirfd, "zeros.bin", (int64_t)sample, &bits) == 0,
          "zeros classify as compressible");
    check(bits >= 0.0 && bits < 0.01, "zeros measure ~0 bits/byte");

    bits = -1.0;
    check(tp_sample_is_incompressible(dirfd, "text.txt", (int64_t)sample, &bits) == 0,
          "text classifies as compressible");
    check(bits > 1.0 && bits < 6.0, "text measures mid-range entropy");

    bits = -1.0;
    check(tp_sample_is_incompressible(dirfd, "random.bin", (int64_t)(2 * sample), &bits) == 1,
          "random bytes classify as incompressible");
    check(bits >= 7.9, "random bytes measure ~8 bits/byte");

    /* below the minimum size the sample is not meaningful: always normal */
    check(tp_sample_is_incompressible(dirfd, "small.bin", 4096, &bits) == 0,
          "small random file stays normal-class");
    check(bits == 0.0, "small file reports 0.0 bits (no full sample)");

    /* unreadable path must classify as compressible, never error out */
    check(tp_sample_is_incompressible(dirfd, "missing.bin", (int64_t)(2 * sample), &bits) == 0,
          "missing file stays normal-class");

    /* bits_out is optional */
    check(tp_sample_is_incompressible(dirfd, "random.bin", (int64_t)(2 * sample), NULL) == 1,
          "NULL bits_out accepted");

    unlinkat(dirfd, "zeros.bin", 0);
    unlinkat(dirfd, "text.txt", 0);
    unlinkat(dirfd, "random.bin", 0);
    unlinkat(dirfd, "small.bin", 0);
    close(dirfd);
    rmdir(dir);

    if (g_fail_count > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_fail_count);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
