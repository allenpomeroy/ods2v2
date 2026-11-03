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

/* ods2_large_file_selftest - large file import (issue #2, and the
 * regression report that followed it), on a copy of the synthetic
 * disk:
 *
 *  - a 20 MiB file - past the old 12 MiB tool buffer and past the
 *    ~9.6 MiB a single header held with Format 1 pointers only - is
 *    stored as one contiguous extent and reads back byte for byte;
 *  - on deliberately fragmented free space, a file needing more
 *    retrieval pointers than one header holds continues in extension
 *    headers (spec 3.3): every header valid, chained through EXT_FID
 *    with SEG_NUM 0,1,2..., each extension backlinked to the primary,
 *    Formats 1 and 2 mixed, content intact;
 *  - a copy too big for the free space fails and gives back every
 *    block and file number it took;
 *  - deleting a multi-header file frees all its blocks and every
 *    header, primary and extension.
 *
 * Leaves samples/large_file_disk.img (with the 20 MiB file on it) for
 * tests/cli_large_file_check.sh. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_validate.h"
#include "ods2_header_build.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/large_file_disk.img"

/* Copies the (mostly empty) 1.4 GiB image without writing its runs
   of zeros, so the copy stays sparse and quick. */
static void copy_sparse(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    static char buf[1 << 20];
    static const char zero[1 << 20];
    size_t n;
    long total = 0;
    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (n == sizeof(buf) && memcmp(buf, zero, n) == 0) {
            assert(fseek(out, (long) n, SEEK_CUR) == 0);
        } else {
            assert(fwrite(buf, 1, n, out) == n);
        }
        total += (long) n;
    }
    /* Make sure a trailing hole still gives the full length. */
    assert(fseek(out, total - 1, SEEK_SET) == 0);
    {
        char last;
        assert(fseek(in, total - 1, SEEK_SET) == 0);
        assert(fread(&last, 1, 1, in) == 1);
        assert(fwrite(&last, 1, 1, out) == 1);
    }
    fclose(in);
    fclose(out);
}

static uint8_t *pattern(size_t len, unsigned seed)
{
    uint8_t *p = malloc(len);
    uint32_t x = seed * 2654435761u + 1;
    size_t i;
    assert(p != NULL);
    for (i = 0; i < len; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (uint8_t) (x >> 16);
    }
    return p;
}

/* Free clusters according to BITMAP.SYS (VBN 1 is the SCB). */
/* A content source that delivers zeros, then fails after
   `fail_after` bytes, recording the largest chunk asked for. */
typedef struct {
    size_t delivered, fail_after, largest;
} failing_source_t;

static bool failing_read(void *ctx_, uint8_t *buf, size_t n)
{
    failing_source_t *fs = (failing_source_t *) ctx_;
    if (n > fs->largest) fs->largest = n;
    if (fs->delivered + n > fs->fail_after) return false;
    memset(buf, 0, n);
    fs->delivered += n;
    return true;
}

static size_t free_clusters(ods2_volume_t *vol)
{
    uint8_t bh[512];
    size_t len, got, count = 0, i;
    uint8_t *bits;
    assert(ods2_read_header(vol, 2, bh).ok);
    len = ods2_file_content_length(bh);
    bits = malloc(len);
    assert(bits != NULL);
    assert(ods2_read_file(vol, bh, bits, len, &got).ok && got == len);
    for (i = 512; i < len; i++) {
        uint8_t b = bits[i];
        while (b) { count += b & 1u; b >>= 1; }
    }
    free(bits);
    return count;
}

/* File numbers marked in use in the index file bitmap. */
static unsigned used_file_numbers(ods2_volume_t *vol)
{
    unsigned b, i, count = 0;
    for (b = 0; b < vol->home.ibmapsize; b++) {
        uint8_t block[512];
        assert(fseek(vol->fp, (long) (vol->home.ibmaplbn + b) * 512, SEEK_SET) == 0);
        assert(fread(block, 1, 512, vol->fp) == 512);
        for (i = 0; i < 512; i++) {
            uint8_t v = block[i];
            while (v) { count += v & 1u; v >>= 1; }
        }
    }
    return count;
}

static bool index_bit(ods2_volume_t *vol, unsigned n)
{
    uint8_t block[512];
    assert(fseek(vol->fp, (long) (vol->home.ibmaplbn + (n - 1) / 4096) * 512, SEEK_SET) == 0);
    assert(fread(block, 1, 512, vol->fp) == 512);
    return (block[((n - 1) % 4096) / 8] >> ((n - 1) % 8)) & 1;
}

static void read_back_and_compare(ods2_volume_t *vol, unsigned file_number,
                                  const uint8_t *expect, size_t len)
{
    uint8_t h[512];
    uint8_t *buf = malloc(len + 1);
    size_t got = 0;
    assert(buf != NULL);
    assert(ods2_read_header(vol, file_number, h).ok);
    assert(ods2_file_content_length(h) == len);
    assert(ods2_read_file(vol, h, buf, len + 1, &got).ok);
    assert(got == len);
    assert(memcmp(buf, expect, len) == 0);
    free(buf);
}

int main(void)
{
    ods2_volume_t vol;
    uint8_t root[512];
    ods2_result_t r;
    unsigned cluster;

    setvbuf(stdout, NULL, _IONBF, 0); /* progress survives a failed assert */
    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    cluster = vol.home.cluster;

    /* --- Map area capacity per format: what one header holds --- */
    {
        ods2_extent_t e[100];
        int i;
        for (i = 0; i < 100; i++) { e[i].lbn = 1000u * (unsigned) i; e[i].block_count = 256; }
        assert(ods2_header_extents_that_fit(e, 100) == 77);  /* Format 1: 310/4 */
        for (i = 0; i < 100; i++) e[i].block_count = 300;
        assert(ods2_header_extents_that_fit(e, 100) == 51);  /* Format 2: 310/6 */
        for (i = 0; i < 100; i++) e[i].block_count = 20000;
        assert(ods2_header_extents_that_fit(e, 100) == 38);  /* Format 3: 310/8 */
        printf("PASS: one header holds 77 Format 1, 51 Format 2 or 38 Format 3 pointers\n");
    }

    /* --- 20 MiB: past the old 12 MiB buffer and one header's ~9.6 MiB
           of Format 1 pointers --- */
    {
        size_t len = 20u * 1024u * 1024u + 123u; /* not block-aligned */
        uint8_t *content = pattern(len, 1);
        uint8_t h[512];
        const ods2_head_core_t *core = (const ods2_head_core_t *) h;
        ods2_extent_t ext[ODS2_MAX_EXTENTS];
        int n;
        ods2_fid_t fid;
        size_t before = free_clusters(&vol);
        size_t blocks = (len + 511) / 512;

        assert(ods2_read_header(&vol, 4, root).ok);
        r = ods2_create_file(&vol, root, "TWENTY.BIN", content, len, 1, &fid);
        if (!r.ok) printf("create TWENTY.BIN: %s\n", r.problem);
        assert(r.ok);
        assert(ods2_read_header(&vol, fid.fid_num, h).ok);
        assert(ods2_validate_head(h).ok);
        assert(ods2_decode_all_extents(&vol, h, ext, ODS2_MAX_EXTENTS, &n).ok);
        assert(n == 1);
        assert(core->map_inuse == 4);     /* one Format 3 pointer */
        assert(core->ext_fid.fid_num == 0);
        assert(core->filechar & 0x0080);  /* FH2$M_CONTIG */
        assert(ext[0].block_count == (blocks + cluster - 1) / cluster * cluster);
        assert(before - free_clusters(&vol) == ext[0].block_count / cluster);
        read_back_and_compare(&vol, fid.fid_num, content, len);
        printf("PASS: 20 MiB file stored as one contiguous Format 3 extent, reads back identical\n");
        free(content);
    }
    ods2_dismount(&vol);

    /* --- Fragmented free space: needs extension headers --- */
    {
        /* Lay out runs of alternating sizes back to back, then take
           every other one back, leaving many separate holes; the rest
           of the free space is swallowed so only the holes remain. */
        enum { RUNS = 520 };
        static uint32_t run_lbn[RUNS];
        static unsigned run_len[RUNS];
        uint32_t big_lbn[64];
        unsigned big_len[64];
        int bigs = 0, i, segments;
        size_t hole_blocks = 0, len, free_before, free_after_create;
        unsigned files_before;
        uint8_t *content;
        uint8_t h[512];
        ods2_fid_t fid, primary_fid, next;
        unsigned seg_numbers[64];
        int f1 = 0, f2 = 0, total_extents = 0;

        assert(ods2_mount_write(DISK_PATH, &vol).ok);
        for (i = 0; i < RUNS; i++) {
            /* holes (even i) alternate ~64 and ~320 blocks: Formats 1
               and 2; the separators (odd i) are one cluster */
            unsigned want = (i % 2) ? cluster : ((i % 4) ? 320u : 64u);
            assert(ods2_allocate_blocks(&vol, want, &run_lbn[i], &run_len[i]).ok);
        }
        for (;;) {
            uint32_t lbn;
            unsigned got;
            r = ods2_allocate_extent(&vol, 0x40000000u - 0x40000000u % cluster, &lbn, &got);
            if (!r.ok) break;
            if (got <= 320u + cluster) {
                /* only small leftovers remain; give this back and stop */
                assert(ods2_free_blocks(&vol, lbn, got).ok);
                break;
            }
            assert(bigs < 64);
            big_lbn[bigs] = lbn;
            big_len[bigs] = got;
            bigs++;
        }
        for (i = 0; i < RUNS; i += 2) {
            assert(ods2_free_blocks(&vol, run_lbn[i], run_len[i]).ok);
            hole_blocks += run_len[i];
        }

        free_before = free_clusters(&vol);
        files_before = used_file_numbers(&vol);

        /* Refused when bigger than all free space - and nothing leaks. */
        {
            size_t too_big = free_before * cluster * 512u + 1;
            uint8_t *junk = calloc(too_big, 1);
            assert(junk != NULL);
            assert(ods2_read_header(&vol, 4, root).ok);
            r = ods2_create_file(&vol, root, "TOOBIG.BIN", junk, too_big, 1, &fid);
            assert(!r.ok);
            assert(free_clusters(&vol) == free_before);
            assert(used_file_numbers(&vol) == files_before);
            assert(ods2_read_header(&vol, 4, root).ok);
            assert(!ods2_lookup_name(&vol, root, "TOOBIG.BIN", &fid).ok);
            free(junk);
            printf("PASS: a copy larger than the free space fails and gives back every "
                   "block and file number (%s)\n", r.problem);
        }

        /* Streamed content (issue #16): a source that fails part way,
           after some chunks are already on the disk, leaves no file
           and gives everything back. */
        {
            failing_source_t fs;
            ods2_content_source_t src;
            ods2_record_attrs_t attrs;
            fs.delivered = 0;
            fs.fail_after = 3u * ODS2_STREAM_CHUNK;
            fs.largest = 0;
            src.length = 5u * ODS2_STREAM_CHUNK + 123u;
            src.read = failing_read;
            src.ctx = &fs;
            src.problem = NULL;
            src.var_records = false;
            attrs.rtype = ODS2_RFM_UDF;
            attrs.rattrib = 0;
            attrs.rsize = 0;
            attrs.maxrec = 0;
            assert(ods2_read_header(&vol, 4, root).ok);
            r = ods2_create_file_source(&vol, root, "BROKEN.BIN", &src, &attrs, &fid);
            assert(!r.ok);
            assert(fs.delivered > 0 && fs.delivered <= fs.fail_after);
            assert(fs.largest <= ODS2_STREAM_CHUNK);
            assert(free_clusters(&vol) == free_before);
            assert(used_file_numbers(&vol) == files_before);
            assert(ods2_read_header(&vol, 4, root).ok);
            assert(!ods2_lookup_name(&vol, root, "BROKEN.BIN", &fid).ok);
            printf("PASS: a streamed source failing after %zu of %zu bytes leaves no "
                   "file and gives back every block and file number (%s)\n",
                   fs.delivered, src.length, r.problem);
        }

        /* Content that takes every hole leaves no room for INDEXF.SYS
           to grow for the extension headers: the volume really is
           full, and the copy must fail without leaking anything. */
        len = hole_blocks * 512u - 700u;
        content = pattern(len, 2);
        assert(ods2_read_header(&vol, 4, root).ok);
        r = ods2_create_file(&vol, root, "FULL.BIN", content, len, 1, &fid);
        if (!r.ok) {
            assert(free_clusters(&vol) == free_before);
            assert(used_file_numbers(&vol) == files_before);
            printf("PASS: content that leaves no room for its own extension headers "
                   "fails cleanly, nothing leaked (%s)\n", r.problem);
        } else {
            assert(ods2_delete(&vol, 4, "FULL.BIN").ok);
        }
        free(content);

        /* Most of the holes: many extents, with room left for the
           headers and the directory entry. */
        len = hole_blocks * 512u * 85u / 100u;
        content = pattern(len, 3);
        assert(ods2_read_header(&vol, 4, root).ok);
        r = ods2_create_file(&vol, root, "FRAGMENT.BIN", content, len, 1, &primary_fid);
        if (!r.ok) printf("create FRAGMENT.BIN: %s\n", r.problem);
        assert(r.ok);
        free_after_create = free_clusters(&vol);

        /* Walk the header chain by hand. */
        next = primary_fid;
        for (segments = 0; next.fid_num != 0; segments++) {
            const ods2_head_core_t *core = (const ods2_head_core_t *) h;
            const uint8_t *map;
            unsigned w = 0;
            assert(segments < 64);
            seg_numbers[segments] = next.fid_num;
            assert(ods2_read_header(&vol, next.fid_num, h).ok);
            assert(ods2_validate_head(h).ok);
            assert(core->fid.fid_num == next.fid_num && core->fid.fid_seq == next.fid_seq);
            assert(core->seg_num == segments);
            assert(index_bit(&vol, next.fid_num));
            if (segments > 0) {
                assert(core->backlink.fid_num == primary_fid.fid_num);
                assert(core->backlink.fid_seq == primary_fid.fid_seq);
            } else {
                assert(core->backlink.fid_num == 4); /* root */
                assert(!(core->filechar & 0x0080));  /* not contiguous */
            }
            map = h + core->mpoffset * 2;
            while (w < core->map_inuse) {
                unsigned format = map[w * 2 + 1] >> 6;
                assert(format == 1 || format == 2);
                if (format == 1) { f1++; w += 2; } else { f2++; w += 3; }
                total_extents++;
            }
            assert(w == core->map_inuse);
            next = core->ext_fid;
        }
        assert(segments >= 3);
        assert(f1 > 0 && f2 > 0);
        read_back_and_compare(&vol, primary_fid.fid_num, content, len);
        {
            ods2_extent_t ext[ODS2_MAX_EXTENTS];
            int n;
            assert(ods2_read_header(&vol, primary_fid.fid_num, h).ok);
            assert(ods2_decode_all_extents(&vol, h, ext, ODS2_MAX_EXTENTS, &n).ok);
            assert(n == total_extents);
            {
                size_t sum = 0;
                int k;
                for (k = 0; k < n; k++) sum += ext[k].block_count;
                assert(sum == ods2_file_allocated_bytes(h) / 512u); /* HIBLK = whole chain */
            }
        }
        printf("PASS: on fragmented free space, a %zu-byte file spans %d extents "
               "(%d Format 1, %d Format 2) in %d chained headers, all valid, "
               "reads back identical\n", len, total_extents, f1, f2, segments);
        ods2_dismount(&vol);

        /* Fresh mount: same content through a new mount. */
        assert(ods2_mount(DISK_PATH, &vol).ok);
        read_back_and_compare(&vol, primary_fid.fid_num, content, len);
        ods2_dismount(&vol);
        printf("PASS: multi-header file reads back identical after a fresh mount\n");

        /* Delete frees every block and every header. */
        assert(ods2_mount_write(DISK_PATH, &vol).ok);
        assert(ods2_delete(&vol, 4, "FRAGMENT.BIN").ok);
        assert(free_clusters(&vol) == free_before);
        assert(used_file_numbers(&vol) == files_before);
        for (i = 0; i < segments; i++) {
            const ods2_head_core_t *core = (const ods2_head_core_t *) h;
            assert(!index_bit(&vol, seg_numbers[i]));
            assert(ods2_read_header(&vol, seg_numbers[i], h).ok);
            assert(core->filechar & 0x8000u); /* FH2$M_MARKDEL */
            assert(core->fid.fid_num == 0);
        }
        assert(free_after_create < free_before);
        printf("PASS: deleting it frees all its blocks and all %d headers\n", segments);

        /* Put the volume back as it was, for the CLI checks. */
        for (i = 1; i < RUNS; i += 2) {
            assert(ods2_free_blocks(&vol, run_lbn[i], run_len[i]).ok);
        }
        for (i = 0; i < bigs; i++) {
            assert(ods2_free_blocks(&vol, big_lbn[i], big_len[i]).ok);
        }
        ods2_dismount(&vol);
        free(content);
    }

    printf("\nods2_large_file_selftest: all checks passed\n");
    return 0;
}
