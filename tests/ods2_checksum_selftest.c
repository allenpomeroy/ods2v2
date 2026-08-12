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
#include <assert.h>
#include <string.h>
#include "ods2_checksum.h"

int main(void)
{
    uint8_t zeros[64] = {0};
    uint8_t two_words[4] = {0x01, 0x00, 0x02, 0x00}; /* words: 0x0001, 0x0002 */
    uint8_t wrap[4] = {0xff, 0xff, 0xff, 0xff};        /* words: 0xffff, 0xffff */

    assert(ods2_checksum(zeros, 32) == 0);
    printf("PASS: all-zero data checksums to 0\n");

    assert(ods2_checksum(two_words, 2) == 0x0003);
    printf("PASS: 0x0001 + 0x0002 = 0x0003\n");

    /* 0xffff + 0xffff = 0x1fffe, truncated to 16 bits = 0xfffe */
    assert(ods2_checksum(wrap, 2) == 0xfffe);
    printf("PASS: overflow wraps correctly (0xffff+0xffff -> 0xfffe)\n");

    printf("ods2_checksum_selftest: all checks passed\n");
    return 0;
}
