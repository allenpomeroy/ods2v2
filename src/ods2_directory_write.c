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

#include "ods2_directory_write.h"
#include <string.h>
#include <ctype.h>

static uint16_t read_word(const uint8_t *p)
{
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}
static void write_word(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t) (v & 0xff);
    p[1] = (uint8_t) (v >> 8);
}

/* Case-insensitive comparison of an on-disk record's name (namecount
   bytes, not nul-terminated) against a caller's nul-terminated name
   string. Returns <0, 0, >0 like strcmp - shorter-but-matching-prefix
   sorts first, matching normal string ordering. */
static int compare_names(const uint8_t *disk_name, size_t disk_len, const char *name)
{
    size_t name_len = strlen(name);
    size_t min_len = (disk_len < name_len) ? disk_len : name_len;
    size_t i;
    for (i = 0; i < min_len; i++) {
        int a = toupper(disk_name[i]);
        int b = toupper((unsigned char) name[i]);
        if (a != b) return a - b;
    }
    if (disk_len != name_len) return (int) disk_len - (int) name_len;
    return 0;
}

bool ods2_insert_dir_entry(uint8_t *block, size_t block_size,
                            const char *name, uint16_t version, ods2_fid_t fid)
{
    size_t pos = 0;
    size_t insert_pos = (size_t) -1; /* not yet found */
    size_t end_pos; /* position of the sentinel (or end of valid content) */
    size_t namecount = strlen(name);
    size_t name_padded = namecount + (namecount % 2);
    size_t record_total = 6 + name_padded + 8;
    bool had_real_sentinel = false;

    if (namecount > 255) {
        return false; /* DIR$B_NAMECOUNT is a single byte */
    }

    /* Real VMS requires directory records within a block to be
       stored in sorted (alphabetical, case-insensitive) order by
       name - confirmed the hard way: ANALYZE/DISK's BAD_NAMEORDER
       finding on a real volume, from an earlier version of this
       function that simply appended new entries at the end
       regardless of alphabetical position. Scan through existing
       records, remembering the first one that should sort AFTER the
       new entry - that's where the new record needs to go, with
       everything from there through the sentinel shifted forward to
       make room. */
    while (pos + 6 <= block_size) {
        uint16_t size = read_word(block + pos);
        uint8_t existing_namecount;
        if (size == 0xffff) {
            had_real_sentinel = true;
            break;
        }
        if (size == 0) {
            break; /* empty/uninitialized rest of block, no real sentinel yet */
        }
        existing_namecount = block[pos + 5];
        if (insert_pos == (size_t) -1 &&
            compare_names(block + pos + 6, existing_namecount, name) > 0) {
            insert_pos = pos;
        }
        pos += (size_t) size + 2;
    }
    end_pos = pos; /* sentinel (or end-of-content) position */
    if (insert_pos == (size_t) -1) {
        insert_pos = end_pos; /* new entry sorts after everything existing */
    }

    /* Need room for the new record AND the (possibly relocated)
       trailing 2-byte sentinel. */
    if (end_pos + record_total + 2 > block_size) {
        return false;
    }

    /* Shift everything from insert_pos through the sentinel's own 2
       bytes forward by record_total bytes, opening up exactly enough
       room for the new record at insert_pos. If there was no real
       sentinel yet (a fresh/empty block), this just relocates
       harmless zero bytes - the explicit write below establishes a
       real one either way. */
    memmove(block + insert_pos + record_total, block + insert_pos,
            (end_pos + 2) - insert_pos);

    write_word(block + insert_pos, (uint16_t) (record_total - 2));
    write_word(block + insert_pos + 2, 1);      /* verlimit - spec says "ignored", but
                                                     every real VMS-written record examined
                                                     shows 1, never 0; since ANALYZE/DISK's
                                                     BAD_DIRTYPE finding suggests this
                                                     field is validated more strictly
                                                     than the spec implies */
    block[insert_pos + 4] = 0;                   /* flags - confirmed must be 0 */
    block[insert_pos + 5] = (uint8_t) namecount;
    memcpy(block + insert_pos + 6, name, namecount);
    if (name_padded > namecount) {
        block[insert_pos + 6 + namecount] = 0;    /* padding byte */
    }

    {
        size_t entry_pos = insert_pos + 6 + name_padded;
        write_word(block + entry_pos, version);
        write_word(block + entry_pos + 2, fid.fid_num);
        write_word(block + entry_pos + 4, fid.fid_seq);
        block[entry_pos + 6] = fid.fid_rvn;
        block[entry_pos + 7] = fid.fid_nmx;
    }

    /* If insert_pos was at the very end (appending, not inserting
       before something) and there was no real sentinel already
       carried forward by the memmove above, write one explicitly. */
    if (insert_pos == end_pos && !had_real_sentinel) {
        write_word(block + insert_pos + record_total, 0xffff);
    }

    return true;
}

bool ods2_remove_dir_entry(uint8_t *block, size_t block_size, const char *name)
{
    size_t pos = 0;
    size_t match_pos = (size_t) -1;
    size_t match_record_total = 0;
    size_t end_pos;

    /* Scan for a record matching name, remembering where the whole
       used-content region ends (the sentinel, or the point scanning
       stops for lack of a real one). */
    while (pos + 6 <= block_size) {
        uint16_t size = read_word(block + pos);
        uint8_t namecount;
        if (size == 0xffff || size == 0) {
            break;
        }
        namecount = block[pos + 5];
        if (match_pos == (size_t) -1 &&
            compare_names(block + pos + 6, namecount, name) == 0) {
            match_pos = pos;
            match_record_total = (size_t) size + 2;
        }
        pos += (size_t) size + 2;
    }
    end_pos = pos;

    if (match_pos == (size_t) -1) {
        return false; /* not found in this block */
    }

    /* Shift everything after the matched record (through the
       sentinel's own 2 bytes) back by the matched record's size,
       closing the gap exactly - the inverse of the insert-side shift. */
    memmove(block + match_pos, block + match_pos + match_record_total,
            (end_pos + 2) - (match_pos + match_record_total));

    /* The region the shift vacated at the tail is now stale/garbage -
       zero it so it can never be misread as a further record if
       anything ever scans past where it should stop. */
    memset(block + end_pos + 2 - match_record_total, 0, match_record_total);

    return true;
}
