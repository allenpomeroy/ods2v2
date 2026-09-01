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

#include "ods2_volume.h"
#include "ods2_validate.h"
#include "ods2_bitmap.h"
#include "ods2_header_build.h"
#include "ods2_directory_write.h"
#include "ods2_checksum.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>

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

ods2_result_t ods2_write_block(ods2_volume_t *vol, uint32_t lbn, const uint8_t *block)
{
    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
        fwrite(block, 1, 512, vol->fp) != 512) {
        return fail("could not write block");
    }
    if (fflush(vol->fp) != 0) {
        return fail("could not flush written block");
    }
    return ok();
}

ods2_result_t ods2_read_block(ods2_volume_t *vol, uint32_t lbn, uint8_t *block_out)
{
    if (fseek(vol->fp, (long) lbn * 512, SEEK_SET) != 0 ||
        fread(block_out, 1, 512, vol->fp) != 512) {
        return fail("could not read block");
    }
    return ok();
}

void ods2_dismount(ods2_volume_t *vol)
{
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

ods2_result_t ods2_allocate_blocks(ods2_volume_t *vol, unsigned blocks_needed,
                                    uint32_t *lbn_out, unsigned *blocks_allocated_out)
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

    if (!ods2_bitmap_find_free(bitmap_data, total_bits, clusters_needed,
                                &start_cluster, &found_clusters)) {
        /* No single run large enough for the WHOLE request - fall
           back to the largest contiguous run actually available, so
           a big allocation can still proceed via more (but still as
           few as possible) extents instead of failing outright. This
           matters a lot more now that ods2_create_file() requests
           large chunks up front (relying on Format 2/3 retrieval
           pointers to encode them compactly) rather than deliberately
           small ones - a genuinely fragmented volume still needs to
           gracefully fall back to many smaller extents. Only a
           genuinely full volume (no free cluster anywhere) still
           fails here. */
        if (!ods2_bitmap_find_largest_free(bitmap_data, total_bits, clusters_needed,
                                            &start_cluster, &found_clusters)) {
            free(bitmap_data);
            return fail("no free space found on volume for this allocation");
        }
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

/* Decodes ALL of a file's extents, walking the ext_fid chain across
   extension headers if the file needs more than one (spec: a header
   whose Map Area can't hold all its retrieval pointers points to a
   continuation header via FH2$W_EXT_FID; that header's own ext_fid
   may point further still). Without this, any file needing more than
   one header would silently appear truncated. A generous but finite
   segment limit (ODS2_MAX_HEADER_SEGMENTS, ods2_volume.h - shared with
   the write side so nothing either side produces exceeds what the
   other can follow) guards against a corrupted or circular ext_fid
   chain looping forever. */
ods2_result_t ods2_decode_all_extents(ods2_volume_t *vol, const uint8_t *header,
                                       ods2_extent_t *extents_out, size_t max_extents,
                                       int *count_out)
{
    uint8_t current[512];
    int total = 0;
    int segment;

    memcpy(current, header, 512);

    for (segment = 0; segment < ODS2_MAX_HEADER_SEGMENTS; segment++) {
        const ods2_head_core_t *core = (const ods2_head_core_t *) current;
        int n = decode_header_extents(current,
                                       extents_out + total,
                                       (max_extents > (size_t) total)
                                           ? max_extents - (size_t) total : 0);
        if (n < 0) {
            return fail("could not decode retrieval pointers for a header segment");
        }
        total += n;

        if (core->ext_fid.fid_num == 0) {
            /* No further extension header - done. */
            *count_out = total;
            return ok();
        }

        {
            ods2_result_t r = ods2_read_header(vol, core->ext_fid.fid_num, current);
            if (!r.ok) return r;
        }
    }

    return fail("extension header chain exceeded maximum segment limit "
                "(possibly corrupted or circular ext_fid chain)");
}

ods2_result_t ods2_list_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                   ods2_dir_entry_t *entries_out, size_t max_entries,
                                   int *count_out)
{
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    unsigned total_blocks = 0, block_index;
    int total_found = 0;
    int i;
    ods2_result_t r = ods2_decode_all_extents(vol, dir_header, extents, ODS2_MAX_EXTENTS, &extent_count);

    if (!r.ok) return r;
    if (extent_count <= 0) {
        return fail("could not decode directory's retrieval pointers");
    }
    for (i = 0; i < extent_count; i++) total_blocks += extents[i].block_count;

    for (block_index = 1; block_index <= total_blocks; block_index++) {
        uint8_t block[512];
        int n;
        r = ods2_read_file_block(vol, dir_header, block_index, block);
        if (!r.ok) return r;

        n = ods2_parse_directory(block, sizeof(block),
                                  entries_out + total_found,
                                  (max_entries > (size_t) total_found)
                                      ? max_entries - (size_t) total_found : 0);
        if (n < 0) {
            /* Malformed block content is a real problem, but an
               entirely EMPTY block (all zero, size word 0x0000) just
               means "no more entries in this particular block" for a
               multi-block directory - not every block need be full. */
            continue;
        }
        total_found += n;
    }

    *count_out = total_found;
    return ok();
}

size_t ods2_file_content_length(const uint8_t *header)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    uint32_t efblk = ods2_word_swap32(core->recattr.efblk);
    uint16_t ffbyte = core->recattr.ffbyte;

    if (efblk == 0) return 0; /* empty file */
    /* Same rule ods2_read_file() applies to its own last block below:
       ffbyte==0 means the whole last block is valid data. */
    return (size_t) (efblk - 1) * 512 + ((ffbyte == 0) ? 512 : ffbyte);
}

ods2_result_t ods2_read_file(ods2_volume_t *vol, const uint8_t *header,
                              uint8_t *buf_out, size_t buf_size, size_t *bytes_read_out)
{
    const ods2_head_core_t *core = (const ods2_head_core_t *) header;
    uint32_t efblk = ods2_word_swap32(core->recattr.efblk);
    uint16_t ffbyte = core->recattr.ffbyte;
    size_t written = 0;
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    int ext_i;
    unsigned vbn_done = 0;
    ods2_result_t r;

    if (efblk == 0) {
        /* Empty file. */
        *bytes_read_out = 0;
        return ok();
    }

    /* Decode the whole extent chain (walking any extension headers,
       spec 3.3) exactly once, then read directly against it - rather
       than calling ods2_read_file_block() per VBN, which re-decodes
       (and, once extension headers are involved, re-reads every one
       of them from disk) on every single call. For a file with
       thousands of extents across dozens of extension headers (a
       several-hundred-MB CD image is a realistic case now that
       multi-header files are possible), that per-VBN approach would
       mean millions of redundant disk reads. */
    r = ods2_decode_all_extents(vol, header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;

    for (ext_i = 0; ext_i < extent_count && vbn_done < efblk; ext_i++) {
        unsigned b;
        for (b = 0; b < extents[ext_i].block_count && vbn_done < efblk; b++) {
            uint8_t block[512];
            size_t this_block_bytes;
            unsigned vbn = vbn_done + 1; /* 1-based, matches the loop this replaces */

            r = ods2_read_block(vol, extents[ext_i].lbn + b, block);
            if (!r.ok) return r;

            if (vbn < efblk) {
                this_block_bytes = 512;
            } else {
                /* Last block: ffbyte==0 means the whole block is valid
                   data (e.g. fixed-512-byte-record files, confirmed
                   against INDEXF.SYS's own real header), otherwise only
                   the first ffbyte bytes are. */
                this_block_bytes = (ffbyte == 0) ? 512 : ffbyte;
            }

            if (written + this_block_bytes > buf_size) {
                return fail("output buffer too small for file content");
            }
            memcpy(buf_out + written, block, this_block_bytes);
            written += this_block_bytes;
            vbn_done++;
        }
    }

    if (vbn_done < efblk) {
        return fail("file's extents don't cover its own stated EFBLK "
                     "(truncated or corrupted retrieval pointers)");
    }

    *bytes_read_out = written;
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

ods2_result_t ods2_find_free_file_number(ods2_volume_t *vol, unsigned start_from,
                                          unsigned max_search, unsigned *file_number_out,
                                          uint16_t *seq_out)
{
    /* Spec 5.1.7: a "garbage" (never-used) slot gets seq=1; a
       slot that held a deleted file gets seq = (old seq) + 1. Both
       kinds have fid_num==0 (Spec 3.5.1's deleted-header rules
       explicitly zero it), so fid_num alone can't distinguish
       them - FH2$M_MARKDEL (0x8000 in filechar) is the reliable
       signal a slot was genuinely deleted rather than never used;
       a fresh, all-zero slot has filechar==0 entirely. This was a
       documented known limitation until delete support existed to
       actually produce real deleted slots to get this right. */
    unsigned n;
    for (n = start_from; n < start_from + max_search; n++) {
        uint8_t header[512];
        const ods2_head_core_t *core;
        ods2_result_t r = ods2_read_header(vol, n, header);
        if (!r.ok) {
            /* Ran off the end of what INDEXF.SYS's currently-known
               extents cover - not necessarily an error, just means
               we'd need to extend the index file itself to allocate
               here, which this function does not attempt. */
            return fail("search exhausted known INDEXF.SYS extents "
                        "without finding a free header slot");
        }
        core = (const ods2_head_core_t *) header;
        if (core->fid.fid_num == 0) {
            *file_number_out = n;
            if (core->filechar & 0x8000u) { /* FH2$M_MARKDEL - a real, deleted slot */
                *seq_out = (uint16_t) (core->fid.fid_seq + 1);
            } else { /* genuinely never used */
                *seq_out = 1;
            }
            return ok();
        }
    }
    return fail("no free header slot found within max_search range");
}

ods2_result_t ods2_lookup_name(ods2_volume_t *vol, const uint8_t *dir_header,
                                const char *name, ods2_fid_t *fid_out)
{
    ods2_dir_entry_t entries[256];
    int count = 0;
    ods2_result_t r = ods2_list_directory(vol, dir_header, entries, 256, &count);
    int i;

    if (!r.ok) return r;

    for (i = 0; i < count; i++) {
        /* Case-insensitive compare - VMS names are conventionally
           uppercase on disk, but callers may pass either case. */
        const char *a = entries[i].name;
        const char *b = name;
        bool match = true;
        while (*a && *b) {
            if (toupper((unsigned char) *a) != toupper((unsigned char) *b)) {
                match = false;
                break;
            }
            a++; b++;
        }
        if (match && *a == '\0' && *b == '\0') {
            *fid_out = entries[i].fid;
            return ok();
        }
    }
    return fail("name not found in directory");
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

        r = ods2_read_header(vol, current.fid_num, header);
        if (!r.ok) return r;

        strcat(component, ".DIR");
        r = ods2_lookup_name(vol, header, component, &current);
        if (!r.ok) return r;
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
    ods2_result_t r;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    if (contains_wildcard(name)) {
        return fail("directory name cannot contain wildcard characters ('*' or '%')");
    }
    if (strlen(name) + 4 >= sizeof(dirname)) {
        return fail("directory name too long");
    }
    strcpy(dirname, name);
    uppercase_str(dirname); /* real VMS always stores names uppercase */
    strcat(dirname, ".DIR");

    /* Refuse to create a name that already exists matching VMS
       behavior (CREATE/DIRECTORY on an existing name fails) and
       avoids silently shadowing an existing directory. */
    r = ods2_lookup_name(vol, parent_header, dirname, &existing);
    if (r.ok) {
        return fail("a file or directory with this name already exists in the parent");
    }

    r = ods2_find_free_file_number(vol, 1, 4096, &file_number, &seq_num);
    if (!r.ok) return r;

    /* Mark this file number as allocated in the index file's own
       bitmap. */
    r = mark_index_bitmap(vol, file_number, true);
    if (!r.ok) return r;

    /* WARNING Allocate one cluster's worth of content space - small and
       simple for this first implementation; a real, populated directory
       would need extension support (allocating and linking a second
       block/extent) once it outgrows this, which is not yet
       implemented. */
    r = ods2_allocate_blocks(vol, vol->home.cluster, &content_lbn, &content_blocks);
    if (!r.ok) return r;

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
    spec.efblk = 1;   /* just the sentinel block so far */
    spec.ffbyte = 2;  /* sentinel is 2 bytes */

    {
        char ident[260]; /* dirname is already bounded (<256-4 chars,
                             checked above) plus ";1" and a nul - this
                             is always large enough, sized generously
                             to also silence a spurious compiler
                             truncation warning */
        snprintf(ident, sizeof(ident), "%s;1", dirname);
        spec.ident_name = ident;

        if (!ods2_build_file_header(new_header, &spec)) {
            return fail("could not construct new directory's header");
        }
    }

    /* Initialize the new directory's first content block: entirely
       empty except for the terminating sentinel. */
    memset(content_block, 0, sizeof(content_block));
    content_block[0] = 0xff;
    content_block[1] = 0xff;

    r = ods2_write_header(vol, file_number, new_header);
    if (!r.ok) return r;

    r = ods2_write_file_block(vol, new_header, 1, content_block);
    if (!r.ok) return r;

    /* Insert an entry for the new directory into the parent -
       growing the parent's own allocation automatically if none of
       its existing content blocks have room. */
    r = ods2_insert_into_directory(vol, parent_core->fid.fid_num, dirname, 1, spec.fid);
    if (!r.ok) return r;

    *new_fid_out = spec.fid;
    return ok();
}

ods2_result_t ods2_create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                const char *name, const uint8_t *content, size_t content_len,
                                uint8_t rtype, ods2_fid_t *new_fid_out)
{
    const ods2_head_core_t *parent_core = (const ods2_head_core_t *) parent_header;
    ods2_fid_t existing;
    unsigned file_number;
    uint16_t seq_num;
    unsigned blocks_needed;
    unsigned blocks_remaining;
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count = 0;
    unsigned total_allocated = 0;
    ods2_header_spec_t spec;
    ods2_fid_t primary_fid;
    ods2_result_t r;
    char upper_name[256];

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    if (contains_wildcard(name)) {
        return fail("file name cannot contain wildcard characters ('*' or '%')");
    }
    if (strlen(name) >= sizeof(upper_name)) {
        return fail("file name too long");
    }
    strcpy(upper_name, name);
    uppercase_str(upper_name); /* real VMS always stores names uppercase */
    name = upper_name; /* every use below now sees the uppercased name */

    r = ods2_lookup_name(vol, parent_header, name, &existing);
    if (r.ok) {
        return fail("a file or directory with this name already exists in the parent");
    }

    blocks_needed = (unsigned) ((content_len + 511) / 512);
    if (blocks_needed == 0) blocks_needed = 1; /* a zero-length file still needs one block for EFBLK=1... */

    /* Unlike the original Format-1-only version of this code, we no
       longer force every allocation into small, uniformly-sized
       chunks just to guarantee Format 1 encoding - the allocation
       loop below requests the full remaining amount every time and
       lets ods2_encode_retrieval_pointer() pick whichever retrieval
       pointer format (1, 2, or 3 - spec 3.5.4) actually fits each
       extent ods2_allocate_blocks() hands back. On a volume with
       enough contiguous free space, that means a large file can end
       up as a single extent instead of hundreds of small ones -
       exactly the scenario chained extension headers alone couldn't
       fix (see ods2_header_build.h), since Format 1 was always capped
       at 256 blocks per extent no matter how contiguous the free
       space actually was.
       Because the number of extents a given file will actually need
       now depends on how fragmented the volume's free space happens
       to be - not a fixed, predictable chunk size - there's no cheap
       way to reliably predict it up front the way the old safe_chunk
       arithmetic could. The allocation loop's own bounds
       (ODS2_MAX_EXTENTS below, and ODS2_MAX_HEADER_SEGMENTS in the
       header-splitting step further down) are what actually enforce
       the limit; a file that's too fragmented to fit even the maximum
       chain length fails there, with a clear message, rather than
       being rejected on a guess before allocation is even attempted. */

    r = ods2_find_free_file_number(vol, 1, 4096, &file_number, &seq_num);
    if (!r.ok) return r;

    /* Mark this file number as allocated in the index file's own
       bitmap - a gap in earlier code (only header content was ever
       validated when searching for a free slot, never written to
       this bitmap when claiming one). Found while investigating a
       real ANALYZE/DISK BAD_DIRHEADER finding. */
    r = mark_index_bitmap(vol, file_number, true);
    if (!r.ok) return r;

    blocks_remaining = blocks_needed;
    while (blocks_remaining > 0) {
        uint32_t chunk_lbn;
        unsigned chunk_allocated;

        if (extent_count >= ODS2_MAX_EXTENTS) {
            return fail("content too fragmented to fit even across the maximum "
                        "chain of extension headers (ODS2_MAX_HEADER_SEGMENTS)");
        }

        r = ods2_allocate_blocks(vol, blocks_remaining, &chunk_lbn, &chunk_allocated);
        if (!r.ok) return r;

        extents[extent_count].lbn = chunk_lbn;
        extents[extent_count].block_count = chunk_allocated;
        extent_count++;
        total_allocated += chunk_allocated;

        blocks_remaining = (chunk_allocated >= blocks_remaining) ? 0
                                                                    : blocks_remaining - chunk_allocated;
    }

    /* Determine each extent's actual encoded size - 4, 6, or 8 bytes,
       depending on which retrieval pointer format it needs (spec
       3.5.4; see ods2_encode_retrieval_pointer()) - then split the
       extents across as many header segments as needed by CUMULATIVE
       BYTES, not a fixed per-header extent COUNT. That distinction
       matters now: with Format 1 alone every extent was always
       exactly 4 bytes, so "extents per header" was a fixed number
       (ODS2_HEADER_MAX_EXTENTS/ODS2_EXT_HEADER_MAX_EXTENTS); with
       Format 2/3 in the mix, different extents in the same file can
       have different encoded sizes, so how many fit in a given
       header's Map Area depends on which ones they actually are. The
       primary header has 310 bytes of Map Area (510-200); each
       extension header has 430 (510-80, truncated Ident Area - spec
       3.5.3). */
    {
        size_t extent_bytes[ODS2_MAX_EXTENTS];
        int i2;
        int segment_count = 0;
        ods2_fid_t segment_fid[ODS2_MAX_HEADER_SEGMENTS];
        unsigned segment_file_number[ODS2_MAX_HEADER_SEGMENTS];
        int segment_extent_start[ODS2_MAX_HEADER_SEGMENTS];
        int segment_extent_count[ODS2_MAX_HEADER_SEGMENTS];
        int seg;
        int next_extent = 0;

        for (i2 = 0; i2 < extent_count; i2++) {
            uint8_t scratch[8];
            size_t bytes_written;
            if (!ods2_encode_retrieval_pointer(scratch, extents[i2].lbn,
                                                extents[i2].block_count, &bytes_written)) {
                return fail("internal error: an allocated extent doesn't fit "
                            "even a Format 3 retrieval pointer (should be unreachable)");
            }
            extent_bytes[i2] = bytes_written;
        }

        while (next_extent < extent_count) {
            size_t capacity = (segment_count == 0)
                                   ? (510 - ODS2_HEADER_MPOFFSET_BYTES)
                                   : (510 - ODS2_EXT_HEADER_MPOFFSET_BYTES);
            size_t used = 0;
            int start = next_extent;
            int count = 0;

            if (segment_count >= ODS2_MAX_HEADER_SEGMENTS) {
                return fail("content too fragmented to fit even across the "
                            "maximum chain of extension headers");
            }

            while (next_extent < extent_count && used + extent_bytes[next_extent] <= capacity) {
                used += extent_bytes[next_extent];
                next_extent++;
                count++;
            }
            if (count == 0) {
                /* A single extent's own encoding is bigger than an
                   entire header's Map Area could ever hold -
                   genuinely unreachable given Format 3 tops out at 8
                   bytes and even the smaller (extension) header's
                   capacity is 430 bytes, but guarded rather than
                   looping forever. */
                return fail("internal error: a single extent's retrieval "
                            "pointer is larger than an entire header's map "
                            "area (should be unreachable)");
            }

            if (segment_count == 0) {
                segment_file_number[0] = file_number;
                segment_fid[0].fid_num = (uint16_t) file_number;
                segment_fid[0].fid_seq = seq_num;
                segment_fid[0].fid_rvn = 0;
                segment_fid[0].fid_nmx = 0;
            } else {
                unsigned ext_file_number;
                uint16_t ext_seq_num;

                /* Search from just above the previously allocated
                   slot: a slot claimed above is only reflected in the
                   index bitmap so far (its on-disk header content is
                   still written later, below), so a plain
                   from-file-number-1 search would keep finding that
                   same slot again. */
                r = ods2_find_free_file_number(
                    vol, segment_file_number[segment_count - 1] + 1, 4096,
                    &ext_file_number, &ext_seq_num);
                if (!r.ok) return r;
                r = mark_index_bitmap(vol, ext_file_number, true);
                if (!r.ok) return r;

                segment_file_number[segment_count] = ext_file_number;
                segment_fid[segment_count].fid_num = (uint16_t) ext_file_number;
                segment_fid[segment_count].fid_seq = ext_seq_num;
                segment_fid[segment_count].fid_rvn = 0;
                segment_fid[segment_count].fid_nmx = 0;
            }

            segment_extent_start[segment_count] = start;
            segment_extent_count[segment_count] = count;
            segment_count++;
        }

        /* Build and write every segment now that every segment's FID
           is known (each needs the NEXT segment's FID for its own
           ext_fid, so this couldn't be done in the loop above). */
        for (seg = 0; seg < segment_count; seg++) {
            uint8_t seg_header[512];
            bool is_extension = (seg != 0);

            memset(&spec, 0, sizeof(spec));
            spec.fid = segment_fid[seg];
            /* Spec 3.5.2.16: an extension header's backlink is the
               file's PRIMARY header's FID, not the parent directory's -
               only segment 0 points at the parent. */
            spec.backlink = is_extension ? segment_fid[0] : parent_core->fid;
            spec.ext_fid = (seg + 1 < segment_count) ? segment_fid[seg + 1] : (ods2_fid_t) { 0 };
            spec.seg_num = (uint16_t) seg;
            spec.is_extension = is_extension;
            /* FH2$M_CONTIG only makes sense on the primary header,
               and only when the whole file is truly one piece. */
            spec.filechar = (!is_extension && extent_count == 1) ? 0x0080 : 0;
            spec.rtype = rtype;
            spec.rattrib = 0;
            spec.rsize = 512;
            spec.maxrec = 512;
            spec.extents = extents + segment_extent_start[seg];
            spec.extent_count = segment_extent_count[seg];
            /* HIBLK/EFBLK/FFBYTE describe the file as a whole and are
               only meaningful read from the primary header - real VMS
               headers examined earlier in this project leave them
               populated on extension headers too, so mirror that
               rather than leaving them zero. */
            spec.hiblk = total_allocated;
            spec.efblk = blocks_needed;
            spec.ffbyte = (uint16_t) (content_len % 512); /* 0 = "whole last block used" */

            if (!is_extension) {
                char ident[260]; /* name is already bounded (<256 chars,
                                     checked above) plus ";1" and a nul -
                                     always large enough, sized generously
                                     to also silence a spurious compiler
                                     truncation warning */
                snprintf(ident, sizeof(ident), "%s;1", name);
                spec.ident_name = ident;

                if (!ods2_build_file_header(seg_header, &spec)) {
                    return fail("could not construct new file's primary header");
                }
            } else {
                if (!ods2_build_file_header(seg_header, &spec)) {
                    return fail("could not construct new file's extension header");
                }
            }

            r = ods2_write_header(vol, segment_file_number[seg], seg_header);
            if (!r.ok) return r;
        }

        primary_fid = segment_fid[0];
    }

    /* Write content directly against the extents we already
       allocated, rather than going through ods2_write_file_block()
       per VBN - that call re-decodes the header (and, once a file
       spans extension headers, re-reads every one of them from disk)
       on every single invocation. For a file with thousands of
       extents across dozens of extension headers (a several-hundred-
       MB CD image is a realistic case now that multi-header files are
       possible), that would mean millions of redundant disk reads.
       We already know exactly which LBN each VBN belongs to from the
       allocation loop above, so write straight to it. */
    {
        /* Counts VBNs written, NOT bytes - a zero-length file still
           has blocks_needed==1 (forced above) and must get that one
           all-zero block written, matching the original per-VBN loop
           this replaces (which ran unconditionally for vbn in
           1..blocks_needed regardless of content_len). Gating on
           content_len instead would silently skip that write and
           leave a freshly-allocated, not-yet-zeroed block's stale
           on-disk bytes as the "content" of an empty file. */
        unsigned vbn_written = 0;
        int ext_i;
        for (ext_i = 0; ext_i < extent_count && vbn_written < blocks_needed; ext_i++) {
            unsigned b;
            for (b = 0; b < extents[ext_i].block_count && vbn_written < blocks_needed; b++) {
                uint8_t block[512];
                size_t offset = (size_t) vbn_written * 512;
                size_t remaining = content_len - offset;
                size_t this_len = (remaining > 512) ? 512 : remaining;

                memset(block, 0, sizeof(block));
                if (this_len > 0 && this_len <= remaining) {
                    memcpy(block, content + offset, this_len);
                }
                r = ods2_write_block(vol, extents[ext_i].lbn + b, block);
                if (!r.ok) return r;
                vbn_written++;
            }
        }
    }

    /* Insert an entry for the new file into the parent - growing the
       parent's own allocation automatically if none of its existing
       content blocks have room. Always the PRIMARY header's FID - a
       directory entry never points at an extension header (spec
       3.5.2.8's ext_fid chain, followed from the primary header, is
       how those get reached instead). */
    r = ods2_insert_into_directory(vol, parent_core->fid.fid_num, name, 1, primary_fid);
    if (!r.ok) return r;

    *new_fid_out = primary_fid;
    return ok();
}

/* Appends one already-allocated extent (an LBN + block count, as
 * returned by ods2_allocate_blocks) to the tail of an existing file's
 * retrieval-pointer chain: into the primary header if it still has
 * room, into an existing extension header if the primary is full but
 * an extension already exists with room, or by chaining on a
 * brand-new extension header (spec 3.3) if even the current tail
 * header is full. Does NOT touch HIBLK/EFBLK/FFBYTE or the revision
 * date/count - those live only on the PRIMARY header and are always
 * the caller's responsibility, since callers vary in what else they
 * need to update alongside the append (see ods2_insert_into_directory
 * below, the only current caller). `primary_fid` is the file's own
 * (primary header's) FID, needed for FH2$W_BACKLINK if a new
 * extension header ends up being created (spec 3.5.2.16: an
 * extension header's backlink is the primary header's FID). */
static ods2_result_t ods2_append_extent_to_chain(ods2_volume_t *vol,
                                                  unsigned primary_file_number,
                                                  ods2_fid_t primary_fid,
                                                  uint32_t new_lbn,
                                                  unsigned new_block_count)
{
    uint8_t seg_header[512];
    unsigned seg_file_number = primary_file_number;
    int segment_index = 0;
    ods2_result_t r;

    r = ods2_read_header(vol, seg_file_number, seg_header);
    if (!r.ok) return r;

    for (;;) {
        ods2_head_core_t *core = (ods2_head_core_t *) seg_header;
        size_t map_area_offset = (size_t) core->mpoffset * 2;
        size_t current_map_bytes = (size_t) core->map_inuse * 2;
        uint8_t encoded[8];
        size_t bytes_written;

        if (!ods2_encode_retrieval_pointer(encoded, new_lbn, new_block_count, &bytes_written)) {
            return fail("could not encode the new extent (allocation too "
                        "large even for a Format 3 pointer)");
        }

        if (map_area_offset + current_map_bytes + bytes_written <= 510) {
            /* This segment (primary or extension, doesn't matter -
               the arithmetic is driven entirely by its own mpoffset/
               map_inuse) has room right here. */
            uint16_t checksum;
            memcpy(seg_header + map_area_offset + current_map_bytes, encoded, bytes_written);
            core->map_inuse = (uint8_t) ((current_map_bytes + bytes_written) / 2);
            checksum = ods2_checksum(seg_header, 255);
            seg_header[510] = (uint8_t) (checksum & 0xff);
            seg_header[511] = (uint8_t) (checksum >> 8);
            return ods2_write_header(vol, seg_file_number, seg_header);
        }

        if (core->ext_fid.fid_num != 0) {
            /* This segment is full, but already has a further
               extension header - follow the chain and try there. */
            seg_file_number = core->ext_fid.fid_num;
            segment_index++;
            if (segment_index >= ODS2_MAX_HEADER_SEGMENTS) {
                return fail("extension header chain exceeded maximum segment "
                            "limit (possibly corrupted or circular ext_fid chain)");
            }
            r = ods2_read_header(vol, seg_file_number, seg_header);
            if (!r.ok) return r;
            continue;
        }

        /* The tail of the chain is full and has no further extension
           header - allocate a brand-new one and link it in (spec
           3.3: extension headers are linked in ascending order via
           FH2$W_EXT_FID). */
        {
            unsigned new_ext_file_number;
            uint16_t new_ext_seq;
            ods2_header_spec_t spec;
            uint8_t new_ext_header[512];
            ods2_extent_t one_extent;
            uint16_t checksum;

            if (segment_index + 1 >= ODS2_MAX_HEADER_SEGMENTS) {
                return fail("file already has the maximum number of chained "
                            "extension headers - cannot add another extent");
            }

            r = ods2_find_free_file_number(vol, primary_file_number + 1, 4096,
                                            &new_ext_file_number, &new_ext_seq);
            if (!r.ok) return r;
            r = mark_index_bitmap(vol, new_ext_file_number, true);
            if (!r.ok) return r;

            one_extent.lbn = new_lbn;
            one_extent.block_count = new_block_count;

            memset(&spec, 0, sizeof(spec));
            spec.fid.fid_num = (uint16_t) new_ext_file_number;
            spec.fid.fid_seq = new_ext_seq;
            spec.backlink = primary_fid;
            spec.seg_num = (uint16_t) (segment_index + 1);
            spec.is_extension = true;
            spec.rtype = core->recattr.rtype;
            spec.rattrib = core->recattr.rattrib;
            spec.rsize = core->recattr.rsize;
            spec.maxrec = core->recattr.maxrec;
            spec.extents = &one_extent;
            spec.extent_count = 1;
            /* hiblk/efblk/ffbyte/ident are only meaningful read from
               the primary header - left zero/NULL here. */

            if (!ods2_build_file_header(new_ext_header, &spec)) {
                return fail("could not construct new extension header");
            }
            r = ods2_write_header(vol, new_ext_file_number, new_ext_header);
            if (!r.ok) return r;

            /* Link the (now former) tail header to the new one. */
            core->ext_fid = spec.fid;
            checksum = ods2_checksum(seg_header, 255);
            seg_header[510] = (uint8_t) (checksum & 0xff);
            seg_header[511] = (uint8_t) (checksum >> 8);
            return ods2_write_header(vol, seg_file_number, seg_header);
        }
    }
}

ods2_result_t ods2_insert_into_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                          const char *name, uint16_t version, ods2_fid_t fid)
{
    uint8_t dir_header[512];
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    unsigned total_blocks = 0, vbn;
    int i;
    ods2_result_t r;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }

    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;

    r = ods2_decode_all_extents(vol, dir_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    if (extent_count <= 0) {
        return fail("could not decode directory's retrieval pointers");
    }
    for (i = 0; i < extent_count; i++) total_blocks += extents[i].block_count;

    /* First, try each existing content block. */
    for (vbn = 1; vbn <= total_blocks; vbn++) {
        uint8_t block[512];
        r = ods2_read_file_block(vol, dir_header, vbn, block);
        if (!r.ok) return r;

        if (ods2_insert_dir_entry(block, sizeof(block), name, version, fid)) {
            return ods2_write_file_block(vol, dir_header, vbn, block);
        }
    }

    /* No existing block had room - extend the directory: allocate a
       new block, append it as an additional extent to the directory's
       own header chain (growing into/creating an extension header if
       the primary header's own map area is already full - spec 3.3),
       then insert into the fresh block. The primary header itself is
       modified in place, not rebuilt from scratch, so the original
       creation date is preserved - only the revision date/count
       change, matching what a real revision to an existing file
       should do. */
    {
        ods2_head_core_t *core = (ods2_head_core_t *) dir_header;
        uint32_t new_lbn;
        unsigned new_blocks_allocated;
        uint16_t checksum;
        uint8_t new_block[512];
        uint32_t new_hiblk, new_efblk;
        uint64_t vms_now;
        ods2_fid_t primary_fid = core->fid;

        r = ods2_allocate_blocks(vol, vol->home.cluster, &new_lbn, &new_blocks_allocated);
        if (!r.ok) return r;

        r = ods2_append_extent_to_chain(vol, dir_file_number, primary_fid,
                                         new_lbn, new_blocks_allocated);
        if (!r.ok) return r;

        /* HIBLK/EFBLK/FFBYTE/revision only ever live on the PRIMARY
           header - re-read it fresh, since the append above may have
           just modified it on disk directly (grown its own map data
           in place, or linked in a brand-new extension header). */
        r = ods2_read_header(vol, dir_file_number, dir_header);
        if (!r.ok) return r;
        core = (ods2_head_core_t *) dir_header;

        new_hiblk = ods2_word_swap32(core->recattr.hiblk) + new_blocks_allocated;
        core->recattr.hiblk = ods2_word_swap32(new_hiblk);
        new_efblk = total_blocks + 1; /* the new block, 1-based VBN */
        core->recattr.efblk = ods2_word_swap32(new_efblk);
        core->recattr.ffbyte = 2; /* sentinel is 2 bytes */

        /* Update FI2$W_REVISION/FI2$Q_REVDATE in place - this file
           genuinely has been revised, but FI2$Q_CREDATE (untouched,
           earlier in the Ident Area) must stay exactly as it was. */
        {
            uint8_t *ident = dir_header + 80; /* fixed idoffset, confirmed earlier */
            uint16_t revision = (uint16_t) ident[20] | ((uint16_t) ident[21] << 8);
            revision++;
            ident[20] = (uint8_t) (revision & 0xff);
            ident[21] = (uint8_t) (revision >> 8);

            vms_now = ((uint64_t) time(NULL) + 3506716800ULL) * 10000000ULL;
            for (i = 0; i < 8; i++) {
                ident[30 + i] = (uint8_t) ((vms_now >> (i * 8)) & 0xff); /* REVDATE */
            }
        }

        checksum = ods2_checksum(dir_header, 255);
        dir_header[510] = (uint8_t) (checksum & 0xff);
        dir_header[511] = (uint8_t) (checksum >> 8);

        r = ods2_write_header(vol, dir_file_number, dir_header);
        if (!r.ok) return r;

        memset(new_block, 0, sizeof(new_block));
        new_block[0] = 0xff;
        new_block[1] = 0xff;
        if (!ods2_insert_dir_entry(new_block, sizeof(new_block), name, version, fid)) {
            return fail("internal error: fresh block had no room for one entry "
                        "(should be unreachable)");
        }

        return ods2_write_file_block(vol, dir_header, total_blocks + 1, new_block);
    }
}

ods2_result_t ods2_free_blocks(ods2_volume_t *vol, uint32_t lbn, unsigned block_count)
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

ods2_result_t ods2_delete(ods2_volume_t *vol, unsigned dir_file_number, const char *name)
{
    uint8_t dir_header[512];
    ods2_fid_t target_fid;
    uint8_t target_header[512];
    ods2_head_core_t *target_core;
    ods2_extent_t extents[ODS2_MAX_EXTENTS];
    int extent_count;
    int i;
    ods2_result_t r;
    unsigned vbn;
    ods2_extent_t dir_extents[ODS2_MAX_EXTENTS];
    int dir_extent_count;
    unsigned dir_total_blocks = 0;
    bool removed = false;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }

    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;

    r = ods2_lookup_name(vol, dir_header, name, &target_fid);
    if (!r.ok) return fail("name not found in directory");

    r = ods2_read_header(vol, target_fid.fid_num, target_header);
    if (!r.ok) return r;
    target_core = (ods2_head_core_t *) target_header;

    /* Refuse to delete a non-empty directory - matches VMS's own
       DELETE/DIRECTORY behavior and avoids silently orphaning its
       contents (their headers would become unreachable but never
       freed - a real, if recoverable via ANALYZE/DISK, leak). */
    if (target_core->filechar & 0x2000u) { /* FH2$M_DIRECTORY, confirmed value */
        ods2_dir_entry_t sub_entries[1];
        int sub_count = 0;
        r = ods2_list_directory(vol, target_header, sub_entries, 1, &sub_count);
        if (!r.ok) return r;
        if (sub_count > 0) {
            return fail("directory is not empty - refusing to delete "
                        "(matches VMS's own DELETE/DIRECTORY behavior)");
        }
    }

    /* Free every content block back to BITMAP.SYS, walking all
       extents (across any extension headers). */
    r = ods2_decode_all_extents(vol, target_header, extents, ODS2_MAX_EXTENTS, &extent_count);
    if (!r.ok) return r;
    for (i = 0; i < extent_count; i++) {
        r = ods2_free_blocks(vol, extents[i].lbn, extents[i].block_count);
        if (!r.ok) return r;
    }

    /* Free every header segment's own file-number slot and mark each
       one deleted, walking the ext_fid chain (spec 3.3) starting from
       the primary header we already have in memory - not just the
       primary. Without this, a multi-header file's extension headers
       would leak on delete: their file numbers would stay marked "in
       use" in the index bitmap forever, and their header blocks would
       remain live and readable despite the file itself being gone. */
    {
        uint8_t seg_header[512];
        unsigned seg_file_number = target_fid.fid_num;
        int segment = 0;

        memcpy(seg_header, target_header, sizeof(seg_header));

        for (;;) {
            ods2_head_core_t *seg_core = (ods2_head_core_t *) seg_header;
            ods2_fid_t next_ext_fid = seg_core->ext_fid;

            r = mark_index_bitmap(vol, seg_file_number, false);
            if (!r.ok) return r;

            /* Mark the header itself deleted, per spec 3.5.1's exact
               rules: FH2$V_MARKDEL set, FID_NUM/NMX/RVN zeroed - but
               FH2$W_FID_SEQ is explicitly NOT touched, since spec
               5.1.7 requires it to survive so the next use of this
               slot gets seq+1, not a fresh seq of 1 (that rule is
               only for a slot that was never used at all - "garbage" -
               which this no longer is). Checksum is set to zero, not
               recomputed - also an explicit spec rule, not an
               oversight. */
            seg_core->filechar |= 0x8000u; /* FH2$M_MARKDEL, confirmed value */
            seg_core->fid.fid_num = 0;
            seg_core->fid.fid_nmx = 0;
            seg_core->fid.fid_rvn = 0;
            seg_header[510] = 0;
            seg_header[511] = 0;

            r = ods2_write_header(vol, seg_file_number, seg_header);
            if (!r.ok) return r;

            if (next_ext_fid.fid_num == 0) break; /* no further extension header */
            segment++;
            if (segment >= ODS2_MAX_HEADER_SEGMENTS) {
                return fail("extension header chain exceeded maximum segment "
                            "limit while deleting (possibly corrupted or "
                            "circular ext_fid chain)");
            }
            seg_file_number = next_ext_fid.fid_num;
            r = ods2_read_header(vol, seg_file_number, seg_header);
            if (!r.ok) return r;
        }
    }

    /* Remove the directory entry from the parent - trying each
       content block until the (sorted) matching record is found. */
    r = ods2_decode_all_extents(vol, dir_header, dir_extents, ODS2_MAX_EXTENTS, &dir_extent_count);
    if (!r.ok) return r;
    for (i = 0; i < dir_extent_count; i++) dir_total_blocks += dir_extents[i].block_count;

    for (vbn = 1; vbn <= dir_total_blocks && !removed; vbn++) {
        uint8_t block[512];
        r = ods2_read_file_block(vol, dir_header, vbn, block);
        if (!r.ok) return r;
        if (ods2_remove_dir_entry(block, sizeof(block), name)) {
            r = ods2_write_file_block(vol, dir_header, vbn, block);
            if (!r.ok) return r;
            removed = true;
        }
    }
    if (!removed) {
        return fail("internal error: entry existed at lookup time but was not "
                    "found for removal (should be unreachable)");
    }

    return ok();
}
