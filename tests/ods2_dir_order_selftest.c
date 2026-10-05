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

/* ods2_dir_order_selftest - multi-block directories as VMS needs them
 * (spec 4.2), on a copy of the synthetic disk:
 *
 *  - entries sorted across the whole directory, not just per block -
 *    VMS's search relies on it (else TYPE: RMS-E-FNF, ANALYZE/DISK:
 *    BAD_NAMEORDER);
 *  - every entry inside the directory's end-of-file, and every block
 *    inside it holding entries - VMS reads no further than the EOF;
 *  - the directory one contiguous extent as it grows (FH2$M_CONTIG);
 *  - deleting the last entry of a block removes the block. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_directory.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/dir_order_disk.img"
#define N_NAMES 150

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

/* Checks every invariant above; returns the blocks in use and
   fills first_names[] with each block's first name. */
static unsigned check_directory(ods2_volume_t *vol, unsigned dir_num, int expect_entries,
                                char first_names[][ODS2_DIR_MAX_NAME + 1])
{
    uint8_t h[512], block[512];
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count, total = 0;
    unsigned used, v;
    char prev[ODS2_DIR_MAX_NAME + 1] = "";

    assert(ods2_read_header(vol, dir_num, h).ok);
    assert(ods2_decode_all_extents(vol, h, extents, ODS2_MAX_EXTENTS, &extent_count).ok);
    assert(extent_count == 1);                       /* contiguous */
    assert(core->filechar & 0x0080u);                /* FH2$M_CONTIG */
    used = (unsigned) ((ods2_file_content_length(h) + 511) / 512);
    assert(used >= 1 && used <= extents[0].block_count);
    for (v = 1; v <= used; v++) {
        ods2_dir_entry_t e[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
        int n, i;
        assert(ods2_read_file_block(vol, h, v, block).ok);
        n = ods2_parse_directory(block, 512, e, ODS2_DIR_MAX_ENTRIES_PER_BLOCK);
        assert(n > 0 || (n == 0 && used == 1)); /* no empty block inside the EOF */
        for (i = 0; i < n; i++) {
            assert(strcmp(prev, e[i].name) < 0);    /* order, across blocks too */
            memcpy(prev, e[i].name, sizeof prev);    /* same size, nul-terminated */
        }
        if (first_names != NULL && n > 0) {
            memcpy(first_names[v - 1], e[0].name, ODS2_DIR_MAX_NAME + 1);
        }
        total += n;
    }
    assert(total == expect_entries);                 /* nothing beyond the EOF */
    return used;
}

/* Creates a one-byte file `name` in directory `dir_num`, reading the
   directory's header afresh - it may have moved since the last one. */
static void create_in(ods2_volume_t *vol, unsigned dir_num, const char *name)
{
    uint8_t h[512], content = 'x';
    ods2_fid_t fid;
    ods2_result_t r;
    assert(ods2_read_header(vol, dir_num, h).ok);
    r = ods2_create_file(vol, h, name, &content, 1, 5, &fid);
    if (!r.ok) printf("create %s: %s\n", name, r.problem);
    assert(r.ok);
}

/* Name i, in a scrambled order and of varying length. */
static void name_for(int i, char *out, size_t size)
{
    static const char *types[] = { "C", "TXT", "COM", "H", "LONGERTYPE" };
    int k = (i * 37) % N_NAMES;                       /* 37 is coprime to 150 */
    snprintf(out, size, "N%03d%.*s.%s", k, k % 9, "XXXXXXXXX", types[k % 5]);
}

int main(void)
{
    ods2_volume_t vol;
    uint8_t root[512];
    ods2_fid_t dir_fid;
    char name[64];
    int i;
    unsigned used, splits_seen = 0, last_used = 1;

    copy_file(SOURCE_DISK_PATH, DISK_PATH);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    assert(ods2_read_header(&vol, 4, root).ok);
    assert(ods2_create_directory(&vol, root, "ORDER", &dir_fid).ok);

    for (i = 0; i < N_NAMES; i++) {
        name_for(i, name, sizeof name);
        create_in(&vol, dir_fid.fid_num, name);
        used = check_directory(&vol, dir_fid.fid_num, i + 1, NULL);
        if (used > last_used) splits_seen++;
        last_used = used;
    }
    /* A name before everything else lands in the first block, even
       though that block is full - it splits rather than the name
       going elsewhere. */
    create_in(&vol, dir_fid.fid_num, "AAAA.TXT");
    used = check_directory(&vol, dir_fid.fid_num, N_NAMES + 1, NULL);
    printf("PASS: %d names inserted out of order: always sorted across all %u "
           "blocks, all inside the EOF, one contiguous extent (%u splits)\n",
           N_NAMES + 1, used, splits_seen);

    /* Every name is found by lookup - VMS's search depends on the
       order, ods2v2's doesn't, so this checks the entries themselves. */
    {
        uint8_t h[512];
        ods2_fid_t f;
        assert(ods2_read_header(&vol, dir_fid.fid_num, h).ok);
        for (i = 0; i < N_NAMES; i++) {
            name_for(i, name, sizeof name);
            assert(ods2_lookup_name(&vol, h, name, &f).ok);
        }
        printf("PASS: every name is found by lookup\n");
    }

    /* Delete every name in the second block: that block goes away. */
    {
        static char firsts[256][ODS2_DIR_MAX_NAME + 1];
        uint8_t h[512], block[512];
        ods2_dir_entry_t e[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
        int n, k, remaining = N_NAMES + 1;
        unsigned before = check_directory(&vol, dir_fid.fid_num, remaining, firsts);

        assert(before >= 3);
        assert(ods2_read_header(&vol, dir_fid.fid_num, h).ok);
        assert(ods2_read_file_block(&vol, h, 2, block).ok);
        n = ods2_parse_directory(block, 512, e, ODS2_DIR_MAX_ENTRIES_PER_BLOCK);
        assert(n > 0);
        for (k = 0; k < n; k++) {
            assert(ods2_delete(&vol, dir_fid.fid_num, e[k].name).ok);
            remaining--;
            check_directory(&vol, dir_fid.fid_num, remaining, NULL);
        }
        used = check_directory(&vol, dir_fid.fid_num, remaining, NULL);
        assert(used == before - 1);
        printf("PASS: deleting all %d entries of block 2 removed the block "
               "(%u -> %u in use), order and EOF intact\n", n, before, used);
    }

    /* MAKEFILE is stored as MAKEFILE. and found either way. */
    {
        uint8_t h[512], content = 'x';
        ods2_fid_t f, made;
        assert(ods2_create_file(&vol, root, "makefile", &content, 1, 5, &made).ok);
        assert(ods2_read_header(&vol, 4, h).ok);
        assert(ods2_lookup_name(&vol, h, "MAKEFILE", &f).ok && f.fid_num == made.fid_num);
        assert(ods2_lookup_name(&vol, h, "MAKEFILE.", &f).ok && f.fid_num == made.fid_num);
        assert(!ods2_create_file(&vol, root, "src/x.c", &content, 1, 5, &made).ok);
        printf("PASS: MAKEFILE is stored as MAKEFILE. and found with or without "
               "the dot; src/x.c is refused\n");
    }

    ods2_dismount(&vol);
    remove(DISK_PATH);
    printf("\nods2_dir_order_selftest: all checks passed\n");
    return 0;
}
