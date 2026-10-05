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
#include "ods2_time.h"
#include <string.h>
#include <ctype.h>
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

/* Decodes ALL of a file's extents, walking the ext_fid chain across
   extension headers if the file needs more than one (spec: a header
   whose Map Area can't hold all its retrieval pointers points to a
   continuation header via FH2$W_EXT_FID; that header's own ext_fid
   may point further still). Without this, any file needing more than
   one header would silently appear truncated. A generous but finite
   segment limit guards against a corrupted or circular ext_fid chain
   looping forever. */
#define ODS2_MAX_HEADER_SEGMENTS 64

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

ods2_result_t ods2_read_file(ods2_volume_t *vol, const uint8_t *header,
                              uint8_t *buf_out, size_t buf_size, size_t *bytes_read_out)
{
    size_t content_len = ods2_file_content_length(header);
    size_t allocated = ods2_file_allocated_bytes(header);
    size_t written = 0;
    unsigned vbn = 1;

    if (content_len > allocated) {
        /* Corrupt header: the EOF mark claims more data than the file
           has blocks. Say so, rather than fail later on an unmapped
           VBN (or have a caller size a buffer from a bogus length). */
        return fail("file's end-of-file mark lies beyond its allocated blocks");
    }
    if (content_len > buf_size) {
        return fail("output buffer too small for file content");
    }

    while (written < content_len) {
        uint8_t block[512];
        size_t this_block_bytes = content_len - written;
        ods2_result_t r = ods2_read_file_block(vol, header, vbn, block);
        if (!r.ok) return r;

        if (this_block_bytes > 512) this_block_bytes = 512;
        memcpy(buf_out + written, block, this_block_bytes);
        written += this_block_bytes;
        vbn++;
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

/* Allocates more blocks to INDEXF.SYS and maps them, so more headers
   fit. Up to 256 blocks at a time (one Format 1 retrieval pointer);
   falls back to a single cluster if no run that long is free. */
static ods2_result_t extend_indexf(ods2_volume_t *vol, uint8_t *indexf_header)
{
    ods2_head_core_t *core = (ods2_head_core_t *) indexf_header;
    size_t map_offset = (size_t) core->mpoffset * 2;
    size_t map_bytes = (size_t) core->map_inuse * 2;
    size_t map_end = (size_t) core->acoffset * 2;
    unsigned want = (256u / vol->home.cluster) * vol->home.cluster;
    uint32_t lbn, hiblk;
    unsigned got;
    ods2_result_t r;

    if (want == 0) want = vol->home.cluster;
    if (map_offset + map_bytes + 4 > map_end || map_end > 510) {
        return fail("INDEXF.SYS's header has no room for another extent "
                    "(would need an extension header - not yet implemented)");
    }
    r = ods2_allocate_blocks(vol, want, &lbn, &got);
    if (!r.ok && want > vol->home.cluster) {
        want = vol->home.cluster;
        r = ods2_allocate_blocks(vol, want, &lbn, &got);
    }
    if (!r.ok) return r;
    if (!ods2_encode_retrieval_pointer_format1(indexf_header + map_offset + map_bytes,
                                               lbn, got)) {
        /* ods2_allocate_blocks() has already marked them used; give
           them back rather than leak them. */
        ods2_free_blocks(vol, lbn, got);
        return fail("cannot map more INDEXF.SYS blocks: the free space found lies "
                    "beyond LBN 4194303, which needs a Format 2 retrieval pointer "
                    "(not yet implemented)");
    }
    core->map_inuse = (uint8_t) ((map_bytes + 4) / 2);
    hiblk = ods2_word_swap32(core->recattr.hiblk) + got;
    core->recattr.hiblk = ods2_word_swap32(hiblk);
    /* INITIALIZE leaves FH2$L_HIGHWATER at HIBLK + 1 on INDEXF.SYS;
       keep that relationship. */
    if (core->highwater != 0) core->highwater = hiblk + 1;
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
    r = ods2_lookup_name(vol, parent_header, dirname, &existing);
    if (r.ok) {
        return fail("a file or directory with this name already exists in the parent");
    }

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
    unsigned safe_chunk;
    ods2_header_spec_t spec;
    uint8_t new_header[512];
    ods2_result_t r;
    unsigned vbn;
    char upper_name[256];

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

    r = ods2_lookup_name(vol, parent_header, name, &existing);
    if (r.ok) {
        return fail("a file or directory with this name already exists in the parent");
    }

    blocks_needed = (unsigned) ((content_len + 511) / 512);
    if (blocks_needed == 0) blocks_needed = 1; /* a zero-length file still needs one block for EFBLK=1... */

    /* Request chunks small enough that cluster-rounding can never
       push a single extent's actual allocation past Format 1's
       256-block limit: rounding up to the next cluster multiple adds
       at most (cluster-1) extra blocks, so requesting no more than
       257-cluster per chunk guarantees the allocated result stays
       <= 256 even in the worst case. */
    safe_chunk = 257u - vol->home.cluster;
    if (safe_chunk > 256) safe_chunk = 256; /* cluster==1 edge case */

    /* Each extent is one Format 1 pointer (4 bytes) in the fixed
       200-byte-offset map area, which has 510-200=310 bytes of room -
       up to 77 extents in a single header (a second, extension
       header would be needed beyond that - not yet implemented). */
    if ((blocks_needed + safe_chunk - 1) / safe_chunk > 77) {
        return fail("content far too large for a single header's map area "
                    "(needs a second, extension header - not yet implemented)");
    }

    /* A file number, its header slot inside INDEXF.SYS's end-of-file,
       and its bit marked in use (see allocate_file_number()). */
    r = allocate_file_number(vol, &file_number, &seq_num);
    if (!r.ok) return r;

    blocks_remaining = blocks_needed;
    while (blocks_remaining > 0) {
        uint32_t chunk_lbn;
        unsigned chunk_allocated;
        unsigned chunk_request = (blocks_remaining < safe_chunk) ? blocks_remaining : safe_chunk;

        if (extent_count >= ODS2_MAX_EXTENTS) {
            return fail("internal error: exceeded extent buffer capacity "
                        "(should be unreachable given the 77-extent check above)");
        }

        r = ods2_allocate_blocks(vol, chunk_request, &chunk_lbn, &chunk_allocated);
        if (!r.ok) return r;
        if (chunk_allocated > 256) {
            return fail("internal error: cluster rounding produced an extent "
                        "over 256 blocks despite the safe_chunk guard "
                        "(should be unreachable)");
        }

        extents[extent_count].lbn = chunk_lbn;
        extents[extent_count].block_count = chunk_allocated;
        extent_count++;
        total_allocated += chunk_allocated;

        blocks_remaining = (chunk_allocated >= blocks_remaining) ? 0
                                                                    : blocks_remaining - chunk_allocated;
    }

    memset(&spec, 0, sizeof(spec));
    spec.fid.fid_num = (uint16_t) file_number;
    spec.fid.fid_seq = seq_num;
    spec.backlink = parent_core->fid;
    spec.filechar = (extent_count == 1) ? 0x0080 : 0; /* FH2$M_CONTIG only when truly one piece */
    spec.rtype = rtype;
    spec.rattrib = 0;
    spec.rsize = 512;
    spec.maxrec = 512;
    spec.extents = extents;
    spec.extent_count = extent_count;
    spec.hiblk = total_allocated;
    /* End-of-file mark per spec 6.1.5/6.1.6, in the preferred form
       VMS itself writes: EFBLK is the VBN holding the EOF position,
       FFBYTE the bytes in use there. Content ending exactly on a
       block boundary (including an empty file) gets EFBLK = blocks+1
       and FFBYTE = 0. (An earlier version wrote EFBLK = blocks and
       FFBYTE = 0 for that case, which VMS reads as one block SHORTER
       than the real content, and an empty file as 512 bytes.) */
    spec.efblk = (uint32_t) (content_len / 512) + 1;
    spec.ffbyte = (uint16_t) (content_len % 512);

    {
        char ident[260]; /* name is already bounded (<256 chars, checked
                             above) plus ";1" and a nul - always large
                             enough, sized generously to also silence a
                             spurious compiler truncation warning */
        snprintf(ident, sizeof(ident), "%s;1", name);
        spec.ident_name = ident;

        if (!ods2_build_file_header(new_header, &spec)) {
            return fail("could not construct new file's header");
        }
    }

    r = ods2_write_header(vol, file_number, new_header);
    if (!r.ok) return r;

    for (vbn = 1; vbn <= blocks_needed; vbn++) {
        uint8_t block[512];
        size_t offset = (size_t) (vbn - 1) * 512;
        size_t remaining = content_len - offset;
        size_t this_len = (remaining > 512) ? 512 : remaining;

        memset(block, 0, sizeof(block));
        if (this_len > 0) {
            memcpy(block, content + offset, this_len);
        }
        r = ods2_write_file_block(vol, new_header, vbn, block);
        if (!r.ok) return r;
    }

    /* Insert an entry for the new file into the parent - growing the
       parent's own allocation automatically if none of its existing
       content blocks have room. */
    r = ods2_insert_into_directory(vol, parent_core->fid.fid_num, name, 1, spec.fid);
    if (!r.ok) return r;

    *new_fid_out = spec.fid;
    return ok();
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
static ods2_result_t store_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                     uint8_t *dir_header, const uint8_t *blocks,
                                     unsigned used, unsigned old_used, unsigned first_changed)
{
    ods2_head_core_t *core = (ods2_head_core_t *) dir_header;
    ods2_extent_t old_extents[ODS2_MAX_EXTENTS];
    int old_count, i;
    unsigned allocated = 0, v;
    bool moved = false;
    uint8_t zero[512];
    ods2_result_t r;

    memset(zero, 0, sizeof(zero));
    r = ods2_decode_all_extents(vol, dir_header, old_extents, ODS2_MAX_EXTENTS, &old_count);
    if (!r.ok) return r;
    for (i = 0; i < old_count; i++) allocated += old_extents[i].block_count;

    if (used > allocated) {
        /* One Format 1 retrieval pointer maps at most 256 blocks. */
        unsigned max_blocks = (256u / vol->home.cluster) * vol->home.cluster;
        unsigned want = allocated * 2;
        uint32_t lbn;
        unsigned got;
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
        if (lbn + got - 1 > 0x3fffffu || map_end > 510 || map_end < map_start + 4) {
            ods2_free_blocks(vol, lbn, got);
            return fail("cannot map the directory's new area: it lies beyond LBN "
                        "4194303, which needs a Format 2 retrieval pointer "
                        "(not yet implemented)");
        }
        for (v = 0; v < got; v++) {
            r = ods2_write_block(vol, lbn + v, (v < used) ? blocks + (size_t) v * 512 : zero);
            if (!r.ok) return r;
        }
        memset(dir_header + map_start, 0, map_end - map_start);
        ods2_encode_retrieval_pointer_format1(dir_header + map_start, lbn, got);
        core->map_inuse = 2;
        core->recattr.hiblk = ods2_word_swap32(got);
        moved = true;
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

    blocks = calloc((size_t) used + spare, 512);
    if (blocks == NULL) return fail("could not allocate memory for the directory");
    for (v = 0; v < used; v++) {
        r = ods2_read_file_block(vol, dir_header, v + 1, blocks + (size_t) v * 512);
        if (!r.ok) {
            free(blocks);
            return r;
        }
    }
    *blocks_out = blocks;
    *used_out = used;
    return ok();
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
    ods2_result_t r;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }
    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;
    r = load_directory(vol, dir_header, 1, &blocks, &used);
    if (!r.ok) return r;

    t = ods2_dir_choose_block(blocks, used, name);
    if (ods2_insert_dir_entry(blocks + (size_t) t * 512, 512, name, version, fid)) {
        if (ods2_file_content_length(dir_header) == 0) {
            /* An end-of-file that didn't cover even block 1 */
            r = store_directory(vol, dir_file_number, dir_header, blocks, used, used, t);
        } else {
            r = ods2_write_file_block(vol, dir_header, t + 1, blocks + (size_t) t * 512);
        }
    } else {
        uint8_t combined[1024];
        memcpy(combined, blocks + (size_t) t * 512, 512);
        memset(combined + 512, 0, 512);
        if (!ods2_insert_dir_entry(combined, sizeof(combined), name, version, fid)) {
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
    return ods2_delete_version(vol, dir_file_number, name, ODS2_VERSION_HIGHEST, NULL);
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
    ods2_result_t r;
    bool removed = false;

    if (!vol->writable) {
        return fail("volume was not mounted for write (use ods2_mount_write)");
    }

    r = ods2_read_header(vol, dir_file_number, dir_header);
    if (!r.ok) return r;

    r = ods2_lookup_name_version(vol, dir_header, name, version, &target_fid, &target_version);
    if (!r.ok) return r;

    r = ods2_read_header(vol, target_fid.fid_num, target_header);
    if (!r.ok) return r;
    target_core = (ods2_head_core_t *) target_header;

    /* Refuse to delete a non-empty directory - matches VMS's own
       DELETE/DIRECTORY behavior and avoids silently orphaning its
       contents (their headers would become unreachable but never
       freed - a real, if recoverable via ANALYZE/DISK, leak). */
    if (target_core->filechar & 0x2000u) { /* FH2$M_DIRECTORY, confirmed value */
        int sub_count = 0;
        r = ods2_count_directory_entries(vol, target_header, &sub_count);
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

    /* Free the file number in the index file's own bitmap. */
    r = mark_index_bitmap(vol, target_fid.fid_num, false);
    if (!r.ok) return r;

    /* Mark the header itself deleted, per spec 3.5.1's exact rules:
       FH2$V_MARKDEL set, FID_NUM/NMX/RVN zeroed - but FH2$W_FID_SEQ
       is explicitly NOT touched, since spec 5.1.7 requires it to
       survive so the next use of this slot gets seq+1, not a fresh
       seq of 1 (that rule is only for a slot that was never used at
       all - "garbage" - which this no longer is). Checksum is set to
       zero, not recomputed - also an explicit spec rule, not an
       oversight. */
    target_core->filechar |= 0x8000u; /* FH2$M_MARKDEL, confirmed value */
    target_core->fid.fid_num = 0;
    target_core->fid.fid_nmx = 0;
    target_core->fid.fid_rvn = 0;
    target_header[510] = 0;
    target_header[511] = 0;

    r = ods2_write_header(vol, target_fid.fid_num, target_header);
    if (!r.ok) return r;

    /* Remove just this version's entry from the parent - trying each
       block in use until the record holding it is found. Removing the
       whole record instead (as an earlier version did) would also drop
       every other version of the name stored there, leaving their
       headers allocated but unreachable. A block left with no entries
       is removed and the blocks after it move up, as the VMS file
       system does, so every block inside the end-of-file holds
       entries. */
    {
        uint8_t *blocks = NULL;
        unsigned used, v;
        r = load_directory(vol, dir_header, 0, &blocks, &used);
        if (!r.ok) return r;
        for (v = 0; v < used && !removed; v++) {
            uint8_t *block = blocks + (size_t) v * 512;
            if (!ods2_remove_dir_version(block, 512, name, target_version)) continue;
            removed = true;
            if (used > 1 && ods2_dir_block_is_empty(block)) {
                memmove(block, block + 512, (size_t) (used - v - 1) * 512);
                r = store_directory(vol, dir_file_number, dir_header, blocks, used - 1,
                                    used, v);
            } else {
                r = ods2_write_file_block(vol, dir_header, v + 1, block);
            }
        }
        free(blocks);
        if (!r.ok) return r;
    }
    if (!removed) {
        return fail("internal error: entry existed at lookup time but was not "
                    "found for removal (should be unreachable)");
    }

    if (version_out != NULL) *version_out = target_version;
    return ok();
}
