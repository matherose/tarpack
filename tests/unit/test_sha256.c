/*
 * Plain-C test runner for the vendored SHA-256 implementation.
 * No framework: checks against NIST test vectors, counts failures,
 * and returns non-zero exit status if any check fails.
 */
#include <stdio.h>
#include <string.h>

#include "sha256.h"

static void to_hex(const BYTE hash[SHA256_BLOCK_SIZE], char out[SHA256_BLOCK_SIZE * 2 + 1]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        out[i * 2] = digits[(hash[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[hash[i] & 0xF];
    }
    out[SHA256_BLOCK_SIZE * 2] = '\0';
}

static int check_vector(int *fail_count, const char *label, const char *input, const char *expected_hex) {
    SHA256_CTX ctx;
    BYTE hash[SHA256_BLOCK_SIZE];
    char actual_hex[SHA256_BLOCK_SIZE * 2 + 1];

    sha256_init(&ctx);
    sha256_update(&ctx, (const BYTE *)input, strlen(input));
    sha256_final(&ctx, hash);
    to_hex(hash, actual_hex);

    if (strcmp(actual_hex, expected_hex) != 0) {
        fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n", label, expected_hex, actual_hex);
        (*fail_count)++;
        return 0;
    }

    printf("PASS: %s\n", label);
    return 1;
}

int main(void) {
    int fail_count = 0;

    check_vector(&fail_count, "empty string", "",
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    check_vector(&fail_count, "\"abc\"", "abc",
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    check_vector(&fail_count, "56-char message",
                 "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    if (fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", fail_count);
        return 1;
    }

    printf("\nall checks passed\n");
    return 0;
}
