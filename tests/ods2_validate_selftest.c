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
#include "ods2_ondisk.h"
#include "ods2_checksum.h"
#include "ods2_validate.h"

static void build_valid_home(ods2_home_t *home)
{
    memset(home, 0, sizeof(*home));
    memcpy(home->format, "DECFILE11B  ", 12);
    home->struclev  = 0x0201;   /* level 2, version 1 */
    home->maxfiles  = 38900;
    home->altidxlbn = 12345;    /* just needs to be non-zero for this test */
    home->cluster   = 3;

    home->checksum1 = ods2_checksum((const uint8_t *) home, 29);
    home->checksum2 = ods2_checksum((const uint8_t *) home, 255);
}

int main(void)
{
    ods2_home_t home;
    ods2_validate_result_t r;

    /* 1. A correctly-built home block validates successfully. */
    build_valid_home(&home);
    r = ods2_validate_home(&home);
    assert(r.ok);
    printf("PASS: correctly-built home block validates\n");

    /* 2. Reproduce the real bug: modify maxfiles (a field within
       checksum1's 29-word coverage) and recompute ONLY checksum2,
       exactly as the old code's first attempt did. The validator must
       catch this - this is the local-sandbox equivalent of what
       ANALYZE/DISK's ALTIHDBAD finding caught on real VMS. */
    build_valid_home(&home);
    home.maxfiles = 40960;
    home.checksum2 = ods2_checksum((const uint8_t *) &home, 255); /* only this recomputed */
    r = ods2_validate_home(&home);
    assert(!r.ok);
    printf("PASS: stale checksum1 after modifying a covered field is caught: %s\n", r.problem);

    /* 3. Fixing checksum1 too makes it valid again - but note
       checksum2 covers bytes 0-509, which includes checksum1's own
       position (offset 58), so checksum2 must be recomputed AFTER
       checksum1 changes, not just checksum1 itself. */
    home.checksum1 = ods2_checksum((const uint8_t *) &home, 29);
    home.checksum2 = ods2_checksum((const uint8_t *) &home, 255);
    r = ods2_validate_home(&home);
    assert(r.ok);
    printf("PASS: recomputing both checksums (in the right order) makes it valid again\n");

    /* 4. Wrong format string is caught. */
    build_valid_home(&home);
    memcpy(home.format, "SOMETHINGELSE", 12);
    r = ods2_validate_home(&home);
    assert(!r.ok);
    printf("PASS: wrong format string is caught: %s\n", r.problem);

    /* 5. altidxlbn == 0 is caught (checksums recomputed after the
       change, so this isolates the altidxlbn check itself rather than
       incidentally tripping the checksum1 check too, since altidxlbn
       falls within checksum1's coverage). */
    build_valid_home(&home);
    home.altidxlbn = 0;
    home.checksum1 = ods2_checksum((const uint8_t *) &home, 29);
    home.checksum2 = ods2_checksum((const uint8_t *) &home, 255);
    r = ods2_validate_home(&home);
    assert(!r.ok);
    printf("PASS: zero altidxlbn is caught: %s\n", r.problem);

    /* --- ods2_validate_head() tests --- */
    {
        uint8_t raw[512];
        ods2_validate_result_t hr;

        /* 6. A minimal, correctly-checksummed header with no mapping
           data (map_inuse=0) validates successfully. */
        memset(raw, 0, sizeof(raw));
        {
            uint16_t c = ods2_checksum(raw, 255);
            raw[510] = (uint8_t) (c & 0xff);
            raw[511] = (uint8_t) (c >> 8);
        }
        hr = ods2_validate_head(raw);
        assert(hr.ok);
        printf("PASS: minimal correctly-checksummed header (no mapping data) validates\n");

        /* 7. Corrupting one byte without updating the checksum is caught. */
        raw[100] ^= 0xff;
        hr = ods2_validate_head(raw);
        assert(!hr.ok);
        printf("PASS: corrupted header content with stale checksum is caught: %s\n", hr.problem);
    }

    /* 8. Real data: read INDEXF.SYS's own header from the sample and
       confirm it now validates correctly. This field's apparent
       "corruption" (FAT$L_HIBLK reading as ~7.8 million blocks
       against a real extent sum of 120) turned out to be a
       word-swapped-encoding misread on my part, not real data
       corruption - confirmed by testing against two independently
       VMS-INITIALIZE'd volumes that both showed the identical
       pattern (exactly real_value << 16). Once ods2_word_swap32()
       was applied correctly, this same real header validates fine. */
    {
        FILE *f = fopen("samples/indexf_headers.bin", "rb");
        if (f == NULL) {
            printf("SKIP: samples/indexf_headers.bin not found (run from ods2v2/ "
                   "directory to include this real-data check)\n");
        } else {
            uint8_t raw[512];
            ods2_validate_result_t hr;
            size_t got;
            if (fseek(f, 90L * 512, SEEK_SET) != 0) {
                printf("SKIP: could not seek to header 1\n");
            } else {
                got = fread(raw, 1, sizeof(raw), f);
                if (got != sizeof(raw)) {
                    printf("SKIP: short read on real header sample\n");
                } else {
                    hr = ods2_validate_head(raw);
                    assert(hr.ok);
                    printf("PASS: real INDEXF.SYS header validates correctly now that "
                           "FAT$L_HIBLK's word-swapped encoding is handled properly\n");
                }
            }
            fclose(f);
        }
    }

    printf("ods2_validate_selftest: all checks passed\n");
    return 0;
}
