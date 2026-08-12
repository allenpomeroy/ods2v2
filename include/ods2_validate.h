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

/* ods2_validate.h - structural validation, independent of read/write
 * paths. The goal: catch what ANALYZE/DISK would catch, locally, in
 * milliseconds, before a single byte goes near real VMS.
 */
#ifndef ODS2_VALIDATE_H
#define ODS2_VALIDATE_H

#include "ods2_ondisk.h"
#include <stdbool.h>

typedef struct {
    bool        ok;
    const char *problem;   /* NULL if ok; a short, human-readable
                               description otherwise (static string,
                               caller must not free) */
} ods2_validate_result_t;

/* Validates a home block's internal consistency: the format
 * identifier string, structure level, and BOTH checksums (checksum1
 * over the first 29 words, checksum2 over the first 255 words) -
 * exactly the field the old project got wrong first, by only ever
 * recomputing checksum2 and leaving checksum1 stale.
 *
 * IMPORTANT ORDERING NOTE for any future code that writes a home
 * block: checksum2's coverage (bytes 0-509) includes checksum1's own
 * position (byte offset 58). This means checksum1 MUST be computed
 * and written first, then checksum2 computed over the result -
 * computing them in the other order, or independently from stale
 * data, leaves checksum2 inconsistent even if checksum1 itself is
 * correct. This was caught by our own validate_selftest failing
 * during development, not inferred from the spec text - a direct
 * demonstration of the "catch it locally" principle this validator
 * exists for. */
ods2_validate_result_t ods2_validate_home(const ods2_home_t *home);

/* Validates a file header's checksum (FH2$W_CHECKSUM, the last word of
 * the 512-byte header, covering everything before it) and, if
 * mapping data is available, that FAT$L_HIBLK is at least roughly
 * consistent with the sum of the header's own retrieval pointers -
 * this specific check is what caught a real, genuine data-corruption
 * bug on an old test disk during development (FAT$L_HIBLK claimed
 * ~7.8 million blocks; the header's own retrieval pointers, decoded
 * independently, summed to 120).
 *
 * `raw_header` must point to the full 512-byte header block (not just
 * the ods2_head_core_t portion) - the checksum covers the whole
 * block, and hiblk cross-validation needs the Map Area, which lives
 * past the core struct.
 */
ods2_validate_result_t ods2_validate_head(const uint8_t *raw_header);

#endif /* ODS2_VALIDATE_H */
