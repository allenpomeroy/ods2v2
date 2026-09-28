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

/* ods2_versions_selftest.c - file versions and end-of-file marks,
 * end to end through a mounted volume.
 *
 * ods2v2 itself only ever writes version 1, so the multi-version
 * directory VMS would produce is laid out here by hand, block by
 * block, the way the real disk in issue #4 has it:
 *
 *   [VERS] block 1:  NOTE.TXT   ;3 ;2 ;1   (three real, distinct files)
 *                    SAVAGE.LIS ;64 ;63 ;62
 *   [VERS] block 2:  SAVAGE.LIS ;61 ... ;1  (the same name continues in
 *                                            a second record, as VMS
 *                                            splits a name whose
 *                                            versions do not fit)
 *   [VERS] block 3:  YOW.ELC    ;1
 *
 * Covers: listing every version (issue #4), addressing a version
 * explicitly, by ";"/";0" and relatively (issue #5), deleting one
 * version without disturbing the others (issue #5), and the
 * spec 6.1.5/6.1.6 end-of-file mark - checked against real
 * VMS-written headers - that ods2_file_content_length() and
 * ods2_read_file() use (issue #6).
 *
 * Also leaves samples/versions_cli_disk.img (the fixture, before any
 * deletes) for the Makefile's CLI checks.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define CLI_DISK_PATH    "samples/versions_cli_disk.img"
#define DISK_PATH        "samples/versions_disk.img"

#define SAVAGE_VERSIONS 64
#define SAVAGE_BLOCK1   3   /* ;64 ;63 ;62 in block 1, ;61..;1 in block 2 */

/* Copies a disk image, seeking over all-zero blocks instead of
   writing them, so the (sparse, 1.5GB-apparent) synthetic image stays
   sparse and the copy takes a moment rather than 1.5GB of disk. */
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
    /* Make the file its full length even if it ends in zeros. */
    assert(fseek(out, total - 1, SEEK_SET) == 0);
    assert(fputc(0, out) != EOF);
    fclose(in);
    fclose(out);
}

/* Writes one directory record at `pos`: name, then the given
   (version, FID) pairs, newest first. Returns the end position. */
static size_t put_record(uint8_t *block, size_t pos, const char *name,
                         const uint16_t *versions, const ods2_fid_t *fids, int n)
{
    size_t namelen = strlen(name);
    size_t padded = namelen + (namelen % 2);
    size_t total = 6 + padded + 8u * (size_t) n;
    size_t e = pos + 6 + padded;
    int i;

    assert(pos + total + 2 <= 512);
    block[pos] = (uint8_t) ((total - 2) & 0xff);
    block[pos + 1] = (uint8_t) ((total - 2) >> 8);
    block[pos + 2] = 1; block[pos + 3] = 0;  /* verlimit */
    block[pos + 4] = 0;                      /* flags */
    block[pos + 5] = (uint8_t) namelen;
    memcpy(block + pos + 6, name, namelen);
    if (padded > namelen) block[pos + 6 + namelen] = 0;
    for (i = 0; i < n; i++, e += 8) {
        block[e] = (uint8_t) (versions[i] & 0xff);
        block[e + 1] = (uint8_t) (versions[i] >> 8);
        block[e + 2] = (uint8_t) (fids[i].fid_num & 0xff);
        block[e + 3] = (uint8_t) (fids[i].fid_num >> 8);
        block[e + 4] = (uint8_t) (fids[i].fid_seq & 0xff);
        block[e + 5] = (uint8_t) (fids[i].fid_seq >> 8);
        block[e + 6] = fids[i].fid_rvn;
        block[e + 7] = fids[i].fid_nmx;
    }
    block[pos + total] = 0xff;               /* sentinel */
    block[pos + total + 1] = 0xff;
    return pos + total;
}

static unsigned total_blocks(ods2_volume_t *vol, const uint8_t *header)
{
    ods2_extent_t ext[ODS2_MAX_EXTENTS];
    int n, i;
    unsigned total = 0;
    ods2_result_t r = ods2_decode_all_extents(vol, header, ext, ODS2_MAX_EXTENTS, &n);
    assert(r.ok);
    for (i = 0; i < n; i++) total += ext[i].block_count;
    return total;
}

static void create(ods2_volume_t *vol, unsigned dir_num, const char *name,
                   const char *content, size_t len, ods2_fid_t *fid_out)
{
    uint8_t dir_header[512];
    ods2_result_t r = ods2_read_header(vol, dir_num, dir_header);
    assert(r.ok);
    r = ods2_create_file(vol, dir_header, name, (const uint8_t *) content, len, 5, fid_out);
    if (!r.ok) fprintf(stderr, "create %s: %s\n", name, r.problem);
    assert(r.ok);
}

/* Reads the content of `name`;`version` in the directory into buf. */
static size_t read_version(ods2_volume_t *vol, unsigned dir_num, const char *name,
                           int version, char *buf, size_t buf_size, uint16_t *got_version)
{
    uint8_t dir_header[512], file_header[512];
    ods2_fid_t fid;
    size_t n = 0;
    ods2_result_t r = ods2_read_header(vol, dir_num, dir_header);
    assert(r.ok);
    r = ods2_lookup_name_version(vol, dir_header, name, version, &fid, got_version);
    assert(r.ok);
    r = ods2_read_header(vol, fid.fid_num, file_header);
    assert(r.ok);
    r = ods2_read_file(vol, file_header, (uint8_t *) buf, buf_size - 1, &n);
    assert(r.ok);
    buf[n] = '\0';
    return n;
}

static const char *const note_text[4] = {
    NULL, "note version one\n", "note version two\n", "note version three\n"
};

int main(void)
{
    ods2_volume_t vol;
    ods2_result_t r;
    uint8_t root_header[512], vers_header[512];
    ods2_fid_t vers_fid, note_fid[4], savage_fid, yow_fid;
    int i;

    /* ================= build the fixture ================= */
    copy_sparse(SOURCE_DISK_PATH, CLI_DISK_PATH);
    r = ods2_mount_write(CLI_DISK_PATH, &vol);
    assert(r.ok);
    r = ods2_read_header(&vol, 4, root_header);
    assert(r.ok);
    r = ods2_create_directory(&vol, root_header, "VERS", &vers_fid);
    assert(r.ok);

    /* Real files under temporary names; their directory entries are
       replaced by the hand-built blocks below. */
    for (i = 1; i <= 3; i++) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "NOTE%d.TMP", i);
        create(&vol, vers_fid.fid_num, tmp, note_text[i], strlen(note_text[i]), &note_fid[i]);
    }
    create(&vol, vers_fid.fid_num, "SAVAGE.TMP", "savage listing\n", 15, &savage_fid);
    create(&vol, vers_fid.fid_num, "YOW.TMP", "Yow! Are we having fun yet?\n", 28, &yow_fid);

    /* Grow [VERS] to at least 3 blocks, the same way a directory
       grows in normal use. */
    for (i = 0; ; i++) {
        char filler[32];
        r = ods2_read_header(&vol, vers_fid.fid_num, vers_header);
        assert(r.ok);
        if (total_blocks(&vol, vers_header) >= 3) break;
        snprintf(filler, sizeof(filler), "F%04d.TMP", i);
        r = ods2_insert_into_directory(&vol, vers_fid.fid_num, filler, 1, savage_fid);
        assert(r.ok);
    }

    {
        uint8_t block[512];
        uint16_t versions[SAVAGE_VERSIONS];
        ods2_fid_t fids[SAVAGE_VERSIONS];
        static const uint16_t note_versions[] = {3, 2, 1};
        ods2_fid_t note_fids[3];
        unsigned vbn, blocks = total_blocks(&vol, vers_header);
        size_t pos;

        note_fids[0] = note_fid[3];
        note_fids[1] = note_fid[2];
        note_fids[2] = note_fid[1];
        for (i = 0; i < SAVAGE_VERSIONS; i++) {
            versions[i] = (uint16_t) (SAVAGE_VERSIONS - i);
            fids[i] = savage_fid;
        }

        memset(block, 0, sizeof(block));
        pos = put_record(block, 0, "NOTE.TXT", note_versions, note_fids, 3);
        (void) put_record(block, pos, "SAVAGE.LIS", versions, fids, SAVAGE_BLOCK1);
        r = ods2_write_file_block(&vol, vers_header, 1, block);
        assert(r.ok);

        memset(block, 0, sizeof(block));
        (void) put_record(block, 0, "SAVAGE.LIS", versions + SAVAGE_BLOCK1, fids + SAVAGE_BLOCK1,
                          SAVAGE_VERSIONS - SAVAGE_BLOCK1);
        r = ods2_write_file_block(&vol, vers_header, 2, block);
        assert(r.ok);

        {
            static const uint16_t one[] = {1};
            memset(block, 0, sizeof(block));
            (void) put_record(block, 0, "YOW.ELC", one, &yow_fid, 1);
            r = ods2_write_file_block(&vol, vers_header, 3, block);
            assert(r.ok);
        }
        memset(block, 0, sizeof(block));
        for (vbn = 4; vbn <= blocks; vbn++) {
            r = ods2_write_file_block(&vol, vers_header, vbn, block);
            assert(r.ok);
        }
    }
    ods2_dismount(&vol);
    copy_sparse(CLI_DISK_PATH, DISK_PATH); /* tests below modify their own copy */

    r = ods2_mount_write(DISK_PATH, &vol);
    assert(r.ok);
    r = ods2_read_header(&vol, vers_fid.fid_num, vers_header);
    assert(r.ok);

    /* ================= issue #4: listing ================= */
    {
        ods2_dir_entry_t *e = NULL;
        int count = 0, counted = 0;
        r = ods2_list_directory_alloc(&vol, vers_header, &e, &count);
        assert(r.ok);
        assert(count == 3 + SAVAGE_VERSIONS + 1);
        for (i = 0; i < 3; i++) {
            assert(strcmp(e[i].name, "NOTE.TXT") == 0 && e[i].version == 3 - i);
            assert(e[i].fid.fid_num == note_fid[3 - i].fid_num);
        }
        for (i = 0; i < SAVAGE_VERSIONS; i++) {
            assert(strcmp(e[3 + i].name, "SAVAGE.LIS") == 0);
            assert(e[3 + i].version == SAVAGE_VERSIONS - i);
        }
        assert(strcmp(e[count - 1].name, "YOW.ELC") == 0 && e[count - 1].version == 1);
        free(e);
        r = ods2_count_directory_entries(&vol, vers_header, &counted);
        assert(r.ok && counted == count);
        printf("PASS: [VERS] lists all %d entries: NOTE.TXT;3..;1, all 64 versions of "
               "SAVAGE.LIS across its two records (issue #4 showed 2), YOW.ELC;1\n", count);
    }

    /* ================= issue #5: addressing versions ================= */
    {
        ods2_fid_t fid;
        uint16_t v = 0;

        r = ods2_lookup_name(&vol, vers_header, "savage.lis", &fid);
        assert(r.ok);
        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", 0, &fid, &v);
        assert(r.ok && v == 64);
        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", 62, &fid, &v);
        assert(r.ok && v == 62);                      /* last pair of record 1 */
        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", 61, &fid, &v);
        assert(r.ok && v == 61);                      /* first pair of record 2 */
        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", 1, &fid, &v);
        assert(r.ok && v == 1 && fid.fid_num == savage_fid.fid_num);
        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", -1, &fid, &v);
        assert(r.ok && v == 63);
        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", -3, &fid, &v);
        assert(r.ok && v == 61);                      /* counts across the record split */
        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", -63, &fid, &v);
        assert(r.ok && v == 1);
        printf("PASS: SAVAGE.LIS resolves ;64 ;62 ;61 ;1, ;0 (-> 64), ;-1 (-> 63) and "
               ";-3 (-> 61, across the record boundary)\n");

        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", -64, &fid, &v);
        assert(!r.ok && strcmp(r.problem, "version not found") == 0);
        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", 65, &fid, &v);
        assert(!r.ok && strcmp(r.problem, "version not found") == 0);
        r = ods2_lookup_name_version(&vol, vers_header, "NOPE.TXT", 1, &fid, &v);
        assert(!r.ok && strcmp(r.problem, "name not found in directory") == 0);
        r = ods2_lookup_name_version(&vol, vers_header, "SAVAGE.LIS", 40000, &fid, &v);
        assert(!r.ok);
        printf("PASS: absent versions, absent names and out-of-range versions are "
               "reported distinctly\n");

        r = ods2_lookup_name_version(&vol, vers_header, "YOW.ELC", 1, &fid, &v);
        assert(r.ok && fid.fid_num == yow_fid.fid_num);
        printf("PASS: YOW.ELC;1 resolves (issue #5's first example)\n");
    }
    {
        char buf[64];
        uint16_t v;
        read_version(&vol, vers_fid.fid_num, "NOTE.TXT", 1, buf, sizeof(buf), &v);
        assert(strcmp(buf, note_text[1]) == 0 && v == 1);
        read_version(&vol, vers_fid.fid_num, "NOTE.TXT", 2, buf, sizeof(buf), &v);
        assert(strcmp(buf, note_text[2]) == 0);
        read_version(&vol, vers_fid.fid_num, "NOTE.TXT", 0, buf, sizeof(buf), &v);
        assert(strcmp(buf, note_text[3]) == 0 && v == 3);
        read_version(&vol, vers_fid.fid_num, "NOTE.TXT", -2, buf, sizeof(buf), &v);
        assert(strcmp(buf, note_text[1]) == 0 && v == 1);
        printf("PASS: each NOTE.TXT version reads its own content (;1 ;2 ;0 ;-2)\n");
    }

    /* ================= issue #5: deleting one version ================= */
    {
        uint16_t v = 0;
        uint8_t hdr[512];
        char buf[64];
        ods2_dir_entry_t *e = NULL;
        int count = 0;

        r = ods2_delete_version(&vol, vers_fid.fid_num, "NOTE.TXT", 2, &v);
        assert(r.ok && v == 2);
        r = ods2_read_header(&vol, note_fid[2].fid_num, hdr);
        assert(r.ok && (((ods2_head_core_t *) hdr)->filechar & 0x8000u)); /* MARKDEL */

        r = ods2_read_header(&vol, vers_fid.fid_num, vers_header);
        assert(r.ok);
        r = ods2_list_directory_alloc(&vol, vers_header, &e, &count);
        assert(r.ok && count == 2 + SAVAGE_VERSIONS + 1);
        assert(e[0].version == 3 && e[1].version == 1 && strcmp(e[2].name, "SAVAGE.LIS") == 0);
        free(e);
        read_version(&vol, vers_fid.fid_num, "NOTE.TXT", 3, buf, sizeof(buf), &v);
        assert(strcmp(buf, note_text[3]) == 0);
        read_version(&vol, vers_fid.fid_num, "NOTE.TXT", 1, buf, sizeof(buf), &v);
        assert(strcmp(buf, note_text[1]) == 0);
        printf("PASS: deleting NOTE.TXT;2 frees only that file; ;3 and ;1 stay listed "
               "and readable (previously the whole record went)\n");

        r = ods2_delete_version(&vol, vers_fid.fid_num, "NOTE.TXT", 2, &v);
        assert(!r.ok && strcmp(r.problem, "version not found") == 0);

        r = ods2_delete(&vol, vers_fid.fid_num, "NOTE.TXT"); /* highest: ;3 */
        assert(r.ok);
        r = ods2_read_header(&vol, vers_fid.fid_num, vers_header);
        assert(r.ok);
        read_version(&vol, vers_fid.fid_num, "NOTE.TXT", 0, buf, sizeof(buf), &v);
        assert(v == 1 && strcmp(buf, note_text[1]) == 0);
        printf("PASS: ods2_delete() with no version deletes the highest, leaving ;1\n");
    }

    /* ================= issue #6: end-of-file mark ================= */
    {
        /* Real VMS-written headers from the sample disk. */
        uint8_t hdr[512];
        uint8_t block[512];
        size_t n = 99;

        r = ods2_read_header(&vol, 3, hdr);                 /* BADBLK.SYS: EFBLK 1, FFBYTE 0 */
        assert(r.ok);
        assert(ods2_file_content_length(hdr) == 0);
        r = ods2_read_file(&vol, hdr, block, sizeof(block), &n);
        assert(r.ok && n == 0);
        r = ods2_read_header(&vol, 4, hdr);                 /* 000000.DIR: EFBLK 2, FFBYTE 0 */
        assert(r.ok);
        assert(ods2_file_content_length(hdr) == 512);
        r = ods2_read_file(&vol, hdr, block, sizeof(block), &n);
        assert(r.ok && n == 512);
        r = ods2_read_header(&vol, 1, hdr);                 /* INDEXF.SYS: EFBLK 116, FFBYTE 0 */
        assert(r.ok);
        assert(ods2_file_content_length(hdr) == 115u * 512u);
        printf("PASS: real VMS headers: BADBLK.SYS is 0 bytes (read used to fail), "
               "000000.DIR 512, INDEXF.SYS 115 blocks\n");
    }
    {
        static const size_t sizes[] = {0, 1, 511, 512, 513, 1024};
        static const uint32_t want_efblk[] = {1, 1, 1, 2, 2, 3};
        static const uint16_t want_ffbyte[] = {0, 1, 511, 0, 1, 0};
        static uint8_t content[1024];
        uint8_t back[1100];
        size_t k;
        for (k = 0; k < sizeof(content); k++) content[k] = (uint8_t) ('A' + k % 26);

        for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            char name[16];
            ods2_fid_t fid;
            uint8_t hdr[512];
            ods2_head_core_t *core = (ods2_head_core_t *) hdr;
            size_t n = 0;

            snprintf(name, sizeof(name), "EOF%zu.BIN", sizes[k]);
            create(&vol, 4, name, (const char *) content, sizes[k], &fid);
            r = ods2_read_header(&vol, fid.fid_num, hdr);
            assert(r.ok);
            assert(ods2_word_swap32(core->recattr.efblk) == want_efblk[k]);
            assert(core->recattr.ffbyte == want_ffbyte[k]);
            assert(ods2_file_content_length(hdr) == sizes[k]);
            r = ods2_read_file(&vol, hdr, back, sizeof(back), &n);
            assert(r.ok && n == sizes[k] && memcmp(back, content, n) == 0);
        }
        printf("PASS: new files of 0/1/511/512/513/1024 bytes get the EOF mark VMS "
               "writes (e.g. 512 -> EFBLK 2 FFBYTE 0) and read back exactly\n");
    }
    {
        /* An EOF mark beyond the allocation is refused, not read. */
        uint8_t hdr[512], buf[512];
        ods2_head_core_t *core = (ods2_head_core_t *) hdr;
        size_t n;
        r = ods2_read_header(&vol, yow_fid.fid_num, hdr);
        assert(r.ok);
        core->recattr.efblk = ods2_word_swap32(ods2_word_swap32(core->recattr.hiblk) + 5);
        assert(ods2_file_content_length(hdr) > ods2_file_allocated_bytes(hdr));
        r = ods2_read_file(&vol, hdr, buf, sizeof(buf), &n);
        assert(!r.ok);
        printf("PASS: an EOF mark beyond the file's allocation is rejected: %s\n", r.problem);
    }
    {
        /* Names can never carry a version. */
        uint8_t hdr[512];
        ods2_fid_t fid;
        r = ods2_read_header(&vol, 4, hdr);
        assert(r.ok);
        r = ods2_create_file(&vol, hdr, "BAD.TXT;2", (const uint8_t *) "x", 1, 5, &fid);
        assert(!r.ok);
        r = ods2_create_directory(&vol, hdr, "BAD;1", &fid);
        assert(!r.ok);
        printf("PASS: creating a file or directory whose name contains ';' is refused\n");
    }

    ods2_dismount(&vol);
    remove(DISK_PATH);
    printf("\nods2_versions_selftest: all checks passed\n");
    return 0;
}
