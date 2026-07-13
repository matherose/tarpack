/*
 * Plain-C test runner for tp_utf8_valid() and tp_base64_encode().
 * No framework: checks conditions, counts failures, and returns
 * non-zero exit status if any check fails. Mirrors tests/unit/test_sha256.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static int g_fail_count = 0;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail_count++;
        return;
    }
    printf("PASS: %s\n", label);
}

static void check_utf8_valid(const char *label, const uint8_t *s, size_t n, int expected) {
    int actual = tp_utf8_valid(s, n);
    if ((actual != 0) != (expected != 0)) {
        fprintf(stderr, "FAIL: %s (expected %d, got %d)\n", label, expected, actual);
        g_fail_count++;
        return;
    }
    printf("PASS: %s\n", label);
}

static void check_base64(const char *label, const uint8_t *in, size_t n, const char *expected) {
    char *actual = tp_base64_encode(in, n);
    if (actual == NULL) {
        fprintf(stderr, "FAIL: %s (tp_base64_encode returned NULL)\n", label);
        g_fail_count++;
        return;
    }
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n", label, expected, actual);
        g_fail_count++;
        free(actual);
        return;
    }
    printf("PASS: %s\n", label);
    free(actual);
}

int main(void) {
    /* --- tp_utf8_valid --- */
    check_utf8_valid("ascii", (const uint8_t *)"hello world", 11, 1);
    check_utf8_valid("empty string is valid", (const uint8_t *)"", 0, 1);

    /* 2-byte, 3-byte, 4-byte valid sequences: e.g. "café" (c3 a9), euro sign (e2 82 ac), and an emoji (f0 9f 98 80) */
    {
        const uint8_t multibyte[] = {'c', 'a', 'f', 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80};
        check_utf8_valid("valid multibyte (2/3/4-byte sequences)", multibyte, sizeof(multibyte), 1);
    }

    /* invalid continuation byte: 0xC3 followed by an ASCII byte instead of a continuation byte */
    {
        const uint8_t bad_cont[] = {0xC3, 0x28};
        check_utf8_valid("invalid continuation byte", bad_cont, sizeof(bad_cont), 0);
    }

    /* overlong encoding: 0xC0 0x80 is an overlong encoding of NUL */
    {
        const uint8_t overlong[] = {0xC0, 0x80};
        check_utf8_valid("overlong encoding (C0 80)", overlong, sizeof(overlong), 0);
    }

    /* another overlong: 0xE0 0x80 0x80 */
    {
        const uint8_t overlong3[] = {0xE0, 0x80, 0x80};
        check_utf8_valid("overlong 3-byte encoding (E0 80 80)", overlong3, sizeof(overlong3), 0);
    }

    /* surrogate: U+D800 encoded as ED A0 80 (surrogates are invalid in UTF-8) */
    {
        const uint8_t surrogate[] = {0xED, 0xA0, 0x80};
        check_utf8_valid("surrogate (ED A0 80)", surrogate, sizeof(surrogate), 0);
    }

    /* > U+10FFFF: F4 90 80 80 encodes U+110000, which is out of range */
    {
        const uint8_t too_big[] = {0xF4, 0x90, 0x80, 0x80};
        check_utf8_valid("beyond U+10FFFF (F4 90 80 80)", too_big, sizeof(too_big), 0);
    }

    /* stray 0xFF byte, never valid in UTF-8 */
    {
        const uint8_t stray_ff[] = {'b', 'a', 'd', 0xFF, 'n', 'a', 'm', 'e'};
        check_utf8_valid("stray 0xFF byte", stray_ff, sizeof(stray_ff), 0);
    }

    /* --- tp_base64_encode: RFC 4648 test vectors --- */
    check_base64("empty", (const uint8_t *)"", 0, "");
    check_base64("\"f\"", (const uint8_t *)"f", 1, "Zg==");
    check_base64("\"fo\"", (const uint8_t *)"fo", 2, "Zm8=");
    check_base64("\"foo\"", (const uint8_t *)"foo", 3, "Zm9v");
    check_base64("\"foob\"", (const uint8_t *)"foob", 4, "Zm9vYg==");
    check_base64("\"fooba\"", (const uint8_t *)"fooba", 5, "Zm9vYmE=");
    check_base64("\"foobar\"", (const uint8_t *)"foobar", 6, "Zm9vYmFy");

    /* sanity: base64 encode of raw non-UTF8 bytes should round trip conceptually
     * (decoding is tested indirectly via manifest tests); here just confirm we
     * get a non-NULL, deterministic string for arbitrary bytes. */
    {
        const uint8_t raw[] = {0x00, 0xFF, 0x10, 0x7F};
        char *enc1 = tp_base64_encode(raw, sizeof(raw));
        char *enc2 = tp_base64_encode(raw, sizeof(raw));
        check(enc1 != NULL && enc2 != NULL && strcmp(enc1, enc2) == 0, "base64 encode is deterministic for raw bytes");
        free(enc1);
        free(enc2);
    }

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }

    printf("\nall checks passed\n");
    return 0;
}
