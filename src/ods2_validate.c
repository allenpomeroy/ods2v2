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

#include "ods2_validate.h"
#include "ods2_checksum.h"
#include "ods2_retrieval.h"
#include <string.h>

static ods2_validate_result_t ok(void)
{
    ods2_validate_result_t r;
    r.ok = true;
    r.problem = NULL;
    return r;
}

static ods2_validate_result_t fail(const char *why)
{
    ods2_validate_result_t r;
    r.ok = false;
    r.problem = why;
    return r;
}

ods2_validate_result_t ods2_validate_home(const ods2_home_t *home)
{
    const uint8_t *raw = (const uint8_t *) home;
    uint16_t computed1, computed2;

    if (memcmp(home->format, "DECFILE11B  ", 12) != 0) {
        return fail("HM2$T_FORMAT is not \"DECFILE11B  \"");
    }

    /* struclev high byte = structure level, low byte = version.
       ODS-2 is level 2; version 1 or higher. */
    if ((home->struclev >> 8) != 2) {
        return fail("HM2$W_STRUCLEV structure level is not 2 (not ODS-2)");
    }
    if ((home->struclev & 0xff) < 1) {
        return fail("HM2$W_STRUCLEV version is 0 (expected >= 1)");
    }

    /* checksum1 covers the first 29 words (bytes 0-57) - the
       volume-identification portion only, per spec 5.1.9.30. */
    computed1 = ods2_checksum(raw, 29);
    if (computed1 != home->checksum1) {
        return fail("HM2$W_CHECKSUM1 does not match computed value "
                     "(checksum1 stale relative to the fields it covers)");
    }

    /* checksum2 covers the first 255 words (bytes 0-509) - the whole
       block except itself. */
    computed2 = ods2_checksum(raw, 255);
    if (computed2 != home->checksum2) {
        return fail("HM2$W_CHECKSUM2 does not match computed value "
                     "(checksum2 stale relative to the fields it covers)");
    }

    if (home->altidxlbn == 0) {
        return fail("HM2$L_ALTIDXLBN is zero (must be non-zero per spec 5.1.9.3)");
    }

    return ok();
}

ods2_validate_result_t ods2_validate_head(const uint8_t *raw_header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) raw_header;
    uint16_t checksum_stored, checksum_computed;
    ods2_extent_t extents[64];
    int n;
    uint64_t total_blocks; /* wider than uint32_t: sum could legitimately
                               overflow 32 bits while still being a
                               real, valid consistency-check total */
    size_t i;
    size_t map_word_count;

    /* FH2$W_CHECKSUM occupies the last word of the 512-byte block,
       covering the 255 words before it (same algorithm/coverage
       pattern as the home block's checksum2). */
    checksum_stored = (uint16_t) raw_header[510] | ((uint16_t) raw_header[511] << 8);
    checksum_computed = ods2_checksum(raw_header, 255);
    if (checksum_stored != checksum_computed) {
        return fail("FH2$W_CHECKSUM does not match computed value");
    }

    /* Cross-validate FAT$L_HIBLK against the header's own retrieval
       pointers, if any are present. map_inuse is a word count. */
    map_word_count = core->map_inuse;
    if (map_word_count > 0 && (size_t) core->mpoffset * 2 + map_word_count * 2 <= 512) {
        n = ods2_decode_retrieval_pointers(raw_header + (size_t) core->mpoffset * 2,
                                            map_word_count, extents,
                                            sizeof(extents) / sizeof(extents[0]));
        if (n > 0) {
            total_blocks = 0;
            for (i = 0; i < (size_t) n; i++) {
                total_blocks += extents[i].block_count;
            }
            /* FAT$L_HIBLK describes the file's TOTAL allocation across
               its WHOLE header chain (spec 3.3), not just this one
               header's own retrieval pointers - so this specific
               check is only meaningful for a header that's provably
               NOT part of a multi-header chain: no further extension
               header (ext_fid==0) and not itself an extension header
               (seg_num==0). A genuinely single-header file/directory
               is still checked exactly as before; a header that's
               part of a chain is skipped here rather than produce a
               spurious mismatch (this function only ever sees one
               raw header buffer, with no way to read the rest of the
               chain itself - a caller with a mounted volume can get
               the real, whole-file answer via
               ods2_decode_all_extents() instead, e.g. as
               ods2_check_headers does). */
            {
                bool part_of_chain = (core->ext_fid.fid_num != 0) || (core->seg_num != 0);
                if (!part_of_chain && total_blocks != ods2_word_swap32(core->recattr.hiblk)) {
                    return fail("FAT$L_HIBLK does not match the sum of this header's "
                                "own retrieval pointer extents");
                }
            }
        }
    }

    return ok();
}
