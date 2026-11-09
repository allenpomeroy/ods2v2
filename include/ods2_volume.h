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

/* Most extents (retrieval pointers) a file may have across all its
   headers - the size of the extent lists used when reading, writing
   and deleting. ods2_create_file() refuses a file that would need
   more, which happens only on badly fragmented free space. */
#define ODS2_MAX_EXTENTS 1024

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
   exists. Reads any header, primary or extension (seg_num > 0). */
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

/* Looks up `name` (case-insensitive, e.g. "NETLIB020.DIR", no
   ";version") within the directory whose header is `dir_header`,
   returning the FID of its HIGHEST version. Equivalent to
   ods2_lookup_name_version(..., ODS2_VERSION_HIGHEST, ...). */
ods2_result_t ods2_lookup_name(ods2_volume_t *vol, const uint8_t *dir_header,
                                const char *name, ods2_fid_t *fid_out);

/* Version selectors for ods2_lookup_name_version() and
   ods2_delete_version(), following VMS file-specification rules:
     > 0   that exact version (1..32767)
     0     the highest existing version (VMS "NAME;" or "NAME;0")
     < 0   relative to the highest existing version: -1 is the next
           lower EXISTING version, -2 the one below that, and so on
           (VMS "NAME;-1"). Counts existing versions, not numbers, so
           gaps (e.g. ;7 and ;3 only) are skipped over. */
#define ODS2_VERSION_HIGHEST 0

/* Looks up one version of `name` (case-insensitive, no ";version")
   within the directory whose header is `dir_header`, selected by
   `version` (see ODS2_VERSION_HIGHEST above). On success returns the
   FID and, if `version_out` is non-NULL, the actual version number
   selected. Fails with "name not found in directory" if the name
   does not exist at all, or "version not found" if it does but the
   selected version does not. Handles names whose versions span
   several directory records and blocks. */
ods2_result_t ods2_lookup_name_version(ods2_volume_t *vol, const uint8_t *dir_header,
                                        const char *name, int version,
                                        ods2_fid_t *fid_out, uint16_t *version_out);

/* Splits a VMS-style directory path (e.g. "DECUS.NETLIB020", no
   brackets) on '.' and walks from the root directory (FID 4,4),
   returning the final directory's FID. An empty path returns root
   itself. */
ods2_result_t ods2_lookup_path(ods2_volume_t *vol, const char *path,
                                ods2_fid_t *fid_out);

/* Lists all entries in the directory whose header is `dir_header`
   into a caller-supplied buffer of `max_entries`. Never truncates
   silently: if the directory has more entries than fit, this returns
   a failure, stores the first `max_entries`, and sets *count_out to
   the directory's TOTAL entry count so the caller can size a buffer
   and retry. For directories of unknown size prefer
   ods2_list_directory_alloc(). */
ods2_result_t ods2_list_directory(ods2_volume_t *vol, const uint8_t *dir_header,
                                   ods2_dir_entry_t *entries_out, size_t max_entries,
                                   int *count_out);

/* Lists all entries in the directory whose header is `dir_header`
   into a malloc'd array sized to fit, however large the directory.
   On success *entries_out must be released with free() (it may be
   NULL when *count_out is 0). On failure nothing needs freeing. */
ods2_result_t ods2_list_directory_alloc(ods2_volume_t *vol, const uint8_t *dir_header,
                                         ods2_dir_entry_t **entries_out, int *count_out);

/* Counts the entries in the directory whose header is `dir_header`,
   without storing them. */
ods2_result_t ods2_count_directory_entries(ods2_volume_t *vol, const uint8_t *dir_header,
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

/* Returns the number of bytes of logical content in the file whose
   header is `header`, from its end-of-file mark: (EFBLK - 1) * 512 +
   FFBYTE (spec 6.1.5/6.1.6). An EOF on a block boundary may be
   recorded either as EFBLK=n+1/FFBYTE=0 (the preferred form, and
   what VMS itself writes) or as EFBLK=n/FFBYTE=512; both give n*512.
   EFBLK 0 is treated as an empty file. This is exactly how many bytes
   ods2_read_file() returns, so callers can size its buffer to fit
   rather than guess. It is not checked against the allocation here;
   a corrupt header can claim more than the file's HIBLK blocks
   (ods2_read_file() rejects that), so callers allocating from it
   should check against HIBLK * 512 first - see
   ods2_file_allocated_bytes(). */
size_t ods2_file_content_length(const uint8_t *header);

/* Returns the number of bytes the file's allocation can hold:
   FAT$L_HIBLK * 512. */
size_t ods2_file_allocated_bytes(const uint8_t *header);

/* Finds a free file number for a new file, and the file sequence
   number to give it, following spec 5.1.6/5.1.7: the number comes from
   the index file bitmap (searching start_from up to start_from +
   max_search - 1, within MAXFILES); its header slot, if inside
   INDEXF.SYS's end-of-file, is then checked - a valid header there is
   never reused even when the bitmap says free, a deleted one gives
   (old seq)+1, anything else gives 1. A slot beyond the end-of-file
   always gives 1. Changes nothing on disk: ods2_create_file() and
   ods2_create_directory() also move INDEXF.SYS's end-of-file past the
   new header (extending INDEXF.SYS if needed) and mark the bitmap. */
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

/* Like ods2_allocate_blocks(), but when no free run of `max_blocks`
   (rounded up to whole clusters) exists, allocates the longest free
   run there is instead, so a large file can be laid down in as few
   pieces as possible. *blocks_allocated_out says how much was
   reserved - possibly less than asked for; the caller allocates again
   for the rest. Fails only when the volume has no free cluster. */
ods2_result_t ods2_allocate_extent(ods2_volume_t *vol, unsigned max_blocks,
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

/* Appends one extent (`block_count` blocks at `lbn`) to INDEXF.SYS's
 * own header (file 1) in memory: encodes it as the smallest retrieval
 * pointer that fits - Format 1, or Format 2/3 above LBN 4,194,303 -
 * after the pointers already in use, and raises HIBLK (and HIGHWATER,
 * when set) by the same count. Writes nothing to disk and takes no
 * blocks; the caller has allocated them and writes the header. Fails,
 * changing nothing, when the header's map area has no room left for
 * the pointer. Used when INDEXF.SYS grows to hold more headers.
 */
ods2_result_t ods2_indexf_map_extent(uint8_t *indexf_header, uint32_t lbn, unsigned block_count);

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
 * Any size of file the volume has room for: the content is allocated
 * in as few, as large pieces as the free space allows (one piece on a
 * volume with a long enough free run), each described by the smallest
 * retrieval pointer that fits it (Format 1, 2 or 3). Pointers that
 * don't fit in the primary header's map area continue in extension
 * headers, chained through FH2$W_EXT_FID, so even a file spread over
 * fragmented free space is stored whole. A file that would need more
 * than ODS2_MAX_EXTENTS pieces, or more space than is free, fails
 * cleanly and gives back everything it had allocated. Same
 * parent-has-no-room-so-it-grows and duplicate-name behavior as
 * ods2_create_directory().
 * The volume must be mounted for write.
 */
ods2_result_t ods2_create_file(ods2_volume_t *vol, const uint8_t *parent_header,
                                const char *name, const uint8_t *content, size_t content_len,
                                uint8_t rtype, ods2_fid_t *new_fid_out);

/* RMS record attributes for a new file (FH2$W_RECATTR, spec 3.5.2.9).
   Record formats (FAT$B_RTYPE, sequential organization): */
#define ODS2_RFM_UDF   0 /* FAB$C_UDF: undefined - a stream of bytes */
#define ODS2_RFM_FIX   1 /* FAB$C_FIX: fixed-length records */
#define ODS2_RFM_VAR   2 /* FAB$C_VAR: variable-length (only as converted
                            records - see ods2_text_var.h) */
#define ODS2_RFM_VFC   3 /* FAB$C_VFC: variable with control (not importable) */
#define ODS2_RFM_STM   4 /* FAB$C_STM: stream, CR-LF terminated */
#define ODS2_RFM_STMLF 5 /* FAB$C_STMLF: stream, LF terminated */
#define ODS2_RFM_STMCR 6 /* FAB$C_STMCR: stream, CR terminated */
/* Record attributes (FAT$B_RATTRIB), combinable as bits: */
#define ODS2_RAT_FTN   0x01 /* FAB$M_FTN: FORTRAN carriage control */
#define ODS2_RAT_CR    0x02 /* FAB$M_CR: implied carriage return */
#define ODS2_RAT_PRN   0x04 /* FAB$M_PRN: print-file carriage control */
#define ODS2_RAT_BLK   0x08 /* FAB$M_BLK: records don't span blocks */

typedef struct {
    uint8_t  rtype;   /* ODS2_RFM_* */
    uint8_t  rattrib; /* ODS2_RAT_* bits; at most one of FTN/CR/PRN */
    uint16_t rsize;   /* FAT$W_RSIZE: the record size of a fixed file,
                         otherwise the longest record (LRL) */
    uint16_t maxrec;  /* FAT$W_MAXREC (MRS): maximum record size; for a
                         fixed file always equal to rsize, as VMS sets
                         it (e.g. INDEXF.SYS: FIX, RSIZE = MRS = 512) */
} ods2_record_attrs_t;

/* Checks `attrs` as the record attributes of a new file holding
 * `content_len` bytes taken from the host as they are. Returns NULL if
 * they're acceptable, else why not:
 *  - only UDF, FIX and the stream formats: VAR and VFC files carry a
 *    count word before each record on disk, which host bytes don't;
 *  - a fixed record size is 2..32766 bytes and even (RMS pads an odd
 *    fixed record to a word boundary on disk - not supported), MRS
 *    equals it, and the content is a whole number of records;
 *  - other sizes are at most 32767; unknown RAT bits, or more than one
 *    carriage control, are refused. */
const char *ods2_check_record_attrs(const ods2_record_attrs_t *attrs, size_t content_len);

/* Checks `attrs` for a new file whose `content_len` bytes are already
 * variable-length records (issue #15). Returns NULL if acceptable,
 * else why not: the format must be VAR; the longest record (RSIZE) at
 * most 32767; MRS 0 (no limit, as VMS FTP leaves it) or at least the
 * longest record; the content a whole number of words; not RAT:BLK
 * (converted records run on across blocks, which BLK forbids); at most
 * one carriage control; no unknown RAT bits. */
const char *ods2_check_var_record_attrs(const ods2_record_attrs_t *attrs, size_t content_len);

/* ods2_create_file() with chosen record attributes, checked first by
 * ods2_check_record_attrs() - a file whose attributes don't fit its
 * content is refused, not created. ods2_create_file(..., rtype, ...)
 * is this with rattrib 0 and RSIZE = MRS = 512, unchecked, as before.
 */
ods2_result_t ods2_create_file_attrs(ods2_volume_t *vol, const uint8_t *parent_header,
                                      const char *name, const uint8_t *content,
                                      size_t content_len, const ods2_record_attrs_t *attrs,
                                      ods2_fid_t *new_fid_out);

/* Content for a new file, read in order rather than held in memory
 * (issue #16): `length` bytes in all, known before anything is
 * allocated, delivered by `read`, which fills exactly `n` bytes of
 * `buf` with the next part of the content and returns true, or returns
 * false if it can't (a read error, or a source shorter than `length`).
 * `read` is called with chunks of at most ODS2_STREAM_CHUNK bytes, in
 * order, `length` bytes in total. `problem`, if `read` sets it, says
 * why it failed; otherwise the failure is reported generically. */
#define ODS2_STREAM_CHUNK (1u << 20)

typedef struct {
    size_t length;
    bool (*read)(void *ctx, uint8_t *buf, size_t n);
    void *ctx;
    const char *problem;
    /* true when the content is already RMS variable-length records,
       count words included (ods2_text_var.h makes them from host
       text): RFM:VAR is then accepted, and checked by
       ods2_check_var_record_attrs() instead. */
    bool var_records;
} ods2_content_source_t;

/* ods2_create_file_attrs() with the content taken from `src` a chunk
 * at a time, so memory use stays the same whatever the file's size.
 * Everything is allocated (from src->length) before the first read;
 * if `src` fails part way, everything is given back and no file is
 * created. */
ods2_result_t ods2_create_file_source(ods2_volume_t *vol, const uint8_t *parent_header,
                                       const char *name, ods2_content_source_t *src,
                                       const ods2_record_attrs_t *attrs,
                                       ods2_fid_t *new_fid_out);

/* Reads a file's logical content (as ods2_read_file()) and hands it to
 * `sink` in order, a chunk of at most ODS2_STREAM_CHUNK bytes at a
 * time, so memory use stays the same whatever the file's size. If
 * `sink` returns false the read stops and fails with *sink_failed_out
 * (if non-NULL) set true, telling that apart from a volume read error.
 * *bytes_out (if non-NULL) is how much reached the sink. */
ods2_result_t ods2_read_file_stream(ods2_volume_t *vol, const uint8_t *header,
                                     bool (*sink)(void *ctx, const uint8_t *data, size_t n),
                                     void *ctx, size_t *bytes_out, bool *sink_failed_out);

/* SET FILE/VERSION_LIMIT: sets the version limit of the entries named `name` in the directory (all versions); *changed
 * is the number of directory records changed. For volumes written before ods2v2 gave entries VMS's limit (32767). */
ods2_result_t ods2_set_entry_version_limit(ods2_volume_t *vol, unsigned dir_file_number, const char *name,
                                           uint16_t limit, unsigned *changed);

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

/* Same as ods2_delete(), for one version of `name` selected by
 * `version` (see ODS2_VERSION_HIGHEST). Only that version's
 * (version, FID) entry is removed from the directory; the name's
 * other versions stay listed and readable. ods2_delete() is this
 * with ODS2_VERSION_HIGHEST. If `version_out` is non-NULL it receives
 * the actual version number deleted.
 */
ods2_result_t ods2_delete_version(ods2_volume_t *vol, unsigned dir_file_number,
                                  const char *name, int version, uint16_t *version_out);


/* --- the volume label (issue #13) --- */

/* Checks `label` as a volume label and writes it to `out` in its
 * on-disk form: uppercased and space-padded to 12 bytes (HM2$T_VOLNAME,
 * spec 5.1.9.36). Returns NULL if acceptable, else why not: it may not
 * be empty (spec 2.1) or over 12 characters, and - keeping to what the
 * spec recommends and what a DISK$label logical name can hold - only
 * letters, digits, '$', '_' and '-' are allowed. */
const char *ods2_make_volume_label(const char *label, char out[12]);

/* Relabels the volume, as VMS's SET VOLUME/LABEL does, for a volume
 * VMS does not have mounted. Every copy of the home block is changed:
 * the home block and its cluster fillers (INDEXF.SYS VBNs 2 to 2v) and
 * the backup home block's cluster (VBNs 2v+1 to 3v), v the cluster
 * factor (spec 5.1.3-5.1.4). Each keeps its own HOMELBN and HOMEVBN,
 * and both its checksums are recomputed.
 *
 * Nothing is written unless every one of those blocks is first found
 * to be a valid home block naming its own position and otherwise
 * identical to the primary, with the primary and backup among them.
 * Refused for a volume in a volume set (a structure name, a relative
 * volume number or a set count), since one member can't be relabelled
 * alone. The primary is written last and every copy read back.
 * *copies_out (if non-NULL) is how many blocks were rewritten. */
ods2_result_t ods2_set_volume_label(ods2_volume_t *vol, const char *label,
                                     unsigned *copies_out);

/* The volume's size in blocks (SCB$L_VOLSIZE, from BITMAP.SYS's
 * storage control block) and how many of them are free (free clusters
 * in the storage bitmap times the cluster factor). Read only. */
ods2_result_t ods2_volume_space(ods2_volume_t *vol, uint64_t *total_blocks_out,
                                 uint64_t *free_blocks_out);

#endif /* ODS2_VOLUME_H */
