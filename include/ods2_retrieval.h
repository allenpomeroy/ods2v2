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

/* ods2_retrieval.h - decodes the Map Area's retrieval pointers (spec
 * 3.5.4). Each pointer describes one extent: a starting LBN and a
 * block count. Format 1 decoding confirmed against real INDEXF.SYS
 * header data - every decoded extent's LBN exactly matched a
 * separately-known-good field (HM2$L_ALTIDXLBN, HM2$L_IBMAPLBN).
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
 * of extents actually decoded, or -1 on a format it doesn't recognize
 * (Format 0 - placement data - is skipped, not decoded as an extent,
 * since it describes allocation hints, not an extent itself; Formats
 * 2 and 3 are not yet implemented - only Format 1 has been verified
 * against real data so far).
 */
int ods2_decode_retrieval_pointers(const uint8_t *map_area, size_t word_count,
                                    ods2_extent_t *extents_out, size_t max_extents);

/* Encodes a single extent as a Format 1 retrieval pointer (spec
   3.5.4.2) into `out` (4 bytes / 2 words). Format 1 supports up to
   256 blocks per extent (8-bit count field, n+1) and LBNs up to
   2^22-1 (6-bit high word + 16-bit low word) - sufficient for disks
   up to ~4.19 million blocks, comfortably covering our RA92-class
   test volume (2,940,951 blocks) and most hobbyist-scale ODS-2
   volumes. Larger extents or LBNs need Format 2/3, not yet
   implemented on the write side. Returns false (and writes nothing)
   if `block_count` or `lbn` don't fit in Format 1's range - callers
   must check this rather than silently truncating. */
bool ods2_encode_retrieval_pointer_format1(uint8_t *out, uint32_t lbn, uint32_t block_count);

#endif /* ODS2_RETRIEVAL_H */
