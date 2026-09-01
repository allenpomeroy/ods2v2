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

/* ods2_retrieval.h - decodes and encodes the Map Area's retrieval
 * pointers (spec 3.5.4). Each pointer describes one extent: a
 * starting LBN and a block count. Format 1 confirmed against real
 * INDEXF.SYS header data - every decoded extent's LBN exactly matched
 * a separately-known-good field (HM2$L_ALTIDXLBN, HM2$L_IBMAPLBN).
 * Formats 2 and 3 are implemented per spec on both the decode and
 * encode side but haven't yet been cross-checked against a real
 * VMS-written header actually using them (Format 1 alone was
 * sufficient for every real header examined so far, since none
 * needed a single extent over 256 blocks).
 */
#ifndef ODS2_RETRIEVAL_H
#define ODS2_RETRIEVAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint32_t lbn;         /* starting logical block number of this extent */
    uint32_t block_count; /* number of blocks in this extent */
} ods2_extent_t;

/* Decodes `word_count` 16-bit words of retrieval-pointer data starting
 * at `map_area` (raw bytes, little-endian words) into extents, writing
 * up to `max_extents` results into `extents_out`. Returns the number
 * of extents actually decoded, or -1 on a truncated/malformed pointer
 * (Format 0 - placement data - is skipped, not decoded as an extent,
 * since it describes allocation hints, not an extent itself).
 */
int ods2_decode_retrieval_pointers(const uint8_t *map_area, size_t word_count,
                                    ods2_extent_t *extents_out, size_t max_extents);

/* Encodes a single extent as the smallest retrieval pointer format
 * (spec 3.5.4) that can represent it, writing between 4 and 8 bytes
 * into `out` (which must have room for the maximum, 8) and reporting
 * the actual size via `*bytes_written_out`. This is the function
 * almost every caller should use - see its definition in
 * ods2_retrieval.c for exactly which format gets picked and why.
 * Returns false (writing nothing) only if `block_count` is zero or
 * exceeds even Format 3's ~2^30-block range. */
bool ods2_encode_retrieval_pointer(uint8_t *out, uint32_t lbn, uint32_t block_count,
                                    size_t *bytes_written_out);

/* Encodes a single extent as a Format 1 retrieval pointer (spec
   3.5.4.2) into `out` (4 bytes / 2 words). Format 1 supports up to
   256 blocks per extent (8-bit count field, n+1) and LBNs up to
   2^22-1 (6-bit high word + 16-bit low word). Returns false (and
   writes nothing) if `block_count` or `lbn` don't fit in Format 1's
   range - most callers should use ods2_encode_retrieval_pointer()
   instead, which falls back to Format 2/3 automatically. */
bool ods2_encode_retrieval_pointer_format1(uint8_t *out, uint32_t lbn, uint32_t block_count);

/* Encodes a single extent as a Format 2 retrieval pointer (spec
   3.5.4.3) into `out` (6 bytes / 3 words). Up to 16384 blocks per
   extent (14-bit count field, n+1), full 32-bit LBN. Returns false
   (and writes nothing) if `block_count` is zero or exceeds 16384. */
bool ods2_encode_retrieval_pointer_format2(uint8_t *out, uint32_t lbn, uint32_t block_count);

/* Encodes a single extent as a Format 3 retrieval pointer (spec
   3.5.4.4) into `out` (8 bytes / 4 words). Up to 2^30 blocks per
   extent (30-bit count field, n+1), full 32-bit LBN. Returns false
   (and writes nothing) if `block_count` is zero or exceeds 2^30. */
bool ods2_encode_retrieval_pointer_format3(uint8_t *out, uint32_t lbn, uint32_t block_count);

#endif /* ODS2_RETRIEVAL_H */
