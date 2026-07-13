/*
 * Unit tests for the pure bin-assignment functions (tp_bin_assign_next_fit,
 * tp_bin_assign_ffd), tp_estimate_entry_cost, and the --target-size string
 * parser (tp_parse_size). No I/O; no framework, mirrors the other unit
 * test files in this repo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* tp_parse_size                                                       */
/* ------------------------------------------------------------------ */

static void test_parse_size(void) {
    int64_t v;

    check(tp_parse_size("1024", &v) == 0 && v == 1024, "parse_size: plain bytes");
    check(tp_parse_size("0", &v) == 0 && v == 0, "parse_size: zero");
    check(tp_parse_size("100M", &v) == 0 && v == (int64_t)100 * 1024 * 1024,
          "parse_size: 100M");
    check(tp_parse_size("50G", &v) == 0 && v == (int64_t)50 * 1024 * 1024 * 1024,
          "parse_size: 50G");
    check(tp_parse_size("512K", &v) == 0 && v == (int64_t)512 * 1024, "parse_size: 512K");
    check(tp_parse_size("100m", &v) == 0 && v == (int64_t)100 * 1024 * 1024,
          "parse_size: lowercase suffix accepted");

    check(tp_parse_size("", &v) != 0, "parse_size: empty string rejected");
    check(tp_parse_size("abc", &v) != 0, "parse_size: non-numeric rejected");
    check(tp_parse_size("100X", &v) != 0, "parse_size: bad suffix rejected");
    check(tp_parse_size("100M5", &v) != 0, "parse_size: trailing garbage rejected");
    check(tp_parse_size("-5", &v) != 0, "parse_size: negative rejected");
    check(tp_parse_size("99999999999999999999999", &v) != 0, "parse_size: overflow rejected");
    check(tp_parse_size(NULL, &v) != 0, "parse_size: NULL rejected");
}

/* ------------------------------------------------------------------ */
/* tp_estimate_entry_cost                                              */
/* ------------------------------------------------------------------ */

static void test_estimate_entry_cost(void) {
    /* header(512) + ceil(size/512)*512 + 1536 */
    check(tp_estimate_entry_cost(0) == 512 + 0 + 1536, "estimate: zero-size file");
    check(tp_estimate_entry_cost(1) == 512 + 512 + 1536, "estimate: 1-byte file rounds up to 512");
    check(tp_estimate_entry_cost(512) == 512 + 512 + 1536, "estimate: exact 512 boundary");
    check(tp_estimate_entry_cost(513) == 512 + 1024 + 1536, "estimate: 513 rounds up to 1024");
    check(tp_estimate_entry_cost(1024) == 512 + 1024 + 1536, "estimate: exact 1024 boundary");
}

/* ------------------------------------------------------------------ */
/* tp_bin_assign_next_fit                                              */
/* ------------------------------------------------------------------ */

static void test_next_fit_empty(void) {
    long n = tp_bin_assign_next_fit(NULL, 0, 100, NULL);
    check(n == 0, "next_fit: empty input -> zero bins");
}

static void test_next_fit_single_object(void) {
    struct tp_bin_item items[1] = {{0, 50}};
    size_t bins[1];
    long n = tp_bin_assign_next_fit(items, 1, 100, bins);
    check(n == 1, "next_fit: single object -> one bin");
    check(bins[0] == 0, "next_fit: single object assigned to bin 0");
}

static void test_next_fit_keeps_input_order_and_respects_target(void) {
    /* target 100; costs 40,40,40,40 -> bin0:[40,40] (80<=100, +40=120>100 close)
     * bin1:[40,40] */
    struct tp_bin_item items[4] = {{0, 40}, {1, 40}, {2, 40}, {3, 40}};
    size_t bins[4];
    long n = tp_bin_assign_next_fit(items, 4, 100, bins);
    check(n == 2, "next_fit: 4x40 with target 100 -> 2 bins");
    check(bins[0] == 0 && bins[1] == 0, "next_fit: first two items in bin 0");
    check(bins[2] == 1 && bins[3] == 1, "next_fit: next two items in bin 1 (input order preserved)");
}

static void test_next_fit_oversized_object_alone(void) {
    /* target 100; item[1] costs 150 (> target) -> must be alone in its own
     * bin, not merged with neighbors even though they'd otherwise fit. */
    struct tp_bin_item items[3] = {{0, 30}, {1, 150}, {2, 30}};
    size_t bins[3];
    long n = tp_bin_assign_next_fit(items, 3, 100, bins);
    check(n == 3, "next_fit: oversized item forces 3 bins total");
    check(bins[0] == 0, "next_fit: item0 in bin 0");
    check(bins[1] == 1, "next_fit: oversized item1 isolated in its own bin");
    check(bins[2] == 2, "next_fit: item2 starts a fresh bin after the oversized one");
}

static void test_next_fit_exact_fit_stays_in_same_bin(void) {
    struct tp_bin_item items[2] = {{0, 60}, {1, 40}};
    size_t bins[2];
    long n = tp_bin_assign_next_fit(items, 2, 100, bins);
    check(n == 1, "next_fit: items summing exactly to target share one bin");
    check(bins[0] == 0 && bins[1] == 0, "next_fit: both items in bin 0");
}

/* ------------------------------------------------------------------ */
/* tp_bin_assign_ffd                                                    */
/* ------------------------------------------------------------------ */

static void test_ffd_empty(void) {
    long n = tp_bin_assign_ffd(NULL, 0, 100, NULL);
    check(n == 0, "ffd: empty input -> zero bins");
}

static void test_ffd_single_object(void) {
    struct tp_bin_item items[1] = {{0, 50}};
    size_t bins[1];
    long n = tp_bin_assign_ffd(items, 1, 100, bins);
    check(n == 1, "ffd: single object -> one bin");
    check(bins[0] == 0, "ffd: single object assigned to bin 0");
}

static void test_ffd_places_largest_first_and_fills_earlier_bins(void) {
    /* target 100. Items (input order): id0=20, id1=90, id2=30, id3=50.
     * Descending by cost: id1(90), id3(50), id2(30), id0(20).
     * id1(90) -> bin0 (rem 10)
     * id3(50) -> doesn't fit bin0(10) -> bin1 (rem 50)
     * id2(30) -> doesn't fit bin0(10); fits bin1(50->20) -> bin1
     * id0(20) -> fits bin0(10)? no (20>10); fits bin1(20)? yes -> bin1
     * Expect: id1->bin0; id3,id2,id0->bin1. Total bins = 2. */
    struct tp_bin_item items[4] = {{0, 20}, {1, 90}, {2, 30}, {3, 50}};
    size_t bins[4];
    long n = tp_bin_assign_ffd(items, 4, 100, bins);
    check(n == 2, "ffd: fills earlier bins when room remains -> 2 bins total");
    check(bins[1] == 0, "ffd: largest item (id1=90) opens bin 0");
    check(bins[3] == 1, "ffd: id3=50 opens bin 1 (doesn't fit bin0's remaining 10)");
    check(bins[2] == 1, "ffd: id2=30 fits into bin1's remaining room");
    check(bins[0] == 1, "ffd: id0=20 fits into bin1's remaining room ahead of a new bin");
}

static void test_ffd_oversized_object_alone(void) {
    struct tp_bin_item items[3] = {{0, 30}, {1, 150}, {2, 30}};
    size_t bins[3];
    long n = tp_bin_assign_ffd(items, 3, 100, bins);
    check(n == 2, "ffd: oversized item gets its own bin; the two 30s share another");
    check(bins[1] != bins[0] && bins[1] != bins[2],
          "ffd: oversized item1 is not sharing a bin with any other item");
    check(bins[0] == bins[2], "ffd: the two same-size 30-cost items share a bin");
}

int main(void) {
    test_parse_size();
    test_estimate_entry_cost();
    test_next_fit_empty();
    test_next_fit_single_object();
    test_next_fit_keeps_input_order_and_respects_target();
    test_next_fit_oversized_object_alone();
    test_next_fit_exact_fit_stays_in_same_bin();
    test_ffd_empty();
    test_ffd_single_object();
    test_ffd_places_largest_first_and_fills_earlier_bins();
    test_ffd_oversized_object_alone();

    if (g_fail_count != 0) {
        fprintf(stderr, "\n%d check(s) failed\n", g_fail_count);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
