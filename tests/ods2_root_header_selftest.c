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
#include "ods2_ondisk.h"
#include "ods2_retrieval.h"

int main(void)
{
    /* Real map-area bytes for root's own header (file 4, "000000.DIR"),
       read from samples/indexf_headers.bin at relative block 93
       (header_lbn(4) = ibmaplbn + (ibmapsize+4-1) = 1470720+93=1470813).
       map_inuse=2 for this header - exactly one Format 1 extent. */
    static const uint8_t real_map_area[4] = { 0x02, 0x56, 0x0a, 0x70 };
    ods2_extent_t extents[4];
    int n = ods2_decode_retrieval_pointers(real_map_area, 2, extents, 4);

    assert(n == 1);
    printf("PASS: decoded exactly 1 extent from root directory's map area\n");

    assert(extents[0].lbn == 1470474);
    assert(extents[0].block_count == 3);
    printf("PASS: root's directory content = LBN 1470474, 3 blocks\n");

    printf("\nTo read root's actual directory content, dd this range:\n");
    printf("  dd if=transfer.dsk of=root_dir.bin bs=512 skip=%u count=%u\n",
           extents[0].lbn, extents[0].block_count);

    printf("\nods2_root_header_selftest: all checks passed\n");
    return 0;
}
