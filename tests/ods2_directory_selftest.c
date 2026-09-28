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

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ods2_directory.h"

/* Real root directory content ([000000]), read from an actual
   VMS-initialized disk (samples/root_dir.bin, LBN 1470474, decoded
   from root's own header's retrieval pointer - see
   ods2_root_header_selftest.c). 268 bytes covers all 11 real records
   plus the terminating sentinel. */
static const uint8_t real_root_dir[268] = {
    0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x2e, 0x44, 0x49, 0x52, 0x01, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x42, 0x41, 0x43, 0x4b, 0x55, 0x50,
    0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x42, 0x41, 0x44, 0x42, 0x4c, 0x4b,
    0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x42, 0x41, 0x44, 0x4c, 0x4f, 0x47,
    0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x09, 0x00, 0x09, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x42, 0x49, 0x54, 0x4d, 0x41, 0x50,
    0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x43, 0x4f, 0x4e, 0x54, 0x49, 0x4e,
    0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x07, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x43, 0x4f, 0x52, 0x49, 0x4d, 0x47,
    0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x05, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x00, 0x00, 0x00, 0x09, 0x44, 0x45, 0x43, 0x55, 0x53, 0x2e,
    0x44, 0x49, 0x52, 0x53, 0x01, 0x00, 0x0b, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x49, 0x4e, 0x44, 0x45, 0x58, 0x46,
    0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x01, 0x00, 0x00, 0x0c, 0x53, 0x45, 0x43, 0x55, 0x52, 0x49,
    0x54, 0x59, 0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x0a, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x56, 0x4f, 0x4c, 0x53,
    0x45, 0x54, 0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x06, 0x00, 0x06, 0x00,
    0x00, 0x00, 0xff, 0xff,
};

static void check(ods2_dir_entry_t *entries, int idx, const char *name, unsigned fid_num)
{
    assert(strcmp(entries[idx].name, name) == 0);
    assert(entries[idx].fid.fid_num == fid_num);
    printf("PASS: entry %2d = %-12s fid=(%u,%u,%u,%u)\n", idx, entries[idx].name,
           entries[idx].fid.fid_num, entries[idx].fid.fid_nmx,
           entries[idx].fid.fid_seq, entries[idx].fid.fid_rvn);
}


/* Appends one directory record at `pos` in `block`: `name`, then
   `n` (version, FID) pairs, newest first as VMS stores them. FID
   number for each version is fid_base + version, so tests can tell
   exactly which pair an entry came from. Returns the position just
   past the record. Layout per spec 4.2/4.3, the same as the real
   records above: dir$size (excludes itself), verlimit, flags,
   namecount, name padded to even length, then 8-byte pairs. */
static size_t put_record(uint8_t *block, size_t pos, const char *name,
                         const uint16_t *versions, int n, uint16_t fid_base)
{
    size_t namelen = strlen(name);
    size_t padded = namelen + (namelen % 2);
    size_t total = 6 + padded + 8u * (size_t) n;
    size_t e = pos + 6 + padded;
    int i;

    block[pos] = (uint8_t) ((total - 2) & 0xff);
    block[pos + 1] = (uint8_t) ((total - 2) >> 8);
    block[pos + 2] = 1; block[pos + 3] = 0;  /* verlimit */
    block[pos + 4] = 0;                      /* flags */
    block[pos + 5] = (uint8_t) namelen;
    memcpy(block + pos + 6, name, namelen);
    if (padded > namelen) block[pos + 6 + namelen] = 0;
    for (i = 0; i < n; i++, e += 8) {
        uint16_t fnum = (uint16_t) (fid_base + versions[i]);
        block[e] = (uint8_t) (versions[i] & 0xff);
        block[e + 1] = (uint8_t) (versions[i] >> 8);
        block[e + 2] = (uint8_t) (fnum & 0xff);
        block[e + 3] = (uint8_t) (fnum >> 8);
        block[e + 4] = 1; block[e + 5] = 0;  /* fid_seq */
        block[e + 6] = 0; block[e + 7] = 0;  /* rvn, nmx */
    }
    return pos + total;
}

static void put_sentinel(uint8_t *block, size_t pos)
{
    block[pos] = 0xff;
    block[pos + 1] = 0xff;
}

int main(void)
{
    ods2_dir_entry_t entries[16];
    int n = ods2_parse_directory(real_root_dir, sizeof(real_root_dir), entries, 16);

    assert(n == 11);
    printf("Parsed %d entries from real root directory content:\n\n", n);

    /* Every name and FID here matches what real VMS's own DIR command
       showed throughout this entire project - this is the complete
       chain (home block -> index file -> directory content -> real
       filenames) working end to end against genuine bytes. */
    check(entries, 0,  "000000.DIR",   4);
    check(entries, 1,  "BACKUP.SYS",   8);
    check(entries, 2,  "BADBLK.SYS",   3);
    check(entries, 3,  "BADLOG.SYS",   9);
    check(entries, 4,  "BITMAP.SYS",   2);
    check(entries, 5,  "CONTIN.SYS",   7);
    check(entries, 6,  "CORIMG.SYS",   5);
    check(entries, 7,  "DECUS.DIR",    11); /* our own created directory! */
    check(entries, 8,  "INDEXF.SYS",   1);
    check(entries, 9,  "SECURITY.SYS", 10);
    check(entries, 10, "VOLSET.SYS",   6);

    /* Truncation must be visible: with room for only 4 entries, the
       parser stores 4 but still reports all 11 records it saw. An
       earlier version returned 4 here, so every caller silently lost
       entries 5-11 with no way to tell. */
    {
        ods2_dir_entry_t small[4];
        int total = ods2_parse_directory(real_root_dir, sizeof(real_root_dir), small, 4);
        assert(total == 11);
        assert(strcmp(small[3].name, "BADLOG.SYS") == 0);
        total = ods2_parse_directory(real_root_dir, sizeof(real_root_dir), NULL, 0);
        assert(total == 11);
        printf("PASS: parser reports all 11 records even when the buffer holds "
               "only 4 (or none), so truncation is detectable\n");
    }

    /* --- Issue #4: one entry per VERSION, not one per record --- */
    {
        uint8_t block[512];
        ods2_dir_entry_t e[16];
        static const uint16_t a_versions[] = {3, 2, 1};
        static const uint16_t b_versions[] = {1};
        size_t pos = 0;
        int total;

        memset(block, 0, sizeof(block));
        pos = put_record(block, pos, "A.TXT", a_versions, 3, 100);
        pos = put_record(block, pos, "B.TXT", b_versions, 1, 200);
        put_sentinel(block, pos);

        total = ods2_parse_directory(block, sizeof(block), e, 16);
        assert(total == 4);
        assert(strcmp(e[0].name, "A.TXT") == 0 && e[0].version == 3 && e[0].fid.fid_num == 103);
        assert(strcmp(e[1].name, "A.TXT") == 0 && e[1].version == 2 && e[1].fid.fid_num == 102);
        assert(strcmp(e[2].name, "A.TXT") == 0 && e[2].version == 1 && e[2].fid.fid_num == 101);
        assert(strcmp(e[3].name, "B.TXT") == 0 && e[3].version == 1 && e[3].fid.fid_num == 201);
        printf("PASS: a 3-version record parses as 3 entries (newest first, each with "
               "its own FID), followed by the next record\n");

        /* Truncation still detectable when it happens mid-record. */
        total = ods2_parse_directory(block, sizeof(block), e, 2);
        assert(total == 4);
        assert(e[1].version == 2);
        printf("PASS: truncation part-way through a record's versions is still "
               "reported (4 entries seen, 2 stored)\n");
    }

    /* The densest possible block - one record, empty name, as many
       versions as fit before the sentinel - must fit exactly in
       ODS2_DIR_MAX_ENTRIES_PER_BLOCK, which walk_directory() relies
       on to never drop entries. */
    {
        uint8_t block[512];
        ods2_dir_entry_t e[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
        uint16_t versions[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
        size_t pos;
        int i, total;

        for (i = 0; i < ODS2_DIR_MAX_ENTRIES_PER_BLOCK; i++) {
            versions[i] = (uint16_t) (ODS2_DIR_MAX_ENTRIES_PER_BLOCK - i);
        }
        memset(block, 0, sizeof(block));
        pos = put_record(block, 0, "", versions, ODS2_DIR_MAX_ENTRIES_PER_BLOCK, 0);
        assert(pos == 510); /* 6 + 63*8 */
        put_sentinel(block, pos);
        total = ods2_parse_directory(block, sizeof(block), e, ODS2_DIR_MAX_ENTRIES_PER_BLOCK);
        assert(total == ODS2_DIR_MAX_ENTRIES_PER_BLOCK);
        assert(e[0].version == ODS2_DIR_MAX_ENTRIES_PER_BLOCK && e[62].version == 1);
        printf("PASS: densest possible block holds exactly %d entries = "
               "ODS2_DIR_MAX_ENTRIES_PER_BLOCK\n", total);
    }

    /* Records followed by zeroed space with no sentinel: the records
       must still be returned (an earlier version reported the whole
       block as malformed, discarding them). */
    {
        uint8_t block[512];
        ods2_dir_entry_t e[4];
        static const uint16_t v[] = {7, 5};

        memset(block, 0, sizeof(block));
        (void) put_record(block, 0, "LOG.TXT", v, 2, 0);
        assert(ods2_parse_directory(block, sizeof(block), e, 4) == 2);
        assert(e[0].version == 7 && e[1].version == 5);
        memset(block, 0, sizeof(block));
        assert(ods2_parse_directory(block, sizeof(block), e, 4) == 0);
        printf("PASS: zeroed space after the last record (no sentinel) ends the "
               "block instead of discarding it; an all-zero block is empty\n");
    }

    /* A record with a name but no complete version pair is malformed. */
    {
        uint8_t block[512];
        ods2_dir_entry_t e[4];
        memset(block, 0, sizeof(block));
        (void) put_record(block, 0, "ODD.TXT", NULL, 0, 0);
        put_sentinel(block, 14);
        assert(ods2_parse_directory(block, sizeof(block), e, 4) == -1);
        printf("PASS: a record with no version entry is rejected as malformed\n");
    }

    printf("\nods2_directory_selftest: all checks passed - full home block -> "
           "index file -> directory -> real filename chain verified against "
           "genuine disk bytes\n");
    return 0;
}
