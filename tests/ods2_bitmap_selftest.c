/*
 * MIT License
 *
 * Copyright (c) 2026 Allen Pomeroy
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ods2_bitmap.h"

/* Sets bits [lo, hi) to 1 (free) in an all-zero (all-allocated) buffer. */
static void set_free_range(uint8_t *bitmap, size_t lo, size_t hi)
{
    size_t i;
    for (i = lo; i < hi; i++) {
        bitmap[i / 8] |= (uint8_t) (1u << (i % 8));
    }
}

/* Test-local bit reader - independent of ods2_bitmap.c's internal
   (static) bit_is_set(), so this test verifies actual byte-level
   output rather than trusting the implementation's own helper. */
static int bit_is_set_for_test(const uint8_t *bitmap, size_t bit_index)
{
    return (bitmap[bit_index / 8] >> (bit_index % 8)) & 1;
}

int main(void)
{
    uint8_t bitmap[16] = {0}; /* 128 bits, all allocated (clear) initially */
    size_t start, found;

    /* Test 1: no free bits at all -> not found. */
    assert(!ods2_bitmap_find_free(bitmap, 128, 1, &start, &found));
    printf("PASS: all-allocated bitmap reports no free run\n");

    /* Test 2: THE bug scenario. Bits 5-19 are a free run of 15
       clusters (bits 5,6,...,19 inclusive = 15 bits). Request only 3.
       The old code's bug (best_cluster = cluster + bit_no, without
       subtracting the run length) would have reported the position
       near the END of wherever it stopped scanning, not the actual
       START of the free run - which, depending on the exact bug
       variant, could point at an already-allocated cluster or overlap
       a previous allocation. The correct answer here is unambiguous:
       start_bit must be exactly 5. */
    memset(bitmap, 0, sizeof(bitmap));
    set_free_range(bitmap, 5, 20); /* bits 5..19 free, 15 bits */
    assert(ods2_bitmap_find_free(bitmap, 128, 3, &start, &found));
    assert(start == 5);
    assert(found == 3);
    printf("PASS: 3-bit request from a 15-bit free run starting at bit 5 "
           "correctly returns start=5 (not shifted)\n");

    /* Test 3: request exactly the full run length. */
    memset(bitmap, 0, sizeof(bitmap));
    set_free_range(bitmap, 5, 20);
    assert(ods2_bitmap_find_free(bitmap, 128, 15, &start, &found));
    assert(start == 5);
    assert(found == 15);
    printf("PASS: requesting the exact full run length works\n");

    /* Test 4: request one more than the available run -> not found,
       even though a smaller run exists elsewhere. This specifically
       guards against a search that finds SOME run and incorrectly
       reports success without checking the length is sufficient. */
    memset(bitmap, 0, sizeof(bitmap));
    set_free_range(bitmap, 5, 20); /* 15 free bits */
    assert(!ods2_bitmap_find_free(bitmap, 128, 16, &start, &found));
    printf("PASS: requesting more than the largest available run correctly fails\n");

    /* Test 5: two separate free runs; a request that only fits in the
       second one must skip the first and land in the second, at ITS
       correct start position - guards against the search getting
       confused by an earlier, too-small run. */
    memset(bitmap, 0, sizeof(bitmap));
    set_free_range(bitmap, 2, 4);    /* bits 2-3: run of 2 */
    set_free_range(bitmap, 10, 30);  /* bits 10-29: run of 20 */
    assert(ods2_bitmap_find_free(bitmap, 128, 5, &start, &found));
    assert(start == 10);
    assert(found == 5);
    printf("PASS: a too-small first run is correctly skipped in favor of "
           "a later, sufficient one, landing at the later run's true start\n");

    /* Test 6: a run that ends exactly at total_bits (no trailing
       allocated bit to terminate it) must still be found correctly -
       guards against an off-by-one at the end of the scan. */
    memset(bitmap, 0, sizeof(bitmap));
    set_free_range(bitmap, 120, 128); /* last 8 bits free, nothing after */
    assert(ods2_bitmap_find_free(bitmap, 128, 8, &start, &found));
    assert(start == 120);
    assert(found == 8);
    printf("PASS: a free run ending exactly at the buffer boundary is found correctly\n");

    /* --- ods2_bitmap_mark() tests --- */

    /* Test 7: allocate (mark used) a range, confirm find_free skips
       it, then free it again and confirm find_free finds it once
       more - a full allocate/deallocate round trip. */
    memset(bitmap, 0xff, sizeof(bitmap)); /* all 128 bits free */
    assert(ods2_bitmap_mark(bitmap, 128, 10, 5, true)); /* allocate bits 10-14 */
    assert(ods2_bitmap_find_free(bitmap, 128, 5, &start, &found));
    assert(start == 0); /* bits 0-9 still free, found there instead */
    assert(!ods2_bitmap_find_free(bitmap, 128, 200, &start, &found)); /* sanity */
    printf("PASS: marking a range used correctly removes it from consideration\n");

    assert(ods2_bitmap_mark(bitmap, 128, 10, 5, false)); /* free it again */
    memset(bitmap, 0, sizeof(bitmap));
    assert(ods2_bitmap_mark(bitmap, 128, 10, 5, false)); /* mark bits 10-14 free on an all-allocated map */
    assert(ods2_bitmap_find_free(bitmap, 128, 5, &start, &found));
    assert(start == 10);
    assert(found == 5);
    printf("PASS: allocate/free round trip works correctly\n");

    /* Test 8: marking a range that would run past total_bits fails
       cleanly rather than silently clipping or overflowing. */
    memset(bitmap, 0, sizeof(bitmap));
    assert(!ods2_bitmap_mark(bitmap, 128, 126, 5, false)); /* 126+5=131 > 128 */
    printf("PASS: marking past the end of the bitmap is rejected, not clipped\n");

    /* Test 9: marking doesn't disturb bits outside the requested
       range - a common off-by-one risk area. */
    memset(bitmap, 0xff, sizeof(bitmap)); /* all free */
    assert(ods2_bitmap_mark(bitmap, 128, 20, 10, true)); /* allocate bits 20-29 */
    {
        size_t i;
        for (i = 0; i < 20; i++) assert(bit_is_set_for_test(bitmap, i));
        for (i = 20; i < 30; i++) assert(!bit_is_set_for_test(bitmap, i));
        for (i = 30; i < 128; i++) assert(bit_is_set_for_test(bitmap, i));
    }
    printf("PASS: marking a range leaves bits outside it undisturbed (no off-by-one)\n");

    /* --- ods2_bitmap_find_largest_free() tests --- */

    /* Test 10: no free bits at all -> not found, same as find_free. */
    memset(bitmap, 0, sizeof(bitmap));
    assert(!ods2_bitmap_find_largest_free(bitmap, 128, 5, &start, &found));
    printf("PASS: all-allocated bitmap reports no free run (find_largest_free)\n");

    /* Test 11: request MORE than the largest available run - unlike
       find_free (which would fail), find_largest_free succeeds with
       whatever it actually found. */
    memset(bitmap, 0, sizeof(bitmap));
    set_free_range(bitmap, 5, 20); /* 15 free bits */
    assert(ods2_bitmap_find_largest_free(bitmap, 128, 100, &start, &found));
    assert(start == 5);
    assert(found == 15);
    printf("PASS: requesting more than the largest run returns that run's true "
           "size instead of failing\n");

    /* Test 12: two runs, one bigger than the other - the LARGER one
       must win even though it's not the first one scanned. */
    memset(bitmap, 0, sizeof(bitmap));
    set_free_range(bitmap, 2, 4);    /* bits 2-3: run of 2 */
    set_free_range(bitmap, 10, 30);  /* bits 10-29: run of 20 */
    assert(ods2_bitmap_find_largest_free(bitmap, 128, 100, &start, &found));
    assert(start == 10);
    assert(found == 20);
    printf("PASS: the larger of two runs is correctly preferred\n");

    /* Test 13: when a run at least as large as max_count DOES exist,
       the result must be capped at exactly max_count, matching
       find_free's own contract (never report more than asked for). */
    memset(bitmap, 0, sizeof(bitmap));
    set_free_range(bitmap, 5, 20); /* 15 free bits */
    assert(ods2_bitmap_find_largest_free(bitmap, 128, 5, &start, &found));
    assert(start == 5);
    assert(found == 5);
    printf("PASS: a sufficient run is capped at the requested size, not "
           "over-reported\n");

    printf("ods2_bitmap_selftest: all checks passed\n");
    return 0;
}
