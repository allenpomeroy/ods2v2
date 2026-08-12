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

#include "ods2_checksum.h"

uint16_t ods2_checksum(const uint8_t *data, size_t word_count)
{
    uint32_t sum = 0;
    size_t i;
    /* Read each word explicitly as two little-endian bytes rather than
     * casting to uint16_t* and dereferencing - this is correct
     * regardless of host alignment or endianness, unlike the old
     * codebase's approach of casting straight to unsigned short* and
     * relying on the host being little-endian (true for x86, but not
     * a documented assumption anywhere in that code). */
    for (i = 0; i < word_count; i++) {
        uint16_t w = (uint16_t) data[i * 2] | ((uint16_t) data[i * 2 + 1] << 8);
        sum += w;
    }
    return (uint16_t) (sum & 0xffff);
}
