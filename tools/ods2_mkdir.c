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

/* ods2_mkdir.c - creates a new directory on a real ODS-2 disk image.
 *
 * Usage: ods2_mkdir <disk-image> <parent-path> <new-name>
 *   ods2_mkdir transfer.dsk "" DECUS               (creates [DECUS])
 *   ods2_mkdir transfer.dsk DECUS NETLIB020         (creates [DECUS.NETLIB020])
 */
#include <stdio.h>
#include <stdlib.h>
#include "ods2_volume.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    ods2_fid_t parent_fid, new_fid;
    uint8_t parent_header[512];

    if (argc != 4) {
        fprintf(stderr, "usage: %s <disk-image> <parent-path> <new-name>\n", argv[0]);
        fprintf(stderr, "  parent-path is dot-separated, no brackets (empty string for root)\n");
        fprintf(stderr, "  new-name has no .DIR suffix or version - added automatically\n");
        fprintf(stderr, "  example: %s transfer.dsk DECUS NETLIB020\n", argv[0]);
        return 1;
    }

    r = ods2_mount_write(argv[1], &vol);
    if (!r.ok) {
        fprintf(stderr, "%s: mount failed: %s\n", argv[1], r.problem);
        return 1;
    }

    r = ods2_lookup_path(&vol, argv[2], &parent_fid);
    if (!r.ok) {
        fprintf(stderr, "%s: parent path lookup failed: %s\n", argv[2], r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_read_header(&vol, parent_fid.fid_num, parent_header);
    if (!r.ok) {
        fprintf(stderr, "could not read parent directory header: %s\n", r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_create_directory(&vol, parent_header, argv[3], &new_fid);
    if (!r.ok) {
        fprintf(stderr, "could not create directory: %s\n", r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    printf("Created %s.DIR;1, FID=(%u,%u,%u,%u)\n", argv[3],
           new_fid.fid_num, new_fid.fid_nmx, new_fid.fid_seq, new_fid.fid_rvn);

    ods2_dismount(&vol);
    return 0;
}
