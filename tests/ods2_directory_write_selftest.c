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

    printf("\nods2_directory_write_selftest: all checks passed\n");
    return 0;
}
