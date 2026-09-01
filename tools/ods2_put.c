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

/* ods2_put.c - writes a local file's content onto a real ODS-2 disk
 * image (an IMPORT/COPY-in equivalent).
 *
 * Usage: ods2_put <disk-image> <local-file> <dir-path> <name> [rtype]
 *   ods2_put transfer.dsk readme.txt DECUS.NETLIB020 AAAREADME.DOC
 *   rtype defaults to 5 (FAB$C_STMLF, stream-LF - typical for text);
 *   pass 1 (FAB$C_FIX) for binary content.
 *
 * Supports files of any size ods2_create_file() itself supports -
 * chained extension headers (spec 3.3) let a file span far more than
 * one header's ~77-extent map area when needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include "ods2_volume.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    ods2_fid_t dir_fid, new_fid;
    uint8_t dir_header[512];
    FILE *local_f;
    uint8_t *buf;
    long file_size;
    size_t buf_size;
    size_t content_len;
    uint8_t rtype;

    if (argc < 5 || argc > 6) {
        fprintf(stderr, "usage: %s <disk-image> <local-file> <dir-path> <name> [rtype]\n", argv[0]);
        fprintf(stderr, "  dir-path is dot-separated, no brackets (empty string for root)\n");
        fprintf(stderr, "  rtype: 5=FAB$C_STMLF (default, text), 1=FAB$C_FIX (binary)\n");
        return 1;
    }
    rtype = (argc == 6) ? (uint8_t) atoi(argv[5]) : 5;

    local_f = fopen(argv[2], "rb");
    if (local_f == NULL) {
        perror(argv[2]);
        return 1;
    }
    /* Size the read buffer to the actual local file - see
       ods2_create_file()'s documentation for how large a file it can
       now hold (chained extension headers, not just a single ~9.5MB
       header's worth). */
    if (fseek(local_f, 0, SEEK_END) != 0 || (file_size = ftell(local_f)) < 0 ||
        fseek(local_f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: could not determine file size\n", argv[2]);
        fclose(local_f);
        return 1;
    }
    buf_size = (size_t) file_size;
    buf = malloc(buf_size > 0 ? buf_size : 1);
    if (buf == NULL) {
        fprintf(stderr, "could not allocate read buffer\n");
        fclose(local_f);
        return 1;
    }
    content_len = fread(buf, 1, buf_size, local_f);
    if (content_len != buf_size) {
        fprintf(stderr, "%s: could not read the whole file\n", argv[2]);
        free(buf);
        fclose(local_f);
        return 1;
    }
    fclose(local_f);

    r = ods2_mount_write(argv[1], &vol);
    if (!r.ok) {
        fprintf(stderr, "%s: mount failed: %s\n", argv[1], r.problem);
        free(buf);
        return 1;
    }

    r = ods2_lookup_path(&vol, argv[3], &dir_fid);
    if (!r.ok) {
        fprintf(stderr, "%s: path lookup failed: %s\n", argv[3], r.problem);
        free(buf);
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_read_header(&vol, dir_fid.fid_num, dir_header);
    if (!r.ok) {
        fprintf(stderr, "could not read directory header: %s\n", r.problem);
        free(buf);
        ods2_dismount(&vol);
        return 1;
    }

    r = ods2_create_file(&vol, dir_header, argv[4], buf, content_len, rtype, &new_fid);
    if (!r.ok) {
        fprintf(stderr, "could not create file: %s\n", r.problem);
        free(buf);
        ods2_dismount(&vol);
        return 1;
    }

    printf("Wrote %zu bytes to %s;1, FID=(%u,%u,%u,%u)\n", content_len, argv[4],
           new_fid.fid_num, new_fid.fid_nmx, new_fid.fid_seq, new_fid.fid_rvn);

    free(buf);
    ods2_dismount(&vol);
    return 0;
}
