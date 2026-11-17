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

/* ods2_off64_selftest - blocks and byte counts past 2 GiB and past 4 GiB (issue #25).
 *
 * A disk image is addressed by LBN * 512, and that product does not fit a 32-bit `long` (a 32-bit host, or Windows,
 * where `long` is 32 bits) from LBN 2^22 (2 GiB) on; a file's length does not fit a 32-bit size_t from 2^23 blocks
 * (4 GiB). The volumes ods2v2 reads are VMS volumes of up to 2 TiB (SCB$L_VOLSIZE is 32 bits), and a file may be as
 * long as 2^32 blocks. On a 64-bit Unix host `long` and `size_t` are 64 bits, so this test can only show that the
 * offsets come out right there; on a 32-bit or LLP64 host the same test is what would have failed.
 *
 * On a copy of the synthetic disk (a sparse image, so the blocks far past its end cost no disk space):
 *
 *  - ods2_write_block, ods2_write_file_block and ods2_read_file_block / ods2_read_file reach LBNs of 2^22 + 3
 *    (past 2 GiB), 2^23 + 3 (past 4 GiB), 2^31 + 3 (past 1 TiB) and 2^32 - 20 (the last blocks under 2 TiB). The bytes
 *    are checked by an independent pread() at (off_t) LBN * 512: a block that went to the wrong offset, or one that
 *    wrapped to a low offset, shows as a failure that names the byte offset expected;
 *  - ods2_file_content_length and ods2_file_allocated_bytes return 64-bit values, and the right ones, for a file of
 *    more than 4 GiB (EFBLK 2^23 + 9);
 *  - ods2_read_file_stream delivers all of that file's 2^32 + 4096 bytes: the byte count that comes back is 64-bit,
 *    and the first and the last block arrive at the stream offsets they have in the file.
 *
 * Skips (and succeeds) where the system itself cannot write at a byte offset of 2 TiB (asked with pwrite(), not of ods2v2). */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* fseeko, pread */
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include "ods2_volume.h"
#include "ods2_validate.h"
#include "ods2_header_build.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/off64_disk.img"

static int fails = 0;

/* a block that depends on the number it is given, so that a block in the wrong place is noticed */
static void pattern(uint32_t seed, uint8_t *block)
{
    uint32_t x = seed * 2654435761u + 12345u;
    int i;
    for (i = 0; i < 512; i++) {
        x = x * 1103515245u + 12345u;
        block[i] = (uint8_t) (x >> 16);
    }
}

static void check(int cond, const char *what, long long expected_offset)
{
    if (!cond) {
        printf("FAIL: %s (expected at byte offset %lld = LBN %lld * 512)\n", what, expected_offset, expected_offset / 512);
        fails++;
    }
}

/* Copies the (mostly empty) 1.4 GiB image without writing its runs of zeros, so the copy stays sparse and quick. */
static void copy_sparse(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    static char buf[1 << 20];
    static const char zero[1 << 20];
    size_t n;
    off_t total = 0;
    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (n == sizeof(buf) && memcmp(buf, zero, n) == 0) {
            assert(fseeko(out, (off_t) n, SEEK_CUR) == 0);
        } else {
            assert(fwrite(buf, 1, n, out) == n);
        }
        total += (off_t) n;
    }
    /* make sure a trailing hole still gives the full length */
    assert(fseeko(out, total - 1, SEEK_SET) == 0);
    {
        char last;
        assert(fseeko(in, total - 1, SEEK_SET) == 0);
        assert(fread(&last, 1, 1, in) == 1);
        assert(fwrite(&last, 1, 1, out) == 1);
    }
    fclose(in);
    fclose(out);
}

/* The 512 bytes at `lbn` as the file system holds them, read without going through ods2_volume.c. */
static int raw_block(int fd, uint32_t lbn, uint8_t *block)
{
    return pread(fd, block, 512, (off_t) lbn * 512) == 512;
}

/* A header for a file of `blocks` blocks in one extent at `lbn` (length in blocks: EFBLK = blocks + 1 unless given). */
static void make_header(uint8_t *header, uint32_t lbn, uint32_t blocks, uint32_t efblk, uint16_t ffbyte)
{
    ods2_extent_t ext;
    ods2_header_spec_t spec;
    ext.lbn = lbn;
    ext.block_count = blocks;
    memset(&spec, 0, sizeof(spec));
    spec.fid.fid_num = 40;
    spec.fid.fid_seq = 1;
    spec.backlink.fid_num = 4;
    spec.backlink.fid_seq = 4;
    spec.rtype = 1;
    spec.rsize = 512;
    spec.extents = &ext;
    spec.extent_count = 1;
    spec.hiblk = blocks;
    spec.efblk = efblk;
    spec.ffbyte = ffbyte;
    spec.ident_name = "OFF64.DAT;1";
    assert(ods2_build_file_header(header, &spec));
}

/* four blocks at `lbn`, written and read back through the library, and looked at on the image with pread() */
static void four_blocks_at(ods2_volume_t *vol, int fd, uint32_t lbn, const char *what)
{
    uint8_t header[512], blk[512], got[512], whole[4 * 512];
    uint32_t v;
    size_t n = 0;
    char msg[160];
    make_header(header, lbn, 4, 5, 0);
    for (v = 1; v <= 4; v++) {
        pattern(lbn + v - 1, blk);
        snprintf(msg, sizeof(msg), "%s: ods2_write_file_block of VBN %u succeeds", what, v);
        check(ods2_write_file_block(vol, header, v, blk).ok, msg, (long long) (lbn + v - 1) * 512);
    }
    for (v = 1; v <= 4; v++) {
        long long off = (long long) (lbn + v - 1) * 512;
        pattern(lbn + v - 1, blk);
        snprintf(msg, sizeof(msg), "%s: ods2_write_file_block of VBN %u is on the image", what, v);
        check(raw_block(fd, lbn + v - 1, got) && memcmp(got, blk, 512) == 0, msg, off);
        memset(got, 0, 512);
        snprintf(msg, sizeof(msg), "%s: ods2_read_file_block of VBN %u gives it back", what, v);
        check(ods2_read_file_block(vol, header, v, got).ok && memcmp(got, blk, 512) == 0, msg, off);
    }
    snprintf(msg, sizeof(msg), "%s: ods2_read_file gives the 4 blocks", what);
    check(ods2_read_file(vol, header, whole, sizeof(whole), &n).ok && n == sizeof(whole), msg, (long long) lbn * 512);
    for (v = 0; v < 4 && n == sizeof(whole); v++) {
        pattern(lbn + v, blk);
        check(memcmp(whole + (size_t) v * 512, blk, 512) == 0, msg, (long long) (lbn + v) * 512);
    }
    /* the block before the file is untouched (it would hold a pattern if a write had wrapped to the wrong place) */
    memset(got, 0xAA, 512);
    if (raw_block(fd, lbn - 1, got)) {
        static const uint8_t zero[512];
        snprintf(msg, sizeof(msg), "%s: the block before is untouched", what);
        check(memcmp(got, zero, 512) == 0, msg, (long long) (lbn - 1) * 512);
    }
    printf("PASS: %s: LBN %u (byte offset %lld)\n", what, lbn, (long long) lbn * 512);
}

/* the stream sink: counts the bytes, and keeps the blocks that must be at two known stream offsets */
typedef struct {
    uint64_t pos;
    uint64_t first_at, last_at;       /* stream offsets of the two blocks looked for */
    uint8_t first[512], last[512];
    int first_seen, last_seen;
} sink_t;

static bool sink(void *ctx, const uint8_t *data, size_t n)
{
    sink_t *s = (sink_t *) ctx;
    uint64_t b0 = s->pos, b1 = s->pos + n;
    if (!s->first_seen && s->first_at >= b0 && s->first_at + 512 <= b1) {
        memcpy(s->first, data + (size_t) (s->first_at - b0), 512);
        s->first_seen = 1;
    }
    if (!s->last_seen && s->last_at >= b0 && s->last_at + 512 <= b1) {
        memcpy(s->last, data + (size_t) (s->last_at - b0), 512);
        s->last_seen = 1;
    }
    s->pos = b1;
    return true;
}

int main(void)
{
    ods2_volume_t vol;
    int fd;
    uint8_t header[512], blk[512], got[512];
    sink_t s;
    uint64_t want_len, want_alloc, bytes = 0;
    bool sink_failed = false;
    ods2_result_t r;

    _Static_assert(sizeof(off_t) >= 8, "off_t must be 64 bits");
    signal(SIGXFSZ, SIG_IGN);                   /* a file size limit must fail the write, not kill the test */

    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    /* can this file system hold a sparse file of 2 TiB? Asked of the system, not of ods2_volume.c: if the library cannot
       write there when the system can, that is a failure of the library, not a reason to skip */
    fd = open(DISK_PATH, O_WRONLY);
    assert(fd >= 0);
    memset(blk, 0, sizeof(blk));
    if (pwrite(fd, blk, 512, (off_t) (0xFFFFFFFFu - 19) * 512) != 512) {
        printf("SKIP: the file system cannot hold a sparse image of 2 TiB\n");
        close(fd);
        remove(DISK_PATH);
        return 0;
    }
    close(fd);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    fd = open(DISK_PATH, O_RDONLY);
    assert(fd >= 0);

    pattern(0xFFFFFFFFu - 19, blk);
    check(ods2_write_block(&vol, 0xFFFFFFFFu - 19, blk).ok, "ods2_write_block of LBN 2^32 - 20 succeeds",
          (long long) (0xFFFFFFFFu - 19) * 512);
    check(raw_block(fd, 0xFFFFFFFFu - 19, got) && memcmp(got, blk, 512) == 0,
          "ods2_write_block of LBN 2^32 - 20 is on the image", (long long) (0xFFFFFFFFu - 19) * 512);

    four_blocks_at(&vol, fd, (1u << 22) + 3, "past 2 GiB");
    four_blocks_at(&vol, fd, (1u << 23) + 3, "past 4 GiB");
    four_blocks_at(&vol, fd, (1u << 31) + 3, "past 1 TiB");
    four_blocks_at(&vol, fd, 0xFFFFFFFFu - 19, "the last blocks under 2 TiB");

    /* a file of more than 4 GiB: its length, its allocation and the stream that reads it are 64-bit */
    {
        const uint32_t lbn = (1u << 23) + 16;
        const uint32_t blocks = (1u << 23) + 9;             /* HIBLK */
        const uint32_t efblk = (1u << 23) + 9;              /* EOF in block 2^23 + 8, FFBYTE 0: 2^32 + 4096 bytes */
        make_header(header, lbn, blocks, efblk, 0);
        want_len = ((uint64_t) efblk - 1) * 512;
        want_alloc = (uint64_t) blocks * 512;
        check(sizeof(ods2_file_content_length(header)) >= 8 && sizeof(ods2_file_allocated_bytes(header)) >= 8,
              "ods2_file_content_length and ods2_file_allocated_bytes return at least 64 bits", 0);
        check(ods2_file_content_length(header) == want_len, "ods2_file_content_length is 2^32 + 4096", (long long) want_len);
        check(ods2_file_allocated_bytes(header) == want_alloc, "ods2_file_allocated_bytes is (2^23 + 9) * 512", (long long) want_alloc);
        printf("PASS: a file of %llu bytes (EFBLK %u): content length and allocated bytes are 64-bit\n",
               (unsigned long long) want_len, efblk);

        pattern(1, blk);
        check(ods2_write_file_block(&vol, header, 1, blk).ok, "ods2_write_file_block of the long file's first block succeeds",
              (long long) lbn * 512);
        pattern(2, got);
        check(ods2_write_file_block(&vol, header, efblk - 1, got).ok, "ods2_write_file_block of the long file's last block succeeds",
              (long long) (lbn + efblk - 2) * 512);
        check(raw_block(fd, lbn, got) && memcmp(got, blk, 512) == 0, "the first block of the long file is on the image",
              (long long) lbn * 512);
        pattern(2, blk);
        check(raw_block(fd, lbn + efblk - 2, got) && memcmp(got, blk, 512) == 0, "the last block of the long file is on the image",
              (long long) (lbn + efblk - 2) * 512);

        memset(&s, 0, sizeof(s));
        s.first_at = 0;
        s.last_at = want_len - 512;
        r = ods2_read_file_stream(&vol, header, sink, &s, &bytes, &sink_failed);
        check(r.ok && !sink_failed, "ods2_read_file_stream reads the whole file", (long long) want_len);
        check(bytes == want_len && s.pos == want_len, "ods2_read_file_stream's byte count is 2^32 + 4096 (a 64-bit count)",
              (long long) want_len);
        pattern(1, blk);
        check(s.first_seen && memcmp(s.first, blk, 512) == 0, "the stream's first block is the file's first block", 0);
        pattern(2, blk);
        check(s.last_seen && memcmp(s.last, blk, 512) == 0, "the stream's last block is the file's last block",
              (long long) s.last_at);
        printf("PASS: ods2_read_file_stream of a %llu byte file: byte count and block positions\n", (unsigned long long) want_len);
    }

    close(fd);
    ods2_dismount(&vol);
    remove(DISK_PATH);
    if (fails != 0) {
        printf("\nods2_off64_selftest: %d FAILED\n", fails);
        return 1;
    }
    printf("\nods2_off64_selftest: all checks passed\n");
    return 0;
}
