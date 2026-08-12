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

/* ods2_dump_root.c - dumps root directory's raw content bytes,
 * hex-formatted, directly from a real disk image. Built to compare
 * actual on-disk bytes after ods2_create_directory() against what is
 * expected, byte-for-byte, without needing dd/base64/upload round trips.
 *
 * Usage: ods2_dump_root <disk-image>
 */
#include <stdio.h>
#include "ods2_volume.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    uint8_t root_header[512];
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count, i;
    unsigned total_blocks = 0, vbn;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <disk-image>\n", argv[0]);
        return 1;
    }

    r = ods2_mount(argv[1], &vol);
    if (!r.ok) {
        fprintf(stderr, "mount failed: %s\n", r.problem);
        return 1;
    }

    r = ods2_read_header(&vol, 4, root_header); /* root is always FID 4 */
    if (!r.ok) {
        fprintf(stderr, "could not read root header: %s\n", r.problem);
        return 1;
    }

    r = ods2_decode_all_extents(&vol, root_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) {
        fprintf(stderr, "could not decode root's extents: %s\n", r.problem);
        return 1;
    }

    printf("Root header: mpoffset=%u idoffset=%u map_inuse=%u filechar=0x%08x\n",
           ((ods2_head_core_t *) root_header)->mpoffset,
           ((ods2_head_core_t *) root_header)->idoffset,
           ((ods2_head_core_t *) root_header)->map_inuse,
           ((ods2_head_core_t *) root_header)->filechar);
    printf("Root extents (%d):\n", extent_count);
    for (i = 0; i < extent_count; i++) {
        printf("  LBN %u, %u blocks\n", extents[i].lbn, extents[i].block_count);
        total_blocks += extents[i].block_count;
    }
    printf("\n");

    for (vbn = 1; vbn <= total_blocks; vbn++) {
        uint8_t block[512];
        unsigned off;
        r = ods2_read_file_block(&vol, root_header, vbn, block);
        if (!r.ok) {
            fprintf(stderr, "could not read VBN %u: %s\n", vbn, r.problem);
            return 1;
        }
        printf("--- VBN %u ---\n", vbn);
        for (off = 0; off < 512; off += 16) {
            unsigned j;
            printf("%4u: ", off);
            for (j = 0; j < 16; j++) printf("%02x ", block[off + j]);
            printf(" ");
            for (j = 0; j < 16; j++) {
                unsigned char c = block[off + j];
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            printf("\n");
            /* Stop early as soon as clearly-unused trailing zeros are hit,
               to keep output manageable - print at most 4 more rows
               of all-zero content after the first all-zero row. */
            if (off > 0) {
                int all_zero = 1;
                for (j = 0; j < 16; j++) if (block[off + j] != 0) all_zero = 0;
                if (all_zero) {
                    unsigned k, rest_zero = 1;
                    for (k = off; k < 512; k++) if (block[k] != 0) rest_zero = 0;
                    if (rest_zero) {
                        printf("... (rest of block is zero) ...\n");
                        break;
                    }
                }
            }
        }
    }

    ods2_dismount(&vol);
    return 0;
}
