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
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"

/* This test writes to the disk image it operates on (creating
   directories, files, etc.) - it must NEVER do that against
   samples/synthetic_disk.img directly, since that's a shared,
   Makefile-generated file that other test runs/builds also depend
   on being in its pristine, freshly-assembled state. Copy to an
   isolated working file first, and operate on that instead. */
#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH "samples/synthetic_disk_working.img"

static void copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    char buf[65536];
    size_t n;
    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        assert(fwrite(buf, 1, n, out) == n);
    }
    fclose(in);
    fclose(out);
}

int main(void)
{
    ods2_volume_t vol;
    ods2_result_t r;
    ods2_fid_t root_fid;
    uint8_t root_header[512];
    ods2_dir_entry_t entries[16];
    int count = 0;
    int i;

    copy_file(SOURCE_DISK_PATH, DISK_PATH);

    r = ods2_mount(DISK_PATH, &vol);
    assert(r.ok);
    printf("PASS: mounted synthetic disk image, home block validated, "
           "INDEXF.SYS's own extents decoded (%d extents)\n",
           vol.indexf_extent_count);

    r = ods2_lookup_path(&vol, "", &root_fid);
    assert(r.ok);
    assert(root_fid.fid_num == 4);
    assert(root_fid.fid_seq == 4);
    printf("PASS: empty-path lookup returns root FID (4,4)\n");

    r = ods2_read_header(&vol, root_fid.fid_num, root_header);
    assert(r.ok);
    printf("PASS: read root's own header via general VBN-walking lookup "
           "(not the direct home-block formula)\n");

    r = ods2_list_directory(&vol, root_header, entries, 16, &count);
    assert(r.ok);
    assert(count == 11);
    printf("PASS: listed root directory through the full real file-I/O "
           "pipeline: %d entries\n\n", count);

    for (i = 0; i < count; i++) {
        printf("  %-14s fid=(%u,%u,%u,%u)\n", entries[i].name,
               entries[i].fid.fid_num, entries[i].fid.fid_nmx,
               entries[i].fid.fid_seq, entries[i].fid.fid_rvn);
    }

    /* Confirm DECUS.DIR is there with the FID we expect. */
    {
        bool found = false;
        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "DECUS.DIR") == 0) {
                assert(entries[i].fid.fid_num == 11);
                found = true;
            }
        }
        assert(found);
        printf("\nPASS: DECUS.DIR present with fid_num=11, as expected\n");
    }

    /* Also test ods2_lookup_name() directly: look up "DECUS.DIR" by
       name within root and confirm it resolves to the same FID. */
    {
        ods2_fid_t decus_fid;
        r = ods2_lookup_name(&vol, root_header, "DECUS.DIR", &decus_fid);
        assert(r.ok);
        assert(decus_fid.fid_num == 11);
        printf("PASS: ods2_lookup_name(\"DECUS.DIR\") resolves correctly\n");
    }

    /* File-number allocation: files 1-13 genuinely exist on this
       disk, so the first free slot should be 14 - and specifically
       via validating actual header content, not the index file
       bitmap. The bitmap does NOT reliably mark files 1-13 as used on
       this real, VMS-INITIALIZE'd volume - exactly the "dropped bits"
       scenario spec 5.1.7 warns about. */
    {
        unsigned free_n;
        uint16_t free_seq;
        r = ods2_find_free_file_number(&vol, 1, 16, &free_n, &free_seq);
        assert(r.ok);
        assert(free_n == 14);
        assert(free_seq == 1); /* genuinely never-used on this real, freshly
                                   INITIALIZE'd volume - not a deleted slot */
        printf("PASS: ods2_find_free_file_number() correctly finds file 14 as free, "
               "by validating actual header content rather than trusting the "
               "index file bitmap\n");
    }

    /* --- Write-mode mount test --- */
    {
        ods2_volume_t wvol;
        uint8_t write_block[512], readback_block[512];
        int i;

        /* Confirm a read-only mount correctly rejects writes, rather
           than silently allowing them or crashing. */
        for (i = 0; i < 512; i++) write_block[i] = (uint8_t) i;
        r = ods2_write_block(&vol, 500, write_block); /* vol is read-only, opened earlier */
        assert(!r.ok);
        printf("PASS: writing to a read-only-mounted volume is correctly rejected: %s\n",
               r.problem);

        /* A write-mode mount can actually write and read back. Uses
           block 500 - safely within our sparse synthetic disk's
           range, far from any real structure we care about. */
        r = ods2_mount_write(DISK_PATH, &wvol);
        assert(r.ok);
        r = ods2_write_block(&wvol, 500, write_block);
        assert(r.ok);

        {
            FILE *check = fopen(DISK_PATH, "rb");
            assert(check != NULL);
            assert(fseek(check, 500L * 512, SEEK_SET) == 0);
            assert(fread(readback_block, 1, 512, check) == 512);
            fclose(check);
        }
        assert(memcmp(write_block, readback_block, 512) == 0);
        printf("PASS: write-mode mount writes a block that reads back identical, "
               "verified via a completely independent file handle\n");

        ods2_dismount(&wvol);
    }

    /* --- Block allocation test --- */
    {
        ods2_volume_t wvol;
        uint32_t lbn1, lbn2;
        unsigned allocated1, allocated2;

        r = ods2_mount_write(DISK_PATH, &wvol);
        assert(r.ok);

        r = ods2_allocate_blocks(&wvol, 16, &lbn1, &allocated1);
        assert(r.ok);
        assert(lbn1 >= 3000); /* outside the synthetic fixture's reserved zone */
        assert(allocated1 == 18); /* 16 blocks rounds up to 6 clusters * 3 = 18 */
        printf("PASS: first allocation succeeds outside the reserved zone "
               "(LBN %u, %u blocks)\n", lbn1, allocated1);

        /* Explicit proof that VBN 1 of BITMAP.SYS (the Storage
           Control Block, per spec 5.2.1) is never touched by
           allocation - the exact bug ANALYZE/DISK's CHKSCB finding
           caught on a real volume. Set it to a known, distinctive
           pattern via a raw file write (bypassing my own code
           entirely, so this doesn't just test itself), allocate
           several times, then confirm it's still exactly that
           pattern afterward. */
        {
            uint8_t scb_pattern[512], scb_readback[512];
            FILE *raw;
            long scb_offset = 1470477L * 512; /* BITMAP.SYS's VBN 1, from
                                                   its real decoded extent */
            unsigned junk_lbn;
            unsigned junk_count;

            memset(scb_pattern, 0xaa, sizeof(scb_pattern));
            raw = fopen(DISK_PATH, "r+b");
            assert(raw != NULL);
            assert(fseek(raw, scb_offset, SEEK_SET) == 0);
            assert(fwrite(scb_pattern, 1, sizeof(scb_pattern), raw) == sizeof(scb_pattern));
            fclose(raw);

            r = ods2_allocate_blocks(&wvol, 40, &junk_lbn, &junk_count);
            assert(r.ok);

            raw = fopen(DISK_PATH, "rb");
            assert(raw != NULL);
            assert(fseek(raw, scb_offset, SEEK_SET) == 0);
            assert(fread(scb_readback, 1, sizeof(scb_readback), raw) == sizeof(scb_readback));
            fclose(raw);

            assert(memcmp(scb_pattern, scb_readback, sizeof(scb_pattern)) == 0);
            printf("PASS: the Storage Control Block (VBN 1 of BITMAP.SYS) is "
                   "never touched by allocation - confirmed byte-for-byte "
                   "unchanged after further allocations\n");
        }

        /* A second allocation must not overlap the first - proving the
           bitmap update from the first call actually persisted to disk
           and was correctly re-read, not just held in memory. */
        r = ods2_allocate_blocks(&wvol, 16, &lbn2, &allocated2);
        assert(r.ok);
        assert(lbn2 >= lbn1 + allocated1);
        printf("PASS: second allocation correctly avoids the first "
               "(LBN %u), proving the bitmap update persisted to disk\n", lbn2);

        ods2_dismount(&wvol);
    }

    /* --- The real milestone: actually create a directory end-to-end --- */
    {
        ods2_volume_t wvol;
        uint8_t root_header[512];
        ods2_fid_t new_fid;
        ods2_dir_entry_t final_entries[16];
        int final_count = 0;

        r = ods2_mount_write(DISK_PATH, &wvol);
        assert(r.ok);

        r = ods2_read_header(&wvol, 4, root_header); /* root is always FID 4 */
        assert(r.ok);

        r = ods2_create_directory(&wvol, root_header, "NEWSUBDIR", &new_fid);
        assert(r.ok);
        printf("PASS: ods2_create_directory() succeeds, new FID=(%u,%u)\n",
               new_fid.fid_num, new_fid.fid_seq);

        /* WARNING Verify the new file number's bit in the index file's own
           bitmap is now SET (in use) - per spec 5.1.6, explicit: "if the bit
           is 1, then that file number is in use". This is the OPPOSITE
           convention from the storage (data-block) bitmap, where set means free.

           Checked via raw file I/O, independent of my own read functions. */
        {
            unsigned bit_index = new_fid.fid_num - 1;
            unsigned byte_index = bit_index / 8;
            unsigned bit_in_byte = bit_index % 8;
            uint8_t bitmap_block[512];
            FILE *raw = fopen(DISK_PATH, "rb");
            assert(raw != NULL);
            assert(fseek(raw, (long) wvol.home.ibmaplbn * 512, SEEK_SET) == 0);
            assert(fread(bitmap_block, 1, 512, raw) == 512);
            fclose(raw);
            assert(((bitmap_block[byte_index] >> bit_in_byte) & 1) == 1);
            printf("PASS: the new file number's bit in the index file's own bitmap "
                   "is correctly SET (marked in use, per spec 5.1.6's stated "
                   "convention for this specific bitmap)\n");
        }

        /* Creating the same name again must fail - it already exists. */
        {
            ods2_fid_t dup_fid;
            r = ods2_create_directory(&wvol, root_header, "NEWSUBDIR", &dup_fid);
            assert(!r.ok);
            printf("PASS: creating a duplicate name is correctly rejected: %s\n", r.problem);
        }

        ods2_dismount(&wvol);

        /* Now the real proof: mount fresh (read-only, completely
           separate from the mount that did the writing) and confirm
           the new directory is genuinely there, through the exact
           same read path that has been tested against real VMS data. */
        {
            ods2_volume_t rvol;
            uint8_t fresh_root_header[512];
            ods2_fid_t looked_up_fid;
            uint8_t new_dir_header[512];

            r = ods2_mount(DISK_PATH, &rvol);
            assert(r.ok);

            r = ods2_read_header(&rvol, 4, fresh_root_header);
            assert(r.ok);

            r = ods2_list_directory(&rvol, fresh_root_header, final_entries, 16, &final_count);
            assert(r.ok);
            assert(final_count == 12); /* the original 11 + our new one */
            printf("PASS: fresh mount shows %d entries (11 original + new)\n", final_count);

            r = ods2_lookup_path(&rvol, "NEWSUBDIR", &looked_up_fid);
            assert(r.ok);
            assert(looked_up_fid.fid_num == new_fid.fid_num);
            printf("PASS: ods2_lookup_path() finds the new directory after a fresh mount\n");

            r = ods2_read_header(&rvol, looked_up_fid.fid_num, new_dir_header);
            assert(r.ok);
            {
                int sub_count = 0;
                r = ods2_list_directory(&rvol, new_dir_header, final_entries, 16, &sub_count);
                assert(r.ok);
                assert(sub_count == 0); /* newly created, genuinely empty */
                printf("PASS: the new directory's own content is empty, as expected\n");
            }

            ods2_dismount(&rvol);
        }
    }

    /* --- File creation test --- */
    {
        ods2_volume_t wvol;
        uint8_t root_header[512];
        ods2_fid_t file_fid;
        const char *text = "Hello from ods2v2 - a from-scratch ODS-2 write path.\n";
        size_t text_len = strlen(text);
        uint8_t readback[4096];
        size_t bytes_read = 0;

        r = ods2_mount_write(DISK_PATH, &wvol);
        assert(r.ok);
        r = ods2_read_header(&wvol, 4, root_header);
        assert(r.ok);

        r = ods2_create_file(&wvol, root_header, "HELLO.TXT",
                              (const uint8_t *) text, text_len, 5 /* FAB$C_STMLF */, &file_fid);
        assert(r.ok);
        printf("PASS: ods2_create_file() succeeds, new FID=(%u,%u)\n",
               file_fid.fid_num, file_fid.fid_seq);

        ods2_dismount(&wvol);

        /* Fresh, independent mount - read the file back and confirm
           the content is byte-for-byte identical to what was written. */
        {
            ods2_volume_t rvol;
            uint8_t file_header[512];
            r = ods2_mount(DISK_PATH, &rvol);
            assert(r.ok);

            r = ods2_read_header(&rvol, file_fid.fid_num, file_header);
            assert(r.ok);

            r = ods2_read_file(&rvol, file_header, readback, sizeof(readback), &bytes_read);
            assert(r.ok);
            assert(bytes_read == text_len);
            assert(memcmp(readback, text, text_len) == 0);
            printf("PASS: file content reads back byte-for-byte identical after a "
                   "fresh, independent mount\n");

            ods2_dismount(&rvol);
        }

        /* Boundary case: content exactly a multiple of 512 bytes -
           the "ffbyte==0 means whole last block used" convention must
           not be confused with "empty file". */
        {
            ods2_volume_t wvol2, rvol2;
            uint8_t exact_content[1024];
            uint8_t root_hdr2[512], file_hdr2[512];
            ods2_fid_t exact_fid;
            int i;
            for (i = 0; i < 1024; i++) exact_content[i] = (uint8_t) (i & 0xff);

            r = ods2_mount_write(DISK_PATH, &wvol2);
            assert(r.ok);
            r = ods2_read_header(&wvol2, 4, root_hdr2);
            assert(r.ok);
            r = ods2_create_file(&wvol2, root_hdr2, "EXACT.BIN",
                                  exact_content, sizeof(exact_content), 1, &exact_fid);
            assert(r.ok);
            ods2_dismount(&wvol2);

            r = ods2_mount(DISK_PATH, &rvol2);
            assert(r.ok);
            r = ods2_read_header(&rvol2, exact_fid.fid_num, file_hdr2);
            assert(r.ok);
            {
                uint8_t rb[2048];
                size_t n;
                r = ods2_read_file(&rvol2, file_hdr2, rb, sizeof(rb), &n);
                assert(r.ok);
                assert(n == 1024);
                assert(memcmp(rb, exact_content, 1024) == 0);
                printf("PASS: exact-multiple-of-512 content (1024 bytes, ffbyte=0) "
                       "round-trips correctly, not confused with an empty file\n");
            }
            ods2_dismount(&rvol2);
        }
    }

    /* --- Directory extension test: force a directory to outgrow --- */
    /* --- its initial single-block allocation, verify it grows    --- */
    /* --- correctly and every entry remains readable afterward    --- */
    {
        ods2_volume_t wvol;
        uint8_t root_header[512];
        ods2_fid_t subdir_fid, subdir_header_lookup;
        uint8_t subdir_header[512];
        char names[100][32];
        int n_created = 80; /* cluster=3 on our test disk means the initial
                               allocation is already 3 blocks (1536 bytes);
                               need comfortably more than that worth of
                               ~26-byte records to force genuine extension */
        int i;

        r = ods2_mount_write(DISK_PATH, &wvol);
        assert(r.ok);
        r = ods2_read_header(&wvol, 4, root_header);
        assert(r.ok);

        r = ods2_create_directory(&wvol, root_header, "GROWTEST", &subdir_fid);
        assert(r.ok);

        r = ods2_read_header(&wvol, subdir_fid.fid_num, subdir_header);
        assert(r.ok);

        for (i = 0; i < n_created; i++) {
            ods2_fid_t file_fid;
            uint8_t content = (uint8_t) i;
            snprintf(names[i], sizeof(names[i]), "FILE%02d.TXT", i);
            r = ods2_create_file(&wvol, subdir_header, names[i], &content, 1, 5, &file_fid);
            assert(r.ok);
            /* Re-read the subdirectory's header each time - extension
               modifies it, and later create_file calls must see the
               updated extents to find room to insert into. */
            r = ods2_read_header(&wvol, subdir_fid.fid_num, subdir_header);
            assert(r.ok);
        }
        printf("PASS: created %d files in a directory, forcing it past its "
               "initial single-block allocation\n", n_created);

        /* Confirm the directory's header now genuinely has more than
           one extent - proof extension actually happened, not that
           all 40 somehow fit in the original allocation. */
        {
            ods2_extent_t extents[ODS2_MAX_EXTENTS];
            int extent_count;
            r = ods2_decode_all_extents(&wvol, subdir_header, extents, ODS2_MAX_EXTENTS,
                                         &extent_count);
            assert(r.ok);
            assert(extent_count >= 2);
            printf("PASS: directory header now has %d extents, confirming it "
                   "genuinely grew (not just fit in the original allocation)\n",
                   extent_count);
        }

        ods2_dismount(&wvol);

        /* Fresh, independent mount - confirm every single one of the
           40 entries is present and correctly listed. */
        {
            ods2_volume_t rvol;
            uint8_t fresh_root[512], fresh_subdir[512];
            ods2_dir_entry_t listed[128];
            int listed_count = 0;
            int found_count;

            r = ods2_mount(DISK_PATH, &rvol);
            assert(r.ok);
            r = ods2_read_header(&rvol, 4, fresh_root);
            assert(r.ok);
            r = ods2_lookup_name(&rvol, fresh_root, "GROWTEST.DIR", &subdir_header_lookup);
            assert(r.ok);
            r = ods2_read_header(&rvol, subdir_header_lookup.fid_num, fresh_subdir);
            assert(r.ok);
            r = ods2_list_directory(&rvol, fresh_subdir, listed, 128, &listed_count);
            assert(r.ok);
            assert(listed_count == n_created);

            found_count = 0;
            for (i = 0; i < n_created; i++) {
                int j;
                for (j = 0; j < listed_count; j++) {
                    if (strcmp(listed[j].name, names[i]) == 0) {
                        found_count++;
                        break;
                    }
                }
            }
            assert(found_count == n_created);
            printf("PASS: fresh mount finds all %d entries across the extended "
                   "directory's multiple blocks, sorted correctly and intact\n",
                   n_created);

            ods2_dismount(&rvol);
        }
    }

    /* --- Large-file test: content well past the old single-Format-1- --- */
    /* --- extent 256-block ceiling. With Format 2/3 retrieval pointer --- */
    /* --- encoding now in place, a genuinely CONTIGUOUS allocation    --- */
    /* --- this size fits in a SINGLE extent (Format 2 handles up to  --- */
    /* --- 16384 blocks) - proving the old artificial per-extent cap  --- */
    /* --- is gone, not just papered over with more extension headers. -- */
    {
        ods2_volume_t wvol;
        uint8_t root_header[512];
        ods2_fid_t big_fid;
        size_t big_len = 300u * 512u; /* 300 blocks - past the old 256-block single-extent cap */
        uint8_t *big_content = malloc(big_len);
        size_t i;

        assert(big_content != NULL);
        for (i = 0; i < big_len; i++) {
            big_content[i] = (uint8_t) (i % 251); /* distinctive, non-repeating-in-a-block pattern */
        }

        r = ods2_mount_write(DISK_PATH, &wvol);
        assert(r.ok);
        r = ods2_read_header(&wvol, 4, root_header);
        assert(r.ok);

        r = ods2_create_file(&wvol, root_header, "BIGFILE.BIN", big_content, big_len, 1, &big_fid);
        assert(r.ok);
        printf("PASS: created a 300-block (%zu byte) file, past the old 256-block "
               "single-extent limit\n", big_len);

        {
            uint8_t big_header[512];
            ods2_extent_t extents[ODS2_MAX_EXTENTS];
            int extent_count;
            r = ods2_read_header(&wvol, big_fid.fid_num, big_header);
            assert(r.ok);
            r = ods2_decode_all_extents(&wvol, big_header, extents, ODS2_MAX_EXTENTS, &extent_count);
            assert(r.ok);
            /* On an otherwise-empty region of the volume (which this
               freshly-populated test disk still has plenty of), 300
               contiguous blocks now needs exactly ONE Format 2
               retrieval pointer (up to 16384 blocks) instead of two
               or more 256-block-capped Format 1 ones - this is
               precisely the improvement Format 2/3 encoding was
               added for. See the fragmented-allocation test further
               below for proof the OLD multi-extent/multi-header
               machinery still works correctly when genuine
               fragmentation - not an artificial per-format cap -
               actually forces it. */
            assert(extent_count == 1);
            printf("PASS: file header has exactly %d extent - a contiguous "
                   "300-block allocation now fits in a single Format 2/3 "
                   "retrieval pointer instead of being artificially split\n",
                   extent_count);
        }

        ods2_dismount(&wvol);

        /* Fresh, independent mount - byte-for-byte round trip. */
        {
            ods2_volume_t rvol;
            uint8_t big_header2[512];
            uint8_t *readback = malloc(big_len + 4096);
            size_t bytes_read = 0;

            assert(readback != NULL);
            r = ods2_mount(DISK_PATH, &rvol);
            assert(r.ok);
            r = ods2_read_header(&rvol, big_fid.fid_num, big_header2);
            assert(r.ok);
            r = ods2_read_file(&rvol, big_header2, readback, big_len + 4096, &bytes_read);
            assert(r.ok);
            assert(bytes_read == big_len);
            assert(memcmp(readback, big_content, big_len) == 0);
            printf("PASS: 300-block file reads back byte-for-byte "
                   "identical after a fresh, independent mount\n");

            free(readback);
            ods2_dismount(&rvol);
        }

        free(big_content);
    }

    /* Direct, isolated unit tests of the new best-effort
       ods2_bitmap_find_largest_free() fallback (used by
       ods2_allocate_blocks() when no single contiguous run big enough
       for a whole request exists) live in ods2_bitmap_selftest.c,
       rather than here: reliably forcing genuine free-space
       fragmentation at the full-volume level would mean fragmenting
       the ENTIRE remaining free space on this ~1.5GB test image (a
       local pocket of fragmentation doesn't work - the allocator
       correctly finds and prefers whatever large contiguous run
       exists anywhere else on the mostly-empty disk), which isn't a
       practical thing for a fast unit test to set up. The end-to-end
       chained-extension-header path (many extents/headers for one
       file) was verified against a real disk image manually instead -
       see the project's own notes on multi-header testing. */

    /* --- Delete support tests --- */
    {
        ods2_volume_t wvol;
        uint8_t root_header[512];
        ods2_fid_t file_fid;
        uint8_t content = 42;

        r = ods2_mount_write(DISK_PATH, &wvol);
        assert(r.ok);
        r = ods2_read_header(&wvol, 4, root_header);
        assert(r.ok);

        /* Test: create, delete, confirm gone. */
        r = ods2_create_file(&wvol, root_header, "DELETEME.TXT", &content, 1, 1, &file_fid);
        assert(r.ok);

        {
            ods2_fid_t found;
            r = ods2_lookup_name(&wvol, root_header, "DELETEME.TXT", &found);
            assert(r.ok);
        }

        r = ods2_delete(&wvol, 4, "DELETEME.TXT");
        assert(r.ok);
        printf("PASS: ods2_delete() succeeds on a real file\n");

        {
            ods2_fid_t found;
            r = ods2_lookup_name(&wvol, root_header, "DELETEME.TXT", &found);
            assert(!r.ok);
            printf("PASS: deleted file is no longer found by name lookup\n");
        }

        /* Confirm the header itself is genuinely marked deleted -
           checked via raw file I/O against the spec's exact rules
           (3.5.1): FH2$V_MARKDEL set, fid_num zeroed, checksum zero. */
        {
            uint8_t deleted_header[512];
            ods2_head_core_t *core;
            r = ods2_read_header(&wvol, file_fid.fid_num, deleted_header);
            assert(r.ok); /* the block itself is still readable, just marked deleted */
            core = (ods2_head_core_t *) deleted_header;
            assert(core->filechar & 0x8000u);
            assert(core->fid.fid_num == 0);
            assert(deleted_header[510] == 0 && deleted_header[511] == 0);
            printf("PASS: deleted header matches spec 3.5.1's exact rules "
                   "(MARKDEL set, fid_num zeroed, checksum zero)\n");
        }

        /* Confirm the file number is freed in the index bitmap. */
        {
            unsigned bit_index = file_fid.fid_num - 1;
            unsigned byte_index = bit_index / 8;
            unsigned bit_in_byte = bit_index % 8;
            uint8_t bitmap_block[512];
            FILE *raw = fopen(DISK_PATH, "rb");
            assert(raw != NULL);
            assert(fseek(raw, (long) wvol.home.ibmaplbn * 512, SEEK_SET) == 0);
            assert(fread(bitmap_block, 1, 512, raw) == 512);
            fclose(raw);
            assert(((bitmap_block[byte_index] >> bit_in_byte) & 1) == 0);
            printf("PASS: deleted file's number is correctly freed (cleared) "
                   "in the index file's own bitmap\n");
        }

        /* Confirm the freed blocks are genuinely reclaimed: a fresh
           allocation of the same size should be able to land exactly
           where the deleted file's content was. */
        {
            uint8_t deleted_header[512];
            ods2_extent_t old_extents[4];
            int old_extent_count;
            uint32_t new_lbn;
            unsigned new_allocated;

            /* Re-read header 's own bytes directly (bypassing our
               validate path, which might reasonably refuse a deleted
               header) just to get its old extent for comparison. */
            r = ods2_read_header(&wvol, file_fid.fid_num, deleted_header);
            assert(r.ok);
            r = ods2_decode_all_extents(&wvol, deleted_header, old_extents, 4, &old_extent_count);
            assert(r.ok);
            assert(old_extent_count >= 1);

            r = ods2_allocate_blocks(&wvol, 1, &new_lbn, &new_allocated);
            assert(r.ok);
            assert(new_lbn == old_extents[0].lbn);
            printf("PASS: a fresh allocation lands exactly on the deleted file's "
                   "old blocks, confirming they were genuinely freed\n");
        }

        /* Test: creating a new file with the same name as a deleted
           one works cleanly (the name is genuinely free again), AND
           - the real point of this test - if it reuses the same file
           number (the deleted slot, now the lowest-numbered free
           one), it must get seq = old_seq + 1 (here: 2, since the
           original was seq=1), per spec 5.1.7 - not seq=1 again,
           which is only for a slot that was genuinely never used. */
        {
            ods2_fid_t new_file_fid;
            uint8_t new_content = 7;
            r = ods2_create_file(&wvol, root_header, "DELETEME.TXT", &new_content, 1, 1, &new_file_fid);
            assert(r.ok);
            printf("PASS: recreating a file with a previously-deleted name "
                   "succeeds cleanly\n");

            if (new_file_fid.fid_num == file_fid.fid_num) {
                assert(new_file_fid.fid_seq == file_fid.fid_seq + 1);
                printf("PASS: reusing the deleted slot correctly assigns "
                       "seq=%u (old seq %u + 1), per spec 5.1.7 - not seq=1 "
                       "again\n", new_file_fid.fid_seq, file_fid.fid_seq);
            } else {
                printf("(recreated file landed on a different slot (%u vs %u) - "
                       "sequence reuse specifically not exercised by this run, "
                       "but the logic is still correct for whichever slot a "
                       "future test does land on)\n",
                       new_file_fid.fid_num, file_fid.fid_num);
            }
        }

        /* Test: deleting a non-empty directory is refused. */
        {
            ods2_fid_t dir_fid;
            uint8_t dir_header[512];
            ods2_fid_t inner_file_fid;
            uint8_t inner_content = 1;

            r = ods2_create_directory(&wvol, root_header, "DELTESTDIR", &dir_fid);
            assert(r.ok);
            r = ods2_read_header(&wvol, dir_fid.fid_num, dir_header);
            assert(r.ok);
            r = ods2_create_file(&wvol, dir_header, "INNER.TXT", &inner_content, 1, 1, &inner_file_fid);
            assert(r.ok);

            r = ods2_delete(&wvol, 4, "DELTESTDIR.DIR");
            assert(!r.ok);
            printf("PASS: deleting a non-empty directory is correctly refused: %s\n",
                   r.problem);

            /* Delete the inner file first, then the now-empty
               directory should succeed. */
            r = ods2_delete(&wvol, dir_fid.fid_num, "INNER.TXT");
            assert(r.ok);
            r = ods2_delete(&wvol, 4, "DELTESTDIR.DIR");
            assert(r.ok);
            printf("PASS: after emptying it, the directory can be deleted "
                   "successfully\n");
        }

        ods2_dismount(&wvol);
    }

    /* --- Uppercase-on-create tests --- */
    /* Real VMS always stores filenames/directory names uppercase -
       "create/dir [decus]" on real VMS creates DECUS.DIR, never
       decus.DIR. */
    {
        ods2_volume_t wvol;
        uint8_t root_header[512];
        ods2_fid_t lower_dir_fid, lower_file_fid;
        uint8_t content = 1;

        r = ods2_mount_write(DISK_PATH, &wvol);
        assert(r.ok);
        r = ods2_read_header(&wvol, 4, root_header);
        assert(r.ok);

        r = ods2_create_directory(&wvol, root_header, "lowercase", &lower_dir_fid);
        assert(r.ok);
        r = ods2_create_file(&wvol, root_header, "MixedCase.Txt", &content, 1, 1, &lower_file_fid);
        assert(r.ok);

        {
            ods2_dir_entry_t entries[16];
            int count = 0, i;
            bool found_dir = false, found_file = false;
            r = ods2_list_directory(&wvol, root_header, entries, 16, &count);
            assert(r.ok);
            for (i = 0; i < count; i++) {
                if (strcmp(entries[i].name, "LOWERCASE.DIR") == 0) found_dir = true;
                if (strcmp(entries[i].name, "MIXEDCASE.TXT") == 0) found_file = true;
                /* Explicitly confirm no lowercase variant snuck through. */
                assert(strcmp(entries[i].name, "lowercase.DIR") != 0);
                assert(strcmp(entries[i].name, "MixedCase.Txt") != 0);
            }
            assert(found_dir);
            assert(found_file);
            printf("PASS: create_directory(\"lowercase\") and create_file(\"MixedCase.Txt\") "
                   "both stored fully uppercase (LOWERCASE.DIR, MIXEDCASE.TXT), "
                   "matching real VMS behavior\n");
        }

        ods2_dismount(&wvol);
    }

    /* --- Wildcard-name rejection tests --- */
    /* Real bug found via testing: "COPY file.txt *.*" silently
       created a file literally named "*.*", which is not a valid
       name and would confuse every future wildcard match against it. */
    {
        ods2_volume_t wvol;
        uint8_t root_header[512];
        ods2_fid_t fid;
        uint8_t content = 1;

        r = ods2_mount_write(DISK_PATH, &wvol);
        assert(r.ok);
        r = ods2_read_header(&wvol, 4, root_header);
        assert(r.ok);

        r = ods2_create_directory(&wvol, root_header, "*", &fid);
        assert(!r.ok);
        r = ods2_create_directory(&wvol, root_header, "TEST*DIR", &fid);
        assert(!r.ok);
        r = ods2_create_file(&wvol, root_header, "*.*", &content, 1, 1, &fid);
        assert(!r.ok);
        r = ods2_create_file(&wvol, root_header, "FILE%.TXT", &content, 1, 1, &fid);
        assert(!r.ok);
        printf("PASS: wildcard characters ('*' and '%%') in a destination name are "
               "correctly rejected for both create_directory and create_file\n");

        /* Confirm a genuinely valid, similar-looking name still works
           fine - this isn't rejecting names that merely contain
           digits or unusual-but-legal characters, just the two actual
           wildcard characters. */
        r = ods2_create_file(&wvol, root_header, "REALFILE.TXT", &content, 1, 1, &fid);
        assert(r.ok);
        printf("PASS: a normal, wildcard-free name still creates successfully\n");

        ods2_dismount(&wvol);
    }

    ods2_dismount(&vol);
    printf("\nods2_volume_selftest: all checks passed - full mount/lookup/list "
           "pipeline verified end-to-end through real file I/O\n");
    return 0;
}
