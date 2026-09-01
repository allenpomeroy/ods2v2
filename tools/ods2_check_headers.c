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

/* ods2_check_headers.c - reads the first N file headers on a real
 * disk image and reports, for each, whether FAT$L_HIBLK matches the
 * sum of that header's own retrieval pointer extents. 
 *
 * Usage: ods2_check_headers <disk-image> [count]
 *   count defaults to 16 (the range locatable directly from the home
 *   block alone, per spec 5.1.7).
 */
#include <stdio.h>
#include <stdlib.h>
#include "ods2_volume.h"
#include "ods2_validate.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    unsigned count = 16;
    unsigned n;
    int mismatches = 0, checked = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <disk-image> [count]\n", argv[0]);
        return 1;
    }
    if (argc > 2) count = (unsigned) atoi(argv[2]);

    r = ods2_mount(argv[1], &vol);
    if (!r.ok) {
        fprintf(stderr, "%s: mount failed: %s\n", argv[1], r.problem);
        return 1;
    }

    printf("Checking headers 1-%u on %s\n\n", count, argv[1]);
    printf("%-4s %-14s %-10s %-14s %-10s %s\n",
           "file", "hiblk", "efblk", "extent_sum", "match?", "notes");
    printf("---- -------------- ---------- -------------- ---------- -----\n");

    for (n = 1; n <= count; n++) {
        uint8_t header[512];
        const ods2_head_core_t *core;
        ods2_extent_t extents[ODS2_MAX_EXTENTS];
        int extent_count;
        uint64_t extent_sum = 0;
        int i;
        ods2_validate_result_t vr;

        r = ods2_read_header(&vol, n, header);
        if (!r.ok) {
            printf("%-4u  (could not read header: %s)\n", n, r.problem);
            continue;
        }

        core = (const ods2_head_core_t *) header;
        /* Skip unused header slots: fid_num==0 means this slot has
           never been allocated to a file. */
        if (core->fid.fid_num == 0) {
            continue;
        }
        checked++;

        r = ods2_decode_all_extents(&vol, header, extents, ODS2_MAX_EXTENTS, &extent_count);
        if (r.ok && extent_count > 0) {
            for (i = 0; i < extent_count; i++) extent_sum += extents[i].block_count;
        }

        vr = ods2_validate_head(header);

        {
            uint32_t hiblk_fixed = ods2_word_swap32(core->recattr.hiblk);
            uint32_t efblk_fixed = ods2_word_swap32(core->recattr.efblk);
            bool is_extension_seg = (core->seg_num != 0);

            /* HIBLK/EFBLK describe the FILE AS A WHOLE and are only
               meaningful compared against a chain walk that starts
               from the PRIMARY header (seg_num==0). Starting the walk
               from an extension header instead (which is exactly what
               happens here, since this loop iterates every raw file
               number) only ever sees that header's own remaining
               extents, not the whole file's - a "mismatch" reported
               for a seg_num!=0 header is expected and not a real
               problem; skip it from the mismatch count and say so. */
            printf("%-4u %-14u %-10u %-14llu %-10s %s\n",
                   n, hiblk_fixed, efblk_fixed,
                   (unsigned long long) extent_sum,
                   is_extension_seg ? "n/a" : ((extent_sum == hiblk_fixed) ? "YES" : "NO"),
                   is_extension_seg
                       ? "extension header (seg_num > 0) - HIBLK belongs to its primary, not this segment"
                       : (vr.ok ? "checksum OK" : vr.problem));

            if (!is_extension_seg && extent_sum != hiblk_fixed) mismatches++;
        }
    }

    printf("\n%d header(s) checked, %d hiblk/extent-sum mismatch(es)\n",
           checked, mismatches);

    ods2_dismount(&vol);
    return 0;
}

