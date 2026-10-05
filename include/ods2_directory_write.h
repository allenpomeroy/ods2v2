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

/* ods2_directory_write.h - inserts a new (name, version, fid) record
 * into a directory content block. The record format here matches
 * ods2_directory.c's reader exactly - both were built against the
 * same real, parsed root directory data (see ods2_directory_selftest.c).
 */
#ifndef ODS2_DIRECTORY_WRITE_H
#define ODS2_DIRECTORY_WRITE_H

#include "ods2_ondisk.h"
#include <stddef.h>
#include <stdbool.h>

/* Inserts a new directory entry at the first available space in
 * `block` (searching for the sentinel record - dir$size==0xffff - or
 * the end of existing valid records, then writing there and placing a
 * new sentinel immediately after). `name` is copied as given (callers
 * should pass an already-uppercased name; VMS convention, not
 * enforced here since the directory format itself is case-agnostic).
 * Returns false if there isn't room for the new record plus a
 * trailing sentinel within `block_size` bytes - callers must check
 * this and extend the directory (allocate another block) rather than
 * assume success.
 */
bool ods2_insert_dir_entry(uint8_t *block, size_t block_size,
                            const char *name, uint16_t version, ods2_fid_t fid);

/* Removes the whole directory RECORD matching `name` (case-
 * insensitive) from `block` - the name and EVERY version it holds in
 * this block - shifting all following bytes (through the sentinel)
 * back to close the gap - the exact inverse of ods2_insert_dir_entry().
 * Returns true if a matching record was found and removed, false if
 * no record with that name exists in this block (callers checking
 * multiple blocks should treat false as "not in this one, try the
 * next" rather than an error).
 *
 * To delete one version of a file that may have several, use
 * ods2_remove_dir_version() instead: removing the whole record would
 * also drop the other versions' entries while their headers stay
 * allocated.
 */
bool ods2_remove_dir_entry(uint8_t *block, size_t block_size, const char *name);

/* Removes just the (`version`, FID) pair of `name` (case-insensitive)
 * from `block`, shrinking that record by 8 bytes and shifting all
 * following bytes (through the sentinel) back to close the gap. If
 * that pair was the record's only version, the whole record is
 * removed instead. Other versions of the same name - in this record
 * or in records in other blocks - are left untouched.
 * Returns true if the pair was found and removed, false if this block
 * has no record for `name` holding `version` (callers checking
 * multiple blocks should treat false as "not in this one, try the
 * next").
 */
bool ods2_remove_dir_version(uint8_t *block, size_t block_size, const char *name,
                             uint16_t version);

/* --- Names (spec 4.2.5) ---
 *
 * A directory record's name is "NAME.TYPE": the dot is always there,
 * even when the type (or name) is empty ("MAKEFILE."), and only
 * letters, digits, '$', '_' and '-' may appear. VMS also limits NAME
 * and TYPE to 39 characters each. */
#define ODS2_NAME_PART_MAX 39

/* Validates a file name for a new file and writes its canonical
   on-disk form to `out`: uppercased, with the dot added if missing
   ("makefile" -> "MAKEFILE."). On failure returns false and sets
   *why to a message naming the problem. */
bool ods2_make_file_name(const char *in, char *out, size_t out_size, const char **why);

/* Validates a directory's own name (the part before ".DIR"): 1-39
   letters, digits, '$', '_' or '-'. Sets *why on failure. */
bool ods2_valid_dir_name(const char *name, const char **why);

/* Compares two names the way a directory is ordered: case-insensitive
   byte order, with a name that has no dot compared as if it ended in
   one - so "MAKEFILE" (as ods2v2 once stored it) and "MAKEFILE." are
   the same name. Returns <0, 0, >0 like strcmp. */
int ods2_dir_name_compare(const char *a, size_t a_len, const char *b, size_t b_len);

/* --- Multi-block directories (spec 4.2) ---
 *
 * Entries are sorted across the whole directory, not just within each
 * block: VMS searches a directory by comparing against each block's
 * names and stops looking once it is past where a name would be, so
 * an entry in the wrong block is never found (TYPE gives RMS-E-FNF,
 * ANALYZE/DISK gives BAD_NAMEORDER). */

/* Is `block` empty (no records before its -1 end marker)? */
bool ods2_dir_block_is_empty(const uint8_t *block);

/* Which of `count` consecutive 512-byte blocks `name` belongs in: the
   last non-empty block whose first record sorts at or before `name`,
   or block 0 if `name` sorts before them all. */
unsigned ods2_dir_choose_block(const uint8_t *blocks, unsigned count, const char *name);

/* Splits the records of `src` (a directory block of `src_size` bytes,
   which may be larger than 512 - e.g. a full block with one more
   record inserted into a 1024-byte copy) into two 512-byte blocks `a`
   and `b`, keeping their order and dividing them as evenly as
   possible. Each gets its -1 end marker; the rest is zeroed. Returns
   false if they can't be divided into two blocks that fit (fewer than
   two records, or too large). */
bool ods2_dir_split_block(const uint8_t *src, size_t src_size, uint8_t *a, uint8_t *b);

#endif /* ODS2_DIRECTORY_WRITE_H */
