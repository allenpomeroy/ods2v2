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

/* Encodes a single extent as the SMALLEST retrieval pointer format
 * (spec 3.5.4) that can represent it - Format 1 (4 bytes, up to 256
 * blocks) when it fits, else Format 2 (6 bytes, up to 16384 blocks),
 * else Format 3 (8 bytes, up to 2^30 blocks - enough for any extent
 * this project could realistically need). `out` must have room for
 * the largest possible encoding (8 bytes) even though fewer may
 * actually be written; `*bytes_written_out` reports exactly how many.
 * Returns false (writing nothing) only if `block_count` is zero or
 * exceeds even Format 3's range. Preferring the smallest format that
 * fits keeps ordinary small extents exactly as compact as before
 * (unchanged 4-byte Format 1 encoding) while removing the old
 * artificial 256-block-per-extent ceiling for anything larger. */
bool ods2_encode_retrieval_pointer(uint8_t *out, uint32_t lbn, uint32_t block_count,
                                    size_t *bytes_written_out)
{
    if (ods2_encode_retrieval_pointer_format1(out, lbn, block_count)) {
        *bytes_written_out = 4;
        return true;
    }
    if (ods2_encode_retrieval_pointer_format2(out, lbn, block_count)) {
        *bytes_written_out = 6;
        return true;
    }
    if (ods2_encode_retrieval_pointer_format3(out, lbn, block_count)) {
        *bytes_written_out = 8;
        return true;
    }
    return false;
}

/* Encodes a single extent as a Format 1 retrieval pointer (spec
   3.5.4.2) into `out` (4 bytes / 2 words). Format 1 supports up to
   256 blocks per extent (8-bit count field, n+1) and LBNs up to
   2^22-1 (6-bit high word + 16-bit low word) - sufficient for disks
   up to ~4.19 million blocks, comfortably covering our RA92-class
   test volume (2,940,951 blocks) and most hobbyist-scale ODS-2
   volumes. Larger extents need Format 2 or 3 instead (see
   ods2_encode_retrieval_pointer(), which picks whichever of the
   three actually fits) - most callers should prefer that over calling
   this directly. Returns false (and writes nothing) if `block_count`
   or `lbn` don't fit in Format 1's range - callers must check this
   rather than silently truncating. */
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

/* Encodes a single extent as a Format 2 retrieval pointer (spec
   3.5.4.3) into `out` (6 bytes / 3 words):
     word0 = [format:2 = 0b10][count:14]   (count = block_count - 1)
     word1 = LBN low 16 bits
     word2 = LBN high 16 bits
   14-bit count field: up to 16384 blocks per extent (8MB). Full
   32-bit LBN, unlike Format 1's 22-bit field - not a practical
   concern for this project's volumes, but not artificially
   restricted either. Returns false (and writes nothing) if
   `block_count` is zero or exceeds 16384 - use Format 3 instead. */
bool ods2_encode_retrieval_pointer_format2(uint8_t *out, uint32_t lbn, uint32_t block_count)
{
    uint16_t count, word0, lbn_lo, lbn_hi;

    if (block_count == 0 || block_count > 16384) {
        return false; /* count field is n+1, 14 bits: 1..16384 */
    }

    count = (uint16_t) (block_count - 1);
    word0 = (uint16_t) (2u << 14) | (count & 0x3fffu);
    lbn_lo = (uint16_t) (lbn & 0xffffu);
    lbn_hi = (uint16_t) ((lbn >> 16) & 0xffffu);

    out[0] = (uint8_t) (word0 & 0xff);
    out[1] = (uint8_t) (word0 >> 8);
    out[2] = (uint8_t) (lbn_lo & 0xff);
    out[3] = (uint8_t) (lbn_lo >> 8);
    out[4] = (uint8_t) (lbn_hi & 0xff);
    out[5] = (uint8_t) (lbn_hi >> 8);
    return true;
}

/* Encodes a single extent as a Format 3 retrieval pointer (spec
   3.5.4.4) into `out` (8 bytes / 4 words):
     word0 = [format:2 = 0b11][high_count:14]
     word1 = low_count (16 bits)             (count = block_count - 1,
                                                30 bits total)
     word2 = LBN low 16 bits
     word3 = LBN high 16 bits
   30-bit count field: up to 2^30 blocks per extent (over 500GB) -
   comfortably beyond anything this project's volumes could need, so
   in practice this is the format that can encode literally any
   extent ods2_allocate_blocks() could ever hand back. Returns false
   (and writes nothing) only if `block_count` is zero or exceeds
   2^30. */
bool ods2_encode_retrieval_pointer_format3(uint8_t *out, uint32_t lbn, uint32_t block_count)
{
    uint32_t count;
    uint16_t word0, word1, lbn_lo, lbn_hi;

    if (block_count == 0) return false;
    count = block_count - 1;
    if (count > 0x3fffffffu) return false; /* 30-bit count field */

    word0 = (uint16_t) (3u << 14) | (uint16_t) ((count >> 16) & 0x3fffu);
    word1 = (uint16_t) (count & 0xffffu);
    lbn_lo = (uint16_t) (lbn & 0xffffu);
    lbn_hi = (uint16_t) ((lbn >> 16) & 0xffffu);

    out[0] = (uint8_t) (word0 & 0xff);
    out[1] = (uint8_t) (word0 >> 8);
    out[2] = (uint8_t) (word1 & 0xff);
    out[3] = (uint8_t) (word1 >> 8);
    out[4] = (uint8_t) (lbn_lo & 0xff);
    out[5] = (uint8_t) (lbn_lo >> 8);
    out[6] = (uint8_t) (lbn_hi & 0xff);
    out[7] = (uint8_t) (lbn_hi >> 8);
    return true;
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
