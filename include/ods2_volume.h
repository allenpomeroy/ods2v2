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

/* ods2_volume.h - the real read path: open a disk image, read its
 * home block, locate any file's header (general VBN-walking approach,
 * not limited to the first 16 files), read file content, and look up
 * paths by walking directory content.
 */
#ifndef ODS2_VOLUME_H
#define ODS2_VOLUME_H

#include "ods2_ondisk.h"
#include "ods2_retrieval.h"
#include "ods2_directory.h"
#include <stdio.h>
#include <stdbool.h>

#define ODS2_MAX_EXTENTS 128

typedef struct {
    FILE          *fp;
    bool           writable;
    ods2_home_t    home;
    /* INDEXF.SYS's own extents, decoded once at mount time - needed
       to locate every other file's header, including files beyond
       the first 16 (which can be located directly from home block
       fields alone; anything past that genuinely requires walking
       INDEXF.SYS's own content, which is what this enables). */
    ods2_extent_t  indexf_extents[ODS2_MAX_EXTENTS];
    int            indexf_extent_count;
} ods2_volume_t;

typedef struct {
    bool        ok;
    const char *problem; /* NULL if ok */
} ods2_result_t;

/* Opens `path` and reads/validates the home block, then reads and
   decodes INDEXF.SYS's own header so later header lookups can walk
   its extents. `vol` is caller-allocated; on failure vol->fp is
   closed automatically if it was opened. Opens read-only. */
ods2_result_t ods2_mount(const char *path, ods2_volume_t *vol);

/* Same as ods2_mount(), but opens the underlying file for read-write
   ("r+b") so ods2_write_block()/ods2_write_header() etc. can be used.
   The file must already exist (this never creates a new disk image -
   that's INITIALIZE's job, deliberately out of scope here). */
ods2_result_t ods2_mount_write(const char *path, ods2_volume_t *vol);

/* Writes a single 512-byte block at absolute LBN `lbn`. Fails if the
   volume was mounted read-only via ods2_mount() rather than
   ods2_mount_write(). */
ods2_result_t ods2_write_block(ods2_volume_t *vol, uint32_t lbn, const uint8_t *block);

/* Closes the volume. Safe to call even if mount failed partway. */
void ods2_dismount(ods2_volume_t *vol);

/* Reads the 512-byte header for `file_number` (1-based) into
   `header_out`. Works for any file number whose header falls within
   the VBN range covered by INDEXF.SYS's decoded extents - which for a
   volume with a small number of files (well under what a full
   INDEXF.SYS extent list would cover) is normally everything that
   exists. Extension headers (seg_num > 0) are not yet supported. */
ods2_result_t ods2_read_header(ods2_volume_t *vol, unsigned file_number,
                                uint8_t *header_out);

/* Write counterpart to ods2_read_header() - writes a caller-built
   512-byte header to file_number's known location. The volume must
   be mounted for write. */
ods2_result_t ods2_write_header(ods2_volume_t *vol, unsigned file_number,
                                 const uint8_t *header_in);

/* Reads virtual block `vbn` (1-based) of the file described by
   `header` into `block_out` (512 bytes), by decoding the header's own
   Map Area and walking its extents. */
ods2_result_t ods2_read_file_block(ods2_volume_t *vol, const uint8_t *header,
                                    unsigned vbn, uint8_t *block_out);

/* Looks up `name` (case-insensitive, e.g. "NETLIB020.DIR") within the
   directory whose header is `dir_header`, returning its FID. */
ods2_result_t ods2_lookup_name(ods2_volume_t *vol, const uint8_t *dir_header,
                                const char *name, ods2_fid_t *fid_out);

/* Splits a VMS-style directory path (e.g. "DECUS.NETLIB020", no
   brackets) on '.' and walks from the root directory (FID 4,4),
   returning the final directory's FID. An empty path returns root
   itself. */
ods2_result_t ods2_lookup_path(ods2_volume_t *vol, const char *path,
                                ods2_fid_t *fid_out);

/* Lists all entries in the directory whose header is `dir_header`. */
ods2_result_t ods2_list_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                   ods2_dir_entry_t *entries_out, size_t max_entries,
                                   int *count_out);

/* Decodes ALL of a file's extents, walking the ext_fid chain across
   extension headers if the file needs more than one. See
   ods2_volume.c for the reasoning - without this, files needing more
   than one header would silently appear truncated. */
ods2_result_t ods2_decode_all_extents(ods2_volume_t *vol, const uint8_t *header,
                                       ods2_extent_t *extents_out, size_t max_extents,
                                       int *count_out);

/* Reads a file's entire logical content (respecting FAT$L_EFBLK/
   FAT$W_FFBYTE - the end-of-file mark - not just its physical
   allocation, which is normally larger) into a caller-allocated
   buffer. Returns the number of bytes actually written into
   `buf_out`, or a failure result if `buf_out` is too small or a
   block couldn't be read. Intended for text/binary file content
   (a TYPE/COPY equivalent), not directories - use
   ods2_list_directory() for those. */
ods2_result_t ods2_read_file(ods2_volume_t *vol, const uint8_t *header,
                              uint8_t *buf_out, size_t buf_size, size_t *bytes_read_out);

/* Finds a free file number for a new file, and the correct file
   sequence number to assign it (spec 5.1.7: seq=1 for a genuinely
   never-used slot, or (old seq)+1 for a slot that held a deleted
   file - distinguished via FH2$M_MARKDEL, since both cases have
   fid_num==0). See ods2_volume.c for why this validates actual
   header content rather than trusting the index file bitmap alone -
   the spec itself documents the bitmap can have "dropped bits" and
   the header content is the real authority. */
ods2_result_t ods2_find_free_file_number(ods2_volume_t *vol, unsigned start_from,
                                          unsigned max_search, unsigned *file_number_out,
                                          uint16_t *seq_out);

/* Writes virtual block `vbn` (1-based) of the file described by
   `header` from `block_in` (512 bytes), by decoding the header's own
   Map Area (walking ext_fid segments) and finding the matching LBN.
   The volume must be mounted for write. */
ods2_result_t ods2_write_file_block(ods2_volume_t *vol, const uint8_t *header,
                                     unsigned vbn, const uint8_t *block_in);

/* Allocates `blocks_needed` blocks from BITMAP.SYS's own storage
   bitmap (spec 5.2), rounding up to the volume's cluster factor (the
   bitmap tracks clusters, not individual blocks - confirmed by the
   old project's own working allocation code). On success, *lbn_out
   is the starting LBN and *blocks_allocated_out is the actual number
   of blocks reserved (may be more than requested, due to cluster
   rounding - callers should use this actual count, not their
   original request, when building extents/hiblk). The volume must be
   mounted for write. */
ods2_result_t ods2_allocate_blocks(ods2_volume_t *vol, unsigned blocks_needed,
                                    uint32_t *lbn_out, unsigned *blocks_allocated_out);

/* Frees `block_count` blocks starting at `lbn` back to BITMAP.SYS's
 * storage bitmap - the exact inverse of ods2_allocate_blocks(). Marks
 * the corresponding clusters free (set, per spec 5.2.2); `lbn` and
 * `block_count` must be cluster-aligned (i.e. exactly what an earlier
 * ods2_allocate_blocks() call returned - this does not check
 * alignment itself, freeing a non-cluster-aligned range would corrupt
 * the bitmap by marking part of an adjacent cluster free too early).
 * The volume must be mounted for write.
 */
ods2_result_t ods2_free_blocks(ods2_volume_t *vol, uint32_t lbn, unsigned block_count);

/* Creates a new, empty subdirectory named `name` (e.g. "NETLIB020",
 * without ".DIR" - that's appended internally, matching VMS
 * convention) within the directory described by `parent_header`.
 * Allocates a file number, allocates content space, builds and writes
 * the new directory's own header, initializes its content block with
 * just the sentinel record, and inserts an entry for it into the
 * parent's content (via ods2_insert_into_directory() - the parent
 * grows automatically if none of its existing content blocks have
 * room). Fails (without leaving a half-created directory on disk,
 * per-step) if the name already exists in the parent, or if no free
 * file number or disk space is available. The volume must be mounted
 * for write. On success, *new_fid_out is the newly created
 * directory's FID.
 */
ods2_result_t ods2_create_directory(ods2_volume_t *vol, const uint8_t *parent_header,
                                     const char *name, ods2_fid_t *new_fid_out);

/* Creates a new file named `name` (e.g. "README.TXT") within the
 * directory described by `parent_header`, writing `content_len` bytes
 * from `content` as its data. `rtype` should typically be 5
 * (FAB$C_STMLF, stream-LF - the common convention for text files) or
 * 1 (FAB$C_FIX, fixed-length records) for binary data laid out in
 * uniform-size chunks; ods2_header_build.h's ods2_header_spec_t has
 * the full field if finer control is needed via a lower-level call.
 *
 * Supports multi-extent files (content allocated across several
 * retrieval pointers when it exceeds Format 1's 256-block/128KB
 * single-extent limit, chunked in a way that guarantees cluster
 * rounding never pushes any one extent over that limit). A single
 * header's map area holds up to ~77 extents; content needing more
 * than that (a genuinely large, badly fragmented file) fails cleanly
 * rather than silently truncating - a second, extension header would
 * be needed and is not yet implemented. Same parent-has-no-room-so-
 * it-grows and duplicate-name behavior as ods2_create_directory().
 * The volume must be mounted for write.
 */
ods2_result_t ods2_create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                const char *name, const uint8_t *content, size_t content_len,
                                uint8_t rtype, ods2_fid_t *new_fid_out);

/* Inserts (name, version, fid) into the directory whose header is at
 * `dir_file_number`, trying each of its existing content blocks
 * first; if none have room, allocates a new block, extends the
 * directory's own header with an additional extent (modified in
 * place - preserves the original creation date, only the revision
 * date/count are updated, unlike rebuilding the header from scratch),
 * and inserts there. Shared by ods2_create_directory() and
 * ods2_create_file() - both need identical "insert into a directory,
 * growing it if necessary" behavior. The volume must be mounted for
 * write. Fails if the directory would need a second extension header
 * (more than roughly 77 extents) - not yet implemented.
 */
ods2_result_t ods2_insert_into_directory(ods2_volume_t *vol, unsigned dir_file_number,
                                          const char *name, uint16_t version, ods2_fid_t fid);

/* Deletes `name` from the directory at `dir_file_number`: looks it
 * up, refuses if it's a non-empty directory (matching VMS's own
 * DELETE/DIRECTORY behavior - avoids silently orphaning its
 * contents), frees all of its content blocks back to BITMAP.SYS
 * (walking every extent across any extension headers), frees its
 * file number in the index file's own bitmap, marks its header
 * deleted per spec 3.5.1's exact rules (FH2$V_MARKDEL set,
 * FID_NUM/NMX/RVN zeroed - but NOT fid_seq, which must survive for
 * spec 5.1.7's "next use gets seq+1" rule - and FH2$W_CHECKSUM set to
 * zero), and removes its directory entry from the parent. The volume
 * must be mounted for write. Each step fails cleanly with a specific
 * error rather than leaving partial, inconsistent state where
 * possible, though - like every operation here - a crash mid-
 * operation could still leave the volume in a state needing repair;
 * this is not a transactional filesystem.
 */
ods2_result_t ods2_delete(ods2_volume_t *vol, unsigned dir_file_number, const char *name);

#endif /* ODS2_VOLUME_H */
