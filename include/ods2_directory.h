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

/* Parses directory records starting at `data` (raw block bytes) for
 * up to `len` bytes, writing entries into `entries_out` (up to
 * `max_entries`). Stops at the first sentinel record (dir$size ==
 * 0xffff) or when `len` is exhausted. Returns the number of entries
 * found, or -1 if a record's declared size would run past `len`
 * (a genuinely malformed/truncated block - callers should treat this
 * as a real error, not a soft empty-directory case). */
int ods2_parse_directory(const uint8_t *data, size_t len,
                          ods2_dir_entry_t *entries_out, size_t max_entries);

#endif /* ODS2_DIRECTORY_H */
