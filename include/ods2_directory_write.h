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

/* Removes the directory entry matching `name` (case-insensitive) from
 * `block`, shifting all following bytes (through the sentinel) back
 * to close the gap - the exact inverse of ods2_insert_dir_entry().
 * Returns true if a matching entry was found and removed, false if no
 * entry with that name exists in this block (callers checking
 * multiple blocks should treat false as "not in this one, try the
 * next" rather than an error).
 */
bool ods2_remove_dir_entry(uint8_t *block, size_t block_size, const char *name);

#endif /* ODS2_DIRECTORY_WRITE_H */
