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
#include "ods2_directory_write.h"
#include "ods2_directory.h"


/* Appends one directory record (name + n version pairs, newest
   first) at `pos`; FID number = fid_base + version. See the same
   helper in ods2_directory_selftest.c. Returns the end position. */
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
    block[pos + 2] = 1; block[pos + 3] = 0;
    block[pos + 4] = 0;
    block[pos + 5] = (uint8_t) namelen;
    memcpy(block + pos + 6, name, namelen);
    if (padded > namelen) block[pos + 6 + namelen] = 0;
    for (i = 0; i < n; i++, e += 8) {
        uint16_t fnum = (uint16_t) (fid_base + versions[i]);
        block[e] = (uint8_t) (versions[i] & 0xff);
        block[e + 1] = (uint8_t) (versions[i] >> 8);
        block[e + 2] = (uint8_t) (fnum & 0xff);
        block[e + 3] = (uint8_t) (fnum >> 8);
        block[e + 4] = 1; block[e + 5] = 0;
        block[e + 6] = 0; block[e + 7] = 0;
    }
    return pos + total;
}

int main(void)
{
    uint8_t block[512];
    ods2_fid_t fid;
    ods2_dir_entry_t entries[32];
    int n;

    /* Test 1: insert into a completely empty (all-zero) block. */
    memset(block, 0, sizeof(block));
    fid.fid_num = 11; fid.fid_seq = 1; fid.fid_rvn = 0; fid.fid_nmx = 0;
    assert(ods2_insert_dir_entry(block, sizeof(block), "DECUS.DIR", 1, fid));

    n = ods2_parse_directory(block, sizeof(block), entries, 32);
    assert(n == 1);
    assert(strcmp(entries[0].name, "DECUS.DIR") == 0);
    assert(entries[0].version == 1);
    assert(entries[0].fid.fid_num == 11);
    printf("PASS: insert into an empty block, read back correctly\n");

    /* Explicit check: DIR$W_VERLIMIT must be 1, matching every real
       VMS-written record examined (000000.DIR, BACKUP.SYS, INDEXF.SYS,
       etc. all show 1) - not 0, despite the spec summary calling this
       field "ignored".  Visible difference from every native
       entry, found by dumping actual bytes from a real disk. */
    {
        uint16_t verlimit = (uint16_t) block[2] | ((uint16_t) block[3] << 8);
        assert(verlimit == 1);
        printf("PASS: DIR$W_VERLIMIT is 1, matching real VMS-written records\n");
    }

    /* Test 2: insert a second entry into the same block. */
    fid.fid_num = 12;
    assert(ods2_insert_dir_entry(block, sizeof(block), "NETLIB020.DIR", 1, fid));
    n = ods2_parse_directory(block, sizeof(block), entries, 32);
    assert(n == 2);
    assert(strcmp(entries[0].name, "DECUS.DIR") == 0);
    assert(strcmp(entries[1].name, "NETLIB020.DIR") == 0);
    assert(entries[1].fid.fid_num == 12);
    printf("PASS: second insert coexists correctly with the first\n");

    /* Test 3: insert alongside the REAL root directory content
       (embedded from ods2_directory_selftest.c, already verified
       against genuine disk bytes). A new entry should be added after
       the 11 real ones without disturbing them. */
    {
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
        uint8_t real_block[512];
        memset(real_block, 0, sizeof(real_block));
        memcpy(real_block, real_root_dir, sizeof(real_root_dir));

        fid.fid_num = 14;
        assert(ods2_insert_dir_entry(real_block, sizeof(real_block), "NEWDIR.DIR", 1, fid));

        n = ods2_parse_directory(real_block, sizeof(real_block), entries, 32);
        assert(n == 12); /* 11 real entries + the new one */
        /* NEWDIR.DIR sorts alphabetically between INDEXF.SYS and
           SECURITY.SYS - real VMS requires sorted order (confirmed
           via ANALYZE/DISK's BAD_NAMEORDER finding). */
        assert(strcmp(entries[8].name, "INDEXF.SYS") == 0);
        assert(strcmp(entries[9].name, "NEWDIR.DIR") == 0);
        assert(entries[9].fid.fid_num == 14);
        assert(strcmp(entries[10].name, "SECURITY.SYS") == 0);
        /* Confirm none of the original 11 were disturbed - just
           shifted to make room, values still correct. */
        assert(strcmp(entries[0].name, "000000.DIR") == 0);
        assert(strcmp(entries[7].name, "DECUS.DIR") == 0);
        assert(entries[7].fid.fid_num == 11);
        printf("PASS: inserting alongside real root directory data places the new "
               "entry at its correct SORTED position (between INDEXF.SYS and "
               "SECURITY.SYS), not just appended at the end - matches real VMS's "
               "sorted-order requirement\n");
    }

    /* Test 4: no room fails cleanly rather than corrupting the block. */
    {
        uint8_t tiny[10]; /* too small for any real record */
        memset(tiny, 0, sizeof(tiny));
        fid.fid_num = 99;
        assert(!ods2_insert_dir_entry(tiny, sizeof(tiny), "TOOLONGANAME.TXT", 1, fid));
        printf("PASS: insufficient space is correctly rejected\n");
    }

    /* Test 5: dedicated sort-order test - insert names in a
       deliberately non-alphabetical order and confirm they always
       read back sorted, regardless of insertion order. */
    {
        uint8_t sort_block[512];
        memset(sort_block, 0, sizeof(sort_block));
        fid.fid_num = 1;
        assert(ods2_insert_dir_entry(sort_block, sizeof(sort_block), "ZEBRA.TXT", 1, fid));
        fid.fid_num = 2;
        assert(ods2_insert_dir_entry(sort_block, sizeof(sort_block), "APPLE.TXT", 1, fid));
        fid.fid_num = 3;
        assert(ods2_insert_dir_entry(sort_block, sizeof(sort_block), "MANGO.TXT", 1, fid));

        n = ods2_parse_directory(sort_block, sizeof(sort_block), entries, 32);
        assert(n == 3);
        assert(strcmp(entries[0].name, "APPLE.TXT") == 0);
        assert(strcmp(entries[1].name, "MANGO.TXT") == 0);
        assert(strcmp(entries[2].name, "ZEBRA.TXT") == 0);
        assert(entries[0].fid.fid_num == 2); /* APPLE was inserted second but sorts first */
        printf("PASS: names inserted in non-alphabetical order (ZEBRA, APPLE, MANGO) "
               "read back correctly sorted (APPLE, MANGO, ZEBRA)\n");
    }

    /* --- ods2_remove_dir_entry() tests --- */

    /* Test 6: basic insert-then-remove round trip. */
    {
        uint8_t rblock[512];
        memset(rblock, 0, sizeof(rblock));
        fid.fid_num = 1;
        assert(ods2_insert_dir_entry(rblock, sizeof(rblock), "APPLE.TXT", 1, fid));
        fid.fid_num = 2;
        assert(ods2_insert_dir_entry(rblock, sizeof(rblock), "MANGO.TXT", 1, fid));
        fid.fid_num = 3;
        assert(ods2_insert_dir_entry(rblock, sizeof(rblock), "ZEBRA.TXT", 1, fid));

        assert(ods2_remove_dir_entry(rblock, sizeof(rblock), "MANGO.TXT"));
        n = ods2_parse_directory(rblock, sizeof(rblock), entries, 32);
        assert(n == 2);
        assert(strcmp(entries[0].name, "APPLE.TXT") == 0);
        assert(strcmp(entries[1].name, "ZEBRA.TXT") == 0);
        printf("PASS: removing a middle entry leaves the other two intact and "
               "correctly sorted\n");

        /* Removing something not present returns false, doesn't
           disturb anything. */
        assert(!ods2_remove_dir_entry(rblock, sizeof(rblock), "NOTTHERE.TXT"));
        n = ods2_parse_directory(rblock, sizeof(rblock), entries, 32);
        assert(n == 2);
        printf("PASS: removing a non-existent name returns false and leaves "
               "the block unchanged\n");

        /* Remove the remaining two, block should end up empty. */
        assert(ods2_remove_dir_entry(rblock, sizeof(rblock), "APPLE.TXT"));
        assert(ods2_remove_dir_entry(rblock, sizeof(rblock), "ZEBRA.TXT"));
        n = ods2_parse_directory(rblock, sizeof(rblock), entries, 32);
        assert(n == 0);
        printf("PASS: removing all entries leaves a correctly empty block\n");
    }

    /* Test 7: remove from real root directory data, confirm the
       other 10 real entries survive intact and correctly sorted. */
    {
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
            0x16, 0x00, 0x01, 0x00, 0x00, 0x09, 0x44, 0x45, 0x43, 0x55, 0x53, 0x2e,
            0x44, 0x49, 0x52, 0x00, 0x01, 0x00, 0x0b, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x49, 0x4e, 0x44, 0x45, 0x58, 0x46,
            0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x18, 0x00, 0x01, 0x00, 0x00, 0x0c, 0x53, 0x45, 0x43, 0x55, 0x52, 0x49,
            0x54, 0x59, 0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x0a, 0x00, 0x0a, 0x00,
            0x00, 0x00, 0x16, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x56, 0x4f, 0x4c, 0x53,
            0x45, 0x54, 0x2e, 0x53, 0x59, 0x53, 0x01, 0x00, 0x06, 0x00, 0x06, 0x00,
            0x00, 0x00, 0xff, 0xff,
        };
        uint8_t rblock2[512];
        memset(rblock2, 0, sizeof(rblock2));
        memcpy(rblock2, real_root_dir, sizeof(real_root_dir));

        assert(ods2_remove_dir_entry(rblock2, sizeof(rblock2), "DECUS.DIR"));
        n = ods2_parse_directory(rblock2, sizeof(rblock2), entries, 32);
        assert(n == 10); /* 11 real entries minus DECUS.DIR */
        {
            int i;
            for (i = 0; i < n; i++) {
                assert(strcmp(entries[i].name, "DECUS.DIR") != 0);
            }
        }
        assert(strcmp(entries[7].name, "INDEXF.SYS") == 0);
        assert(strcmp(entries[8].name, "SECURITY.SYS") == 0);
        printf("PASS: removing DECUS.DIR from real root directory data leaves "
               "the other 10 real entries intact and correctly sorted (INDEXF.SYS "
               "now directly followed by SECURITY.SYS)\n");
    }

    /* --- ods2_remove_dir_version(): delete ONE version (issue #5) --- */
    {
        uint8_t vb[512], before[512];
        static const uint16_t a_versions[] = {3, 2, 1};
        static const uint16_t b_versions[] = {1};
        size_t pos;
        ods2_fid_t cfid;

        memset(vb, 0, sizeof(vb));
        pos = put_record(vb, 0, "A.TXT", a_versions, 3, 100);
        pos = put_record(vb, pos, "B.TXT", b_versions, 1, 200);
        vb[pos] = 0xff; vb[pos + 1] = 0xff;

        /* Middle version: record shrinks by 8, others untouched. */
        assert(ods2_remove_dir_version(vb, sizeof(vb), "a.txt", 2));
        n = ods2_parse_directory(vb, sizeof(vb), entries, 32);
        assert(n == 3);
        assert(strcmp(entries[0].name, "A.TXT") == 0 && entries[0].version == 3 &&
               entries[0].fid.fid_num == 103);
        assert(strcmp(entries[1].name, "A.TXT") == 0 && entries[1].version == 1 &&
               entries[1].fid.fid_num == 101);
        assert(strcmp(entries[2].name, "B.TXT") == 0 && entries[2].fid.fid_num == 201);
        assert(vb[0] == 6 + 6 + 16 - 2); /* dir$size: header+name+2 pairs, minus itself */
        printf("PASS: removing A.TXT;2 leaves A.TXT;3 and ;1 (with their own FIDs) "
               "and B.TXT intact, record shrunk by one pair\n");

        /* A version that is not there changes nothing. */
        memcpy(before, vb, sizeof(vb));
        assert(!ods2_remove_dir_version(vb, sizeof(vb), "A.TXT", 2));
        assert(!ods2_remove_dir_version(vb, sizeof(vb), "NOPE.TXT", 1));
        assert(memcmp(before, vb, sizeof(vb)) == 0);
        printf("PASS: removing an absent version or name returns false and leaves "
               "the block byte-for-byte unchanged\n");

        /* Last remaining versions: the record itself goes. */
        assert(ods2_remove_dir_version(vb, sizeof(vb), "A.TXT", 3));
        assert(ods2_remove_dir_version(vb, sizeof(vb), "A.TXT", 1));
        n = ods2_parse_directory(vb, sizeof(vb), entries, 32);
        assert(n == 1 && strcmp(entries[0].name, "B.TXT") == 0);
        printf("PASS: removing a name's last version removes its whole record\n");

        /* The edited block is still a valid target for inserts. */
        cfid.fid_num = 300; cfid.fid_seq = 1; cfid.fid_rvn = 0; cfid.fid_nmx = 0;
        assert(ods2_insert_dir_entry(vb, sizeof(vb), "A.TXT", 1, cfid));
        n = ods2_parse_directory(vb, sizeof(vb), entries, 32);
        assert(n == 2 && strcmp(entries[0].name, "A.TXT") == 0 &&
               strcmp(entries[1].name, "B.TXT") == 0);
        printf("PASS: the block accepts a sorted insert after version removals\n");
    }

    /* A completely full block (one record filling all 512 bytes, so
       no room for a sentinel): after removing a version the block
       must end in a real sentinel, not zeros. */
    {
        uint8_t fb[512];
        uint16_t versions[63];
        int i;
        for (i = 0; i < 63; i++) versions[i] = (uint16_t) (63 - i);
        memset(fb, 0, sizeof(fb));
        assert(put_record(fb, 0, "AB", versions, 63, 0) == 512);
        n = ods2_parse_directory(fb, sizeof(fb), entries, 32);
        assert(n == 63);

        assert(ods2_remove_dir_version(fb, sizeof(fb), "AB", 40));
        assert(fb[504] == 0xff && fb[505] == 0xff);
        {
            ods2_dir_entry_t all[63];
            n = ods2_parse_directory(fb, sizeof(fb), all, 63);
            assert(n == 62);
            for (i = 0; i < n; i++) assert(all[i].version != 40);
            assert(all[0].version == 63 && all[61].version == 1);
        }
        printf("PASS: removing a version from a completely full block (no "
               "sentinel) writes a sentinel at the new end\n");
    }

    /* --- File and directory names (spec 4.2.5) --- */
    {
        char out[64];
        const char *why = NULL;
        struct { const char *in, *out; } good[] = {
            { "readme.md", "README.MD" },
            { "makefile", "MAKEFILE." },          /* the dot is always stored */
            { "MAKEFILE.", "MAKEFILE." },
            { ".login", ".LOGIN" },                /* null name, as VMS allows */
            { "ods2-spec_v1$.txt", "ODS2-SPEC_V1$.TXT" },
            { "A23456789012345678901234567890123456789.B23456789012345678901234567890123456789",
              "A23456789012345678901234567890123456789.B23456789012345678901234567890123456789" },
        };
        const char *bad[] = { "src/ods2_bitmap.c", "a.b.c", "", ".", "with space.txt",
                              "star*.txt", "back\\slash.c",
                              "A234567890123456789012345678901234567890.TXT" /* 40 */ };
        size_t i;
        for (i = 0; i < sizeof good / sizeof good[0]; i++) {
            char big[128];
            assert(ods2_make_file_name(good[i].in, big, sizeof big, &why));
            assert(strcmp(big, good[i].out) == 0);
        }
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            char big[128];
            why = NULL;
            assert(!ods2_make_file_name(bad[i], big, sizeof big, &why) && why != NULL);
        }
        assert(!ods2_make_file_name("src/x.c", out, sizeof out, &why) && strstr(why, "'/'"));
        assert(ods2_valid_dir_name("convert", &why) && ods2_valid_dir_name("A-B_C$1", &why));
        assert(!ods2_valid_dir_name("", &why) && !ods2_valid_dir_name("A.B", &why));
        assert(!ods2_valid_dir_name("src/x", &why));
        assert(!ods2_valid_dir_name("A234567890123456789012345678901234567890", &why));
        printf("PASS: names: only A-Z 0-9 $ _ -, one dot always stored "
               "(MAKEFILE -> MAKEFILE.), 39-character parts; '/' explained\n");
    }
    {
        assert(ods2_dir_name_compare("MAKEFILE", 8, "MAKEFILE.", 9) == 0);
        assert(ods2_dir_name_compare("makefile.", 9, "MAKEFILE.", 9) == 0);
        assert(ods2_dir_name_compare("A.B", 3, "A-B.C", 5) > 0);  /* '-' < '.' */
        assert(ods2_dir_name_compare("A.B", 3, "A.BC", 4) < 0);
        assert(ods2_dir_name_compare("AB", 2, "A.B", 3) > 0);     /* "AB." vs "A.B" */
        printf("PASS: name comparison is byte order, case-insensitive, with "
               "MAKEFILE equal to MAKEFILE.\n");
    }

    /* --- Choosing a block, splitting a full one --- */
    {
        static uint8_t blocks[3 * 512];
        ods2_fid_t f = { 1, 1, 0, 0 };
        memset(blocks, 0, sizeof blocks);
        blocks[0] = blocks[1] = 0xff;
        assert(ods2_insert_dir_entry(blocks, 512, "B.TXT", 1, f));
        assert(ods2_insert_dir_entry(blocks, 512, "C.TXT", 1, f));
        blocks[512] = blocks[513] = 0xff;                /* block 1: empty */
        blocks[1024] = blocks[1025] = 0xff;
        assert(ods2_insert_dir_entry(blocks + 1024, 512, "M.TXT", 1, f));
        assert(ods2_insert_dir_entry(blocks + 1024, 512, "P.TXT", 1, f));
        assert(ods2_dir_block_is_empty(blocks + 512) && !ods2_dir_block_is_empty(blocks));
        assert(ods2_dir_choose_block(blocks, 3, "A.TXT") == 0);  /* before all */
        assert(ods2_dir_choose_block(blocks, 3, "D.TXT") == 0);
        assert(ods2_dir_choose_block(blocks, 3, "M.TXT") == 2);  /* equal to a first */
        assert(ods2_dir_choose_block(blocks, 3, "Z.TXT") == 2);
        printf("PASS: a name goes in the last block whose first entry sorts "
               "at or before it (empty blocks skipped)\n");
    }
    {
        static uint8_t combined[1024], a[512], b[512];
        ods2_dir_entry_t ea[64], eb[64];
        ods2_fid_t f = { 1, 1, 0, 0 };
        char name[40], last_a[sizeof ea[0].name];
        int i, na, nb;
        memset(combined, 0, sizeof combined);
        combined[0] = combined[1] = 0xff;
        for (i = 0; i < 36; i++) {              /* 36 x 26-byte records: > 512 */
            snprintf(name, sizeof name, "FILE%04d.TXT", i * 7 % 36); /* out of order */
            assert(ods2_insert_dir_entry(combined, sizeof combined, name, 1, f));
        }
        assert(ods2_dir_split_block(combined, sizeof combined, a, b));
        na = ods2_parse_directory(a, 512, ea, 64);
        nb = ods2_parse_directory(b, 512, eb, 64);
        assert(na + nb == 36 && na >= 17 && nb >= 17);    /* about half each */
        for (i = 1; i < na; i++) assert(strcmp(ea[i - 1].name, ea[i].name) < 0);
        for (i = 1; i < nb; i++) assert(strcmp(eb[i - 1].name, eb[i].name) < 0);
        snprintf(last_a, sizeof last_a, "%s", ea[na - 1].name);
        assert(strcmp(last_a, eb[0].name) < 0);           /* order across the split */
        {
            static uint8_t one[1024];
            memset(one, 0, sizeof one);
            one[0] = one[1] = 0xff;
            assert(ods2_insert_dir_entry(one, sizeof one, "ONLY.TXT", 1, f));
            assert(!ods2_dir_split_block(one, sizeof one, a, b)); /* needs 2 records */
        }
        printf("PASS: a full block splits into two halves (%d + %d records), in "
               "order within and across them\n", na, nb);
    }

    printf("\nods2_directory_write_selftest: all checks passed\n");
    return 0;
}
