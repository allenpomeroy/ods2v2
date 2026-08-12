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

/* ods2_checksum.h - the ODS2 checksum algorithm: sum of N 16-bit words,
 * truncated to 16 bits. Used for the home block's two checksums and
 * every file header's single checksum. Spec section 5.1.9.30 area /
 * 3.5.2 area describe the coverage ranges (which words); this file is
 * just the summation itself, deliberately kept separate from any
 * particular structure so it can't accidentally depend on struct
 * layout details.
 */
#ifndef ODS2_CHECKSUM_H
#define ODS2_CHECKSUM_H

#include <stdint.h>
#include <stddef.h>

/* Computes the ODS2 checksum over `word_count` little-endian 16-bit
 * words starting at `data`. `data` need not be aligned. */
uint16_t ods2_checksum(const uint8_t *data, size_t word_count);

#endif /* ODS2_CHECKSUM_H */
