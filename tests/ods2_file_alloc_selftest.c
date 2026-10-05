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

/* ods2_file_alloc_selftest - file number allocation against spec
 * 5.1.6/5.1.7, on a copy of the synthetic disk (INDEXF.SYS exactly as
 * VMS INITIALIZE left it: HIBLK 120, EFBLK 116, files 1-13 in use):
 *
 *  - a new header always lies inside INDEXF.SYS's end-of-file (VMS
 *    treats a header beyond it as a nonexistent file);
 *  - INDEXF.SYS is extended when its allocation runs out, staying
 *    self-consistent, with the backup header kept identical;
 *  - numbers come from the index bitmap, but a valid header is never
 *    reused even if its bit is clear; a deleted header gives seq+1,
 *    garbage gives 1;
 *  - a volume left with headers beyond the end-of-file by earlier
 *    ods2v2 versions is put right by the next create. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_validate.h"
#include "ods2_checksum.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/file_alloc_disk.img"

static void copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    static char buf[1 << 20];
    size_t n;
    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        assert(fwrite(buf, 1, n, out) == n);
    }
    fclose(in);
    fclose(out);
}

static uint32_t word_swap(uint32_t v) { return (v << 16) | (v >> 16); }

static void read_block(ods2_volume_t *vol, uint32_t lbn, uint8_t *block)
{
    assert(fseek(vol->fp, (long) lbn * 512, SEEK_SET) == 0);
    assert(fread(block, 1, 512, vol->fp) == 512);
}

static uint32_t header_vbn(const ods2_volume_t *vol, unsigned n)
{
    return (uint32_t) vol->home.cluster * 4 + vol->home.ibmapsize + n;
}

static bool bitmap_bit(ods2_volume_t *vol, unsigned n)
{
    uint8_t block[512];
    read_block(vol, vol->home.ibmaplbn + (n - 1) / 4096, block);
    return (block[((n - 1) % 4096) / 8] >> ((n - 1) % 8)) & 1;
}

static void set_bitmap_bit(ods2_volume_t *vol, unsigned n, bool used)
{
    uint8_t block[512];
    uint32_t lbn = vol->home.ibmaplbn + (n - 1) / 4096;
    read_block(vol, lbn, block);
    if (used) block[((n - 1) % 4096) / 8] |= (uint8_t) (1u << ((n - 1) % 8));
    else      block[((n - 1) % 4096) / 8] &= (uint8_t) ~(1u << ((n - 1) % 8));
    assert(ods2_write_block(vol, lbn, block).ok);
}

/* Checks the invariants on INDEXF.SYS after every change: its header
   is valid (checksum, HIBLK == sum of its extents), the backup copy is
   identical, and every file number in use has its header inside the
   end-of-file. Returns EFBLK. */
static uint32_t check_indexf(ods2_volume_t *vol, unsigned *highest_used_out)
{
    uint8_t ix[512], backup[512];
    const ods2_head_core_t *core = (const ods2_head_core_t *) ix;
    uint32_t efblk;
    unsigned n, highest = 0;

    assert(ods2_read_header(vol, 1, ix).ok);
    assert(ods2_validate_head(ix).ok);
    read_block(vol, vol->home.altidxlbn, backup);
    assert(memcmp(ix, backup, 512) == 0);
    efblk = word_swap(core->recattr.efblk);
    assert(core->recattr.ffbyte == 0);
    assert(efblk <= word_swap(core->recattr.hiblk) + 1);
    for (n = 1; n <= 4096; n++) {
        if (bitmap_bit(vol, n)) {
            uint8_t h[512];
            highest = n;
            assert(header_vbn(vol, n) < efblk);       /* visible to VMS */
            assert(ods2_read_header(vol, n, h).ok);
            assert(((ods2_head_core_t *) h)->fid.fid_num == n);
            assert(ods2_validate_head(h).ok);
        }
    }
    if (highest_used_out) *highest_used_out = highest;
    return efblk;
}

static ods2_fid_t create(ods2_volume_t *vol, const char *name)
{
    uint8_t root[512];
    ods2_fid_t fid;
    uint8_t content = 'x';
    ods2_result_t r;
    assert(ods2_read_header(vol, 4, root).ok);
    r = ods2_create_file(vol, root, name, &content, 1, 5, &fid);
    if (!r.ok) printf("create %s: %s\n", name, r.problem);
    assert(r.ok);
    return fid;
}

int main(void)
{
    ods2_volume_t vol;
    unsigned n, highest;
    uint16_t seq;
    uint32_t efblk;
    uint8_t ix[512];
    const ods2_head_core_t *core = (const ods2_head_core_t *) ix;

    copy_file(SOURCE_DISK_PATH, DISK_PATH);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);

    /* --- the fixture, as VMS left it --- */
    assert(ods2_read_header(&vol, 1, ix).ok);
    assert(word_swap(core->recattr.hiblk) == 120 && word_swap(core->recattr.efblk) == 116);
    assert(header_vbn(&vol, 14) == 116); /* file 14's header: exactly at the EOF */

    /* --- the issue: first header beyond VMS's end-of-file --- */
    assert(ods2_find_free_file_number(&vol, 1, 4096, &n, &seq).ok);
    assert(n == 14 && seq == 1);
    {
        ods2_fid_t fid = create(&vol, "FIRST.TXT");
        assert(fid.fid_num == 14 && fid.fid_seq == 1);
    }
    efblk = check_indexf(&vol, &highest);
    assert(efblk == 117 && highest == 14);
    printf("PASS: the first file past INITIALIZE's end-of-file moves EFBLK "
           "116 -> 117, so its header (VBN 116) is visible to VMS\n");

    /* --- run past HIBLK: INDEXF.SYS must grow --- */
    {
        char name[32];
        int i;
        for (i = 0; i < 10; i++) { /* files 15-24; 19+ need VBN > 120 */
            snprintf(name, sizeof name, "GROW%02d.TXT", i);
            create(&vol, name);
            efblk = check_indexf(&vol, &highest);
            assert(efblk == header_vbn(&vol, highest) + 1);
        }
        assert(ods2_read_header(&vol, 1, ix).ok);
        assert(word_swap(core->recattr.hiblk) > 120);
        assert(core->highwater == word_swap(core->recattr.hiblk) + 1);
        printf("PASS: INDEXF.SYS extended past HIBLK 120 (now %u, %d map words) "
               "with its header and backup consistent; every header inside EOF\n",
               (unsigned) word_swap(core->recattr.hiblk), core->map_inuse);
    }

    /* --- reuse rules --- */
    {
        /* A valid header is never reused, even if its bit is clear. */
        set_bitmap_bit(&vol, 15, false);
        assert(ods2_find_free_file_number(&vol, 1, 4096, &n, &seq).ok);
        assert(n != 15);
        set_bitmap_bit(&vol, 15, true);
        printf("PASS: a valid header (file 15) is not reused although its "
               "bitmap bit was cleared\n");
    }
    {
        /* A deleted header gives the next file seq + 1. */
        uint8_t h[512];
        ods2_fid_t fid;
        assert(ods2_delete(&vol, 4, "GROW03.TXT").ok); /* file 18 */
        assert(!bitmap_bit(&vol, 18));
        assert(ods2_find_free_file_number(&vol, 1, 4096, &n, &seq).ok);
        assert(n == 18 && seq == 2);
        fid = create(&vol, "REUSED.TXT");
        assert(fid.fid_num == 18 && fid.fid_seq == 2);
        assert(ods2_read_header(&vol, 18, h).ok);
        check_indexf(&vol, NULL);
        printf("PASS: a deleted header's slot is reused with sequence number "
               "+1 (18,1 -> 18,2)\n");
    }
    {
        /* Garbage inside the EOF gives seq 1. */
        uint8_t junk[512];
        assert(ods2_delete(&vol, 4, "GROW05.TXT").ok); /* file 20 */
        memset(junk, 0xA5, sizeof junk);
        assert(ods2_write_header(&vol, 20, junk).ok);
        assert(ods2_find_free_file_number(&vol, 1, 4096, &n, &seq).ok);
        assert(n == 20 && seq == 1);
        create(&vol, "OVERJUNK.TXT");
        check_indexf(&vol, NULL);
        printf("PASS: a garbage block inside the end-of-file is reused with "
               "sequence number 1\n");
    }

    /* --- repair: headers left beyond the EOF by earlier versions --- */
    {
        assert(ods2_read_header(&vol, 1, ix).ok);
        ((ods2_head_core_t *) ix)->recattr.efblk = word_swap(116); /* as old ods2v2 left it */
        {
            uint16_t c = ods2_checksum(ix, 255);
            ix[510] = (uint8_t) c; ix[511] = (uint8_t) (c >> 8);
        }
        assert(ods2_write_header(&vol, 1, ix).ok);
        assert(ods2_write_block(&vol, vol.home.altidxlbn, ix).ok);
        create(&vol, "HEAL.TXT");
        efblk = check_indexf(&vol, &highest);
        assert(efblk == header_vbn(&vol, highest) + 1);
        printf("PASS: on a volume whose headers lie beyond the end-of-file (as "
               "ods2v2 before this fix left them), the next create moves the "
               "EOF past all of them\n");
    }

    ods2_dismount(&vol);
    remove(DISK_PATH);
    printf("\nods2_file_alloc_selftest: all checks passed\n");
    return 0;
}
