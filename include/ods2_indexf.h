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

/* ods2_indexf.h - locating file headers within INDEXF.SYS.
 *
 * Per spec section 5.1.7: "The file header for file number n is
 * located at virtual block v*4+m+n" (v = storage map cluster factor,
 * m = number of blocks in the index file bitmap). The bitmap itself
 * starts at virtual block v*4+1, at absolute LBN HM2$L_IBMAPLBN (given
 * directly in the home block - this is exactly the mechanism that
 * lets the first 16 file headers be located without needing to have
 * already read INDEXF.SYS's own header, solving the obvious
 * bootstrapping problem of "you need the index file's header to find
 * anything, including itself").
 *
 * Converting the VBN formula to an absolute LBN: since the bitmap's
 * first block (VBN v*4+1) is at LBN ibmaplbn, file header n (at VBN
 * v*4+m+n) is (m+n-1) blocks after that:
 *     header_lbn(n) = ibmaplbn + (m + n - 1)
 */
#ifndef ODS2_INDEXF_H
#define ODS2_INDEXF_H

#include "ods2_ondisk.h"
#include <stdint.h>

/* Returns the absolute LBN of the file header for file number
 * `file_number` (1-based - file 1 is INDEXF.SYS itself). Only valid
 * for file_number 1-16 per the spec; headers beyond 16 require
 * reading INDEXF.SYS's own mapping pointers, which this function
 * deliberately does not attempt (that's a real read operation on
 * index file content, not a pure home-block computation). */
uint32_t ods2_indexf_header_lbn(const ods2_home_t *home, unsigned file_number);

#endif /* ODS2_INDEXF_H */
