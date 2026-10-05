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

/* Character `i` of a name's canonical form: uppercased, and with a
   trailing dot if it has none. Returns -1 past the end. */
static int canonical_char(const char *s, size_t len, bool has_dot, size_t i)
{
    if (i < len) return toupper((unsigned char) s[i]);
    if (i == len && !has_dot) return '.';
    return -1;
}

int ods2_dir_name_compare(const char *a, size_t a_len, const char *b, size_t b_len)
{
    bool a_dot = memchr(a, '.', a_len) != NULL;
    bool b_dot = memchr(b, '.', b_len) != NULL;
    size_t i;
    for (i = 0;; i++) {
        int ca = canonical_char(a, a_len, a_dot, i);
        int cb = canonical_char(b, b_len, b_dot, i);
        if (ca != cb) return ca - cb; /* -1 (ended) sorts first */
        if (ca == -1) return 0;
    }
}

/* An on-disk record's name (namecount bytes, not nul-terminated)
   against a caller's nul-terminated name. */
static int compare_names(const uint8_t *disk_name, size_t disk_len, const char *name)
{
    return ods2_dir_name_compare((const char *) disk_name, disk_len, name, strlen(name));
}

static bool name_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '$' || c == '_' || c == '-';
}

/* Explains a bad character, with a hint for the common case of a
   host path typed as a file name. */
static const char *bad_char_reason(char c)
{
    if (c == '/' || c == '\\') {
        return "'/' is not allowed in an ODS-2 name - put the file in a "
               "subdirectory instead, e.g. [TEST1.SRC]FILE.C";
    }
    return "an ODS-2 name may only contain letters, digits, '$', '_' and '-' "
           "(plus one '.' before the type)";
}

bool ods2_make_file_name(const char *in, char *out, size_t out_size, const char **why)
{
    size_t len = strlen(in), i, name_len = 0, type_len = 0;
    bool seen_dot = false;

    if (len + 2 > out_size) {
        *why = "file name too long";
        return false;
    }
    for (i = 0; i < len; i++) {
        char c = (char) toupper((unsigned char) in[i]);
        if (c == '.') {
            if (seen_dot) {
                *why = "an ODS-2 name has only one '.', between the name and the type";
                return false;
            }
            seen_dot = true;
        } else if (!name_char_ok(c)) {
            *why = bad_char_reason(c);
            return false;
        } else if (seen_dot) {
            type_len++;
        } else {
            name_len++;
        }
        out[i] = c;
    }
    if (!seen_dot) out[len++] = '.'; /* "MAKEFILE" is stored as "MAKEFILE." */
    out[len] = '\0';
    if (name_len == 0 && type_len == 0) {
        *why = "empty file name";
        return false;
    }
    if (name_len > ODS2_NAME_PART_MAX || type_len > ODS2_NAME_PART_MAX) {
        *why = "an ODS-2 file name and type are each limited to 39 characters";
        return false;
    }
    return true;
}

bool ods2_valid_dir_name(const char *name, const char **why)
{
    size_t len = strlen(name), i;
    if (len == 0) {
        *why = "empty directory name";
        return false;
    }
    if (len > ODS2_NAME_PART_MAX) {
        *why = "an ODS-2 directory name is limited to 39 characters";
        return false;
    }
    for (i = 0; i < len; i++) {
        char c = (char) toupper((unsigned char) name[i]);
        if (!name_char_ok(c)) {
            *why = (c == '.') ? "a directory name cannot contain '.' (use [A.B] for "
                                "a subdirectory)"
                              : bad_char_reason(c);
            return false;
        }
    }
    return true;
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

bool ods2_remove_dir_version(uint8_t *block, size_t block_size, const char *name,
                             uint16_t version)
{
    size_t pos = 0;
    size_t end_pos;
    size_t record_pos = (size_t) -1;
    size_t record_total = 0;
    size_t pair_pos = (size_t) -1;

    /* Find the record for `name` that holds `version`, remembering
       where the used-content region ends (the sentinel, or where
       scanning stops for lack of a real one). */
    while (pos + 6 <= block_size) {
        uint16_t size = read_word(block + pos);
        size_t this_total;
        uint8_t namecount;
        if (size == 0xffff || size == 0) {
            break;
        }
        this_total = (size_t) size + 2;
        if (pos + this_total > block_size) {
            return false; /* malformed - refuse to edit */
        }
        namecount = block[pos + 5];
        if (pair_pos == (size_t) -1 &&
            compare_names(block + pos + 6, namecount, name) == 0) {
            size_t name_end = pos + 6 + namecount;
            size_t e = name_end + (name_end % 2);
            for (; e + 8 <= pos + this_total; e += 8) {
                if (read_word(block + e) == version) {
                    record_pos = pos;
                    record_total = this_total;
                    pair_pos = e;
                    break;
                }
            }
        }
        pos += this_total;
    }
    end_pos = pos;

    if (pair_pos == (size_t) -1) {
        return false; /* not in this block */
    }

    {
        size_t name_end = record_pos + 6 + block[record_pos + 5];
        size_t first_pair = name_end + (name_end % 2);
        size_t pair_count = (record_pos + record_total - first_pair) / 8;
        size_t tail_end = (end_pos + 2 <= block_size) ? end_pos + 2 : block_size;

        if (pair_count <= 1) {
            /* Last version in this record: drop the record itself. */
            return ods2_remove_dir_entry(block, block_size, name);
        }

        /* Close the 8-byte gap (through the sentinel's own 2 bytes),
           shrink the record's dir$size, and zero the vacated tail so
           it can never be misread as a further record. */
        memmove(block + pair_pos, block + pair_pos + 8, tail_end - (pair_pos + 8));
        memset(block + tail_end - 8, 0, 8);
        write_word(block + record_pos, (uint16_t) (record_total - 8 - 2));
        /* Content now ends 8 bytes earlier. Put a real sentinel there
           even if the block was completely full and had none before,
           so the zeroed tail is never parsed as a record. */
        write_word(block + end_pos - 8, 0xffff);
    }
    return true;
}

/* --- Multi-block directories --- */

/* Walks the records of a block: returns the number of records and
   fills offsets[]/sizes[] (record start, total bytes including the
   size word) when given. Stops at the -1 end marker, a zero size
   word, or anything that would run past the block. */
static int block_records(const uint8_t *block, size_t block_size, size_t *offsets,
                         size_t *sizes, int max)
{
    size_t pos = 0;
    int n = 0;
    while (pos + 2 <= block_size) {
        uint16_t size = read_word(block + pos);
        if (size == 0xffff || size == 0 || pos + (size_t) size + 2 > block_size) break;
        if (offsets != NULL && n < max) {
            offsets[n] = pos;
            sizes[n] = (size_t) size + 2;
        }
        n++;
        pos += (size_t) size + 2;
    }
    return n;
}

bool ods2_dir_block_is_empty(const uint8_t *block)
{
    return block_records(block, 512, NULL, NULL, 0) == 0;
}

unsigned ods2_dir_choose_block(const uint8_t *blocks, unsigned count, const char *name)
{
    unsigned i, choice = 0;
    for (i = 0; i < count; i++) {
        const uint8_t *b = blocks + (size_t) i * 512;
        if (ods2_dir_block_is_empty(b)) continue;
        if (compare_names(b + 6, b[5], name) <= 0) {
            choice = i;
        } else {
            break; /* blocks are in order: none further can qualify */
        }
    }
    return choice;
}

bool ods2_dir_split_block(const uint8_t *src, size_t src_size, uint8_t *a, uint8_t *b)
{
    /* A 1024-byte block holds at most 1024 / 16 records (the smallest
       record is 16 bytes). */
    size_t offsets[128], sizes[128], total = 0, before = 0, best_diff = (size_t) -1;
    int n = block_records(src, src_size, offsets, sizes, 128), i, best_k = -1;

    if (n < 2 || n > 128) return false;
    for (i = 0; i < n; i++) total += sizes[i];
    /* Split before record k: records [0, k) go to a, [k, n) to b. Each
       block needs 2 bytes for its end marker. */
    for (i = 1; i < n; i++) {
        size_t diff;
        before += sizes[i - 1];
        if (before + 2 > 512 || total - before + 2 > 512) continue;
        diff = (before > total - before) ? before - (total - before) : (total - before) - before;
        if (diff < best_diff) {
            best_diff = diff;
            best_k = i;
        }
    }
    if (best_k < 0) return false;

    memset(a, 0, 512);
    memset(b, 0, 512);
    before = offsets[best_k];                         /* bytes of records [0, k) */
    memcpy(a, src, before);
    write_word(a + before, 0xffff);
    memcpy(b, src + before, total - before);
    write_word(b + (total - before), 0xffff);
    return true;
}
