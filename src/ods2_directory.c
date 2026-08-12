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

#include "ods2_directory.h"
#include <string.h>

static uint16_t read_word(const uint8_t *p)
{
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

int ods2_parse_directory(const uint8_t *data, size_t len,
                          ods2_dir_entry_t *entries_out, size_t max_entries)
{
    size_t pos = 0;
    size_t count = 0;

    while (pos + 6 <= len) {
        uint16_t size = read_word(data + pos);
        uint16_t namecount;
        size_t name_start, name_end, entry_start, record_total;

        if (size == 0xffff) {
            /* Sentinel: end of records in this block. */
            break;
        }

        /* dir$size excludes itself (verified against real data: a
           record with header(6)+name(10)+entry(8)=24 total bytes
           reported dir$size=22, i.e. total-2). */
        record_total = (size_t) size + 2;
        if (pos + record_total > len) {
            return -1; /* declared size runs past the buffer - malformed */
        }

        namecount = data[pos + 5];
        name_start = pos + 6;
        name_end = name_start + namecount;
        if (name_end > len) {
            return -1;
        }
        /* Name is padded to an even byte boundary before the
           per-version entries begin. */
        entry_start = name_end + (name_end % 2);
        if (entry_start + 8 > len || entry_start + 8 > pos + record_total) {
            return -1;
        }

        if (count < max_entries) {
            size_t copy_len = namecount;
            if (copy_len > ODS2_DIR_MAX_NAME) copy_len = ODS2_DIR_MAX_NAME;
            memcpy(entries_out[count].name, data + name_start, copy_len);
            entries_out[count].name[copy_len] = '\0';

            entries_out[count].version = read_word(data + entry_start);
            entries_out[count].fid.fid_num = read_word(data + entry_start + 2);
            entries_out[count].fid.fid_seq = read_word(data + entry_start + 4);
            entries_out[count].fid.fid_rvn = data[entry_start + 6];
            entries_out[count].fid.fid_nmx = data[entry_start + 7];
            count++;
        }

        pos += record_total;
    }

    return (int) count;
}
