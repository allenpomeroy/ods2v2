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

/* ods2_verlimit_selftest - the version limit of a directory entry (DIR$W_VERLIMIT), on a copy of the synthetic disk.
 *
 * VMS shows the word as the file's "Version limit" and purges to it: when version 2 of a name is made and the limit is
 * 1, version 1 goes. ods2v2 wrote 1 for every entry, so a COPY or an edit of one of its files on VMS silently deleted
 * version 1 (seen on a physical VAX, OpenVMS V7.3: README.TXT;1 purged by COPY README.TXT;1 README.TXT;2, which left
 * its aliases dangling; reproduced on V7.3 and V8.4 with batch "vlimit"). Every user file of every VMS-made volume
 * examined (V7.3, V7.3-2, V8.4; AXP1SYS: 40,000 entries) has 32767, or the default version limit of its directory
 * (the directory's FAT$W_VERSIONS) when that is set, and a directory made in one with a default carries it too.
 * So: new entries get 32767 or the parent's default, a new directory header gets the parent's default (a file's
 * header 0), and ods2_set_entry_version_limit() changes the entries of one name (for volumes written before). */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_checksum.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/verlimit_disk.img"

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

/* The version limit of the record for `name` in directory `dir_num` (-1: not found). */
static int limit_of(ods2_volume_t *vol, unsigned dir_num, const char *name)
{
    uint8_t h[512], blk[512];
    unsigned vbn;

    assert(ods2_read_header(vol, dir_num, h).ok);
    for (vbn = 1; vbn <= 4; vbn++) {
        size_t pos = 0;
        if (!ods2_read_file_block(vol, h, vbn, blk).ok) break;
        while (pos + 6 <= 512) {
            uint16_t size = (uint16_t) (blk[pos] | (blk[pos + 1] << 8));
            if (size == 0xffff || size == 0) break;
            if (blk[pos + 5] == strlen(name) && memcmp(blk + pos + 6, name, blk[pos + 5]) == 0) {
                return blk[pos + 2] | (blk[pos + 3] << 8);
            }
            pos += (size_t) size + 2;
        }
    }
    return -1;
}

static unsigned header_versions(ods2_volume_t *vol, unsigned file_num)
{
    uint8_t h[512];
    assert(ods2_read_header(vol, file_num, h).ok);
    return ((const ods2_head_core_t *) h)->recattr.versions;
}

/* Gives a directory a default version limit (FAT$W_VERSIONS), as SET DIRECTORY/VERSION_LIMIT does. */
static void set_default_limit(ods2_volume_t *vol, unsigned dir_num, uint16_t limit)
{
    uint8_t h[512];
    ods2_head_core_t *core = (ods2_head_core_t *) h;
    uint16_t sum;

    assert(ods2_read_header(vol, dir_num, h).ok);
    core->recattr.versions = limit;
    sum = ods2_checksum(h, 255);
    h[510] = (uint8_t) (sum & 0xff);
    h[511] = (uint8_t) (sum >> 8);
    assert(ods2_write_header(vol, dir_num, h).ok);
}

int main(void)
{
    ods2_volume_t vol;
    uint8_t root[512], d[512];
    ods2_fid_t fid, dfid;
    ods2_result_t r;
    unsigned changed;

    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    r = ods2_mount_write(DISK_PATH, &vol);
    assert(r.ok);
    assert(ods2_read_header(&vol, 4, root).ok);

    /* the reserved files' entries keep the 1 VMS gave them */
    assert(limit_of(&vol, 4, "INDEXF.SYS") == 1);

    /* a new file and a new directory in [000000]: no limit */
    assert(ods2_create_file(&vol, root, "NEW.TXT", (const uint8_t *) "x\n", 2, 5, &fid).ok);
    assert(ods2_create_directory(&vol, root, "SUB", &dfid).ok);
    assert(limit_of(&vol, 4, "NEW.TXT") == 32767);
    assert(limit_of(&vol, 4, "SUB.DIR") == 32767);
    assert(header_versions(&vol, fid.fid_num) == 0 && header_versions(&vol, dfid.fid_num) == 0);
    printf("PASS: a new file's and a new directory's entry carry version limit 32767 (their headers' FAT limit is 0)\n");

    /* a directory with a default version limit: its new entries and its new subdirectories carry it */
    set_default_limit(&vol, dfid.fid_num, 3);
    assert(ods2_read_header(&vol, dfid.fid_num, d).ok);
    assert(ods2_create_file(&vol, d, "IN.TXT", (const uint8_t *) "x\n", 2, 5, &fid).ok);
    assert(ods2_create_directory(&vol, d, "INNER", &dfid) .ok);
    assert(limit_of(&vol, ((const ods2_head_core_t *) d)->fid.fid_num, "IN.TXT") == 3);
    assert(limit_of(&vol, ((const ods2_head_core_t *) d)->fid.fid_num, "INNER.DIR") == 3);
    assert(header_versions(&vol, dfid.fid_num) == 3);   /* the directory made in it passes the default on */
    assert(header_versions(&vol, fid.fid_num) == 0);    /* a file does not */
    printf("PASS: a directory with default version limit 3: its new entries get 3, a directory made in it carries 3, a file 0\n");

    /* SET FILE/VERSION_LIMIT: one name; the others and the reserved entries are left alone */
    assert(ods2_set_entry_version_limit(&vol, 4, "NEW.TXT", 5, &changed).ok && changed == 1);
    assert(limit_of(&vol, 4, "NEW.TXT") == 5 && limit_of(&vol, 4, "SUB.DIR") == 32767);
    assert(ods2_set_entry_version_limit(&vol, 4, "NEW.TXT", 5, &changed).ok && changed == 0);
    assert(ods2_set_entry_version_limit(&vol, 4, "NOSUCH.TXT", 5, &changed).ok && changed == 0);
    assert(limit_of(&vol, 4, "INDEXF.SYS") == 1);
    printf("PASS: ods2_set_entry_version_limit changes the named entry only\n");

    ods2_dismount(&vol);
    remove(DISK_PATH);
    printf("\nods2_verlimit_selftest: all checks passed\n");
    return 0;
}
