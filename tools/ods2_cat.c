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

/* ods2_cat.c - prints a file's content from a real ODS-2 disk image
 * (a TYPE/COPY-to-stdout equivalent). Binary-safe: writes raw bytes
 * to stdout, does not assume text content.
 *
 * Usage: ods2_cat <disk-image> <dir-path> <filename>
 *   ods2_cat transfer.dsk DECUS.NETLIB020 AAAREADME.DOC
 *   ods2_cat transfer.dsk "" INDEXF.SYS > indexf_copy.sys
 */
#include <stdio.h>
#include <stdlib.h>
#include "ods2_volume.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    ods2_fid_t dir_fid, file_fid;
    uint8_t dir_header[512], file_header[512];
    uint8_t *buf;
    size_t buf_size;
    size_t bytes_read = 0;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <disk-image> <dir-path> <filename>\n", argv[0]);
        fprintf(stderr, "  dir-path is dot-separated, no brackets (empty string for root)\n");
        fprintf(stderr, "  example: %s transfer.dsk DECUS.NETLIB020 AAAREADME.DOC\n", argv[0]);
        return 1;
    }

    r = ods2_mount(argv[1], &vol);
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

    r = ods2_read_header(&vol, dir_fid.fid_num, dir_header);
    if (!r.ok) {
        fprintf(stderr, "could not read directory header: %s\n", r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_lookup_name(&vol, dir_header, argv[3], &file_fid);
    if (!r.ok) {
        fprintf(stderr, "%s: not found: %s\n", argv[3], r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_read_header(&vol, file_fid.fid_num, file_header);
    if (!r.ok) {
        fprintf(stderr, "could not read file's own header: %s\n", r.problem);
        ods2_dismount(&vol);
        return 1;
    }

    /* Size the buffer exactly to this file's own stated content
       length (from EFBLK/FFBYTE, already in the header we just read)
       rather than a fixed cap - a multi-header file can be far larger
       than a hobbyist-scale hard-coded ceiling would allow. */
    buf_size = ods2_file_content_length(file_header);
    buf = malloc(buf_size > 0 ? buf_size : 1);
    if (buf == NULL) {
        fprintf(stderr, "could not allocate read buffer\n");
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_read_file(&vol, file_header, buf, buf_size, &bytes_read);
    if (!r.ok) {
        fprintf(stderr, "could not read file content: %s\n", r.problem);
        free(buf);
        ods2_dismount(&vol);
        return 1;
    }

    fwrite(buf, 1, bytes_read, stdout);

    free(buf);
    ods2_dismount(&vol);
    return 0;
}

