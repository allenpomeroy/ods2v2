/*
 * MIT License
 *
 * Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer)
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

/* ods2_fid_selftest - a directory entry names a file by number, sequence number, relative volume number and extended
 * file number (FID_NMX: the file number is NMX << 16 | NUM). The header used to be found by the low 16 bits of the
 * number alone, so
 *   - an entry with an RVN (a file on another volume of a set) or an NMX (a file number above 65,535) silently read
 *     another file's header, and a delete or a directory walk went through it;
 *   - an entry whose sequence number is not the header's (the file was deleted and its number reused: VMS gives a
 *     reused number a new sequence number, and calls the entry's file "no such file") opened the file that now has
 *     the number.
 * ods2_read_header_fid() refuses all of them. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/fid_disk.img"

static void copy_sparse(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    static const uint8_t zero[512];
    uint8_t buf[512];
    size_t n;
    long total = 0;

    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (n == sizeof(buf) && memcmp(buf, zero, sizeof(buf)) == 0) {
            assert(fseek(out, (long) n, SEEK_CUR) == 0);
        } else {
            assert(fwrite(buf, 1, n, out) == n);
        }
        total += (long) n;
    }
    assert(fseek(out, total - 1, SEEK_SET) == 0);
    assert(fputc(0, out) != EOF);
    fclose(in);
    fclose(out);
}

int main(void)
{
    ods2_volume_t vol;
    uint8_t root[512], hdr[512];
    ods2_fid_t fid, bad, found;
    ods2_result_t r;

    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    assert(ods2_read_header(&vol, 4, root).ok);
    assert(ods2_create_file(&vol, root, "REAL.TXT", (const uint8_t *) "real\n", 5, 5, &fid).ok);

    /* the right FID reads its header */
    assert(ods2_read_header_fid(&vol, fid, hdr).ok);
    printf("PASS: the FID of a file reads its header\n");

    /* an RVN or an NMX is refused, not read as another file's header */
    bad = fid; bad.fid_rvn = 1;
    r = ods2_read_header_fid(&vol, bad, hdr);
    assert(!r.ok && strstr(r.problem, "volume sets") != NULL);
    bad = fid; bad.fid_nmx = 1;
    r = ods2_read_header_fid(&vol, bad, hdr);
    assert(!r.ok && strstr(r.problem, "65,535") != NULL);
    printf("PASS: a FID with a relative volume number or an extended file number is refused\n");

    /* a sequence number that is not the header's: the file is gone */
    bad = fid; bad.fid_seq = (uint16_t) (fid.fid_seq + 1);
    r = ods2_read_header_fid(&vol, bad, hdr);
    assert(!r.ok && r.problem == ods2_stale_entry);
    printf("PASS: an entry whose sequence number is not the header's is a stale entry, not the file\n");

    /* entries in a directory that carry such FIDs: a path through one is refused, as is a lookup of its target */
    {
        ods2_fid_t d;
        uint8_t dh[512];
        assert(ods2_create_directory(&vol, root, "TRAP", &d).ok);
        assert(ods2_read_header(&vol, d.fid_num, dh).ok);
        /* TRAP.DIR is a real directory; BADRVN.DIR / BADNMX.DIR are entries in [000000] naming the directory TRAP
           with another RVN / NMX: walking into them must not land in TRAP */
        bad = d; bad.fid_rvn = 1;
        assert(ods2_insert_into_directory(&vol, 4, "BADRVN.DIR", 1, bad).ok);
        bad = d; bad.fid_nmx = 1;
        assert(ods2_insert_into_directory(&vol, 4, "BADNMX.DIR", 1, bad).ok);
        bad = d; bad.fid_seq = (uint16_t) (d.fid_seq + 1);
        assert(ods2_insert_into_directory(&vol, 4, "STALE.DIR", 1, bad).ok);
        assert(ods2_lookup_path(&vol, "TRAP", &found).ok);
        assert(!ods2_lookup_path(&vol, "BADRVN", &found).ok);
        assert(!ods2_lookup_path(&vol, "BADNMX", &found).ok);
        assert(!ods2_lookup_path(&vol, "STALE", &found).ok);
        printf("PASS: a path through an entry with a wrong RVN, NMX or sequence number is refused\n");
    }

    /* a deleted file's header (marked for delete, FID zeroed) is not a file either */
    assert(ods2_delete_version(&vol, 4, "REAL.TXT", 1, NULL).ok);
    r = ods2_read_header_fid(&vol, fid, hdr);
    assert(!r.ok && r.problem == ods2_stale_entry);
    printf("PASS: the header of a deleted file is not a file\n");

    ods2_dismount(&vol);
    remove(DISK_PATH);
    printf("\nods2_fid_selftest: all checks passed\n");
    return 0;
}
