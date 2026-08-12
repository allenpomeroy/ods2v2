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

/* ods2_bitmap.h - storage bitmap search/allocation logic (spec 5.2.2).
 * Bit convention (confirmed by spec text, section 5.2.2): a SET bit
 * means the corresponding cluster is FREE; a CLEAR bit means
 * allocated. Bits are packed right-to-left within each byte
 * (bit 0 of byte 0 is the lowest-numbered cluster).
 */
#ifndef ODS2_BITMAP_H
#define ODS2_BITMAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Searches `bitmap` (total_bits bits, packed as described above) for
 * the first run of at least `count` consecutive free (set) bits.
 * On success, returns true and sets *start_bit to the index of the
 * FIRST bit of that run (not the last - this is the exact distinction
 * the old codebase got backwards). *found_count is set to `count`
 * itself on success (the search stops as soon as it finds enough
 * bits, first-fit style - it does not report a longer extent even if
 * the underlying free run continues further; a caller wanting to know
 * the full run length would need a separate query). Returns false if
 * no run of the requested length exists anywhere in the bitmap.
 */
bool ods2_bitmap_find_free(const uint8_t *bitmap, size_t total_bits,
                            size_t count, size_t *start_bit, size_t *found_count);

/* Marks `count` bits starting at `start_bit` as either free (set,
   `used=false`) or allocated (clear, `used=true`). Returns false if
   the range would run past total_bits (caller error - never silently
   clips), true otherwise. This is the write counterpart to
   ods2_bitmap_find_free() - deliberately still pure/synchronous, no
   file I/O, so it stays trivially testable in isolation; the volume
   layer is responsible for reading the real bitmap in, calling this,
   and writing the modified bytes back out. */
bool ods2_bitmap_mark(uint8_t *bitmap, size_t total_bits,
                       size_t start_bit, size_t count, bool used);

#endif /* ODS2_BITMAP_H */
