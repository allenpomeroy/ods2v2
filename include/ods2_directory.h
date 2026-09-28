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

/* ods2_directory.h - parses directory file content (spec 4.2/4.3)
 * into a simple list of (name, version, fid) entries.
 */
#ifndef ODS2_DIRECTORY_H
#define ODS2_DIRECTORY_H

#include "ods2_ondisk.h"
#include <stddef.h>

#define ODS2_DIR_MAX_NAME 80

typedef struct {
    char       name[ODS2_DIR_MAX_NAME + 1]; /* nul-terminated */
    uint16_t   version;
    ods2_fid_t fid;
} ods2_dir_entry_t;

/* Upper bound on how many ENTRIES one 512-byte directory block can
 * physically hold. An entry is one (name, version, FID) triple, and a
 * directory record is one name followed by one or more 8-byte version
 * entries (spec 4.2/4.3, newest version first). The densest possible
 * block is a single record with an empty name: 6-byte record header
 * plus 8 bytes per version, so at most (512-6)/8 = 63 entries. Every
 * additional record only adds header bytes, so no block can hold
 * more. Callers that parse block by block use this to size a
 * per-block buffer that never truncates. */
#define ODS2_DIR_MAX_ENTRIES_PER_BLOCK 63

/* Highest file version ODS-2 allows (versions are 1..32767). */
#define ODS2_MAX_VERSION 32767

/* Parses directory records starting at `data` (raw block bytes) for
 * up to `len` bytes, writing entries into `entries_out` (up to
 * `max_entries`). Stops at the first sentinel record (dir$size ==
 * 0xffff) or when `len` is exhausted.
 *
 * Produces ONE ENTRY PER VERSION, not one per record: a record holds
 * a name followed by every (version, FID) pair that fits, newest
 * first, and each pair becomes its own entry carrying the record's
 * name. (An earlier version read only the first pair of each record,
 * so a file with many versions showed up once per record - e.g. a
 * 64-version file split across two records listed as 2 entries.)
 * Entries come out in on-disk order: records sorted by name, and
 * versions within a name newest first.
 *
 * Returns the TOTAL number of entries found in the block - which can
 * be larger than `max_entries`. Only the first `max_entries` are
 * stored; a return value greater than `max_entries` is how a caller
 * knows entries were dropped.
 *
 * Returns -1 if a record's declared size would run past `len`, or a
 * record holds no complete version entry (a genuinely malformed or
 * truncated block - callers should treat this as a real error, not a
 * soft empty-directory case). */
int ods2_parse_directory(const uint8_t *data, size_t len,
                          ods2_dir_entry_t *entries_out, size_t max_entries);

#endif /* ODS2_DIRECTORY_H */
