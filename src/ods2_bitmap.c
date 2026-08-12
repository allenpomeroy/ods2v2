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

#include "ods2_bitmap.h"

static bool bit_is_set(const uint8_t *bitmap, size_t bit_index)
{
    size_t byte_index = bit_index / 8;
    unsigned bit_in_byte = bit_index % 8;
    return (bitmap[byte_index] >> bit_in_byte) & 1;
}

static void bit_set(uint8_t *bitmap, size_t bit_index, bool value)
{
    size_t byte_index = bit_index / 8;
    unsigned bit_in_byte = bit_index % 8;
    if (value) {
        bitmap[byte_index] |= (uint8_t) (1u << bit_in_byte);
    } else {
        bitmap[byte_index] &= (uint8_t) ~(1u << bit_in_byte);
    }
}

bool ods2_bitmap_mark(uint8_t *bitmap, size_t total_bits,
                       size_t start_bit, size_t count, bool used)
{
    size_t i;
    if (start_bit + count > total_bits) {
        return false;
    }
    for (i = start_bit; i < start_bit + count; i++) {
        bit_set(bitmap, i, !used); /* bit set = free, per spec 5.2.2 */
    }
    return true;
}

bool ods2_bitmap_find_free(const uint8_t *bitmap, size_t total_bits,
                            size_t count, size_t *start_bit, size_t *found_count)
{
    size_t i;
    size_t run_start = 0;
    size_t run_len = 0;
    bool in_run = false;

    if (count == 0) {
        return false;
    }

    for (i = 0; i < total_bits; i++) {
        if (bit_is_set(bitmap, i)) {
            if (!in_run) {
                /* Starting a new run - remember exactly where it
                   began. This explicit tracking is the whole point:
                   the old code's bug came from trying to derive the
                   run's start position algebraically from the current
                   scan position and run length, and getting that
                   arithmetic wrong. Storing the actual start index
                   directly removes the opportunity for that mistake. */
                run_start = i;
                in_run = true;
            }
            run_len++;
            if (run_len >= count) {
                *start_bit = run_start;
                *found_count = run_len;
                return true;
            }
        } else {
            in_run = false;
            run_len = 0;
        }
    }

    return false;
}
