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

/* Diagnostic - how many INDEXF.SYS header slots does this volume
 * have room for in total, and how many are currently free? */
#include <stdio.h>
#include "ods2_volume.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    uint8_t indexf_header[512];
    const ods2_head_core_t *core;
    unsigned v, m;
    uint32_t hiblk;
    unsigned max_file_number;
    unsigned n, used = 0, free_slots = 0, first_free = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <disk-image>\n", argv[0]);
        return 1;
    }

    r = ods2_mount(argv[1], &vol);
    if (!r.ok) { fprintf(stderr, "mount: %s\n", r.problem); return 1; }

    r = ods2_read_header(&vol, 1, indexf_header); /* INDEXF.SYS is always file 1 */
    if (!r.ok) { fprintf(stderr, "read INDEXF.SYS header: %s\n", r.problem); return 1; }
    core = (const ods2_head_core_t *) indexf_header;
    hiblk = ods2_word_swap32(core->recattr.hiblk);

    v = vol.home.cluster;
    m = vol.home.ibmapsize;
    /* Matches header_lbn()'s own formula in ods2_volume.c:
       target_vbn = v*4 + m + file_number, so the header area runs
       from file_number=1 up to whatever hiblk covers. */
    max_file_number = hiblk - (v * 4 + m);

    printf("INDEXF.SYS hiblk=%u, cluster=%u, ibmapsize=%u\n", hiblk, v, m);
    printf("Header-storage region covers file numbers 1..%u (%u total slots)\n",
           max_file_number, max_file_number);

    for (n = 1; n <= max_file_number; n++) {
        uint8_t header[512];
        const ods2_head_core_t *c;
        r = ods2_read_header(&vol, n, header);
        if (!r.ok) { printf("  (file %u: could not read - %s)\n", n, r.problem); break; }
        c = (const ods2_head_core_t *) header;
        if (c->fid.fid_num != 0) {
            used++;
        } else {
            free_slots++;
            if (first_free == 0) first_free = n;
        }
    }

    printf("Used: %u   Free: %u   (first free file number: %u)\n", used, free_slots, first_free);
    ods2_dismount(&vol);
    return 0;
}
