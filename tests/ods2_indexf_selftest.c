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
#include "ods2_ondisk.h"
#include "ods2_indexf.h"

int main(void)
{
    ods2_home_t home = {0};
    uint32_t h1, h2, h16;

    /* Known-real values from our validated sample.bin (and matching
       the old tool's diagnostics on the same disk). */
    home.cluster   = 3;
    home.ibmaplbn  = 1470720;
    home.ibmapsize = 90;

    h1  = ods2_indexf_header_lbn(&home, 1);
    h2  = ods2_indexf_header_lbn(&home, 2);
    h16 = ods2_indexf_header_lbn(&home, 16);

    printf("header(1)  = LBN %u\n", h1);
    printf("header(2)  = LBN %u\n", h2);
    printf("header(16) = LBN %u\n", h16);

    /* header_lbn(n) = ibmaplbn + (m + n - 1) = 1470720 + (90 + n - 1) */
    assert(h1 == 1470720 + 90);       /* 1470810 */
    assert(h2 == 1470720 + 91);       /* 1470811 */
    assert(h16 == 1470720 + 105);     /* 1470825 */

    /* Headers must be contiguous, one block apart, per the spec's
       "logically contiguous with the bitmap" statement. */
    assert(h2 == h1 + 1);
    assert(h16 == h1 + 15);

    printf("PASS: header LBN formula matches expected values and is contiguous\n");
    printf("\nTo test against real bytes, dd this range from your real disk:\n");
    printf("  dd if=transfer.dsk of=indexf_headers.bin bs=512 skip=%u count=%u\n",
           home.ibmaplbn, home.ibmapsize + 16);
    printf("(covers the bitmap itself, blocks %u-%u, plus the first 16 "
           "file headers, blocks %u-%u)\n",
           home.ibmaplbn, home.ibmaplbn + home.ibmapsize - 1, h1, h16);

    return 0;
}
