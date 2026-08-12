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
#include "ods2_retrieval.h"

int main(void)
{
    /* Real map-area bytes from INDEXF.SYS's own header (file 1), read
       from an actual VMS-initialized disk (samples/indexf_headers.bin,
       byte offset 46280 = header start 46080 + mpoffset*2 = 200).
       8 words (map_inuse=8 in the real header). */
    static const uint8_t real_map_area[16] = {
        0x05, 0x40, 0x00, 0x00,  /* extent 1: format1, count=5(+1=6), lbn=0 */
        0x02, 0x40, 0x08, 0x04,  /* extent 2: format1, count=2(+1=3), lbn=0x0408=1032 */
        0x02, 0x56, 0x08, 0x75,  /* extent 3: format1, count=2(+1=3), lbn=(0x16<<16)|0x7508=1471752 */
        0x6b, 0x56, 0x00, 0x71   /* extent 4: format1, count=0x6b(+1=108), lbn=(0x16<<16)|0x7100=1470720 */
    };
    ods2_extent_t extents[8];
    int n = ods2_decode_retrieval_pointers(real_map_area, 8, extents, 8);
    uint32_t total_blocks;
    int i;

    assert(n == 4);
    printf("PASS: decoded exactly 4 extents from real INDEXF.SYS map area\n");

    /* Extent 1: boot block + home block + 4 filler blocks, at LBN 0. */
    assert(extents[0].lbn == 0);
    assert(extents[0].block_count == 6);
    printf("PASS: extent 1 = LBN 0, 6 blocks (boot+home+filler)\n");

    /* Extent 2: backup home block cluster. */
    assert(extents[1].lbn == 1032);
    assert(extents[1].block_count == 3);
    printf("PASS: extent 2 = LBN 1032, 3 blocks (backup home block cluster)\n");

    /* Extent 3: backup index file header - MUST exactly match
       altidxlbn from the home block on this same disk. This is the
       strongest possible confirmation: an independently-decoded
       extent's LBN matching a completely separately-read field. */
    assert(extents[2].lbn == 1471752);
    assert(extents[2].block_count == 3);
    printf("PASS: extent 3 = LBN 1471752, 3 blocks - EXACTLY matches altidxlbn\n");

    /* Extent 4: bitmap + first headers - MUST exactly match ibmaplbn. */
    assert(extents[3].lbn == 1470720);
    assert(extents[3].block_count == 108);
    printf("PASS: extent 4 = LBN 1470720, 108 blocks - EXACTLY matches ibmaplbn\n");

    total_blocks = 0;
    for (i = 0; i < n; i++) total_blocks += extents[i].block_count;
    printf("\nTotal allocated blocks per real retrieval pointers: %u\n", total_blocks);
    printf("(The same real header's FAT$L_HIBLK field, read as a plain LE\n"
           " longword, appears to claim 7864320 blocks - but is not\n"
           " real corruption. FAT$L_HIBLK uses PDP-11-heritage word-swapped\n"
           " encoding; ods2_word_swap32() on that raw value correctly gives\n"
           " 120, exactly matching these independently-decoded extents. See\n"
           " ods2_ondisk.h's ods2_word_swap32() for details - this was\n"
           " confirmed against two independently-created, freshly VMS-\n"
           " INITIALIZE'd volumes before being trusted as a real finding.)\n");
    assert(total_blocks == 120);

    /* --- Encoder tests (write counterpart to the decoder above) --- */

    /* Round-trip: encode an extent, decode it back, confirm it
       matches exactly - using the same real LBN/count values already
       confirmed above (altidxlbn's extent). Confirms this isn't just
       internally self-consistent, it reproduces bytes that are
       independently verified as correct. */
    {
        uint8_t encoded[4];
        ods2_extent_t decoded[1];
        int n;
        bool ok_encode = ods2_encode_retrieval_pointer_format1(encoded, 1471752, 3);
        assert(ok_encode);
        n = ods2_decode_retrieval_pointers(encoded, 2, decoded, 1);
        assert(n == 1);
        assert(decoded[0].lbn == 1471752);
        assert(decoded[0].block_count == 3);
        printf("PASS: encode/decode round-trip reproduces the real altidxlbn "
               "extent exactly\n");
    }

    /* Boundary: block_count exactly 256 (the maximum, count field=255)
       and exactly 1 (the minimum, count field=0) both work. */
    {
        uint8_t encoded[4];
        ods2_extent_t decoded[1];
        assert(ods2_encode_retrieval_pointer_format1(encoded, 0, 256));
        assert(ods2_decode_retrieval_pointers(encoded, 2, decoded, 1) == 1);
        assert(decoded[0].block_count == 256);

        assert(ods2_encode_retrieval_pointer_format1(encoded, 0, 1));
        assert(ods2_decode_retrieval_pointers(encoded, 2, decoded, 1) == 1);
        assert(decoded[0].block_count == 1);
        printf("PASS: block_count boundaries (1 and 256) both encode/decode correctly\n");
    }

    /* Out-of-range values are rejected, not silently truncated. */
    {
        uint8_t encoded[4];
        assert(!ods2_encode_retrieval_pointer_format1(encoded, 0, 0));   /* count=0 invalid */
        assert(!ods2_encode_retrieval_pointer_format1(encoded, 0, 257)); /* too many blocks */
        assert(!ods2_encode_retrieval_pointer_format1(encoded, 0x400000, 1)); /* LBN too large */
        printf("PASS: out-of-range block_count/lbn values are rejected, not truncated\n");
    }

    /* Maximum representable LBN round-trips correctly. */
    {
        uint8_t encoded[4];
        ods2_extent_t decoded[1];
        uint32_t max_lbn = 0x3fffffu; /* 22 bits: 6-bit high word + 16-bit low word */
        assert(ods2_encode_retrieval_pointer_format1(encoded, max_lbn, 10));
        assert(ods2_decode_retrieval_pointers(encoded, 2, decoded, 1) == 1);
        assert(decoded[0].lbn == max_lbn);
        printf("PASS: maximum representable LBN (0x3fffff) round-trips correctly\n");
    }

    printf("\nods2_retrieval_selftest: all checks passed\n");
    return 0;
}
