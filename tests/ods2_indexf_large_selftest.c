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

/* ods2_indexf_large_selftest - INDEXF.SYS above LBN 4,194,304
 * (issue #14).
 *
 * A Format 1 retrieval pointer can't address an LBN past 4,194,302,
 * so INDEXF.SYS used to stop growing on a large volume whose free
 * space lay above that. ods2_indexf_map_extent() now uses the
 * smallest pointer that fits:
 *
 *  - below the limit it is still Format 1 (2 words);
 *  - above it, Format 2 (3 words), decoding back to the same extent,
 *    with HIBLK and HIGHWATER raised;
 *  - when the map area is full it fails and leaves the header as it
 *    was.
 *
 * Then, end to end on a sparse copy of the synthetic disk grown past
 * 5,000,256 blocks: INDEXF.SYS is given an extent at LBN 5,000,000
 * (a test fixture - the synthetic disk's bitmap is too small for the
 * allocator ever to return such an LBN itself), the volume is
 * remounted, and new files are created until their headers must lie
 * in that extent. Every one is found at the LBN expected, valid, and
 * INDEXF.SYS's header and backup stay identical and valid. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include "ods2_volume.h"
#include "ods2_validate.h"
#include "ods2_checksum.h"
#include "ods2_retrieval.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/indexf_large_disk.img"
#define HIGH_LBN         5000000u
#define HIGH_BLOCKS      256u

static uint32_t word_swap(uint32_t v) { return (v << 16) | (v >> 16); }

/* Copies the (mostly empty) image without writing its runs of zeros,
   so the copy stays sparse and quick. */
static void copy_sparse(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    static char buf[1 << 20];
    static const char zero[1 << 20];
    size_t n;
    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (n == sizeof(buf) && memcmp(buf, zero, n) == 0) {
            assert(fseek(out, (long) n, SEEK_CUR) == 0);
        } else {
            assert(fwrite(buf, 1, n, out) == n);
        }
    }
    fclose(in);
    fclose(out);
}

static void read_block(ods2_volume_t *vol, uint32_t lbn, uint8_t *block)
{
    assert(fseek(vol->fp, (long) lbn * 512, SEEK_SET) == 0);
    assert(fread(block, 1, 512, vol->fp) == 512);
}

static uint32_t header_vbn(const ods2_volume_t *vol, unsigned n)
{
    return (uint32_t) vol->home.cluster * 4 + vol->home.ibmapsize + n;
}

static int decode(const uint8_t *header, ods2_extent_t *ext, size_t max)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    return ods2_decode_retrieval_pointers(header + (size_t) core->mpoffset * 2,
                                          core->map_inuse, ext, max);
}

static void set_checksum(uint8_t *header)
{
    uint16_t c = ods2_checksum(header, 255);
    header[510] = (uint8_t) (c & 0xff);
    header[511] = (uint8_t) (c >> 8);
}

int main(void)
{
    ods2_volume_t vol;
    uint8_t ix[512], before[512], root[512];
    ods2_head_core_t *core = (ods2_head_core_t *) ix;
    ods2_extent_t ext[ODS2_MAX_EXTENTS];
    int count, base_count;
    uint32_t hiblk;
    unsigned words;

    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    /* Room for an extent at LBN 5,000,000 (zeros, sparse). */
    assert(truncate(DISK_PATH, (off_t) (HIGH_LBN + HIGH_BLOCKS + 16) * 512) == 0);

    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    assert(ods2_read_header(&vol, 1, ix).ok);
    base_count = decode(ix, ext, ODS2_MAX_EXTENTS);
    assert(base_count > 0);

    /* --- below the limit: still Format 1 --- */
    memcpy(before, ix, 512);
    hiblk = word_swap(core->recattr.hiblk);
    words = core->map_inuse;
    assert(ods2_indexf_map_extent(ix, 2000000u, 9).ok);
    assert(core->map_inuse == words + 2);
    assert(word_swap(core->recattr.hiblk) == hiblk + 9);
    count = decode(ix, ext, ODS2_MAX_EXTENTS);
    assert(count == base_count + 1);
    assert(ext[count - 1].lbn == 2000000u && ext[count - 1].block_count == 9);
    printf("PASS: an extent below LBN 4,194,303 is mapped with a Format 1 pointer\n");

    /* --- above it: Format 2 --- */
    memcpy(ix, before, 512);
    assert(ods2_indexf_map_extent(ix, HIGH_LBN, HIGH_BLOCKS).ok);
    assert(core->map_inuse == words + 3);
    assert(word_swap(core->recattr.hiblk) == hiblk + HIGH_BLOCKS);
    if (core->highwater != 0) assert(core->highwater == hiblk + HIGH_BLOCKS + 1);
    count = decode(ix, ext, ODS2_MAX_EXTENTS);
    assert(count == base_count + 1);
    assert(ext[count - 1].lbn == HIGH_LBN && ext[count - 1].block_count == HIGH_BLOCKS);
    printf("PASS: an extent at LBN %u is mapped with a Format 2 pointer and decodes "
           "back to the same extent; HIBLK and HIGHWATER follow\n", HIGH_LBN);

    /* --- map area full: refused, header unchanged --- */
    {
        uint8_t full[512];
        int added = 0;
        memcpy(full, before, 512);
        while (ods2_indexf_map_extent(full, HIGH_LBN + 1000u * (unsigned) added, 8).ok) {
            added++;
            assert(added < 300);
        }
        {
            uint8_t snapshot[512];
            memcpy(snapshot, full, 512);
            assert(!ods2_indexf_map_extent(full, HIGH_LBN, 8).ok);
            assert(memcmp(snapshot, full, 512) == 0);
        }
        printf("PASS: with the map area full (%d Format 2 pointers added) the next "
               "extent is refused and the header left unchanged\n", added);
    }

    /* --- end to end: headers stored above LBN 4,194,304 --- */
    memcpy(ix, before, 512);
    assert(ods2_indexf_map_extent(ix, HIGH_LBN, HIGH_BLOCKS).ok);
    set_checksum(ix);
    assert(ods2_validate_head(ix).ok);
    assert(ods2_write_header(&vol, 1, ix).ok);
    assert(ods2_write_block(&vol, vol.home.altidxlbn, ix).ok);
    ods2_dismount(&vol);

    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    {
        uint32_t old_hiblk = hiblk; /* VBNs past this lie in the new extent */
        int i, high = 0;
        for (i = 0; i < 40; i++) {
            char name[32];
            ods2_fid_t fid;
            uint8_t content = 'x', h[512];
            uint32_t vbn;
            ods2_result_t r;

            snprintf(name, sizeof name, "HIGH%02d.TXT", i);
            assert(ods2_read_header(&vol, 4, root).ok);
            r = ods2_create_file(&vol, root, name, &content, 1, 5, &fid);
            if (!r.ok) printf("create %s: %s\n", name, r.problem);
            assert(r.ok);

            vbn = header_vbn(&vol, fid.fid_num);
            if (vbn > old_hiblk) {
                /* Read the raw block where the header must be. */
                read_block(&vol, HIGH_LBN + (vbn - old_hiblk - 1), h);
                assert(ods2_validate_head(h).ok);
                assert(((ods2_head_core_t *) h)->fid.fid_num == fid.fid_num);
                high++;
            }
        }
        assert(high > 20);
        {
            uint8_t backup[512];
            assert(ods2_read_header(&vol, 1, ix).ok);
            assert(ods2_validate_head(ix).ok);
            read_block(&vol, vol.home.altidxlbn, backup);
            assert(memcmp(ix, backup, 512) == 0);
            assert(word_swap(core->recattr.efblk) <= word_swap(core->recattr.hiblk) + 1);
        }
        {
            ods2_fid_t fid;
            assert(ods2_read_header(&vol, 4, root).ok);
            assert(ods2_lookup_name(&vol, root, "HIGH39.TXT", &fid).ok);
        }
        printf("PASS: %d new headers stored in INDEXF.SYS's extent at LBN %u, each "
               "valid at the LBN expected; INDEXF.SYS's header and backup agree\n",
               high, HIGH_LBN);
    }

    ods2_dismount(&vol);
    remove(DISK_PATH);
    printf("\nods2_indexf_large_selftest: all checks passed\n");
    return 0;
}
