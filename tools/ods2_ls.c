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

/* ods2_ls.c - lists a directory on a real ODS-2 disk image.
 *
 * Usage: ods2_ls <disk-image> [path] [wildcard]
 *   path is dot-separated with no brackets, e.g.:
 *     ods2_ls transfer.dsk                         (lists root, [000000])
 *     ods2_ls transfer.dsk DECUS                    (lists [DECUS])
 *     ods2_ls transfer.dsk DECUS.NETLIB020           (lists [DECUS.NETLIB020])
 *     ods2_ls transfer.dsk DECUS.NETLIB020 "*.DIR"   (wildcard filter)
 */
#include <stdio.h>
#include <stdlib.h>
#include "ods2_volume.h"
#include "ods2_wildcard.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    ods2_fid_t dir_fid;
    uint8_t dir_header[512];
    ods2_dir_entry_t entries[512];
    int count = 0;
    int i;
    int shown = 0;
    const char *disk_path;
    const char *dir_path;
    const char *pattern;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <disk-image> [path] [wildcard]\n", argv[0]);
        fprintf(stderr, "  path is dot-separated, no brackets, e.g. DECUS.NETLIB020\n");
        fprintf(stderr, "  wildcard supports '*' and '%%', e.g. \"*.DIR\"\n");
        return 1;
    }
    disk_path = argv[1];
    dir_path = (argc > 2) ? argv[2] : "";
    pattern = (argc > 3) ? argv[3] : "*";

    r = ods2_mount(disk_path, &vol);
    if (!r.ok) {
        fprintf(stderr, "%s: mount failed: %s\n", disk_path, r.problem);
        return 1;
    }

    r = ods2_lookup_path(&vol, dir_path, &dir_fid);
    if (!r.ok) {
        fprintf(stderr, "%s: path lookup failed: %s\n", dir_path, r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_read_header(&vol, dir_fid.fid_num, dir_header);
    if (!r.ok) {
        fprintf(stderr, "could not read directory header: %s\n", r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_list_directory(&vol, dir_header, entries,
                             sizeof(entries) / sizeof(entries[0]), &count);
    if (!r.ok) {
        fprintf(stderr, "could not list directory: %s\n", r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    printf("Directory [%s]\n\n", (dir_path[0] == '\0') ? "000000" : dir_path);
    for (i = 0; i < count; i++) {
        if (!ods2_wildcard_match(pattern, entries[i].name)) continue;
        printf("%-20s;%u\n", entries[i].name, entries[i].version);
        shown++;
    }
    printf("\nTotal of %d file%s.\n", shown, (shown == 1) ? "" : "s");

    ods2_dismount(&vol);
    return 0;
}
