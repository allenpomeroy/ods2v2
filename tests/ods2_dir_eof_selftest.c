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

/* ods2_dir_eof_selftest - the end-of-file of a directory, on a copy of
 * the synthetic disk.
 *
 * A directory is whole blocks, and VMS writes its end-of-file as
 * EFBLK blocks-in-use+1, FFBYTE 0 (a one-block directory: EFBLK 2,
 * FFBYTE 0). ods2v2 once made new directories with EFBLK 1, FFBYTE 2.
 * OpenVMS VAX V7.3 copies the parent's FFBYTE into the directory that
 * CREATE/DIRECTORY makes inside such a directory, so the child got EOF
 * (2,2): a second block of two bytes that is not a directory block.
 * ANALYZE/DISK reports BAD_DIRSIZE and BAD_DIRTYPE for it and the
 * directory gives SYSTEM-W-BADIRECTORY. So:
 *
 *  - a new directory has EOF (2,0);
 *  - a directory with the old EOF (1,2) gets EOF (2,0) the first time
 *    it is written to (an insert or a delete);
 *  - a directory the VAX damaged - EOF (2,2), block 1 a valid empty
 *    block, block 2 all zeros - is repaired by the first write: EOF
 *    (2,0), the zero block outside it, block 1 holding the entry or,
 *    after a delete, its end-of-records marker. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_checksum.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/dir_eof_disk.img"

/* Copies a disk image, seeking over all-zero blocks so the (sparse)
   synthetic image stays sparse. */
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

static uint32_t efblk_of(const uint8_t *header)
{
    return ods2_word_swap32(((const ods2_head_core_t *) header)->recattr.efblk);
}

static uint16_t ffbyte_of(const uint8_t *header)
{
    return ((const ods2_head_core_t *) header)->recattr.ffbyte;
}

static unsigned hiblk_of(const uint8_t *header)
{
    return ods2_word_swap32(((const ods2_head_core_t *) header)->recattr.hiblk);
}

/* Rewrites a directory's header with the end-of-file (efblk, ffbyte). */
static void set_eof(ods2_volume_t *vol, unsigned dir_num, uint32_t efblk, uint16_t ffbyte)
{
    uint8_t h[512];
    ods2_head_core_t *core = (ods2_head_core_t *) h;
    uint16_t sum;

    assert(ods2_read_header(vol, dir_num, h).ok);
    core->recattr.efblk = ods2_word_swap32(efblk);
    core->recattr.ffbyte = ffbyte;
    sum = ods2_checksum(h, 255);
    h[510] = (uint8_t) (sum & 0xff);
    h[511] = (uint8_t) (sum >> 8);
    assert(ods2_write_header(vol, dir_num, h).ok);
}

static void make_file(ods2_volume_t *vol, unsigned dir_num, const char *name)
{
    uint8_t dir_header[512];
    ods2_fid_t fid;
    ods2_result_t r = ods2_read_header(vol, dir_num, dir_header);
    assert(r.ok);
    r = ods2_create_file(vol, dir_header, name, (const uint8_t *) "x\n", 2, 5, &fid);
    if (!r.ok) fprintf(stderr, "create %s: %s\n", name, r.problem);
    assert(r.ok);
}

static bool listed(ods2_volume_t *vol, unsigned dir_num, const char *name)
{
    uint8_t h[512];
    ods2_fid_t fid;
    assert(ods2_read_header(vol, dir_num, h).ok);
    return ods2_lookup_name(vol, h, name, &fid).ok;
}

/* The end-of-file is (2,0): one block in use, whole blocks. */
static void assert_eof_one_block(ods2_volume_t *vol, unsigned dir_num)
{
    uint8_t h[512];
    assert(ods2_read_header(vol, dir_num, h).ok);
    assert(efblk_of(h) == 2 && ffbyte_of(h) == 0);
    assert(ods2_file_content_length(h) == 512);
}

/* A new directory, as the starting point of each test. */
static unsigned new_directory(ods2_volume_t *vol, const char *name)
{
    uint8_t root_header[512];
    ods2_fid_t fid;
    assert(ods2_read_header(vol, 4, root_header).ok);
    assert(ods2_create_directory(vol, root_header, name, &fid).ok);
    return fid.fid_num;
}

int main(void)
{
    ods2_volume_t vol;
    uint8_t h[512], blk[512];
    unsigned d1, d2, d3, d4;
    int i;

    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);

    /* ---- a new directory has EOF (2,0) ---- */
    d1 = new_directory(&vol, "NEWDIR");
    assert_eof_one_block(&vol, d1);
    assert(ods2_read_header(&vol, d1, h).ok);
    assert(ods2_read_file_block(&vol, h, 1, blk).ok);
    assert(blk[0] == 0xff && blk[1] == 0xff);           /* the end-of-records marker */
    make_file(&vol, d1, "ONE.TXT");
    assert_eof_one_block(&vol, d1);                     /* a write leaves it alone */
    assert(listed(&vol, d1, "ONE.TXT"));
    printf("PASS: a new directory has EOF block 2, byte 0 (what VMS writes), and keeps it\n");

    /* ---- the old EOF (1,2) is put right by an insert ---- */
    d2 = new_directory(&vol, "OLDEOF1");
    set_eof(&vol, d2, 1, 2);
    assert(ods2_read_header(&vol, d2, h).ok && efblk_of(h) == 1 && ffbyte_of(h) == 2);
    make_file(&vol, d2, "ONE.TXT");
    assert_eof_one_block(&vol, d2);
    assert(listed(&vol, d2, "ONE.TXT"));
    printf("PASS: a directory with the old EOF (1,2) gets EOF (2,0) when an entry is inserted\n");

    /* ---- ... and by a delete ---- */
    set_eof(&vol, d2, 1, 2);
    assert(ods2_delete(&vol, d2, "ONE.TXT").ok);
    assert_eof_one_block(&vol, d2);
    assert(!listed(&vol, d2, "ONE.TXT"));
    assert(ods2_read_header(&vol, d2, h).ok);
    assert(ods2_read_file_block(&vol, h, 1, blk).ok && blk[0] == 0xff && blk[1] == 0xff);
    printf("PASS: a directory with the old EOF (1,2) gets EOF (2,0) when an entry is deleted\n");

    /* ---- a directory the VAX damaged: EOF (2,2), block 2 all zeros ---- */
    d3 = new_directory(&vol, "VAXDMG1");
    assert(ods2_read_header(&vol, d3, h).ok && hiblk_of(h) >= 2);   /* block 2 is allocated */
    set_eof(&vol, d3, 2, 2);
    assert(ods2_read_header(&vol, d3, h).ok);
    assert(ods2_file_content_length(h) == 514);
    assert(ods2_read_file_block(&vol, h, 2, blk).ok);
    for (i = 0; i < 512; i++) assert(blk[i] == 0);
    make_file(&vol, d3, "ONE.TXT");
    assert_eof_one_block(&vol, d3);
    assert(ods2_read_header(&vol, d3, h).ok);
    assert(ods2_read_file_block(&vol, h, 1, blk).ok);
    assert(blk[0] != 0 && !(blk[0] == 0xff && blk[1] == 0xff));    /* block 1 holds the entry */
    assert(listed(&vol, d3, "ONE.TXT"));
    printf("PASS: a directory with EOF (2,2) and a zero second block is put right by an insert: "
           "EOF (2,0), the entry in block 1\n");

    /* a delete then leaves block 1 with its marker, and the directory usable */
    assert(ods2_delete(&vol, d3, "ONE.TXT").ok);
    assert_eof_one_block(&vol, d3);
    assert(ods2_read_header(&vol, d3, h).ok);
    assert(ods2_read_file_block(&vol, h, 1, blk).ok && blk[0] == 0xff && blk[1] == 0xff);
    make_file(&vol, d3, "TWO.TXT");
    assert_eof_one_block(&vol, d3);
    assert(listed(&vol, d3, "TWO.TXT") && !listed(&vol, d3, "ONE.TXT"));
    printf("PASS: ... a delete from it leaves block 1 with the end-of-records marker, and it takes "
           "further entries\n");

    /* the damaged directory put right by a delete as the first write */
    d4 = new_directory(&vol, "VAXDMG2");
    make_file(&vol, d4, "ONE.TXT");
    make_file(&vol, d4, "TWO.TXT");
    set_eof(&vol, d4, 2, 2);
    assert(ods2_read_header(&vol, d4, h).ok);
    assert(ods2_read_file_block(&vol, h, 2, blk).ok);
    for (i = 0; i < 512; i++) assert(blk[i] == 0);
    assert(ods2_delete(&vol, d4, "ONE.TXT").ok);
    assert_eof_one_block(&vol, d4);
    assert(!listed(&vol, d4, "ONE.TXT") && listed(&vol, d4, "TWO.TXT"));
    printf("PASS: a directory with EOF (2,2) is put right when the first write is a delete\n");

    ods2_dismount(&vol);
    remove(DISK_PATH);
    printf("\nods2_dir_eof_selftest: all checks passed\n");
    return 0;
}
