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
 * Usage: ods2_put <disk-image> <local-file> <dir-path> <name> [rtype [rsize [rattrib]]]
 *   ods2_put transfer.dsk readme.txt DECUS.NETLIB020 AAAREADME.DOC
 *   ods2_put transfer.dsk kit.pcsi '' KIT.PCSI 1 8192
 *   rtype defaults to 5 (FAB$C_STMLF, stream-LF - typical for text);
 *   1 (FAB$C_FIX) is fixed-length records of rsize bytes (default
 *   512), and the file must be a whole number of them; 0 (UDF), 4
 *   (STM) and 6 (STMCR) are the other formats. rattrib is the RMS
 *   record attributes byte (default 0; 2 = CR carriage control).
 *   See ods2_check_record_attrs() for what is accepted.
 *
 * Any size of file the volume has room for - see ods2_create_file().
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "ods2_volume.h"

/* Reads all of `path` into a malloc'd buffer (*buf_out, *len_out),
   however large: sized from the file's own length, growing as needed
   for something without one (a pipe). Returns false and prints why
   on failure. */
static bool read_whole_file(const char *path, uint8_t **buf_out, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    size_t cap = 0, len = 0;
    long size;

    if (f == NULL) {
        fprintf(stderr, "%%ODS2-E-OPENIN, could not open %s\n", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        cap = (size_t) size + 1; /* +1: a read that fills it means the file grew */
    } else {
        clearerr(f);
        cap = 1u << 20;
    }
    for (;;) {
        size_t n;
        if (len == cap || buf == NULL) {
            uint8_t *grown;
            if (buf != NULL) cap *= 2;
            grown = realloc(buf, cap);
            if (grown == NULL) {
                fprintf(stderr, "%%ODS2-E-NOMEM, could not allocate %zu bytes to read %s\n",
                        cap, path);
                free(buf);
                fclose(f);
                return false;
            }
            buf = grown;
        }
        n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    if (ferror(f)) {
        fprintf(stderr, "%%ODS2-E-READERR, error reading %s\n", path);
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    *buf_out = buf;
    *len_out = len;
    return true;
}

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    ods2_fid_t dir_fid, new_fid;
    uint8_t dir_header[512];
    uint8_t *buf;
    size_t content_len;
    ods2_record_attrs_t attrs;
    unsigned long values[3] = { ODS2_RFM_STMLF, 512, 0 }; /* rtype, rsize, rattrib */
    const char *why;
    int i;

    if (argc < 5 || argc > 8) {
        fprintf(stderr, "usage: %s <disk-image> <local-file> <dir-path> <name> "
                        "[rtype [rsize [rattrib]]]\n", argv[0]);
        fprintf(stderr, "  dir-path is dot-separated, no brackets (empty string for root)\n");
        fprintf(stderr, "  rtype: 5=FAB$C_STMLF (default, text), 1=FAB$C_FIX (fixed-length\n"
                        "         records, e.g. binary), 0=UDF, 4=STM, 6=STMCR\n");
        fprintf(stderr, "  rsize: record size in bytes (default 512); for FIX the file must\n"
                        "         be a whole number of records - e.g. 1 8192 for a PCSI kit\n");
        fprintf(stderr, "  rattrib: RMS record attributes (default 0; 2=CR carriage control)\n");
        return 1;
    }
    for (i = 5; i < argc; i++) {
        char *stop;
        values[i - 5] = strtoul(argv[i], &stop, 10);
        if (argv[i][0] == '\0' || argv[i][0] == '-' || *stop != '\0' ||
            values[i - 5] > (i == 6 ? 65535ul : 255ul)) {
            fprintf(stderr, "%s: not a valid %s\n", argv[i],
                    (i == 5) ? "rtype" : (i == 6) ? "rsize" : "rattrib");
            return 1;
        }
    }
    attrs.rtype = (uint8_t) values[0];
    attrs.rsize = (uint16_t) values[1];
    attrs.maxrec = (uint16_t) values[1]; /* MRS = record size, as VMS sets it */
    attrs.rattrib = (uint8_t) values[2];

    if (!read_whole_file(argv[2], &buf, &content_len)) {
        return 1;
    }
    /* Checked before touching the volume. */
    why = ods2_check_record_attrs(&attrs, content_len);
    if (why != NULL) {
        fprintf(stderr, "%s: %s\n", argv[2], why);
        free(buf);
        return 1;
    }

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

    r = ods2_create_file_attrs(&vol, dir_header, argv[4], buf, content_len, &attrs, &new_fid);
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
