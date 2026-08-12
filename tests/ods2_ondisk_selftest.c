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

/* ods2_ondisk_selftest.c - compile-time + runtime checks that the
 * on-disk struct layouts actually match the Files-11 spec's stated
 * sizes and offsets. This is step one of "validate locally before
 * ever touching real VMS" - every assertion here corresponds to an
 * explicit statement in the spec.
 */

#include <stdio.h>
#include <stddef.h>
#include <assert.h>
#include "ods2_ondisk.h"

int main(void)
{
    /* Spec 5.1.9: home block is exactly one 512-byte block. */
    _Static_assert(sizeof(ods2_home_t) == 512, "HOME block must be 512 bytes");

    /* Spec 5.1.9.30 (HM2$W_CHECKSUM1) sits right after HM2$W_RESERVED2,
     * as the 30th word (word index 29, byte offset 58) - covers only
     * the first 29 words per section 5.1.9.30's own text. */
    _Static_assert(offsetof(ods2_home_t, checksum1) == 58,
                    "HM2$W_CHECKSUM1 must be at byte offset 58");

    /* HM2$W_CHECKSUM2 is documented as covering the first 255 words,
     * i.e. sitting at byte offset 510 (the last word of the block). */
    _Static_assert(offsetof(ods2_home_t, checksum2) == 510,
                    "HM2$W_CHECKSUM2 must be at byte offset 510");

    /* FH2$W_RECATTR is documented (3.5.2.9) as exactly 32 bytes,
     * sitting right after the two FID fields in the header area. */
    _Static_assert(sizeof(((ods2_head_core_t *)0)->recattr) == 32,
                    "FH2$W_RECATTR must be 32 bytes");

    /* And the FAT struct itself (spec 6.2, FAT$C_LENGTH) must also be
     * exactly 32 bytes on its own. */
    _Static_assert(sizeof(ods2_fat_t) == 32, "ods2_fat_t must be 32 bytes");

    /* idoffset must be the FIRST byte of the header (offset 0), not
     * mpoffset - confirmed against a real INDEXF.SYS header where the
     * literal text "INDEXF.SYS;1" was found at exactly
     * idoffset_value*2 bytes into the header. The spec's side-by-side
     * diagram layout puts the lower byte address under the
     * right-hand label, which is the opposite of the naive left-to-
     * right reading and easy to get backwards (I did, initially). */
    _Static_assert(offsetof(ods2_head_core_t, idoffset) == 0,
                    "idoffset must be at byte offset 0 (confirmed against real data)");
    _Static_assert(offsetof(ods2_head_core_t, mpoffset) == 1,
                    "mpoffset must be at byte offset 1 (confirmed against real data)");

    /* Directory record header (4.2): DIR$W_SIZE(2) + DIR$W_VERLIMIT(2)
     * + DIR$B_FLAGS(1) + DIR$B_NAMECOUNT(1) = 6 bytes exactly. This is
     * the exact field the old project's DIRREC_HDRSIZE constant
     * existed to work around (sizeof() with default padding gave 8,
     * not 6) - pack(1) makes sizeof() correct directly instead of
     * requiring every call site to remember a separate constant. */
    _Static_assert(sizeof(ods2_dir_rec_header_t) == 6,
                    "directory record header must be 6 bytes");

    /* Per-version entry: DIR$W_VERSION(2) + FID(6) = 8 bytes. */
    _Static_assert(sizeof(ods2_dir_ent_t) == 8,
                    "directory entry (version+fid) must be 8 bytes");

    /* ods2_word_swap32(): FAT$L_HIBLK/FAT$L_EFBLK use PDP-11-heritage
       word-swapped encoding, discovered by comparing FAT$L_HIBLK
       (read as plain LE) against independently-decoded retrieval
       pointer extent sums on two separately-created, freshly
       VMS-INITIALIZE'd volumes - both showed the identical pattern.
       These are the actual real values observed. */
    if (ods2_word_swap32(7864320u) != 120u) {
        printf("FAIL: ods2_word_swap32(7864320) should be 120 (real INDEXF.SYS "
               "value from two independently-initialized VMS volumes), got %u\n",
               ods2_word_swap32(7864320u));
        return 1;
    }
    if (ods2_word_swap32(196608u) != 3u) {
        printf("FAIL: ods2_word_swap32(196608) should be 3 (real root directory "
               "hiblk value), got %u\n", ods2_word_swap32(196608u));
        return 1;
    }
    if (ods2_word_swap32(0u) != 0u) {
        printf("FAIL: ods2_word_swap32(0) should be 0\n");
        return 1;
    }
    /* Swapping twice must return the original value. */
    if (ods2_word_swap32(ods2_word_swap32(0x12345678u)) != 0x12345678u) {
        printf("FAIL: double word-swap should be identity\n");
        return 1;
    }
    printf("PASS: ods2_word_swap32() matches real values from two independently\n"
           "      VMS-INITIALIZE'd volumes, and is its own inverse\n");

    printf("ods2_ondisk_selftest: all static layout checks passed\n");
    printf("  sizeof(ods2_home_t)      = %zu (expect 512)\n", sizeof(ods2_home_t));
    printf("  offsetof(checksum1)      = %zu (expect 58)\n", offsetof(ods2_home_t, checksum1));
    printf("  offsetof(checksum2)      = %zu (expect 510)\n", offsetof(ods2_home_t, checksum2));
    printf("  sizeof(ods2_head_core_t) = %zu\n", sizeof(ods2_head_core_t));
    printf("  sizeof(ods2_fid_t)       = %zu (expect 6)\n", sizeof(ods2_fid_t));

    assert(sizeof(ods2_fid_t) == 6);
    return 0;
}
