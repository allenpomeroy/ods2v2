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

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* flock(), fileno(), fdatasync() */
#endif

#include "ods2_volume.h"
#include "ods2_validate.h"
#include "ods2_bitmap.h"
#include "ods2_header_build.h"
#include "ods2_directory_write.h"
#include "ods2_checksum.h"
#include "ods2_time.h"
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/file.h>
#include <stdlib.h>

static ods2_result_t ok(void)
{
    ods2_result_t r; r.ok = true; r.problem = NULL; return r;
}
static ods2_result_t fail(const char *why)
{
    ods2_result_t r; r.ok = false; r.problem = why; return r;
}

/* Uppercases a string in place. Real VMS always stores filenames and
   directory names uppercase - "create/dir [decus]" on real VMS
   creates DECUS.DIR. Applied to every user-supplied name at the
   point of creation. */
static void uppercase_str(char *s)
{
    for (; *s; s++) {
        *s = (char) toupper((unsigned char) *s);
    }
}

/* Checks for ODS-2's two wildcard characters ('*' matches any
   sequence, '%' matches exactly one character - see
   ods2_wildcard.h). A wildcard in a DESTINATION name for a create
   operation is always a mistake, never something meaningful to
   actually store: it either means the caller meant to type a real
   name and fat-fingered a wildcard character into it, or meant to
   express something wildcard-related that doesn't make sense for a
   single create. */
static bool contains_wildcard(const char *name)
{
    return strchr(name, '*') != NULL || strchr(name, '%') != NULL;
}

static ods2_result_t header_ok_for_write(const uint8_t *h, unsigned file_number);
static ods2_result_t directory_ready(ods2_volume_t *vol, const uint8_t *dir_header, unsigned dir_file_number);

static int decode_header_extents(const uint8_t *header, ods2_extent_t *extents_out, size_t max)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    size_t map_bytes = (size_t) core->map_inuse * 2;
    size_t map_offset = (size_t) core->mpoffset * 2;
    if (map_bytes == 0 || map_offset + map_bytes > 512) return 0;
    return ods2_decode_retrieval_pointers(header + map_offset, core->map_inuse,
                                           extents_out, max);
}

/* Maps virtual block `vbn` (1-based) to an absolute LBN, given a list
   of extents describing the file's content in order. */
static bool vbn_to_lbn(const ods2_extent_t *extents, int extent_count,
                        unsigned vbn, uint32_t *lbn_out)
{
    unsigned vbn_cursor = 1;
    int i;
    for (i = 0; i < extent_count; i++) {
        unsigned extent_len = extents[i].block_count;
        if (vbn >= vbn_cursor && vbn < vbn_cursor + extent_len) {
            *lbn_out = extents[i].lbn + (vbn - vbn_cursor);
            return true;
        }
        vbn_cursor += extent_len;
    }
    return false;
}

static ods2_result_t mount_internal(const char *path, ods2_volume_t *vol,
                                     const char *fopen_mode, bool writable)
{
    ods2_validate_result_t vr;
    uint8_t indexf_header[512];
    ods2_result_t r;

    memset(vol, 0, sizeof(*vol));
    vol->fp = fopen(path, fopen_mode);
    if (vol->fp == NULL) {
        return fail("could not open disk image file");
    }
    vol->writable = writable;
    /* One writer or any number of readers: two processes writing one image, or one writing an image another reads,
       would corrupt it or read a half-done change. flock() is advisory: it stops another ods2v2, not a simulator that
       has the image open; a file system without flock() gives no lock and no error. ODS2_NOLOCK=1 turns it off (a test
       that holds a volume open while it mounts the same image again). */
    if (getenv("ODS2_NOLOCK") == NULL &&
        flock(fileno(vol->fp), (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        fclose(vol->fp);
        vol->fp = NULL;
        return fail(writable ? "the disk image is in use by another process (it is locked): not written"
                             : "the disk image is being written by another process (it is locked)");
    }

    if (fseek(vol->fp, 512, SEEK_SET) != 0 ||
        fread(&vol->home, 1, sizeof(vol->home), vol->fp) != sizeof(vol->home)) {
        ods2_dismount(vol);
        return fail("could not read home block");
    }

    vr = ods2_validate_home(&vol->home);
    if (!vr.ok) {
        ods2_dismount(vol);
        return fail(vr.problem);
    }

    /* Bootstrap: INDEXF.SYS's own header (file 1) is always locatable
       directly from home block fields alone. Spec 5.1.7 guarantees
       for the first 16 files. */
    {
        uint32_t lbn = vol->home.ibmaplbn + (vol->home.ibmapsize + 1 - 1);
        if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
            fread(indexf_header, 1, sizeof(indexf_header), vol->fp) != sizeof(indexf_header)) {
            ods2_dismount(vol);
            return fail("could not read INDEXF.SYS's own header");
        }
    }

    vol->indexf_extent_count = decode_header_extents(indexf_header,
        vol->indexf_extents, ODS2_MAX_EXTENTS);
    if (vol->indexf_extent_count == ODS2_DECODE_TOO_MANY) {
        ods2_dismount(vol);
        return fail("INDEXF.SYS has more extents than ods2v2 can hold (1,024): the volume is not mounted");
    }
    if (vol->indexf_extent_count <= 0) {
        ods2_dismount(vol);
        return fail("could not decode INDEXF.SYS's own retrieval pointers");
    }

    r = ok();
    return r;
}

ods2_result_t ods2_mount(const char *path, ods2_volume_t *vol)
{
    return mount_internal(path, vol, "rb", false);
}

ods2_result_t ods2_mount_write(const char *path, ods2_volume_t *vol)
{
    return mount_internal(path, vol, "r+b", true);
}

#ifdef ODS2_FAULT_INJECTION
/* Test builds only (tests/ods2_integrity_selftest.c). ods2_fault_after_writes: the (n+1)th write after it is set to
   n, and every one after it, fails, as if the machine stopped there; -1 is off. While ods2_fault_journal_on is set
   every block written is kept with what it replaced, so that the test can put the image back
   (ods2_fault_rollback) instead of copying a volume of 1.5 GB for each interruption it tries. */
long ods2_fault_after_writes = -1;
bool ods2_fault_journal_on = false;
static struct { uint32_t lbn; uint8_t old[512]; } *fault_journal;
static size_t fault_journal_n, fault_journal_cap;

void ods2_fault_forget(void)
{
    fault_journal_n = 0;
}

void ods2_fault_rollback(ods2_volume_t *vol)
{
    while (fault_journal_n > 0) {
        fault_journal_n--;
        if (fseek(vol->fp, (long) fault_journal[fault_journal_n].lbn * 512, SEEK_SET) != 0 ||
            fwrite(fault_journal[fault_journal_n].old, 1, 512, vol->fp) != 512) {
            abort();
        }
    }
    fflush(vol->fp);
}
#endif

ods2_result_t ods2_write_block(ods2_volume_t *vol, uint32_t lbn, const uint8_t *block)
{
    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
#ifdef ODS2_FAULT_INJECTION
    if (ods2_fault_after_writes >= 0) {
        if (ods2_fault_after_writes == 0) return fail("injected write failure");   /* and every write after it */
        ods2_fault_after_writes--;
    }
    if (ods2_fault_journal_on) {
        if (fault_journal_n == fault_journal_cap) {
            fault_journal_cap = fault_journal_cap ? fault_journal_cap * 2 : 256;
            fault_journal = realloc(fault_journal, fault_journal_cap * sizeof(*fault_journal));
            if (fault_journal == NULL) abort();
        }
        fault_journal[fault_journal_n].lbn = lbn;
        if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
            fread(fault_journal[fault_journal_n].old, 1, 512, vol->fp) != 512) {
            memset(fault_journal[fault_journal_n].old, 0, 512);
        }
        fault_journal_n++;
    }
#endif
    if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
        fwrite(block, 1, 512, vol->fp) != 512) {
        return fail("could not write block");
    }
    if (fflush(vol->fp) != 0) {
        return fail("could not flush written block");
    }
    return ok();
}

/* Makes what was written durable (fdatasync): a command whose writes sit in the host's cache is not done. */
ods2_result_t ods2_sync(ods2_volume_t *vol)
{
    if (vol->fp == NULL || !vol->writable) return ok();
    if (fflush(vol->fp) != 0 || fdatasync(fileno(vol->fp)) != 0) {
        return fail("could not make the written blocks durable (fdatasync failed)");
    }
    return ok();
}

void ods2_dismount(ods2_volume_t *vol)
{
    if (vol->fp != NULL && vol->writable) ods2_sync(vol);   /* best effort: a tool that exits without calling it first */
    if (vol->fp != NULL) {
        fclose(vol->fp);
        vol->fp = NULL;
    }
}

static bool header_lbn(ods2_volume_t *vol, unsigned file_number, uint32_t *lbn_out)
{
    unsigned v = vol->home.cluster;
    unsigned m = vol->home.ibmapsize;
    unsigned target_vbn = v * 4 + m + file_number;
    return vbn_to_lbn(vol->indexf_extents, vol->indexf_extent_count, target_vbn, lbn_out);
}

ods2_result_t ods2_read_header(ods2_volume_t *vol, unsigned file_number, uint8_t *header_out)
{
    uint32_t lbn;

    if (!header_lbn(vol, file_number, &lbn)) {
        return fail("file number's header VBN is not covered by INDEXF.SYS's known extents");
    }
    if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
        fread(header_out, 1, 512, vol->fp) != 512) {
        return fail("could not read file header block");
    }
    return ok();
}

/* The header a directory entry's FID names. An entry names a file by number AND sequence number AND relative volume
   number AND extended file number (FID_NMX: the file number is NMX << 16 | NUM). The header used to be found by the low
   16 bits of the number alone, so an entry with an RVN (a file on another volume of a set) or an NMX (a file number
   above 65,535) read some other file's header, silently: it is refused here, as is an entry whose sequence number is
   not the header's (the file was deleted and its number reused: VMS gives a reused number a new sequence number, and
   lists such an entry as "no such file") or whose header is marked for delete. */
const char ods2_stale_entry[] = "no such file: a dangling directory entry, a stale alias (the file it names was deleted; "
                                "its file number may have been reused)";

bool ods2_header_is_file(const uint8_t *header, ods2_fid_t fid)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    return core->fid.fid_num == fid.fid_num && core->fid.fid_seq == fid.fid_seq &&
           core->fid.fid_nmx == fid.fid_nmx && core->fid.fid_rvn == fid.fid_rvn &&
           !(core->filechar & 0x8000u);
}

ods2_result_t ods2_check_fid(ods2_fid_t fid)
{
    if (fid.fid_rvn != 0) {
        return fail("volume sets are not supported (a directory entry names a file on another volume of a set)");
    }
    if (fid.fid_nmx != 0) {
        return fail("file numbers above 65,535 are not supported (a directory entry names a file whose number needs "
                    "FID_NMX)");
    }
    return ok();
}

ods2_result_t ods2_read_header_fid(ods2_volume_t *vol, ods2_fid_t fid, uint8_t *header_out)
{
    ods2_result_t r = ods2_check_fid(fid);
    if (!r.ok) return r;
    r = ods2_read_header(vol, fid.fid_num, header_out);
    if (!r.ok) return r;
    if (!ods2_header_is_file(header_out, fid)) return fail(ods2_stale_entry);
    return ok();
}

ods2_result_t ods2_write_header(ods2_volume_t *vol, unsigned file_number, const uint8_t *header_in)
{
    uint32_t lbn;

    if (!header_lbn(vol, file_number, &lbn)) {
        return fail("file number's header VBN is not covered by INDEXF.SYS's known extents");
    }
    return ods2_write_block(vol, lbn, header_in);
}

ods2_result_t ods2_read_file_block(ods2_volume_t *vol, const uint8_t *header,
                                    unsigned vbn, uint8_t *block_out)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int n;
    uint32_t lbn;
    ods2_result_t r = ods2_decode_all_extents(vol, header, extents, ODS2_MAX_EXTENTS, &n);

    if (!r.ok) return r;
    if (n <= 0) {
        return fail("file has no decodable extents");
    }
    if (!vbn_to_lbn(extents, n, vbn, &lbn)) {
        return fail("requested VBN is not covered by this file's extents");
    }
    if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
        fread(block_out, 1, 512, vol->fp) != 512) {
        return fail("could not read file content block");
    }
    return ok();
}

ods2_result_t ods2_write_file_block(ods2_volume_t *vol, const uint8_t *header,
                                     unsigned vbn, const uint8_t *block_in)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int n;
    uint32_t lbn;
    ods2_result_t r = ods2_decode_all_extents(vol, header, extents, ODS2_MAX_EXTENTS, &n);

    if (!r.ok) return r;
    if (n <= 0) {
        return fail("file has no decodable extents");
    }
    if (!vbn_to_lbn(extents, n, vbn, &lbn)) {
        return fail("requested VBN is not covered by this file's extents");
    }
    return ods2_write_block(vol, lbn, block_in);
}

/* Shared body of ods2_allocate_blocks() and ods2_allocate_extent():
   with `partial_ok`, settles for the longest free run when none is
   long enough, allocating however many clusters that run holds. */
static ods2_result_t allocate_clusters(ods2_volume_t *vol, unsigned blocks_needed,
                                       bool partial_ok, uint32_t *lbn_out,
                                       unsigned *blocks_allocated_out)
{
    uint8_t bitmap_header[512];
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    unsigned total_bitmap_blocks = 0;
    unsigned bits_blocks; /* blocks actually holding bitmap bits, excluding the SCB */
    uint8_t *bitmap_data;
    size_t total_bits, total_bytes;
    size_t start_cluster, found_clusters;
    unsigned clusters_needed = (blocks_needed + vol->home.cluster - 1) / vol->home.cluster;
    ods2_result_t r;
    unsigned i;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }

    /* BITMAP.SYS is always file number 2 - confirmed by real root
       directory data.  BITMAP.SYS FID is consistently (2,2,...) on
       every volume examined. */
    r = ods2_read_header(vol, 2, bitmap_header);
    if (!r.ok) return r;

    r = ods2_decode_all_extents(vol, bitmap_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    if (extent_count <= 0) {
        return fail("could not decode BITMAP.SYS's own extents");
    }
    for (i = 0; i < (unsigned) extent_count; i++) total_bitmap_blocks += extents[i].block_count;

    /* Spec 5.2.1 SCB: Virtual block 1 of the storage bitmap is the
       storage control block, this is a completely separate structure, NOT
       bitmap data. The actual bitmap bits start at VBN 2 confirmed by ANALYZE/DISK
       CHKSCB finding on a real volume. */
    if (total_bitmap_blocks < 2) {
        return fail("BITMAP.SYS is too small to contain both an SCB and bitmap data");
    }
    bits_blocks = total_bitmap_blocks - 1;

    total_bytes = (size_t) bits_blocks * 512;
    total_bits = total_bytes * 8;

    /* Only clusters that lie wholly inside the volume may be handed out. The bitmap's last block has bits past the
       last cluster, and when the volume size is not a multiple of the cluster size the last cluster is partial: a run
       ending in either would map LBNs at or beyond SCB$L_VOLSIZE, writing past the end of the volume (on a SIMH
       container, over its metadata footer, and growing the image). VMS marks those bits allocated, but nothing on
       the write path may depend on that. */
    {
        uint8_t scb[512];
        uint32_t volsize;
        size_t usable_clusters;

        r = ods2_read_file_block(vol, bitmap_header, 1, scb);
        if (!r.ok) return r;
        volsize = (uint32_t) scb[4] | ((uint32_t) scb[5] << 8) |
                  ((uint32_t) scb[6] << 16) | ((uint32_t) scb[7] << 24); /* SCB$L_VOLSIZE, spec 5.2.1 */
        if (volsize == 0) {
            return fail("BITMAP.SYS's storage control block gives a volume size of 0: nothing is allocated");
        }
        usable_clusters = (size_t) volsize / vol->home.cluster;
        if (usable_clusters < total_bits) total_bits = usable_clusters;
    }

    bitmap_data = malloc(total_bytes);
    if (bitmap_data == NULL) {
        return fail("could not allocate memory to read the storage bitmap");
    }

    /* i is a bitmap-data block index (0-based); the corresponding
       real VBN within BITMAP.SYS is i+2 (i=0 -> VBN 2, the first
       real bitmap block, skipping VBN 1's SCB). */
    for (i = 0; i < bits_blocks; i++) {
        r = ods2_read_file_block(vol, bitmap_header, i + 2, bitmap_data + (size_t) i * 512);
        if (!r.ok) {
            free(bitmap_data);
            return r;
        }
    }

    if (partial_ok) {
        if (!ods2_bitmap_find_run(bitmap_data, total_bits, clusters_needed,
                                  &start_cluster, &found_clusters)) {
            free(bitmap_data);
            return fail("no free space left on the volume");
        }
        clusters_needed = (unsigned) found_clusters;
    } else if (!ods2_bitmap_find_free(bitmap_data, total_bits, clusters_needed,
                                      &start_cluster, &found_clusters)) {
        free(bitmap_data);
        return fail("no free space found on volume for this allocation");
    }
    if (!ods2_bitmap_mark(bitmap_data, total_bits, start_cluster, clusters_needed, true)) {
        free(bitmap_data);
        return fail("internal error marking bitmap bits (should be unreachable)");
    }

    /* Write back only the blocks that could have changed covering the bits
       we just marked - rather than the entire potentially large bitmap. Block indices
       here are again bitmap-data-relative; +2 converts back to the real VBN. */
    {
        size_t first_byte = start_cluster / 8;
        size_t last_byte = (start_cluster + clusters_needed - 1) / 8;
        unsigned first_block = (unsigned) (first_byte / 512);
        unsigned last_block = (unsigned) (last_byte / 512);
        for (i = first_block; i <= last_block; i++) {
            r = ods2_write_file_block(vol, bitmap_header, i + 2,
                                       bitmap_data + (size_t) i * 512);
            if (!r.ok) {
                free(bitmap_data);
                return r;
            }
        }
    }

    free(bitmap_data);

    *lbn_out = (uint32_t) (start_cluster * vol->home.cluster);
    *blocks_allocated_out = clusters_needed * vol->home.cluster;
    return ok();
}

ods2_result_t ods2_allocate_blocks(ods2_volume_t *vol, unsigned blocks_needed,
                                    uint32_t *lbn_out, unsigned *blocks_allocated_out)
{
    return allocate_clusters(vol, blocks_needed, false, lbn_out, blocks_allocated_out);
}

ods2_result_t ods2_allocate_extent(ods2_volume_t *vol, unsigned max_blocks,
                                    uint32_t *lbn_out, unsigned *blocks_allocated_out)
{
    return allocate_clusters(vol, max_blocks, true, lbn_out, blocks_allocated_out);
}

/* Decodes ALL of a file's extents, walking the ext_fid chain across
   extension headers if the file needs more than one (spec: a header
   whose Map Area can't hold all its retrieval pointers points to a
   continuation header via FH2$W_EXT_FID; that header's own ext_fid
   may point further still). Without this, any file needing more than
   one header would silently appear truncated. A generous but finite
   segment limit guards against a corrupted or circular ext_fid chain
   looping forever. */
#define ODS2_MAX_HEADER_SEGMENTS 64

/* Is `h`, read for the EXT_FID `ext` of the header chain of file `primary_number`, really segment `segment` of that
   file? Level 2, a sound checksum and offsets, its own FID (number and sequence) equal to the link that named it, its
   segment number the next one, and its back link the primary header (spec 3.5.2.16). A chain read by number alone
   follows a damaged EXT_FID into an unrelated live file: a delete would then destroy that file's header and free its
   clusters, and a read would return its blocks as the tail of this one. */
static bool extension_header_ok(const uint8_t *h, ods2_fid_t ext, unsigned primary_number, unsigned segment)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;

    return (core->struclev >> 8) == 2 && ods2_validate_head(h).ok &&
           core->fid.fid_num == ext.fid_num && core->fid.fid_seq == ext.fid_seq &&
           core->seg_num == segment && core->backlink.fid_num == primary_number;
}

ods2_result_t ods2_decode_all_extents(ods2_volume_t *vol, const uint8_t *header,
                                       ods2_extent_t *extents_out, size_t max_extents,
                                       int *count_out)
{
    uint8_t current[512];
    int total = 0;
    int segment;
    unsigned primary_number = ((const ods2_head_core_t *) header)->fid.fid_num;

    memcpy(current, header, 512);

    for (segment = 0; segment < ODS2_MAX_HEADER_SEGMENTS; segment++) {
        const ods2_head_core_t *core = (const ods2_head_core_t *) current;
        int n = decode_header_extents(current,
                                       extents_out + total,
                                       (max_extents > (size_t) total)
                                           ? max_extents - (size_t) total : 0);
        if (n == ODS2_DECODE_TOO_MANY) {
            /* never go on with a shortened list: a file read short, or deleted with some of its clusters left
               allocated, without a word */
            return fail("the file has more extents than ods2v2 can hold (1,024): it is not read or changed");
        }
        if (n < 0) {
            return fail("could not decode retrieval pointers for a header segment");
        }
        total += n;

        if (core->ext_fid.fid_num == 0 && core->ext_fid.fid_nmx == 0) {
            /* No further extension header - done. */
            *count_out = total;
            return ok();
        }

        {
            ods2_result_t r = ods2_check_fid(core->ext_fid);
            if (!r.ok) return r;
            ods2_fid_t link = core->ext_fid;                 /* (core points into `current`, which is overwritten) */
            r = ods2_read_header(vol, link.fid_num, current);
            if (!r.ok) return r;
            if (!extension_header_ok(current, link, primary_number, (unsigned) segment + 1)) {
                return fail("damaged extension header chain (an extension header that is not this file's): not read or "
                            "changed");
            }
        }
    }

    return fail("extension header chain exceeded maximum segment limit "
                "(possibly corrupted or circular ext_fid chain)");
}

/* Walks every entry of a directory, one block at a time, handing each
   entry to `visit`. `visit` returns false to stop early (e.g. a name
   lookup that has found its match). Each block is parsed into a
   buffer sized for the most records a block can physically hold, so
   nothing is ever dropped here regardless of directory size - the
   caller decides what to keep. This is the one place that reads
   directory blocks; ods2_list_directory(), ods2_list_directory_alloc(),
   ods2_count_directory_entries() and ods2_lookup_name() are all thin
   visitors on top of it. */
typedef bool (*dir_visit_fn)(const ods2_dir_entry_t *entry, void *ctx);

/* Blocks of a directory that hold entries: those inside its
   end-of-file, at most its allocation. Covers both EOF forms in use:
   VMS's EFBLK n+1 / FFBYTE 0, and EFBLK n / FFBYTE 2, which ods2v2
   writes for a new one-block directory. */
static unsigned dir_blocks_in_use(const uint8_t *dir_header, unsigned allocated)
{
    size_t len = ods2_file_content_length(dir_header);
    size_t used = (len + 511) / 512;
    return (used > allocated) ? allocated : (unsigned) used;
}

static ods2_result_t walk_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                    dir_visit_fn visit, void *ctx)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    unsigned total_blocks = 0, block_index;
    int i;
    ods2_result_t r = ods2_decode_all_extents(vol, dir_header, extents, ODS2_MAX_EXTENTS, &extent_count);

    if (!r.ok) return r;
    if (extent_count <= 0) {
        return fail("could not decode directory's retrieval pointers");
    }
    for (i = 0; i < extent_count; i++) total_blocks += extents[i].block_count;
    /* Only blocks inside the end-of-file hold entries - VMS reads no
       further, so neither does this: DIR shows what VMS will find. */
    total_blocks = dir_blocks_in_use(dir_header, total_blocks);

    for (block_index = 1; block_index <= total_blocks; block_index++) {
        uint8_t block[512];
        ods2_dir_entry_t block_entries[ODS2_DIR_MAX_ENTRIES_PER_BLOCK];
        int n;
        r = ods2_read_file_block(vol, dir_header, block_index, block);
        if (!r.ok) return r;

        n = ods2_parse_directory(block, sizeof(block), block_entries,
                                  ODS2_DIR_MAX_ENTRIES_PER_BLOCK);
        if (n < 0) {
            /* Malformed block content is a real problem, but an
               entirely EMPTY block (all zero, size word 0x0000) just
               means "no more entries in this particular block" for a
               multi-block directory - not every block need be full. */
            continue;
        }
        if (n > ODS2_DIR_MAX_ENTRIES_PER_BLOCK) {
            /* Can't happen for anything ods2_parse_directory() accepts
               (see ODS2_DIR_MAX_ENTRIES_PER_BLOCK), but refuse loudly
               rather than silently drop entries if it ever does. */
            return fail("directory block holds more records than is physically possible");
        }
        for (i = 0; i < n; i++) {
            if (!visit(&block_entries[i], ctx)) return ok();
        }
    }
    return ok();
}

/* --- ods2_list_directory(): caller-supplied fixed buffer --- */
typedef struct {
    ods2_dir_entry_t *entries;
    size_t max;
    size_t total;
} fixed_list_ctx_t;

static bool visit_fixed_list(const ods2_dir_entry_t *entry, void *ctx_)
{
    fixed_list_ctx_t *ctx = (fixed_list_ctx_t *) ctx_;
    if (ctx->total < ctx->max) ctx->entries[ctx->total] = *entry;
    ctx->total++; /* keep counting past max so overflow is detectable */
    return true;
}

ods2_result_t ods2_list_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                   ods2_dir_entry_t *entries_out, size_t max_entries,
                                   int *count_out)
{
    fixed_list_ctx_t ctx;
    ods2_result_t r;

    ctx.entries = entries_out;
    ctx.max = max_entries;
    ctx.total = 0;
    r = walk_directory(vol, dir_header, visit_fixed_list, &ctx);
    if (!r.ok) return r;

    *count_out = (int) ctx.total;
    if (ctx.total > max_entries) {
        /* Never silently truncate: report it, with *count_out set to
           the directory's real size so the caller can resize and
           retry (or use ods2_list_directory_alloc()). */
        return fail("directory has more entries than the supplied buffer holds");
    }
    return ok();
}

/* --- ods2_list_directory_alloc(): grows to fit --- */
typedef struct {
    ods2_dir_entry_t *entries;
    size_t count;
    size_t capacity;
    bool out_of_memory;
} alloc_list_ctx_t;

static bool visit_alloc_list(const ods2_dir_entry_t *entry, void *ctx_)
{
    alloc_list_ctx_t *ctx = (alloc_list_ctx_t *) ctx_;
    if (ctx->count == ctx->capacity) {
        size_t new_cap = (ctx->capacity == 0) ? 64 : ctx->capacity * 2;
        ods2_dir_entry_t *grown = realloc(ctx->entries, new_cap * sizeof(*grown));
        if (grown == NULL) {
            ctx->out_of_memory = true;
            return false;
        }
        ctx->entries = grown;
        ctx->capacity = new_cap;
    }
    ctx->entries[ctx->count++] = *entry;
    return true;
}

ods2_result_t ods2_list_directory_alloc(ods2_volume_t *vol, const uint8_t *dir_header,
                                         ods2_dir_entry_t **entries_out, int *count_out)
{
    alloc_list_ctx_t ctx;
    ods2_result_t r;

    ctx.entries = NULL;
    ctx.count = 0;
    ctx.capacity = 0;
    ctx.out_of_memory = false;

    *entries_out = NULL;
    *count_out = 0;

    r = walk_directory(vol, dir_header, visit_alloc_list, &ctx);
    if (r.ok && ctx.out_of_memory) r = fail("out of memory listing directory");
    if (!r.ok) {
        free(ctx.entries);
        return r;
    }
    *entries_out = ctx.entries;
    *count_out = (int) ctx.count;
    return ok();
}

/* --- ods2_count_directory_entries() --- */
static bool visit_count(const ods2_dir_entry_t *entry, void *ctx_)
{
    (void) entry;
    (*(size_t *) ctx_)++;
    return true;
}

ods2_result_t ods2_count_directory_entries(ods2_volume_t *vol, const uint8_t *dir_header,
                                            int *count_out)
{
    size_t total = 0;
    ods2_result_t r = walk_directory(vol, dir_header, visit_count, &total);
    if (!r.ok) return r;
    *count_out = (int) total;
    return ok();
}

size_t ods2_file_content_length(const uint8_t *header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    uint32_t efblk = ods2_word_swap32(core->recattr.efblk);
    uint16_t ffbyte = core->recattr.ffbyte;

    /* Spec 6.1.5/6.1.6: EFBLK is the VBN holding the end-of-file
       position and FFBYTE is the count of bytes in use in that block,
       so the content is (EFBLK-1) full blocks plus FFBYTE bytes. An
       EOF on a block boundary is EFBLK=n+1/FFBYTE=0 (preferred, and
       what every real VMS header in samples/ uses - e.g. BADBLK.SYS,
       empty: EFBLK=1 FFBYTE=0; a one-block directory: EFBLK=2
       FFBYTE=0) or EFBLK=n/FFBYTE=512. An earlier version read
       FFBYTE=0 as "the whole EFBLK block is in use", which returned
       512 bytes of junk for empty files, one block too many for real
       VMS files ending on a block boundary, and failed outright on a
       file with no allocation at all such as BADBLK.SYS. */
    if (efblk == 0) {
        return 0; /* never written; no EOF mark at all */
    }
    if (ffbyte > 512) {
        ffbyte = 512; /* out of range - never claim more than a block */
    }
    return (size_t) (efblk - 1) * 512u + ffbyte;
}

size_t ods2_file_allocated_bytes(const uint8_t *header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    return (size_t) ods2_word_swap32(core->recattr.hiblk) * 512u;
}

ods2_result_t ods2_read_file_stream(ods2_volume_t *vol, const uint8_t *header,
                                     bool (*sink)(void *ctx, const uint8_t *data, size_t n),
                                     void *ctx, size_t *bytes_out, bool *sink_failed_out)
{
    size_t content_len = ods2_file_content_length(header);
    size_t allocated = ods2_file_allocated_bytes(header);
    size_t written = 0;
    ods2_result_t r = ok();

    if (bytes_out) *bytes_out = 0;
    if (sink_failed_out) *sink_failed_out = false;
    if (content_len > allocated) {
        /* Corrupt header: the EOF mark claims more data than the file
           has blocks. Say so, rather than fail later on an unmapped
           VBN (or have a caller size a buffer from a bogus length). */
        return fail("file's end-of-file mark lies beyond its allocated blocks");
    }

    /* Decoded once, then read an extent at a time in chunks of at
       most ODS2_STREAM_CHUNK - a large file is hundreds of thousands
       of blocks, too many to look up one by one, and too much to hold
       in memory at once (issue #16). */
    if (content_len > 0) {
        ods2_extent_t extents[ODS2_MAX_EXTENTS];
        size_t chunk_cap = content_len < ODS2_STREAM_CHUNK ? content_len : ODS2_STREAM_CHUNK;
        uint8_t *chunk;
        int n, i;

        r = ods2_decode_all_extents(vol, header, extents, ODS2_MAX_EXTENTS, &n);
        if (!r.ok) return r;
        chunk = malloc(chunk_cap);
        if (chunk == NULL) return fail("could not allocate a read buffer");
        for (i = 0; i < n && written < content_len && r.ok; i++) {
            size_t want = content_len - written;
            size_t run = (size_t) extents[i].block_count * 512u;
            if (want > run) want = run;
            if (fseek(vol->fp, (long) extents[i].lbn * 512, SEEK_SET) != 0) {
                r = fail("could not read file content block");
                break;
            }
            while (want > 0) {
                size_t piece = want < chunk_cap ? want : chunk_cap;
                if (fread(chunk, 1, piece, vol->fp) != piece) {
                    r = fail("could not read file content block");
                    break;
                }
                if (!sink(ctx, chunk, piece)) {
                    if (sink_failed_out) *sink_failed_out = true;
                    r = fail("could not deliver file content to its destination");
                    break;
                }
                written += piece;
                want -= piece;
            }
        }
        free(chunk);
        if (r.ok && written < content_len) {
            r = fail("requested VBN is not covered by this file's extents");
        }
    }
    if (bytes_out) *bytes_out = written;
    return r;
}

typedef struct {
    uint8_t *buf;
    size_t size, used;
} memory_sink_t;

static bool memory_sink(void *ctx_, const uint8_t *data, size_t n)
{
    memory_sink_t *ctx = (memory_sink_t *) ctx_;
    if (n > ctx->size - ctx->used) return false;
    memcpy(ctx->buf + ctx->used, data, n);
    ctx->used += n;
    return true;
}

ods2_result_t ods2_read_file(ods2_volume_t *vol, const uint8_t *header,
                              uint8_t *buf_out, size_t buf_size, size_t *bytes_read_out)
{
    memory_sink_t ctx;
    ods2_result_t r;

    if (ods2_file_content_length(header) > ods2_file_allocated_bytes(header)) {
        return fail("file's end-of-file mark lies beyond its allocated blocks");
    }
    if (ods2_file_content_length(header) > buf_size) {
        return fail("output buffer too small for file content");
    }
    ctx.buf = buf_out;
    ctx.size = buf_size;
    ctx.used = 0;
    r = ods2_read_file_stream(vol, header, memory_sink, &ctx, NULL, NULL);
    if (!r.ok) return r;
    *bytes_read_out = ctx.used;
    return ok();
}

/* Finds a free file number for a new file, starting the search at
   `start_from` (typically 17, just past the first-16 region that's
   reserved for direct home-block-formula lookup, though any value is
   valid). Spec 5.1.7: "A block containing a valid file header
   must never be used to create a new file, even if it is marked free
   in the index file bitmap. This prevents files from being lost if
   bits are dropped in the bitmap." Follow this literally: rather
   than trusting the index file bitmap as the sole source of truth
   (confirmed empirically to NOT reliably reflect real allocation on
   a freshly VMS-INITIALIZE'd volume - files 1-13 all show as "free"
   in the bitmap despite genuinely existing), we read each candidate
   header and treat fid_num==0 as the actual "unused" signal. This is
   slower than trusting the bitmap but is what the spec describes as
   the only safe approach. */
/* Marks `file_number`'s bit in the index file's own bitmap (the one
   at HM2$L_IBMAPLBN, tracking which file numbers/headers are
   allocated - completely distinct from BITMAP.SYS's data-block
   bitmap, which ods2_allocate_blocks() already maintains).
   Bit 0 corresponds to file number 1. Same set=free/clear=used
   convention as every other ODS-2 bitmap (Spec 5.2.2).

   This bitmap is directly, statically located from home block fields -
   not accessed as a regular file through INDEXF.SYS's own extents. */
static ods2_result_t mark_index_bitmap(ods2_volume_t *vol, unsigned file_number, bool used)
{
    unsigned bit_index = file_number - 1;
    unsigned bits_per_block = 512u * 8u;
    unsigned block_index = bit_index / bits_per_block;
    unsigned bit_in_block = bit_index % bits_per_block;
    unsigned byte_in_block = bit_in_block / 8;
    unsigned bit_in_byte = bit_in_block % 8;
    uint32_t lbn;
    uint8_t block[512];

    if (block_index >= vol->home.ibmapsize) {
        return fail("file number is beyond the index bitmap's own extent");
    }
    lbn = vol->home.ibmaplbn + block_index;

    if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
        fread(block, 1, 512, vol->fp) != 512) {
        return fail("could not read index bitmap block");
    }

    /* Deliberately NOT using ods2_bitmap_mark() here - that helper
       implements the STORAGE bitmap's convention (set=free), and the
       index file bitmap uses the opposite (Spec 5.1.6, explicit:
       "if the bit is 1, then that file number is in use"). An
       earlier version of this code called ods2_bitmap_mark() anyway,
       which cleared the bit when marking a file number used -
       exactly backwards, silently telling ANALYZE/DISK this file
       number was NOT in use while a directory entry claimed it
       existed. */
    if (used) {
        block[byte_in_block] |= (uint8_t) (1u << bit_in_byte);
    } else {
        block[byte_in_block] &= (uint8_t) ~(1u << bit_in_byte);
    }

    return ods2_write_block(vol, lbn, block);
}

/* --- File number allocation (spec 5.1.6, 5.1.7) ---

   File numbers come from the index file bitmap. The header block a
   free bit points at is then checked:

   - Inside INDEXF.SYS's end-of-file, it is validated: a valid header
     is never reused even if its bit says free (dropped bitmap bits
     must not lose files); a deleted header gives the new file its
     sequence number + 1; anything else is garbage and gives 1.
   - Beyond the end-of-file, the block is garbage by definition: 1.

   And INDEXF.SYS's end-of-file must stay at or beyond the last header
   ever used. VMS treats a file whose header lies beyond it as not
   existing at all (SYSTEM-W-NOSUCHFILE, ANALDISK-W-LOSTHEADER), so a
   new header past it has to move it - which a freshly INITIALIZEd
   volume always needs, since INITIALIZE puts the end-of-file just
   after the reserved files. */

/* The 16-bit FID number field is all this code fills in (FID_NMX
   stays 0), so file numbers stop at 65535. */
#define MAX_FILE_NUMBER 65535u

static uint32_t header_vbn(const ods2_volume_t *vol, unsigned file_number)
{
    return (uint32_t) vol->home.cluster * 4 + vol->home.ibmapsize + file_number;
}

typedef enum { SLOT_GARBAGE, SLOT_DELETED, SLOT_VALID } header_slot_t;

/* Classifies a header block found inside the end-of-file (spec 3.5.1,
   5.1.7). A checksum alone can't tell a header from an all-zero block
   (whose checksum is also zero), so a valid header must also have the
   header structure and a nonzero file number. */
static header_slot_t classify_header_slot(const uint8_t *h)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;
    uint16_t stored = (uint16_t) (h[510] | (h[511] << 8));
    bool structured = (core->struclev >> 8) == 2 &&
                      core->idoffset >= 40 && core->idoffset <= core->mpoffset &&
                      core->mpoffset <= core->acoffset && core->acoffset <= core->rsoffset;

    if (!structured) return SLOT_GARBAGE;
    if ((core->fid.fid_num != 0 || core->fid.fid_nmx != 0) &&
        stored == ods2_checksum(h, 255)) {
        return SLOT_VALID;
    }
    if ((core->filechar & 0x8000u) && core->fid.fid_num == 0 && core->fid.fid_nmx == 0) {
        return SLOT_DELETED; /* FH2$M_MARKDEL, FID number zeroed */
    }
    return SLOT_GARBAGE;
}

/* Reads/writes one index file bitmap block (located directly from the
   home block, like mark_index_bitmap()). */
static ods2_result_t read_index_bitmap_block(ods2_volume_t *vol, unsigned block_index,
                                             uint8_t *block)
{
    uint32_t lbn = vol->home.ibmaplbn + block_index;
    if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
        fread(block, 1, 512, vol->fp) != 512) {
        return fail("could not read index bitmap block");
    }
    return ok();
}

/* Highest file number marked in use in the index file bitmap, or 0. */
static ods2_result_t highest_used_file_number(ods2_volume_t *vol, unsigned *highest_out)
{
    unsigned b, i;
    uint8_t block[512];

    *highest_out = 0;
    for (b = 0; b < vol->home.ibmapsize; b++) {
        ods2_result_t r = read_index_bitmap_block(vol, b, block);
        if (!r.ok) return r;
        for (i = 0; i < 512; i++) {
            if (block[i] != 0) {
                int bit;
                for (bit = 7; bit >= 0; bit--) {
                    if (block[i] & (1u << bit)) {
                        *highest_out = b * 4096u + i * 8u + (unsigned) bit + 1u;
                        break;
                    }
                }
            }
        }
    }
    return ok();
}

ods2_result_t ods2_find_free_file_number(ods2_volume_t *vol, unsigned start_from,
                                          unsigned max_search, unsigned *file_number_out,
                                          uint16_t *seq_out)
{
    uint8_t indexf_header[512];
    uint8_t block[512];
    unsigned limit, n, loaded_block = (unsigned) -1;
    uint32_t efblk;
    ods2_result_t r;

    if (start_from == 0) start_from = 1;
    limit = vol->home.maxfiles;
    if (limit > MAX_FILE_NUMBER) limit = MAX_FILE_NUMBER;
    if ((unsigned long) vol->home.ibmapsize * 4096u < limit) {
        limit = (unsigned) vol->home.ibmapsize * 4096u;
    }
    if (max_search < limit && start_from - 1 + max_search < limit) {
        limit = start_from - 1 + max_search;
    }

    r = ods2_read_header(vol, 1, indexf_header);
    if (!r.ok) return r;
    efblk = ods2_word_swap32(((ods2_head_core_t *) indexf_header)->recattr.efblk);

    for (n = start_from; n <= limit; n++) {
        unsigned bit = n - 1;
        unsigned block_index = bit / 4096u;
        uint8_t header[512];

        if (block_index != loaded_block) {
            r = read_index_bitmap_block(vol, block_index, block);
            if (!r.ok) return r;
            loaded_block = block_index;
        }
        if (block[(bit % 4096u) / 8u] & (1u << (bit % 8u))) {
            continue; /* in use */
        }

        if (header_vbn(vol, n) >= efblk) {
            *file_number_out = n; /* beyond the end-of-file: garbage */
            *seq_out = 1;
            return ok();
        }
        r = ods2_read_header(vol, n, header);
        if (!r.ok) return r; /* inside the end-of-file yet unmapped: corrupt */
        switch (classify_header_slot(header)) {
        case SLOT_VALID:
            continue; /* free in the bitmap, but a live header: never reuse */
        case SLOT_DELETED: {
            uint16_t seq = (uint16_t) (((const ods2_head_core_t *) header)->fid.fid_seq + 1);
            *file_number_out = n;
            *seq_out = (seq == 0) ? 1 : seq; /* 0 is not a valid sequence number */
            return ok();
        }
        case SLOT_GARBAGE:
            *file_number_out = n;
            *seq_out = 1;
            return ok();
        }
    }
    return fail("no free file number: the index file bitmap is full (or every "
                "free number's header slot holds a valid header)");
}

/* Writes INDEXF.SYS's own header (file 1) with a fresh checksum, and
   the same bytes to the backup index file header (spec 5.1.5), which
   exists so the volume can be recovered if the primary goes bad - a
   stale copy would describe the wrong extents. Then refreshes the
   cached INDEXF.SYS extents used to locate headers. */
static ods2_result_t write_indexf_header(ods2_volume_t *vol, uint8_t *indexf_header)
{
    uint16_t checksum = ods2_checksum(indexf_header, 255);
    ods2_result_t r;
    int count;

    indexf_header[510] = (uint8_t) (checksum & 0xff);
    indexf_header[511] = (uint8_t) (checksum >> 8);
    r = ods2_write_header(vol, 1, indexf_header);
    if (!r.ok) return r;
    if (vol->home.altidxlbn != 0) {
        r = ods2_write_block(vol, vol->home.altidxlbn, indexf_header);
        if (!r.ok) return r;
    }
    count = decode_header_extents(indexf_header, vol->indexf_extents, ODS2_MAX_EXTENTS);
    if (count <= 0) {
        return fail("INDEXF.SYS's header no longer decodes after updating it");
    }
    vol->indexf_extent_count = count;
    return ok();
}

ods2_result_t ods2_indexf_map_extent(uint8_t *indexf_header, uint32_t lbn, unsigned block_count)
{
    ods2_head_core_t *core = (ods2_head_core_t *) indexf_header;
    size_t map_offset = (size_t) core->mpoffset * 2;
    size_t map_bytes = (size_t) core->map_inuse * 2;
    size_t map_end = (size_t) core->acoffset * 2;
    size_t size = ods2_retrieval_pointer_size(lbn, block_count);
    uint32_t hiblk;

    if (size == 0) {
        return fail("internal error: INDEXF.SYS extent cannot be described by "
                    "any retrieval pointer format");
    }
    if (map_end > 510 || map_offset + map_bytes + size > map_end) {
        return fail("INDEXF.SYS's header has no room for another extent "
                    "(would need an extension header - not yet implemented)");
    }
    if (ods2_encode_retrieval_pointer(indexf_header + map_offset + map_bytes,
                                      lbn, block_count) != size) {
        return fail("internal error: could not encode an INDEXF.SYS retrieval pointer");
    }
    core->map_inuse = (uint8_t) ((map_bytes + size) / 2);
    hiblk = ods2_word_swap32(core->recattr.hiblk) + block_count;
    core->recattr.hiblk = ods2_word_swap32(hiblk);
    /* INITIALIZE leaves FH2$L_HIGHWATER at HIBLK + 1 on INDEXF.SYS;
       keep that relationship. */
    if (core->highwater != 0) core->highwater = hiblk + 1;
    return ok();
}

/* Allocates more blocks to INDEXF.SYS and maps them, so more headers
   fit. Up to 256 blocks at a time; falls back to a single cluster if
   no run that long is free. The new piece is mapped with the smallest
   retrieval pointer that describes it - Format 1 below LBN 4,194,303,
   Format 2 beyond (issue #14) - so INDEXF.SYS can grow anywhere on a
   volume of any size. */
static ods2_result_t extend_indexf(ods2_volume_t *vol, uint8_t *indexf_header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) indexf_header;
    size_t map_offset = (size_t) core->mpoffset * 2;
    size_t map_bytes = (size_t) core->map_inuse * 2;
    size_t map_end = (size_t) core->acoffset * 2;
    unsigned want = (256u / vol->home.cluster) * vol->home.cluster;
    uint32_t lbn;
    unsigned got;
    ods2_result_t r;

    if (want == 0) want = vol->home.cluster;
    /* Check first, before taking any blocks, that the smallest pointer
       (4 bytes) could still fit; the exact size is checked after
       allocation, once the LBN is known. */
    if (map_end > 510 || map_offset + map_bytes + 4 > map_end) {
        return fail("INDEXF.SYS's header has no room for another extent "
                    "(would need an extension header - not yet implemented)");
    }
    r = ods2_allocate_blocks(vol, want, &lbn, &got);
    if (!r.ok && want > vol->home.cluster) {
        want = vol->home.cluster;
        r = ods2_allocate_blocks(vol, want, &lbn, &got);
    }
    if (!r.ok) return r;
    r = ods2_indexf_map_extent(indexf_header, lbn, got);
    if (!r.ok) {
        /* ods2_allocate_blocks() has already marked them used; give
           them back rather than leak them. */
        ods2_free_blocks(vol, lbn, got);
        return r;
    }
    return ok();
}

/* Allocates a file number for a new file: chooses it (above), makes
   sure INDEXF.SYS is big enough to hold its header and that its
   end-of-file covers it, then marks it in use in the bitmap. The
   end-of-file moves to just past whichever is higher: the new header,
   or the highest header the bitmap says is in use - the second puts
   right volumes where earlier ods2v2 versions wrote headers beyond
   the end-of-file, leaving those files invisible to VMS. INDEXF.SYS's
   header is written before the bitmap, so an interruption leaves at
   worst a garbage block inside the end-of-file, which the spec allows. */
static ods2_result_t allocate_file_number(ods2_volume_t *vol, unsigned *file_number_out,
                                          uint16_t *seq_out)
{
    uint8_t indexf_header[512];
    ods2_head_core_t *core = (ods2_head_core_t *) indexf_header;
    unsigned n, highest_used, extensions = 0;
    uint16_t seq;
    uint32_t vbn, efblk, new_efblk;
    bool changed = false;
    ods2_result_t r;

    r = ods2_find_free_file_number(vol, 1, MAX_FILE_NUMBER, &n, &seq);
    if (!r.ok) return r;
    r = ods2_read_header(vol, 1, indexf_header);
    if (!r.ok) return r;
    /* INDEXF.SYS's own header is rewritten with a fresh checksum (and its backup with it): it must be sound first, or
       the one header the volume hangs on would be made to look right */
    r = header_ok_for_write(indexf_header, 1);
    if (!r.ok) return r;

    vbn = header_vbn(vol, n);
    while (vbn > ods2_word_swap32(core->recattr.hiblk)) {
        if (++extensions > 64) {
            return fail("INDEXF.SYS could not be extended far enough to hold "
                        "the new header");
        }
        r = extend_indexf(vol, indexf_header);
        if (!r.ok) return r;
        changed = true;
    }

    r = highest_used_file_number(vol, &highest_used);
    if (!r.ok) return r;
    new_efblk = vbn + 1;
    if (highest_used > n) {
        uint32_t used_vbn = header_vbn(vol, highest_used);
        if (used_vbn <= ods2_word_swap32(core->recattr.hiblk) && used_vbn + 1 > new_efblk) {
            new_efblk = used_vbn + 1;
        }
    }
    efblk = ods2_word_swap32(core->recattr.efblk);
    if (new_efblk > efblk) {
        core->recattr.efblk = ods2_word_swap32(new_efblk);
        core->recattr.ffbyte = 0; /* EFBLK n+1 / FFBYTE 0: as VMS writes it */
        changed = true;
    }
    if (changed) {
        r = write_indexf_header(vol, indexf_header);
        if (!r.ok) return r;
    }

    r = mark_index_bitmap(vol, n, true);
    if (!r.ok) return r;
    *file_number_out = n;
    *seq_out = seq;
    return ok();
}

/* --- ods2_lookup_name(): streams the directory, no size limit --- */
typedef struct {
    const char *name;
    ods2_fid_t fid;
    bool found;
} lookup_ctx_t;

static bool visit_lookup(const ods2_dir_entry_t *entry, void *ctx_)
{
    lookup_ctx_t *ctx = (lookup_ctx_t *) ctx_;
    /* Case-insensitive compare - VMS names are conventionally
       uppercase on disk, but callers may pass either case. */
    if (ods2_dir_name_compare(entry->name, strlen(entry->name),
                              ctx->name, strlen(ctx->name)) == 0) {
        ctx->fid = entry->fid;
        ctx->found = true;
        return false; /* stop walking */
    }
    return true;
}

ods2_result_t ods2_lookup_name(ods2_volume_t *vol, const uint8_t *dir_header,
                                const char *name, ods2_fid_t *fid_out)
{
    /* Walks the directory block by block rather than listing it into
       a fixed buffer first - an earlier version used a 256-entry
       buffer here, so any name sorting after the 256th entry of its
       directory was silently reported as "not found". */
    lookup_ctx_t ctx;
    ods2_result_t r;

    ctx.name = name;
    ctx.found = false;
    r = walk_directory(vol, dir_header, visit_lookup, &ctx);
    if (!r.ok) return r;
    if (!ctx.found) return fail("name not found in directory");
    *fid_out = ctx.fid;
    return ok();
}

/* --- ods2_lookup_name_version(): one specific version of a name --- */

/* Case-insensitive equality of two nul-terminated names. */
/* Same name as a directory compares them: case-insensitive, and
   "MAKEFILE" equals "MAKEFILE." (see ods2_dir_name_compare()). */
static bool names_equal(const char *a, const char *b)
{
    return ods2_dir_name_compare(a, strlen(a), b, strlen(b)) == 0;
}

typedef struct {
    const char *name;
    int version;         /* selector, see ODS2_VERSION_HIGHEST */
    int seen;            /* versions of `name` seen so far */
    bool name_found;
    bool found;
    ods2_fid_t fid;
    uint16_t found_version;
} version_lookup_ctx_t;

static bool visit_lookup_version(const ods2_dir_entry_t *entry, void *ctx_)
{
    version_lookup_ctx_t *ctx = (version_lookup_ctx_t *) ctx_;

    if (!names_equal(entry->name, ctx->name)) {
        /* Directories are sorted by name, so every version of a name
           is contiguous (even when split across several records and
           blocks). Once past them there is nothing more to find. */
        return !ctx->name_found;
    }
    ctx->name_found = true;

    if (ctx->version > 0) {
        if (entry->version == (unsigned) ctx->version) {
            ctx->found = true;
        }
    } else {
        /* Highest (0) or relative (-n): versions are stored newest
           first, so the n-th one encountered (0-based) is the one
           wanted - counting existing versions, as VMS does. */
        if (ctx->seen == -ctx->version) {
            ctx->found = true;
        }
    }
    ctx->seen++;

    if (ctx->found) {
        ctx->fid = entry->fid;
        ctx->found_version = entry->version;
        return false; /* stop walking */
    }
    return true;
}

ods2_result_t ods2_lookup_name_version(ods2_volume_t *vol, const uint8_t *dir_header,
                                        const char *name, int version,
                                        ods2_fid_t *fid_out, uint16_t *version_out)
{
    version_lookup_ctx_t ctx;
    ods2_result_t r;

    if (version > ODS2_MAX_VERSION || version < -ODS2_MAX_VERSION) {
        return fail("version number out of range (1..32767)");
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.name = name;
    ctx.version = version;
    r = walk_directory(vol, dir_header, visit_lookup_version, &ctx);
    if (!r.ok) return r;
    if (!ctx.name_found) return fail("name not found in directory");
    if (!ctx.found) return fail("version not found");
    *fid_out = ctx.fid;
    if (version_out != NULL) *version_out = ctx.found_version;
    return ok();
}

ods2_result_t ods2_lookup_path(ods2_volume_t *vol, const char *path, ods2_fid_t *fid_out)
{
    ods2_fid_t current;
    char component[256];
    const char *p = path;

    /* Root directory's FID is always (4,4) - not a magic constant,
       this is INDEXF.SYS's own well-known convention (file 4 is
       000000.DIR;1 on every ODS-2 volume), confirmed against real
       data throughout this project. */
    current.fid_num = 4;
    current.fid_seq = 4;
    current.fid_rvn = 0;
    current.fid_nmx = 0;

    if (*path == '\0') {
        *fid_out = current;
        return ok();
    }

    while (*p) {
        uint8_t header[512];
        ods2_result_t r;
        size_t len = 0;

        while (*p && *p != '.' && len < sizeof(component) - 5) {
            component[len++] = *p++;
        }
        component[len] = '\0';
        if (*p == '.') p++;

        r = ods2_read_header_fid(vol, current, header);
        if (!r.ok) return r;

        strcat(component, ".DIR");
        r = ods2_lookup_name(vol, header, component, &current);
        if (!r.ok) return r;
        {
            /* the entry's FID must name a header that is a directory: a file called X.DIR is not one (a writer
               would rewrite its data as directory blocks), and an entry with another RVN / NMX / sequence number
               names no file here */
            uint8_t next_header[512];
            r = ods2_read_header_fid(vol, current, next_header);
            if (!r.ok) return r;
            if (!(((const ods2_head_core_t *) next_header)->filechar & 0x2000u)) {
                return fail("a directory name in the path is a file that is not a directory");
            }
        }
    }

    *fid_out = current;
    return ok();
}

ods2_result_t ods2_create_directory(ods2_volume_t *vol, const uint8_t *parent_header,
                                     const char *name, ods2_fid_t *new_fid_out)
{
    const ods2_head_core_t *parent_core = (const ods2_head_core_t *) parent_header;
    char dirname[256];
    ods2_fid_t existing;
    unsigned file_number;
    uint16_t seq_num;
    uint32_t content_lbn;
    unsigned content_blocks;
    ods2_extent_t extent;
    ods2_header_spec_t spec;
    uint8_t new_header[512];
    uint8_t content_block[512];
    bool header_written = false;
    ods2_result_t r;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    if (contains_wildcard(name)) {
        return fail("directory name cannot contain wildcard characters ('*' or '%')");
    }
    if (strchr(name, ';') != NULL) {
        return fail("directory name cannot contain ';' (a version belongs in the "
                    "directory entry, never in the name itself)");
    }
    {
        const char *why;
        if (!ods2_valid_dir_name(name, &why)) return fail(why);
    }
    strcpy(dirname, name); /* at most 39 characters, checked above */
    uppercase_str(dirname); /* real VMS always stores names uppercase */
    strcat(dirname, ".DIR");

    /* Refuse to create a name that already exists matching VMS
       behavior (CREATE/DIRECTORY on an existing name fails) and
       avoids silently shadowing an existing directory. */
    r = directory_ready(vol, parent_header, parent_core->fid.fid_num);
    if (!r.ok) return r;
    r = ods2_lookup_name(vol, parent_header, dirname, &existing);
    if (r.ok) {
        return fail("a file or directory with this name already exists in the parent");
    }
    if (strcmp(r.problem, "name not found in directory") != 0) return r;   /* any other failure is not "no such name" */

    /* A file number, its header slot inside INDEXF.SYS's end-of-file,
       and its bit marked in use. */
    r = allocate_file_number(vol, &file_number, &seq_num);
    if (!r.ok) return r;

    /* WARNING Allocate one cluster's worth of content space - small and
       simple for this first implementation; a real, populated directory
       would need extension support (allocating and linking a second
       block/extent) once it outgrows this, which is not yet
       implemented. */
    r = ods2_allocate_blocks(vol, vol->home.cluster, &content_lbn, &content_blocks);
    if (!r.ok) {
        mark_index_bitmap(vol, file_number, false);      /* the number taken first is given back */
        return r;
    }

    extent.lbn = content_lbn;
    extent.block_count = content_blocks;

    memset(&spec, 0, sizeof(spec));
    spec.fid.fid_num = (uint16_t) file_number;
    spec.fid.fid_seq = seq_num;
    spec.backlink = parent_core->fid;
    /* FH2$M_DIRECTORY | FH2$M_CONTIG - confirmed real values from the
       project fixes, independently re-confirmed by reading real DECUS.DIR/root
       headers earlier in this project. */
    spec.filechar = 0x2080;
    spec.rtype = 2;   /* FAB$C_VAR - confirmed real value for directories */
    spec.rattrib = 8; /* NOSPAN - confirmed real value for directories */
    spec.rsize = 512;
    spec.maxrec = 512;
    spec.extents = &extent;
    spec.extent_count = 1;
    spec.hiblk = content_blocks;
    spec.efblk = 2;   /* one block in use: EFBLK 2, FFBYTE 0, as VMS writes */
    spec.ffbyte = 0;  /* (a directory is whole blocks; OpenVMS VAX copies FFBYTE
                         into the directories it creates inside this one) */
    /* a directory made in one with a default version limit passes it on (AXP1SYS: 114 of 114 directories under a
       default of 3 carry 3); a directory in one without gets none */
    spec.versions = parent_core->recattr.versions;

    {
        char ident[260]; /* dirname is already bounded (<256-4 chars,
                             checked above) plus ";1" and a nul - this
                             is always large enough, sized generously
                             to also silence a spurious compiler
                             truncation warning */
        snprintf(ident, sizeof(ident), "%s;1", dirname);
        spec.ident_name = ident;

        if (!ods2_build_file_header(new_header, &spec)) {
            r = fail("could not construct new directory's header");
            goto undo;
        }
    }

    /* Initialize the new directory's first content block: entirely
       empty except for the terminating sentinel. */
    memset(content_block, 0, sizeof(content_block));
    content_block[0] = 0xff;
    content_block[1] = 0xff;

    /* The first block, with its end marker, before the header (and the header before the entry): whatever an
       interruption leaves is a file number and a cluster that nothing refers to, never a valid directory header over a
       block that is not a directory block (52 such states in 14,605 interrupted runs of the old order). */
    r = ods2_write_block(vol, content_lbn, content_block);
    if (!r.ok) goto undo;
    r = ods2_write_header(vol, file_number, new_header);
    if (!r.ok) goto undo;
    header_written = true;

    /* Insert an entry for the new directory into the parent -
       growing the parent's own allocation automatically if none of
       its existing content blocks have room. */
    r = ods2_insert_into_directory(vol, parent_core->fid.fid_num, dirname, 1, spec.fid);
    if (!r.ok) goto undo;

    *new_fid_out = spec.fid;
    return ok();

undo:
    /* Give back what was taken: the header is marked deleted (a slot holding a valid header is never reused), then the
       cluster and the file number. A refused or failed CREATE/DIRECTORY leaves no directory nobody lists. */
    if (header_written) {
        ods2_head_core_t *core = (ods2_head_core_t *) new_header;
        core->filechar |= 0x8000u;
        core->fid.fid_num = 0;
        core->fid.fid_nmx = 0;
        core->fid.fid_rvn = 0;
        new_header[510] = 0;
        new_header[511] = 0;
        ods2_write_header(vol, file_number, new_header);
    }
    ods2_free_blocks(vol, content_lbn, content_blocks);
    mark_index_bitmap(vol, file_number, false);
    return r;
}

/* Writes src->length bytes, read from `src` a chunk at a time, into
   the blocks described by `extents`, in order, zero-filling the rest
   of the last block and any further allocated blocks. Seeks once per
   extent and writes it a chunk at a time, so memory use stays at one
   chunk however large the file (issue #16). */
static ods2_result_t write_extents(ods2_volume_t *vol, const ods2_extent_t *extents,
                                   int extent_count, ods2_content_source_t *src)
{
    static const uint8_t zeros[65536];
    size_t offset = 0;
    size_t content_len = src->length;
    size_t chunk_cap = content_len < ODS2_STREAM_CHUNK ? content_len : ODS2_STREAM_CHUNK;
    uint8_t *chunk = NULL;
    ods2_result_t r = ok();
    int i;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    if (chunk_cap > 0) {
        chunk = malloc(chunk_cap);
        if (chunk == NULL) return fail("could not allocate a write buffer");
    }
    for (i = 0; i < extent_count && r.ok; i++) {
        size_t run_bytes = (size_t) extents[i].block_count * 512u;
        size_t data = (offset < content_len) ? content_len - offset : 0;

        if (data > run_bytes) data = run_bytes;
        if (fseek(vol->fp, (long) extents[i].lbn * 512, SEEK_SET) != 0) {
            r = fail("could not seek to file content blocks");
            break;
        }
        run_bytes -= data;
        while (data > 0) {
            size_t piece = data < chunk_cap ? data : chunk_cap;
            if (!src->read(src->ctx, chunk, piece)) {
                r = fail(src->problem != NULL ? src->problem
                                              : "could not read the file's content");
                break;
            }
            if (fwrite(chunk, 1, piece, vol->fp) != piece) {
                r = fail("could not write file content blocks");
                break;
            }
            data -= piece;
            offset += piece;
        }
        while (r.ok && run_bytes > 0) {
            size_t n = (run_bytes < sizeof(zeros)) ? run_bytes : sizeof(zeros);
            if (fwrite(zeros, 1, n, vol->fp) != n) {
                r = fail("could not write file content blocks");
                break;
            }
            run_bytes -= n;
        }
    }
    free(chunk);
    if (r.ok && fflush(vol->fp) != 0) {
        r = fail("could not flush written file content");
    }
    return r;
}

/* An ods2_content_source_t over a buffer already in memory. */
typedef struct {
    const uint8_t *data;
    size_t pos;
} memory_source_t;

static bool memory_source_read(void *ctx_, uint8_t *buf, size_t n)
{
    memory_source_t *ctx = (memory_source_t *) ctx_;
    memcpy(buf, ctx->data + ctx->pos, n);
    ctx->pos += n;
    return true;
}

static ods2_result_t create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                 const char *name, ods2_content_source_t *src,
                                 const ods2_record_attrs_t *attrs, ods2_fid_t *new_fid_out);

const char *ods2_check_record_attrs(const ods2_record_attrs_t *attrs, size_t content_len)
{
    unsigned cc = attrs->rattrib & (ODS2_RAT_FTN | ODS2_RAT_CR | ODS2_RAT_PRN);

    switch (attrs->rtype) {
    case ODS2_RFM_FIX:
        if (attrs->rsize == 0) {
            return "a fixed-length record format needs a record size";
        }
        if (attrs->rsize > 32766) {
            return "fixed record size is too large (at most 32766 bytes)";
        }
        if (attrs->rsize % 2 != 0) {
            return "odd fixed record sizes are not supported (RMS pads each such "
                   "record to an even length on disk)";
        }
        if (attrs->maxrec != attrs->rsize) {
            return "for fixed-length records the maximum record size (MRS) must "
                   "equal the record size";
        }
        if (content_len % attrs->rsize != 0) {
            return "content length is not a whole number of fixed-length records";
        }
        break;
    case ODS2_RFM_UDF:
    case ODS2_RFM_STM:
    case ODS2_RFM_STMLF:
    case ODS2_RFM_STMCR:
        if (attrs->rsize > 32767 || attrs->maxrec > 32767) {
            return "record size is too large (at most 32767 bytes)";
        }
        break;
    case ODS2_RFM_VAR:
    case ODS2_RFM_VFC:
        return "variable-length record formats (VAR, VFC) can't be imported: on "
               "disk each record starts with a byte count the host file doesn't have";
    default:
        return "unknown record format";
    }
    if (attrs->rattrib & ~0x0fu) {
        return "unknown record attribute bits";
    }
    if (cc != 0 && (cc & (cc - 1)) != 0) {
        return "only one carriage control (FTN, CR or PRN) can be given";
    }
    if (attrs->rattrib & ODS2_RAT_PRN) {
        /* print-file carriage control reads the record's VFC header bytes: with any other format VMS TYPE refuses the
           file (%RMS-F-RAT, measured on VAX V7.3, Alpha V7.3-2 and V8.4-2L2; plain V8.4 prints such files), and the formats that have a VFC header can't be
           imported here: such a file is never made */
        return "RAT:PRN needs the VFC record format, which can't be imported (VMS V7.3, V7.3-2 and V8.4-2L2 refuse TYPE of "
               "any other: RMS-F-RAT; plain V8.4 prints it - refused here to be safe)";
    }
    return NULL;
}

const char *ods2_check_var_record_attrs(const ods2_record_attrs_t *attrs, size_t content_len)
{
    unsigned cc = attrs->rattrib & (ODS2_RAT_FTN | ODS2_RAT_CR | ODS2_RAT_PRN);

    if (attrs->rtype != ODS2_RFM_VAR) {
        return "converted records are variable-length: the format must be VAR";
    }
    if (attrs->rsize > 32767) {
        return "a record is too long (at most 32767 bytes)";
    }
    if (attrs->maxrec != 0 && attrs->maxrec < attrs->rsize) {
        return "a record is longer than the maximum record size (MRS) given";
    }
    if (content_len % 2 != 0) {
        return "internal error: variable-length records must fill whole words";
    }
    if (attrs->rattrib & ODS2_RAT_BLK) {
        return "RAT:BLK isn't supported with converted records: they run on "
               "across block boundaries, which BLK forbids";
    }
    if (attrs->rattrib & ~0x0fu) {
        return "unknown record attribute bits";
    }
    if (cc != 0 && (cc & (cc - 1)) != 0) {
        return "only one carriage control (FTN, CR or PRN) can be given";
    }
    if (attrs->rattrib & ODS2_RAT_PRN) {
        /* print-file carriage control reads the record's VFC header bytes: with any other format VMS TYPE refuses the
           file (%RMS-F-RAT, measured on VAX V7.3, Alpha V7.3-2 and V8.4-2L2; plain V8.4 prints such files), and the formats that have a VFC header can't be
           imported here: such a file is never made */
        return "RAT:PRN needs the VFC record format, which can't be imported (VMS V7.3, V7.3-2 and V8.4-2L2 refuse TYPE of "
               "any other: RMS-F-RAT; plain V8.4 prints it - refused here to be safe)";
    }
    return NULL;
}

bool ods2_stream_format(uint8_t rtype)
{
    return rtype == ODS2_RFM_STM || rtype == ODS2_RFM_STMLF || rtype == ODS2_RFM_STMCR;
}

/* The length of the records of stream content, as RMS reads them (measured on OpenVMS V8.4, V7.3-2 and VAX V7.3):
     Stream_LF (STMLF)  a record ends at LF; the LF is not part of it; a CR before the LF stays in the record.
     Stream_CR (STMCR)  a record ends at CR; the CR is not part of it.
     Stream (STM)       a record ends at LF, FF or VT, and that byte STAYS in the record ("abc" LF is a record of 4 bytes);
                        it also ends at CR LF, which is stripped ("abc" CR LF is 3 bytes); a lone CR is data.
   The last record may have no terminator. A file whose longest record is longer than its LRL (or MRS, when not 0) cannot be
   read in record mode (%RMS-F-RSZ, %RMS-W-RTB). One state machine, fed any number of pieces in order. */
void ods2_reclen_init(ods2_reclen_t *s, uint8_t rtype)
{
    s->rtype = rtype;
    s->cur = s->longest = 0;
    s->prev_cr = false;
}

static void reclen_end(ods2_reclen_t *s, size_t len)
{
    if (len > s->longest) s->longest = len;
    s->cur = 0;
    s->prev_cr = false;
}

void ods2_reclen_feed(ods2_reclen_t *s, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (s->rtype == ODS2_RFM_STMCR) {
            if (c == '\r') reclen_end(s, s->cur); else s->cur++;
        } else if (s->rtype == ODS2_RFM_STM) {
            if (c == '\n') {
                /* CR LF: the CR (counted as data so far) is part of the terminator; a lone LF stays in the record */
                reclen_end(s, s->prev_cr ? s->cur - 1 : s->cur + 1);
            } else if (c == '\f' || c == '\v') {
                reclen_end(s, s->cur + 1);
            } else {
                s->cur++;
                s->prev_cr = (c == '\r');
            }
        } else {
            if (c == '\n') reclen_end(s, s->cur); else s->cur++;
        }
    }
}

size_t ods2_reclen_longest(const ods2_reclen_t *s)
{
    return s->cur > s->longest ? s->cur : s->longest;
}

size_t ods2_longest_record(uint8_t rtype, const uint8_t *data, size_t len)
{
    ods2_reclen_t st;

    ods2_reclen_init(&st, rtype);
    ods2_reclen_feed(&st, data, len);
    return ods2_reclen_longest(&st);
}

ods2_result_t ods2_create_file_attrs(ods2_volume_t *vol, const uint8_t *parent_header,
                                      const char *name, const uint8_t *content,
                                      size_t content_len, const ods2_record_attrs_t *attrs,
                                      ods2_fid_t *new_fid_out)
{
    memory_source_t ctx;
    ods2_content_source_t src;
    if (ods2_stream_format(attrs->rtype)) {
        /* RMS cannot read a stream file in record mode with a record longer than its MRS or its LRL (%RMS-W-RTB) */
        size_t longest = ods2_longest_record(attrs->rtype, content, content_len);
        if (attrs->maxrec != 0 && longest > attrs->maxrec) {
            return fail("a record is longer than the maximum record size (MRS) given: VMS could not read the file back "
                        "(RMS-W-RTB); use MRS 0");
        }
        if (attrs->rsize != 0 && attrs->rsize != 32767 && longest > attrs->rsize) {
            return fail("a record is longer than the longest record length (LRL) given: VMS could not read the file back "
                        "(RMS-W-RTB)");
        }
        if (attrs->rsize == 0) {                  /* no LRL given: the file's own longest record, as VMS stores it */
            ods2_record_attrs_t own = *attrs;
            own.rsize = (uint16_t) (longest > 32767 ? 32767 : longest);
            return ods2_create_file_attrs(vol, parent_header, name, content, content_len, &own, new_fid_out);
        }
    } else if (attrs->rtype == ODS2_RFM_UDF && attrs->rsize == 0 && attrs->maxrec == 0 && content_len != 0) {
        /* UDF given no LRL: the length, as ods2_create_file() and COPY give it */
        ods2_record_attrs_t own = *attrs;
        own.rsize = (uint16_t) (content_len > 32767 ? 32767 : content_len);
        return ods2_create_file_attrs(vol, parent_header, name, content, content_len, &own, new_fid_out);
    }
    ctx.data = content;
    ctx.pos = 0;
    src.length = content_len;
    src.read = memory_source_read;
    src.ctx = &ctx;
    src.problem = NULL;
    src.var_records = false;
    return ods2_create_file_source(vol, parent_header, name, &src, attrs, new_fid_out);
}

ods2_result_t ods2_create_file_source(ods2_volume_t *vol, const uint8_t *parent_header,
                                       const char *name, ods2_content_source_t *src,
                                       const ods2_record_attrs_t *attrs,
                                       ods2_fid_t *new_fid_out)
{
    const char *why = src->var_records ? ods2_check_var_record_attrs(attrs, src->length)
                                       : ods2_check_record_attrs(attrs, src->length);
    if (why != NULL) {
        return fail(why);
    }
    return create_file(vol, parent_header, name, src, attrs, new_fid_out);
}

ods2_result_t ods2_create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                const char *name, const uint8_t *content, size_t content_len,
                                uint8_t rtype, ods2_fid_t *new_fid_out)
{
    ods2_record_attrs_t attrs;
    attrs.rtype = rtype;
    attrs.rattrib = 0;
    attrs.rsize = 512;
    attrs.maxrec = 512;
    if (ods2_stream_format(rtype)) {
        /* A stream file has no maximum record size (MRS 0, as VMS makes it) and its longest record is its own: MRS 512 made
           RMS refuse such a file with a longer line (%RMS-W-RTB, record too large for the user's buffer). A longer record than
           any record length can be (32767) is VMS's own state for a binary stream file: LRL 32767. */
        size_t longest = ods2_longest_record(rtype, content, content_len);
        attrs.rsize = (uint16_t) (longest > 32767 ? 32767 : longest);
        attrs.maxrec = 0;
    } else if (rtype == ODS2_RFM_UDF) {
        attrs.rsize = (uint16_t) (content_len > 32767 ? 32767 : content_len);
        attrs.maxrec = 0;
    }
    {
        memory_source_t ctx;
        ods2_content_source_t src;
        ctx.data = content;
        ctx.pos = 0;
        src.length = content_len;
        src.read = memory_source_read;
        src.ctx = &ctx;
        src.problem = NULL;
        src.var_records = false;
        return create_file(vol, parent_header, name, &src, &attrs, new_fid_out);
    }
}

static ods2_result_t create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                 const char *name, ods2_content_source_t *src,
                                 const ods2_record_attrs_t *attrs, ods2_fid_t *new_fid_out)
{
    size_t content_len = src->length;
    const ods2_head_core_t *parent_core = (const ods2_head_core_t *) parent_header;
    ods2_fid_t existing;
    size_t blocks_needed;
    size_t blocks_remaining;
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count = 0;
    size_t total_allocated = 0;
    unsigned max_request;
    int seg_first[ODS2_MAX_HEADER_SEGMENTS];
    int seg_count[ODS2_MAX_HEADER_SEGMENTS];
    unsigned seg_file_number[ODS2_MAX_HEADER_SEGMENTS];
    uint16_t seg_seq[ODS2_MAX_HEADER_SEGMENTS];
    int segments = 0, numbered = 0, pos, seg;
    int lowest_written = ODS2_MAX_HEADER_SEGMENTS; /* headers seg.. on disk */
    ods2_header_spec_t spec;
    uint8_t new_header[512];
    ods2_result_t r;
    char upper_name[256];
    char ident[260]; /* name is already bounded (<256 chars, checked
                        above) plus ";1" and a nul - always large
                        enough, sized generously to also silence a
                        spurious compiler truncation warning */

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    if (contains_wildcard(name)) {
        return fail("file name cannot contain wildcard characters ('*' or '%')");
    }
    if (strchr(name, ';') != NULL) {
        /* A "NAME;VER" string stored as the name itself would create
           an entry no VMS system can open. Callers split the version
           off first (see ods2_path.h). */
        return fail("file name cannot contain ';' - pass the name without its version");
    }
    /* Validated and in VMS's on-disk form: uppercase, only name
       characters, and always a dot ("MAKEFILE" -> "MAKEFILE."). */
    {
        const char *why;
        if (!ods2_make_file_name(name, upper_name, sizeof(upper_name), &why)) {
            return fail(why);
        }
    }
    name = upper_name; /* every use below now sees the on-disk name */

    /* The parent must be a directory that can be written before anything is allocated: a refusal later (or a failed
       insert) would leave a complete file that no directory lists. */
    r = directory_ready(vol, parent_header, parent_core->fid.fid_num);
    if (!r.ok) return r;
    r = ods2_lookup_name(vol, parent_header, name, &existing);
    if (r.ok) {
        return fail("a file or directory with this name already exists in the parent");
    }
    if (strcmp(r.problem, "name not found in directory") != 0) return r;   /* any other failure is not "no such name" */

    blocks_needed = (content_len + 511) / 512;
    if (blocks_needed == 0) blocks_needed = 1; /* a zero-length file still needs one block for EFBLK=1... */
    if (blocks_needed > 0xffffffffu - vol->home.cluster) {
        return fail("content is larger than an ODS-2 file can be (2^32 blocks)");
    }

    /* Allocate the content in as few, as large pieces as the free
       space allows: each request asks for everything still needed and
       takes the longest free run when no run is that long. Each piece
       becomes one retrieval pointer - Format 1, 2 or 3, whichever is
       smallest (see ods2_encode_retrieval_pointer()) - so on a volume
       with contiguous free space even a very large file is one or a
       few pointers. A request is capped at Format 3's 2^30-block
       limit, rounded down to whole clusters. */
    /* The primary header's file number first, before the content can
       take the space INDEXF.SYS may need to grow for its header (see
       allocate_file_number()). */
    r = allocate_file_number(vol, &seg_file_number[0], &seg_seq[0]);
    if (!r.ok) return r;
    numbered = 1;

    max_request = (1u << 30) - ((1u << 30) % vol->home.cluster);
    blocks_remaining = blocks_needed;
    while (blocks_remaining > 0) {
        uint32_t chunk_lbn;
        unsigned chunk_allocated;
        unsigned chunk_request = (blocks_remaining < max_request)
                                     ? (unsigned) blocks_remaining : max_request;

        if (extent_count >= ODS2_MAX_EXTENTS) {
            r = fail("the volume's free space is too fragmented to hold this file "
                     "(it would need more than 1024 separate pieces)");
            goto undo;
        }
        r = ods2_allocate_extent(vol, chunk_request, &chunk_lbn, &chunk_allocated);
        if (!r.ok) {
            r = fail("not enough free space on the volume for this file");
            goto undo;
        }
        extents[extent_count].lbn = chunk_lbn;
        extents[extent_count].block_count = chunk_allocated;
        extent_count++;
        total_allocated += chunk_allocated;
        blocks_remaining = (chunk_allocated >= blocks_remaining) ? 0
                                                                 : blocks_remaining - chunk_allocated;
    }

    /* Spread the retrieval pointers over as many headers as they need:
       the primary header holds what fits in its map area, the rest go
       in extension headers chained through FH2$W_EXT_FID (spec 3.3,
       3.5.2.5, 3.5.2.8). */
    for (pos = 0; pos < extent_count; ) {
        int fit = ods2_header_extents_that_fit(extents + pos, extent_count - pos);
        if (fit <= 0) {
            r = fail("internal error: an allocated extent cannot be described by "
                     "any retrieval pointer format (should be unreachable)");
            goto undo;
        }
        if (segments >= ODS2_MAX_HEADER_SEGMENTS) {
            r = fail("the volume's free space is too fragmented to hold this file "
                     "(it would need more than 64 file headers)");
            goto undo;
        }
        seg_first[segments] = pos;
        seg_count[segments] = fit;
        segments++;
        pos += fit;
    }

    /* A file number - and its header slot inside INDEXF.SYS's
       end-of-file, its bit marked in use - for every extension header
       too. */
    while (numbered < segments) {
        r = allocate_file_number(vol, &seg_file_number[numbered], &seg_seq[numbered]);
        if (!r.ok) goto undo;
        numbered++;
    }
    /* The content goes down before any header refers to it. */
    r = write_extents(vol, extents, extent_count, src);
    if (!r.ok) goto undo;

    memset(&spec, 0, sizeof(spec));
    spec.rtype = attrs->rtype;
    spec.rattrib = attrs->rattrib;
    spec.rsize = attrs->rsize;
    spec.maxrec = attrs->maxrec;
    /* Records the whole file's size in every header, as the record
       attributes are copied into extension headers too. */
    spec.hiblk = (uint32_t) total_allocated;
    /* End-of-file mark per spec 6.1.5/6.1.6, in the preferred form
       VMS itself writes: EFBLK is the VBN holding the EOF position,
       FFBYTE the bytes in use there. Content ending exactly on a
       block boundary (including an empty file) gets EFBLK = blocks+1
       and FFBYTE = 0. (An earlier version wrote EFBLK = blocks and
       FFBYTE = 0 for that case, which VMS reads as one block SHORTER
       than the real content, and an empty file as 512 bytes.) */
    spec.efblk = (uint32_t) (content_len / 512) + 1;
    spec.ffbyte = (uint16_t) (content_len % 512);
    spec.filechar = (extent_count == 1) ? 0x0080 : 0; /* FH2$M_CONTIG only when truly one piece */
    snprintf(ident, sizeof(ident), "%s;1", name);
    spec.ident_name = ident;

    /* Extension headers first, last to first, then the primary, so
       every header written only ever points at headers already on
       disk. Each extension header has its own FID, its segment
       number, and a back link to the primary header (spec 3.5.2.16). */
    for (seg = segments - 1; seg >= 0; seg--) {
        spec.fid.fid_num = (uint16_t) seg_file_number[seg];
        spec.fid.fid_seq = seg_seq[seg];
        spec.seg_num = (uint16_t) seg;
        memset(&spec.ext_fid, 0, sizeof(spec.ext_fid));
        if (seg + 1 < segments) {
            spec.ext_fid.fid_num = (uint16_t) seg_file_number[seg + 1];
            spec.ext_fid.fid_seq = seg_seq[seg + 1];
        }
        memset(&spec.backlink, 0, sizeof(spec.backlink));
        if (seg == 0) {
            spec.backlink = parent_core->fid;
        } else {
            spec.backlink.fid_num = (uint16_t) seg_file_number[0];
            spec.backlink.fid_seq = seg_seq[0];
        }
        spec.extents = extents + seg_first[seg];
        spec.extent_count = seg_count[seg];
        if (!ods2_build_file_header(new_header, &spec)) {
            r = fail("could not construct new file's header");
            goto undo;
        }
        r = ods2_write_header(vol, seg_file_number[seg], new_header);
        if (!r.ok) goto undo;
        lowest_written = seg;
    }
    /* spec.fid is now the primary header's, for the directory entry. */

    /* Insert an entry for the new file into the parent - growing the
       parent's own allocation automatically if none of its existing
       content blocks have room. */
    r = ods2_insert_into_directory(vol, parent_core->fid.fid_num, name, 1, spec.fid);
    if (!r.ok) goto undo;       /* a file nobody lists is given back, not left */

    *new_fid_out = spec.fid;
    return ok();

undo:
    /* Give back what was taken before the failure, so a refused or
       failed copy leaves no blocks or file numbers allocated to
       nothing. Any header already written is marked deleted (as
       ods2_delete_version() does), since a slot holding a valid header
       is never reused (spec 5.1.7). */
    for (seg = lowest_written; seg < segments; seg++) {
        memset(new_header, 0, sizeof(new_header));
        if (ods2_read_header(vol, seg_file_number[seg], new_header).ok) {
            ods2_head_core_t *core = (ods2_head_core_t *) new_header;
            core->filechar |= 0x8000u; /* FH2$M_MARKDEL */
            core->fid.fid_num = 0;
            core->fid.fid_nmx = 0;
            core->fid.fid_rvn = 0;
            new_header[510] = 0;
            new_header[511] = 0;
            ods2_write_header(vol, seg_file_number[seg], new_header);
        }
    }
    for (pos = 0; pos < extent_count; pos++) {
        ods2_free_blocks(vol, extents[pos].lbn, extents[pos].block_count);
    }
    for (seg = 0; seg < numbered; seg++) {
        mark_index_bitmap(vol, seg_file_number[seg], false);
    }
    return r;
}

/* Writes back a directory changed in memory: `blocks` holds its
 * `used` blocks, in order; `old_used` is how many it had before, and
 * blocks from index `first_changed` on are written.
 *
 * If `used` exceeds the allocation, the directory moves: spec 4.2 says
 * a directory is a contiguous file (ods2v2 sets FH2$M_CONTIG on every
 * directory it makes), so it can't simply gain a second extent
 * somewhere else on the disk. It gets a new contiguous area of up to
 * twice its old size, is written there in full, its header is switched
 * to the single new extent, and only then is the old area freed.
 *
 * The end-of-file becomes EFBLK used+1 / FFBYTE 0, as VMS writes it,
 * and the directory's revision count and date go up. */
/* True when VMS will read back what is written at `vbn`: the file is
   not high-water marked, or `vbn` is below its high-water mark. */
static bool below_highwater(const uint8_t *header, unsigned vbn)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    return core->highwater == 0 || vbn < core->highwater;
}

/* True when the end-of-file is what VMS writes for `used` blocks:
   EFBLK used+1, FFBYTE 0. Earlier versions made directories with EFBLK 1
   / FFBYTE 2, and OpenVMS VAX V7.3 copies the parent's FFBYTE into the
   directory CREATE/DIRECTORY makes in it, which gave EOF (2,2): a second
   block of two bytes that is not a directory block (ANALYZE/DISK:
   BAD_DIRSIZE, BAD_DIRTYPE; SYSTEM-W-BADIRECTORY). Writing to a
   directory puts its end-of-file right. */
static bool dir_eof_standard(const uint8_t *dir_header, unsigned used)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) dir_header;
    return core->recattr.ffbyte == 0 && ods2_word_swap32(core->recattr.efblk) == used + 1u;
}

static ods2_result_t store_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                     uint8_t *dir_header, const uint8_t *blocks,
                                     unsigned used, unsigned old_used, unsigned first_changed)
{
    ods2_head_core_t *core = (ods2_head_core_t *) dir_header;
    ods2_extent_t old_extents[ODS2_MAX_EXTENTS];
    int old_count, i;
    unsigned allocated = 0, v;
    bool moved = false, in_place_growth = false;
    uint8_t zero[512];
    ods2_result_t r;

    memset(zero, 0, sizeof(zero));
    /* the header is rewritten with a fresh checksum (and its identification area is stored into at IDOFFSET): it must
       be a sound one, or the damage would be made to look right */
    r = header_ok_for_write(dir_header, core->fid.fid_num);
    if (!r.ok) return r;
    if (core->ext_fid.fid_num != 0 || core->ext_fid.fid_nmx != 0 || core->ext_fid.fid_rvn != 0) {
        return fail("this directory has an extension header: it is not changed (a move would leave the extension "
                    "behind)");
    }
    r = ods2_decode_all_extents(vol, dir_header, old_extents, ODS2_MAX_EXTENTS, &old_count);
    if (!r.ok) return r;
    for (i = 0; i < old_count; i++) allocated += old_extents[i].block_count;

    if (used > allocated) {
        /* One Format 1 retrieval pointer maps at most 256 blocks. */
        unsigned max_blocks = (256u / vol->home.cluster) * vol->home.cluster;
        unsigned want = allocated * 2;
        uint32_t lbn;
        unsigned got;
        size_t ptr_size;
        size_t map_start = (size_t) core->mpoffset * 2;
        size_t map_end = (size_t) core->acoffset * 2;

        if (used > max_blocks) {
            return fail("directory would need more than 256 blocks, more than "
                        "one contiguous extent can map here - not yet supported");
        }
        if (want < used) want = used;
        if (want > max_blocks) want = max_blocks;
        r = ods2_allocate_blocks(vol, want, &lbn, &got);
        if (!r.ok && want > used) {
            want = used; /* no run that long free: settle for what's needed */
            r = ods2_allocate_blocks(vol, want, &lbn, &got);
        }
        if (!r.ok) return r;
        /* Format 1 below LBN 4,194,303, Format 2 beyond it (issue #14):
           the new area may lie anywhere on a large volume. */
        ptr_size = ods2_retrieval_pointer_size(lbn, got);
        if (ptr_size == 0 || map_end > 510 || map_end < map_start + ptr_size) {
            ods2_free_blocks(vol, lbn, got);
            return fail("cannot map the directory's new area: no room in its "
                        "header's map area for the retrieval pointer");
        }
        for (v = 0; v < got; v++) {
            r = ods2_write_block(vol, lbn + v, (v < used) ? blocks + (size_t) v * 512 : zero);
            if (!r.ok) return r;
        }
        memset(dir_header + map_start, 0, map_end - map_start);
        ods2_encode_retrieval_pointer(dir_header + map_start, lbn, got);
        core->map_inuse = (uint8_t) (ptr_size / 2);
        core->recattr.hiblk = ods2_word_swap32(got);
        moved = true;
    } else if (used == old_used + 1 && first_changed < old_used) {
        /* A block that splits, with the next blocks moving down one: the blocks are rewritten in an order in which
           every name stays listed at every write, a name listed twice for a moment being harmless and a name missing
           not. (Written in ascending order, as before, the block that split lost the names of its second half until
           the next block was written, and each following block lost its names to the one before it: an interruption
           anywhere in between left names unlisted, their files unreachable.) First the old last block's copy goes
           into the block just past the end-of-file (invisible there); the header then moves the end-of-file over it
           (below); then each block takes the place of the one before it, from the end back to the block that split,
           which is rewritten last: until then its names are all still there, and the new second half is a second copy
           of the names the old block still holds. */
        in_place_growth = true;
        r = ods2_write_file_block(vol, dir_header, used, blocks + (size_t) (used - 1) * 512);
        if (!r.ok) return r;
    } else {
        for (v = first_changed; v < used; v++) {
            r = ods2_write_file_block(vol, dir_header, v + 1, blocks + (size_t) v * 512);
            if (!r.ok) return r;
        }
        for (v = used; v < old_used; v++) { /* blocks no longer in use */
            r = ods2_write_file_block(vol, dir_header, v + 1, zero);
            if (!r.ok) return r;
        }
    }

    core->recattr.efblk = ods2_word_swap32(used + 1);
    core->recattr.ffbyte = 0;
    /* FH2$L_HIGHWATER: the first VBN never written. On a volume with
       high-water marking (the INITIALIZE default), VMS returns zeros
       for any read at or past it, whatever is on the disk. A directory
       INITIALIZE made carries HIGHWATER 2 (only VBN 1 written), so a
       block added past it reads back on VMS as a zero record:
       BAD_NAMEORDER "[empty]", BAD_DIRSIZE, BADIRECTORY. Zero means
       the file isn't high-water marked, and stays zero. */
    if (core->highwater != 0) {
        uint32_t written = moved ? ods2_word_swap32(core->recattr.hiblk) : used;
        if (core->highwater < written + 1) core->highwater = written + 1;
    }
    {
        /* FI2$W_REVISION / FI2$Q_REVDATE; FI2$Q_CREDATE stays. */
        uint8_t *ident = dir_header + (size_t) core->idoffset * 2;
        uint16_t revision = (uint16_t) (ident[20] | (ident[21] << 8));
        revision++;
        ident[20] = (uint8_t) (revision & 0xff);
        ident[21] = (uint8_t) (revision >> 8);
        ods2_store_vms_time(ident + 30, ods2_vms_time_now()); /* local time */
    }
    {
        uint16_t checksum = ods2_checksum(dir_header, 255);
        dir_header[510] = (uint8_t) (checksum & 0xff);
        dir_header[511] = (uint8_t) (checksum >> 8);
    }
    r = ods2_write_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;

    if (in_place_growth) {
        for (v = used - 1; v > first_changed; v--) {     /* blocks[v - 1] .. blocks[first_changed], last first */
            r = ods2_write_file_block(vol, dir_header, v, blocks + (size_t) (v - 1) * 512);
            if (!r.ok) return r;
        }
    }

    if (moved) {
        for (i = 0; i < old_count; i++) {
            r = ods2_free_blocks(vol, old_extents[i].lbn, old_extents[i].block_count);
            if (!r.ok) return r;
        }
    }
    return ok();
}

/* Reads a directory's blocks in use into a new buffer, with room for
   `spare` more blocks after them. *used_out is at least 1 (a directory
   always has its first block). */
static ods2_result_t load_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                    unsigned spare, uint8_t **blocks_out,
                                    unsigned *used_out)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int count, i;
    unsigned allocated = 0, used, v;
    uint8_t *blocks;
    ods2_result_t r = ods2_decode_all_extents(vol, dir_header, extents, ODS2_MAX_EXTENTS,
                                              &count);
    if (!r.ok) return r;
    for (i = 0; i < count; i++) allocated += extents[i].block_count;
    if (allocated == 0) return fail("directory has no blocks allocated");
    used = dir_blocks_in_use(dir_header, allocated);
    if (used == 0) used = 1;
    if (used > 65536u) {
        return fail("directory claims more than 65536 blocks: damaged, or too big to change here");
    }

    blocks = calloc((size_t) used + spare, 512);
    if (blocks == NULL) return fail("could not allocate memory for the directory");
    for (v = 0; v < used; v++) {
        r = ods2_read_file_block(vol, dir_header, v + 1, blocks + (size_t) v * 512);
        if (!r.ok) {
            free(blocks);
            return r;
        }
    }
    /* Trailing blocks that are all zeros (not even an end-of-records
       marker) are not directory blocks: the VAX-made directory with EOF
       (2,2) has one inside its end-of-file. They are dropped from `used`
       so that a write puts the end-of-file right (block 1 stays and
       carries the marker). */
    while (used > 1 && blocks[(size_t) (used - 1) * 512] == 0 &&
           blocks[(size_t) (used - 1) * 512 + 1] == 0) {
        used--;
    }
    /* A directory is changed only if every block of it is well formed: a record whose name count or size runs past
       its block would be read, shifted or split on trust. */
    for (v = 0; v < used; v++) {
        if (!ods2_dir_block_ok(blocks + (size_t) v * 512, 512)) {
            free(blocks);
            return fail("a directory block is damaged (a record runs past the block): the directory is not "
                        "changed - run ANALYZE/DISK_STRUCTURE/REPAIR on VMS");
        }
    }
    *blocks_out = blocks;
    *used_out = used;
    return ok();
}

/* A directory that is about to be changed in any way: its header must be one that may be rewritten (checksum, offsets,
   IDOFFSET room, its own file number), it must not have an extension header (a move or a split would replace only the
   primary header's map and leave the extension behind, its extents free, and the next growth would write into them),
   and every block of it must be well formed. Every operation that changes a directory (or creates something that is
   to be listed in one) asks this before its first write, so that a refusal leaves nothing behind: no header marked for
   delete, no file nobody lists. */
static ods2_result_t directory_ready(ods2_volume_t *vol, const uint8_t *dir_header, unsigned dir_file_number)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) dir_header;
    uint8_t *blocks = NULL;
    unsigned used;
    ods2_result_t r = header_ok_for_write(dir_header, dir_file_number);

    if (!r.ok) return r;
    if (core->ext_fid.fid_num != 0 || core->ext_fid.fid_nmx != 0 || core->ext_fid.fid_rvn != 0) {
        return fail("this directory has an extension header: it is not changed (a move would leave the extension "
                    "behind)");
    }
    r = load_directory(vol, dir_header, 0, &blocks, &used);
    free(blocks);
    return r;
}

/* Inserts (name, version, fid) where it belongs in the directory's
 * overall sorted order (spec 4.2): into the block whose names it falls
 * among. If that block is full, it is split in two - its records,
 * with the new one, divided between it and a new block placed right
 * after it, the following blocks moving down one - so the order holds
 * across blocks and every block inside the end-of-file holds entries.
 * VMS's own directory search depends on that order. */
ods2_result_t ods2_insert_into_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                          const char *name, uint16_t version, ods2_fid_t fid)
{
    uint8_t dir_header[512];
    uint8_t *blocks = NULL;
    unsigned used, t;
    uint16_t verlimit;
    ods2_result_t r;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;
    r = directory_ready(vol, dir_header, dir_file_number);
    if (!r.ok) return r;
    r = load_directory(vol, dir_header, 1, &blocks, &used);
    if (!r.ok) return r;

    t = ods2_dir_choose_block(blocks, used, name);
    verlimit = ((const ods2_head_core_t *) dir_header)->recattr.versions;   /* the directory's default version limit */
    if (ods2_insert_dir_entry_vl(blocks + (size_t) t * 512, 512, name, version, fid, verlimit)) {
        if (ods2_file_content_length(dir_header) == 0 || !dir_eof_standard(dir_header, used) ||
            !below_highwater(dir_header, t + 1)) {
            /* An end-of-file that didn't cover even block 1, or a block
               VMS would read as zeros until the high-water mark moves */
            r = store_directory(vol, dir_file_number, dir_header, blocks, used, used, t);
        } else {
            r = ods2_write_file_block(vol, dir_header, t + 1, blocks + (size_t) t * 512);
        }
    } else {
        uint8_t combined[1024];
        memcpy(combined, blocks + (size_t) t * 512, 512);
        memset(combined + 512, 0, 512);
        if (!ods2_insert_dir_entry_vl(combined, sizeof(combined), name, version, fid, verlimit)) {
            free(blocks);
            return fail("internal error: a block plus one record overflowed 1024 "
                        "bytes (should be unreachable)");
        }
        memmove(blocks + (size_t) (t + 2) * 512, blocks + (size_t) (t + 1) * 512,
                (size_t) (used - t - 1) * 512);
        if (!ods2_dir_split_block(combined, sizeof(combined), blocks + (size_t) t * 512,
                                  blocks + (size_t) (t + 1) * 512)) {
            free(blocks);
            return fail("internal error: could not split a full directory block "
                        "(should be unreachable)");
        }
        r = store_directory(vol, dir_file_number, dir_header, blocks, used + 1, used, t);
    }
    free(blocks);
    return r;
}

/* SET FILE/VERSION_LIMIT: the version limit of the directory entries named `name` in a directory, block by block.
   *changed gets the number of records changed (0 when the name is not there or already has the limit). */
ods2_result_t ods2_set_entry_version_limit(ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                                           uint16_t limit, unsigned *changed)
{
    uint8_t dir_header[512];
    uint8_t *blocks = NULL;
    unsigned used, v;
    ods2_result_t r;

    *changed = 0;
    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;
    r = load_directory(vol, dir_header, 0, &blocks, &used);
    if (!r.ok) return r;
    for (v = 0; v < used && r.ok; v++) {
        int n = ods2_set_dir_entry_verlimit(blocks + (size_t) v * 512, 512, name, limit);
        if (n < 0) { r = fail("a directory block is damaged: not changed"); break; }
        if (n > 0) {
            *changed += (unsigned) n;
            r = ods2_write_file_block(vol, dir_header, v + 1, blocks + (size_t) v * 512);
        }
    }
    free(blocks);
    return r;
}

/* The checks of ods2_free_blocks() without the writes: a caller that is about to change headers asks first, so that a range
   the free would refuse does not fire after the headers are final. */
static ods2_result_t free_blocks_impl(ods2_volume_t *vol, uint32_t lbn, unsigned block_count, bool dry_run);

ods2_result_t ods2_free_blocks(ods2_volume_t *vol, uint32_t lbn, unsigned block_count)
{
    return free_blocks_impl(vol, lbn, block_count, false);
}

static ods2_result_t free_blocks_impl(ods2_volume_t *vol, uint32_t lbn, unsigned block_count, bool dry_run)
{
    uint8_t bitmap_header[512];
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    unsigned total_bitmap_blocks = 0;
    unsigned bits_blocks;
    ods2_result_t r;
    unsigned i;
    size_t start_cluster, clusters_to_free;
    size_t first_byte, last_byte;
    unsigned first_block, last_block;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    if (block_count == 0) {
        return ok(); /* nothing to do */
    }
    if (lbn % vol->home.cluster != 0 || block_count % vol->home.cluster != 0) {
        return fail("lbn/block_count must be cluster-aligned (exactly what "
                    "ods2_allocate_blocks() returned)");
    }

    r = ods2_read_header(vol, 2, bitmap_header); /* BITMAP.SYS is always file 2 */
    if (!r.ok) return r;

    r = ods2_decode_all_extents(vol, bitmap_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    if (extent_count <= 0) {
        return fail("could not decode BITMAP.SYS's own extents");
    }
    for (i = 0; i < (unsigned) extent_count; i++) total_bitmap_blocks += extents[i].block_count;
    if (total_bitmap_blocks < 2) {
        return fail("BITMAP.SYS is too small to contain both an SCB and bitmap data");
    }
    bits_blocks = total_bitmap_blocks - 1; /* excluding VBN 1's SCB, spec 5.2.1 */

    start_cluster = lbn / vol->home.cluster;
    clusters_to_free = block_count / vol->home.cluster;

    first_byte = start_cluster / 8;
    last_byte = (start_cluster + clusters_to_free - 1) / 8;
    first_block = (unsigned) (first_byte / 512);
    last_block = (unsigned) (last_byte / 512);
    if (last_block >= bits_blocks) {
        return fail("block range to free is beyond BITMAP.SYS's own extent");
    }
    if (dry_run) return ok();

    for (i = first_block; i <= last_block; i++) {
        uint8_t bitmap_block[512];
        size_t block_start_cluster = (size_t) i * 512 * 8;
        size_t local_start, local_count;

        r = ods2_read_file_block(vol, bitmap_header, i + 2, bitmap_block); /* +2: skip SCB, spec 5.2.1 */
        if (!r.ok) return r;

        /* Clip the free range to just the bits within this block. */
        local_start = (start_cluster > block_start_cluster)
                          ? start_cluster - block_start_cluster : 0;
        {
            size_t range_end = start_cluster + clusters_to_free; /* exclusive */
            size_t block_end = block_start_cluster + 512 * 8;
            size_t clipped_end = (range_end < block_end) ? range_end : block_end;
            local_count = (clipped_end > block_start_cluster + local_start)
                              ? clipped_end - (block_start_cluster + local_start) : 0;
        }

        if (local_count > 0) {
            if (!ods2_bitmap_mark(bitmap_block, 512u * 8u, local_start, local_count, false)) {
                return fail("internal error freeing bitmap bits (should be unreachable)");
            }
            r = ods2_write_file_block(vol, bitmap_header, i + 2, bitmap_block);
            if (!r.ok) return r;
        }
    }

    return ok();
}

/* A header read for a change: nothing on the write path may rewrite a header with a fresh checksum without looking at it,
   or the damage would be made to look right. The checksum, the level, the offsets in order and inside the block, room
   for the identification area the writers store into (revision count and revision date), and the FID's own file
   number. A header that fails is not changed. */
static ods2_result_t header_ok_for_write(const uint8_t *h, unsigned file_number)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;

    if (ods2_checksum(h, 255) != (uint16_t) (h[510] | (h[511] << 8)) ||
        (core->struclev >> 8) != 2 ||
        core->idoffset < 40 || core->idoffset > core->mpoffset || core->mpoffset > core->acoffset ||
        core->acoffset > core->rsoffset ||
        (size_t) core->idoffset * 2 + 38 > (size_t) core->mpoffset * 2 ||
        (size_t) core->mpoffset * 2 >= 510 ||
        core->fid.fid_num != file_number || core->fid.fid_nmx != 0 || core->fid.fid_rvn != 0) {
        return fail("a file header is damaged (checksum, offsets or file number): it is not changed - "
                    "run ANALYZE/DISK_STRUCTURE/REPAIR on VMS");
    }
    return ok();
}

/* The file name an ODS-2 header carries in its identification area ("NAME.EXT;VER", 20 bytes and a continuation of
   66), without the version, upper-cased. False if the area is not there. */
static bool header_name(const uint8_t *header, char *out, size_t cap)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    const uint8_t *ident = header + (size_t) core->idoffset * 2;
    char buf[100];
    size_t n = 0, i;

    if ((size_t) core->idoffset * 2 + 120 > (size_t) core->mpoffset * 2 || (core->struclev >> 8) != 2) return false;
    memcpy(buf, ident, 20);
    memcpy(buf + 20, ident + 54, 66 < sizeof(buf) - 20 ? 66 : sizeof(buf) - 20);
    n = 20 + (66 < sizeof(buf) - 20 ? 66 : sizeof(buf) - 20);   /* only what was copied: nothing past it is read */
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\0')) n--;
    for (i = 0; i < n; i++) if (buf[i] == ';') { n = i; break; }
    if (n + 1 > cap) return false;
    for (i = 0; i < n; i++) out[i] = (char) toupper((unsigned char) buf[i]);
    out[n] = '\0';
    return true;
}

/* True for a file number the volume reserves for its own files: 1-9 always, and 10 (SECURITY.SYS) when the entry is
   SECURITY.SYS in the master file directory. Deleting one frees clusters the volume still depends on. */
static bool is_reserved_file(unsigned dir_file_number, const char *name, unsigned file_number)
{
    static const char security[] = "SECURITY.SYS";
    size_t i;

    if (file_number >= 1 && file_number <= 9) return true;
    if (file_number != 10 || dir_file_number != 4 || strlen(name) != sizeof(security) - 1) return false;
    for (i = 0; i < sizeof(security) - 1; i++) {
        if (toupper((unsigned char) name[i]) != security[i]) return false;
    }
    return true;
}

/* File 10 is SECURITY.SYS on a volume INITIALIZE made, and a user file on an older one: judged by what its header
   says, wherever the entry that names it is (an alias in another directory is refused too). */
static bool file10_is_security(const uint8_t *header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    char name[100];

    return core->backlink.fid_num == 4 && header_name(header, name, sizeof(name)) && strcmp(name, "SECURITY.SYS") == 0;
}

ods2_result_t ods2_delete(ods2_volume_t *vol, unsigned dir_file_number, const char *name)
{
    return ods2_delete_version(vol, dir_file_number, name, ODS2_VERSION_HIGHEST, NULL);
}

/* Takes the entry (name, version) of the directory out: just that version's, trying each block in use until the record
   holding it is found. Removing the whole record instead (as an earlier version did) would also drop every other
   version of the name stored there, leaving their headers allocated but unreachable. A block left with no entries is
   removed and the blocks after it move up, as the VMS file system does, so every block inside the end-of-file holds
   entries. */
static ods2_result_t remove_directory_entry(ods2_volume_t *vol, unsigned dir_file_number, uint8_t *dir_header,
                                            const char *name, uint16_t version)
{
    uint8_t *blocks = NULL;
    unsigned used, v;
    bool removed = false;
    ods2_result_t r = load_directory(vol, dir_header, 0, &blocks, &used);

    if (!r.ok) return r;
    for (v = 0; v < used && !removed && r.ok; v++) {
        uint8_t *block = blocks + (size_t) v * 512;
        if (!ods2_remove_dir_version(block, 512, name, version)) continue;
        removed = true;
        if (used > 1 && ods2_dir_block_is_empty(block)) {
            memmove(block, block + 512, (size_t) (used - v - 1) * 512);
            r = store_directory(vol, dir_file_number, dir_header, blocks, used - 1, used, v);
        } else if (!below_highwater(dir_header, v + 1) || !dir_eof_standard(dir_header, used)) {
            r = store_directory(vol, dir_file_number, dir_header, blocks, used, used, v);
        } else {
            r = ods2_write_file_block(vol, dir_header, v + 1, block);
        }
    }
    free(blocks);
    if (!r.ok) return r;
    if (!removed) {
        return fail("internal error: entry existed at lookup time but was not found for removal (should be unreachable)");
    }
    return ok();
}

ods2_result_t ods2_delete_version(ods2_volume_t *vol, unsigned dir_file_number,
                                  const char *name, int version, uint16_t *version_out)
{
    uint16_t target_version;
    uint8_t dir_header[512];
    ods2_fid_t target_fid;
    uint8_t target_header[512];
    ods2_head_core_t *target_core;
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    int i;
    uint8_t ext_headers[ODS2_MAX_HEADER_SEGMENTS][512];
    unsigned ext_numbers[ODS2_MAX_HEADER_SEGMENTS];
    int ext_count = 0;
    ods2_result_t r;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }

    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;

    r = ods2_lookup_name_version(vol, dir_header, name, version, &target_fid, &target_version);
    if (!r.ok) return r;

    /* An entry's FID names a file by number, sequence, relative volume number and extended number; this code finds a
       file by the number alone, in this volume. An entry with an RVN or an NMX (a volume set member, a file number
       above 65,535) would resolve to some other file's header: refused. */
    r = ods2_check_fid(target_fid);
    if (!r.ok) return r;

    /* The volume's own files cannot be deleted, whichever directory lists them: INDEXF.SYS, BITMAP.SYS, BADBLK.SYS,
       the master file directory, CORIMG.SYS, VOLSET.SYS, CONTIN.SYS, BACKUP.SYS and BADLOG.SYS are file numbers 1-9;
       SECURITY.SYS is 10 on a volume INITIALIZE made (an older one has no file 10, and a user file may hold the
       number). Deleting one frees clusters the volume still depends on. */
    if (is_reserved_file(dir_file_number, name, target_fid.fid_num)) {
        return fail("this is one of the volume's reserved files (INDEXF.SYS, BITMAP.SYS, ... "
                    "SECURITY.SYS): it cannot be deleted");
    }

    r = ods2_read_header(vol, target_fid.fid_num, target_header);
    if (!r.ok) return r;
    target_core = (ods2_head_core_t *) target_header;
    /* file 10 by what its header says, wherever the entry is (an alias in another directory is refused too; VMS's
       SET FILE/REMOVE took such entries and left LOSTHEADER) */
    if (target_fid.fid_num == 10 && ods2_header_is_file(target_header, target_fid) && file10_is_security(target_header)) {
        return fail("this is one of the volume's reserved files (INDEXF.SYS, BITMAP.SYS, ... "
                    "SECURITY.SYS): it cannot be deleted");
    }
    if (!ods2_header_is_file(target_header, target_fid)) {
        /* A dangling entry: its file went with another name, and the header may be free or belong to another file
           now. Deleting "the file" would free someone else's clusters: the entry is taken out and nothing else is
           touched, as VMS does (a VMS state: the entries left when the primary one of a file with aliases is
           deleted). */
        r = remove_directory_entry(vol, dir_file_number, dir_header, name, target_version);
        if (!r.ok) return r;
        if (version_out != NULL) *version_out = target_version;
        return ok();
    }

    /* Refuse to delete a non-empty directory - matches VMS's own DELETE/DIRECTORY behavior and avoids silently
       orphaning its contents (their headers would become unreachable but never freed - a real, if recoverable via
       ANALYZE/DISK, leak). */
    if (target_core->filechar & 0x2000u) { /* FH2$M_DIRECTORY, confirmed value */
        int sub_count = 0;
        r = ods2_count_directory_entries(vol, target_header, &sub_count);
        if (!r.ok) return r;
        if (sub_count > 0) {
            return fail("directory is not empty - refusing to delete "
                        "(matches VMS's own DELETE/DIRECTORY behavior)");
        }
    }

    /* The header is rewritten (marked for delete) with a fresh checksum: it must be a sound one */
    r = header_ok_for_write(target_header, target_fid.fid_num);
    if (!r.ok) return r;
    /* Everything that can refuse runs before the first write, so that a refusal leaves the file as it was: the
       directory the entry comes out of (a header marked for delete whose entry then could not be removed would stay
       listed, unreadable, until ANALYZE/DISK/REPAIR on VMS) and, for a directory, its own blocks (a damaged one counts
       as empty). */
    r = directory_ready(vol, dir_header, dir_file_number);
    if (!r.ok) return r;
    if (target_core->filechar & 0x2000u) {
        r = directory_ready(vol, target_header, target_fid.fid_num);
        if (!r.ok) return r;
    }

    /* Read everything the delete needs before anything is changed, so that a failure here (a damaged map, an
       extension header that cannot be read, a file with more extents than this program holds) leaves the file as it
       was. */
    r = ods2_decode_all_extents(vol, target_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    for (i = 0; i < extent_count; i++) {
        /* ods2_free_blocks() refuses an extent that is not whole clusters: refuse here, before anything is changed */
        if (extents[i].lbn % vol->home.cluster != 0 || extents[i].block_count % vol->home.cluster != 0) {
            return fail("an extent of this file is not a whole number of clusters at a cluster boundary "
                        "(damaged map): not changed");
        }
        r = free_blocks_impl(vol, extents[i].lbn, extents[i].block_count, true);   /* the range, too, is checked now */
        if (!r.ok) return r;
    }
    {
        ods2_fid_t next = target_core->ext_fid;
        while (next.fid_num != 0 || next.fid_nmx != 0) {
            if (ext_count >= ODS2_MAX_HEADER_SEGMENTS) {
                return fail("extension header chain exceeded maximum segment limit "
                            "(possibly corrupted or circular ext_fid chain)");
            }
            r = ods2_check_fid(next);
            if (!r.ok) return r;
            ext_numbers[ext_count] = next.fid_num;
            r = ods2_read_header(vol, next.fid_num, ext_headers[ext_count]);
            if (!r.ok) return r;
            if (!extension_header_ok(ext_headers[ext_count], next, target_fid.fid_num, (unsigned) ext_count + 1)) {
                return fail("damaged extension header chain (an extension header that is not this file's): not "
                            "changed");
            }
            next = ((ods2_head_core_t *) ext_headers[ext_count])->ext_fid;
            ext_count++;
        }
    }

    /* VMS's own order, so that an interruption never leaves a listed file whose clusters are free, nor a header that
       still maps clusters another file may be given. The earlier order here (free the clusters, release the
       extension headers, zero the header, and only then remove the entry) left, after every write but the last, a
       directory entry naming a file whose blocks were free: a crash part way, or a full disk, and the next file
       written took the blocks of a file still listed.
       (a) mark the header for delete, FID and checksum intact; (b) remove the directory entry; (c) write the extension
       headers and then the primary header in their final deleted form (FID zeroed, checksum 0); (d) free the blocks;
       (e) clear the file numbers' bits. After (a) the file reads as gone (ods2_header_is_file); an interruption after
       (a) leaves a marked header whose clusters are still allocated, and after (c) only clusters that nothing owns
       (lost clusters, which ANALYZE/DISK/REPAIR frees): in no state does a header that could be finished or recovered
       map free clusters. */
    target_core->filechar |= 0x8000u; /* FH2$M_MARKDEL, confirmed value */
    {
        uint16_t checksum = ods2_checksum(target_header, 255);
        target_header[510] = (uint8_t) (checksum & 0xff);
        target_header[511] = (uint8_t) (checksum >> 8);
    }
    r = ods2_write_header(vol, target_fid.fid_num, target_header);
    if (!r.ok) return r;

    /* If the entry cannot be removed the header stays marked: the entry is then dangling and the file reads as gone,
       a leak; writing the old header back could make a half-removed entry's file live and unlisted. */
    r = remove_directory_entry(vol, dir_file_number, dir_header, name, target_version);
    if (!r.ok) return r;

    /* Extension headers (spec 3.3): each has its own file number and header slot, released like the primary. Left
       alone they would stay allocated, owned by nothing. */
    for (i = 0; i < ext_count; i++) {
        ods2_head_core_t *ext_core = (ods2_head_core_t *) ext_headers[i];

        ext_core->filechar |= 0x8000u;
        ext_core->fid.fid_num = 0;
        ext_core->fid.fid_nmx = 0;
        ext_core->fid.fid_rvn = 0;
        ext_headers[i][510] = 0;
        ext_headers[i][511] = 0;
        r = ods2_write_header(vol, ext_numbers[i], ext_headers[i]);
        if (!r.ok) return r;
    }

    /* The header itself, per spec 3.5.1's exact rules: FH2$V_MARKDEL set, FID_NUM/NMX/RVN zeroed - but FH2$W_FID_SEQ
       is explicitly NOT touched, since spec 5.1.7 requires it to survive so the next use of this slot gets seq+1, not
       a fresh seq of 1 (that rule is only for a slot that was never used at all - "garbage" - which this no longer
       is). Checksum is set to zero, not recomputed - also an explicit spec rule, not an oversight. */
    target_core->fid.fid_num = 0;
    target_core->fid.fid_nmx = 0;
    target_core->fid.fid_rvn = 0;
    target_header[510] = 0;
    target_header[511] = 0;
    r = ods2_write_header(vol, target_fid.fid_num, target_header);
    if (!r.ok) return r;

    for (i = 0; i < extent_count; i++) {
        r = ods2_free_blocks(vol, extents[i].lbn, extents[i].block_count);
        if (!r.ok) return r;
    }

    /* the file numbers are free in the index file's bitmap: the extension headers, the primary last */
    for (i = 0; i < ext_count; i++) {
        r = mark_index_bitmap(vol, ext_numbers[i], false);
        if (!r.ok) return r;
    }
    r = mark_index_bitmap(vol, target_fid.fid_num, false);
    if (!r.ok) return r;

    if (version_out != NULL) *version_out = target_version;
    return ok();
}

/* --- the volume label (issue #13) --- */

const char *ods2_make_volume_label(const char *label, char out[12])
{
    size_t len = strlen(label), i;

    if (len == 0) return "a volume label may not be empty";
    if (len > 12) return "a volume label is at most 12 characters";
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char) label[i];
        if (!(isalnum(c) || c == '$' || c == '_' || c == '-')) {
            return "a volume label may contain only letters, digits, '$', '_' and '-'";
        }
    }
    for (i = 0; i < 12; i++) {
        out[i] = (i < len) ? (char) toupper((unsigned char) label[i]) : ' ';
    }
    return NULL;
}

/* A 12-byte name field is blank: spaces (as the spec says) or NULs. */
static bool name_is_blank(const char *field)
{
    int i;
    for (i = 0; i < 12; i++) {
        if (field[i] != ' ' && field[i] != '\0') return false;
    }
    return true;
}

static void set_home_checksums(uint8_t *raw)
{
    /* checksum1 first: checksum2's coverage includes it (see
       ods2_validate.h). */
    uint16_t c1 = ods2_checksum(raw, 29), c2;
    raw[58] = (uint8_t) (c1 & 0xff);
    raw[59] = (uint8_t) (c1 >> 8);
    c2 = ods2_checksum(raw, 255);
    raw[510] = (uint8_t) (c2 & 0xff);
    raw[511] = (uint8_t) (c2 >> 8);
}

/* Is `block` the copy of `primary` that belongs at `lbn` / INDEXF.SYS
   VBN `vbn`? A valid home block whose HOMELBN and HOMEVBN name its own
   position, and otherwise identical to the primary: only those two
   fields and the checksums differ between copies (as on every volume
   VMS INITIALIZE made that has been examined). */
static bool is_home_copy(const uint8_t *block, const uint8_t *primary, uint32_t lbn,
                         uint32_t vbn)
{
    const ods2_home_t *h = (const ods2_home_t *) block;
    size_t i;

    if (!ods2_validate_home(h).ok) return false;
    if (h->homelbn != lbn || h->homevbn != vbn) return false;
    for (i = 0; i < 512; i++) {
        bool own_field = i < 4 || i == 16 || i == 17 || i == 58 || i == 59 || i >= 510;
        if (!own_field && block[i] != primary[i]) return false;
    }
    return true;
}

ods2_result_t ods2_set_volume_label(ods2_volume_t *vol, const char *label,
                                     unsigned *copies_out)
{
    /* The home block and its cluster fillers are INDEXF.SYS VBNs 2 to
       2v, the backup home block's cluster VBNs 2v+1 to 3v, where v is
       the cluster factor (spec 5.1.3, 5.1.4); VBN 1 is the boot
       block. Every one of them holds a copy of the home block. */
    char name[12];
    const char *why;
    uint8_t primary[512];
    uint8_t (*blocks)[512] = NULL;
    uint32_t *lbns = NULL;
    unsigned v = vol->home.cluster, count, i, primary_index = 0;
    bool have_primary = false, have_backup = false;
    ods2_result_t r = ok();

    if (copies_out) *copies_out = 0;
    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    why = ods2_make_volume_label(label, name);
    if (why != NULL) return fail(why);
    if (!name_is_blank(vol->home.strucname) || vol->home.rvn != 0 || vol->home.setcount != 0) {
        return fail("this volume is part of a volume set: relabelling one member "
                    "alone isn't supported - relabel the set on VMS");
    }
    if (v == 0) return fail("home block has a cluster factor of 0");

    memcpy(primary, &vol->home, 512);
    count = 3u * v - 1u; /* VBNs 2 .. 3v */
    blocks = malloc((size_t) count * 512u);
    lbns = malloc((size_t) count * sizeof(*lbns));
    if (blocks == NULL || lbns == NULL) {
        free(blocks);
        free(lbns);
        return fail("could not allocate memory for the home block copies");
    }

    /* Read and check every copy first; write nothing unless all of
       them are what they should be. */
    for (i = 0; i < count && r.ok; i++) {
        uint32_t vbn = i + 2, lbn;
        if (!vbn_to_lbn(vol->indexf_extents, vol->indexf_extent_count, vbn, &lbn)) {
            r = fail("INDEXF.SYS doesn't map all of the home block's clusters");
            break;
        }
        lbns[i] = lbn;
        if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
            fread(blocks[i], 1, 512, vol->fp) != 512) {
            r = fail("could not read a home block copy");
            break;
        }
        if (!is_home_copy(blocks[i], primary, lbn, vbn)) {
            static char msg[200];
            snprintf(msg, sizeof msg, "the block at LBN %u (INDEXF.SYS VBN %u) isn't a "
                     "valid copy of the home block - nothing changed; check the volume "
                     "with ANALYZE/DISK on VMS", (unsigned) lbn, (unsigned) vbn);
            r = fail(msg);
            break;
        }
        if (lbn == vol->home.homelbn) {
            have_primary = true;
            primary_index = i;
        }
        if (lbn == vol->home.alhomelbn) have_backup = true;
    }
    if (r.ok && !have_primary) {
        r = fail("the primary home block isn't among INDEXF.SYS's home block clusters");
    }
    if (r.ok && !have_backup) {
        r = fail("the backup home block isn't among INDEXF.SYS's home block clusters");
    }

    /* Relabel each copy, keeping its own HOMELBN/HOMEVBN, and write the
       primary last: until it is written VMS still sees one consistent
       (old) label, and running this again finishes the job. */
    if (r.ok) {
        for (i = 0; i < count; i++) {
            memcpy(((ods2_home_t *) blocks[i])->volname, name, 12);
            set_home_checksums(blocks[i]);
            if (!ods2_validate_home((const ods2_home_t *) blocks[i]).ok) {
                r = fail("internal error: a relabelled home block doesn't validate");
                break;
            }
        }
    }
    for (i = 0; r.ok && i <= count; i++) {
        unsigned k = (i < count) ? i : primary_index;
        if (i < count && i == primary_index) continue;
        r = ods2_write_block(vol, lbns[k], blocks[k]);
    }
    if (r.ok && fflush(vol->fp) != 0) r = fail("could not flush the home blocks");

    /* Read every copy back. */
    for (i = 0; r.ok && i < count; i++) {
        uint8_t back[512];
        if (fseek(vol->fp, (long) lbns[i] * 512, SEEK_SET) != 0 ||
            fread(back, 1, 512, vol->fp) != 512 || memcmp(back, blocks[i], 512) != 0) {
            r = fail("a home block copy didn't read back as written");
        }
    }
    if (r.ok) {
        memcpy(&vol->home, blocks[primary_index], 512);
        if (copies_out) *copies_out = count;
    }
    free(blocks);
    free(lbns);
    return r;
}

ods2_result_t ods2_volume_space(ods2_volume_t *vol, uint64_t *total_blocks_out,
                                 uint64_t *free_blocks_out)
{
    uint8_t bitmap_header[512], block[512];
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    uint32_t volsize = 0, vbn = 1, cluster = vol->home.cluster, bitmap_blocks = 0;
    uint64_t clusters = 0, free_clusters = 0, bit = 0;
    int i;
    ods2_result_t r;

    r = ods2_read_header(vol, 2, bitmap_header); /* BITMAP.SYS */
    if (!r.ok) return r;
    r = ods2_decode_all_extents(vol, bitmap_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    if (extent_count <= 0 || cluster == 0) return fail("could not decode BITMAP.SYS");
    for (i = 0; i < extent_count; i++) bitmap_blocks += extents[i].block_count;

    /* VBN 1 is the storage control block (spec 5.2.1): SCB$L_VOLSIZE
       at byte 4. The bits follow from VBN 2, one per cluster, set =
       free (spec 5.2.2). */
    for (i = 0; i < extent_count; i++) {
        uint32_t b;
        for (b = 0; b < extents[i].block_count; b++, vbn++) {
            uint32_t k;
            if (fseek(vol->fp, (long) (extents[i].lbn + b) * 512, SEEK_SET) != 0 ||
                fread(block, 1, 512, vol->fp) != 512) {
                return fail("could not read BITMAP.SYS");
            }
            if (vbn == 1) {
                volsize = (uint32_t) block[4] | ((uint32_t) block[5] << 8) |
                          ((uint32_t) block[6] << 16) | ((uint32_t) block[7] << 24);
                clusters = ((uint64_t) volsize + cluster - 1) / cluster;
                /* The bitmap (VBN 2 on) must have a bit for every
                   cluster; an SCB that says otherwise isn't believed. */
                if (volsize == 0 || bitmap_blocks < 2 ||
                    clusters > (uint64_t) (bitmap_blocks - 1) * 4096u) {
                    return fail("BITMAP.SYS's storage control block gives a volume "
                                "size its bitmap doesn't cover");
                }
                *total_blocks_out = volsize;
                continue;
            }
            for (k = 0; k < 4096 && bit < clusters; k++, bit++) {
                if ((block[k / 8] >> (k % 8)) & 1u) free_clusters++;
            }
        }
    }
    if (vbn == 1) return fail("BITMAP.SYS is empty");
    *free_blocks_out = free_clusters * cluster;
    return ok();
}

/* SET FILE/ATTRIBUTES=(MRS:0): repairs a stream file made with a maximum record size (ods2v2 gave every stream file MRS = LRL =
   512 whatever its lines, and RMS cannot read such a file with a longer line in record mode: %RMS-W-RTB), or with an LRL below
   its longest record. MRS becomes 0 and LRL the file's own longest record (at most 32767), as VMS makes such files; the file's
   data is not touched. Only a sound header of a sequential Stream, Stream_LF or Stream_CR file is changed, and only if it is
   not already as VMS leaves such a file (MRS 0 and an LRL of at least the longest record, taken as at most 32767: a binary file
   with a longer "record" is left that way by VMS and is skipped silently). *changed is 1 if the header was written. */
ods2_result_t ods2_set_stream_record_sizes(ods2_volume_t *vol, ods2_fid_t fid, unsigned *changed)
{
    uint8_t h[512], blk[512];
    ods2_head_core_t *core = (ods2_head_core_t *) h;
    uint64_t bytes, done = 0;
    unsigned vbn, rfm;
    size_t longest;
    ods2_reclen_t st;
    uint16_t sum;
    ods2_result_t r;

    *changed = 0;
    if (!vol->writable) return fail("volume was not mounted for write (use ods2_mount_write)");
    r = ods2_read_header_fid(vol, fid, h);
    if (!r.ok) return r;
    r = header_ok_for_write(h, fid.fid_num);
    if (!r.ok) return r;
    rfm = core->recattr.rtype & 0x0Fu;
    if ((core->recattr.rtype >> 4) != 0 || (rfm != ODS2_RFM_STM && rfm != ODS2_RFM_STMLF && rfm != ODS2_RFM_STMCR) ||
        (core->filechar & 0x2000u)) {
        return ok();
    }
    bytes = ((uint64_t) ods2_word_swap32(core->recattr.efblk) - 1) * 512u + core->recattr.ffbyte;
    if (ods2_word_swap32(core->recattr.efblk) == 0) bytes = 0;
    ods2_reclen_init(&st, (uint8_t) rfm);
    for (vbn = 1; done < bytes; vbn++) {
        size_t n = (bytes - done) < 512u ? (size_t) (bytes - done) : 512u;
        r = ods2_read_file_block(vol, h, vbn, blk);
        if (!r.ok) return r;
        ods2_reclen_feed(&st, blk, n);
        done += n;
    }
    longest = ods2_reclen_longest(&st);
    if (longest > 32767) longest = 32767;        /* no record length can be more: VMS's state for such a binary file */
    if (core->recattr.maxrec == 0 && core->recattr.rsize >= longest) return ok();
    core->recattr.maxrec = 0;
    core->recattr.rsize = (uint16_t) longest;
    sum = ods2_checksum(h, 255);
    h[510] = (uint8_t) (sum & 0xff);
    h[511] = (uint8_t) (sum >> 8);
    r = ods2_write_header(vol, fid.fid_num, h);
    if (r.ok) *changed = 1;
    return r;
}
