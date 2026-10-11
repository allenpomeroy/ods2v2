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

#include "ods2_retrieval.h"

static uint16_t read_word(const uint8_t *p)
{
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

bool ods2_encode_retrieval_pointer_format1(uint8_t *out, uint32_t lbn, uint32_t block_count)
{
    uint16_t high_lbn, low_lbn, count, word0;

    if (block_count == 0 || block_count > 256) {
        return false; /* count field is n+1, 8 bits: 1..256 */
    }
    if (lbn > 0x3fffffu) {
        return false; /* 6-bit high word + 16-bit low word = 22 bits */
    }

    count = (uint16_t) (block_count - 1);
    low_lbn = (uint16_t) (lbn & 0xffffu);
    high_lbn = (uint16_t) ((lbn >> 16) & 0x3fu);

    word0 = (uint16_t) (1u << 14) | (uint16_t) (high_lbn << 8) | count;

    out[0] = (uint8_t) (word0 & 0xff);
    out[1] = (uint8_t) (word0 >> 8);
    out[2] = (uint8_t) (low_lbn & 0xff);
    out[3] = (uint8_t) (low_lbn >> 8);
    return true;
}

size_t ods2_retrieval_pointer_size(uint32_t lbn, uint32_t block_count)
{
    if (block_count == 0 || block_count > (1u << 30)) {
        return 0; /* count field is n+1: Format 3's 30 bits top out at 2^30 */
    }
    if (block_count <= 256 && lbn < 0x3fffffu) {
        return 4; /* Format 1; LBN 2^22-1 would read as a sparse range */
    }
    if (lbn == 0xffffffffu) {
        return 0; /* 2^32-1 marks a sparse range in Formats 2 and 3 */
    }
    return (block_count <= 16384) ? 6 : 8;
}

size_t ods2_encode_retrieval_pointer(uint8_t *out, uint32_t lbn, uint32_t block_count)
{
    size_t size = ods2_retrieval_pointer_size(lbn, block_count);
    uint32_t n = block_count - 1; /* every format stores n for n+1 blocks */

    if (size == 4) {
        return ods2_encode_retrieval_pointer_format1(out, lbn, block_count) ? 4 : 0;
    }
    if (size == 6) {
        /* Format 2 (spec 3.5.4.3): [10][count:14], then the 32-bit LBN
           as two little-endian words, low word first. */
        uint16_t word0 = (uint16_t) ((2u << 14) | (n & 0x3fffu));
        out[0] = (uint8_t) (word0 & 0xff);
        out[1] = (uint8_t) (word0 >> 8);
        out[2] = (uint8_t) (lbn & 0xff);
        out[3] = (uint8_t) ((lbn >> 8) & 0xff);
        out[4] = (uint8_t) ((lbn >> 16) & 0xff);
        out[5] = (uint8_t) ((lbn >> 24) & 0xff);
        return 6;
    }
    if (size == 8) {
        /* Format 3 (spec 3.5.4.4): [11][high count:14], low count word,
           then the 32-bit LBN as two little-endian words. */
        uint16_t word0 = (uint16_t) ((3u << 14) | ((n >> 16) & 0x3fffu));
        uint16_t low_count = (uint16_t) (n & 0xffffu);
        out[0] = (uint8_t) (word0 & 0xff);
        out[1] = (uint8_t) (word0 >> 8);
        out[2] = (uint8_t) (low_count & 0xff);
        out[3] = (uint8_t) (low_count >> 8);
        out[4] = (uint8_t) (lbn & 0xff);
        out[5] = (uint8_t) ((lbn >> 8) & 0xff);
        out[6] = (uint8_t) ((lbn >> 16) & 0xff);
        out[7] = (uint8_t) ((lbn >> 24) & 0xff);
        return 8;
    }
    return 0;
}

int ods2_decode_retrieval_pointers(const uint8_t *map_area, size_t word_count,
                                    ods2_extent_t *extents_out, size_t max_extents)
{
    size_t word_pos = 0;
    size_t extent_count = 0;

    while (word_pos < word_count) {
        uint16_t word0 = read_word(map_area + word_pos * 2);
        unsigned format = word0 >> 14;

        if (format == 0) {
            /* Format 0: placement control data for the pointer that
               follows. Not an extent itself - just skip it. */
            word_pos += 1;
            continue;
        } else if (format == 1) {
            /* Format 1 (spec 3.5.4.2), confirmed against real data:
               word0 = [format:2][high_lbn:6][count:8]
               word1 = low_lbn (16 bits)
               LBN = (high_lbn << 16) | low_lbn, block_count = count+1 */
            uint16_t word1;
            unsigned high_lbn, count;
            if (word_pos + 2 > word_count) return -1; /* truncated */
            word1 = read_word(map_area + (word_pos + 1) * 2);
            high_lbn = (word0 >> 8) & 0x3f;
            count = word0 & 0xff;
            if (extent_count < max_extents) {
                extents_out[extent_count].lbn = ((uint32_t) high_lbn << 16) | word1;
                extents_out[extent_count].block_count = count + 1;
                extent_count++;
            }
            word_pos += 2;
        } else if (format == 2) {
            /* Format 2 (spec 3.5.4.3):
               word0 = [format:2][count:14]
               word1,word2 = LBN (32 bits, LE across the two words) */
            uint16_t w1, w2;
            unsigned count;
            if (word_pos + 3 > word_count) return -1;
            w1 = read_word(map_area + (word_pos + 1) * 2);
            w2 = read_word(map_area + (word_pos + 2) * 2);
            count = word0 & 0x3fff;
            if (extent_count < max_extents) {
                extents_out[extent_count].lbn = (uint32_t) w1 | ((uint32_t) w2 << 16);
                extents_out[extent_count].block_count = count + 1;
                extent_count++;
            }
            word_pos += 3;
        } else { /* format == 3 */
            /* Format 3 (spec 3.5.4.4):
               word0 = [format:2][high_count:14]
               word1 = low_count (16 bits)
               word2,word3 = LBN (32 bits, LE) */
            uint16_t w1, w2, w3;
            uint32_t count;
            if (word_pos + 4 > word_count) return -1;
            w1 = read_word(map_area + (word_pos + 1) * 2);
            w2 = read_word(map_area + (word_pos + 2) * 2);
            w3 = read_word(map_area + (word_pos + 3) * 2);
            count = ((uint32_t) (word0 & 0x3fff) << 16) | w1;
            if (extent_count < max_extents) {
                extents_out[extent_count].lbn = (uint32_t) w2 | ((uint32_t) w3 << 16);
                extents_out[extent_count].block_count = count + 1;
                extent_count++;
            }
            word_pos += 4;
        }
    }

    return (int) extent_count;
}
