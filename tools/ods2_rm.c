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

/* ods2_rm.c - deletes a file or (empty) directory on a real ODS-2
 * disk image.
 *
 * Usage: ods2_rm <disk-image> <dir-path> <name>
 *   ods2_rm transfer.dsk DECUS.NETLIB020 OLDFILE.TXT
 *   ods2_rm transfer.dsk DECUS EMPTYDIR.DIR
 *
 * Refuses to delete a non-empty directory, matching VMS's own
 * DELETE/DIRECTORY behavior.
 */
#include <stdio.h>
#include "ods2_volume.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    ods2_fid_t dir_fid;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <disk-image> <dir-path> <name>\n", argv[0]);
        fprintf(stderr, "  dir-path is dot-separated, no brackets (empty string for root)\n");
        fprintf(stderr, "  name includes any suffix, e.g. HELLO.TXT or EMPTYDIR.DIR\n");
        return 1;
    }

    r = ods2_mount_write(argv[1], &vol);
    if (!r.ok) {
        fprintf(stderr, "%s: mount failed: %s\n", argv[1], r.problem);
        return 1;
    }

    r = ods2_lookup_path(&vol, argv[2], &dir_fid);
    if (!r.ok) {
        fprintf(stderr, "%s: path lookup failed: %s\n", argv[2], r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_delete(&vol, dir_fid.fid_num, argv[3]);
    if (!r.ok) {
        fprintf(stderr, "could not delete %s: %s\n", argv[3], r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    printf("Deleted %s\n", argv[3]);

    ods2_dismount(&vol);
    return 0;
}
