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

/* ods2_header_build.h - constructs a valid 512-byte file header from
 * scratch, for writing a new file/directory to disk. Every field
 * written here corresponds to something verified on the read side
 * against real data earlier in this project.
 */
#ifndef ODS2_HEADER_BUILD_H
#define ODS2_HEADER_BUILD_H

#include "ods2_ondisk.h"
#include "ods2_retrieval.h"
#include <stdbool.h>

typedef struct {
    ods2_fid_t     fid;        /* this file's own FID */
    ods2_fid_t     backlink;   /* parent directory's FID */
    uint32_t       filechar;   /* FH2$L_FILECHAR bits */
    uint8_t        rtype;      /* FAT$B_RTYPE */
    uint8_t        rattrib;    /* FAT$B_RATTRIB */
    uint16_t       rsize;      /* FAT$W_RSIZE */
    uint16_t       maxrec;     /* FAT$W_MAXREC */
    const ods2_extent_t *extents;
    int            extent_count;
    uint32_t       hiblk;      /* total allocated blocks (plain value -
                                   word-swap is applied internally) */
    uint32_t       efblk;      /* logical EOF VBN (plain value - also
                                   word-swapped internally) */
    uint16_t       ffbyte;
    const char    *ident_name; /* e.g. "ODS2V2TEST.DIR;1" - the file's
                                   own identification string, written
                                   into the Ident Area. Confirmed
                                   against two real VMS-written headers
                                   (root, INDEXF.SYS) that this area is
                                   ALWAYS a fixed 120 bytes
                                   (idoffset=40 words, mpoffset=100
                                   words), space-padded - not sized to
                                   the name's actual length. Must fit
                                   in 120 bytes; longer names are
                                   truncated (matching VMS's own
                                   ODS-2 39-char name / 39-char type
                                   limits, this should never actually
                                   happen in practice). */
} ods2_header_spec_t;

/* Builds a complete, checksummed 512-byte header into `header_out`
 * from `spec`. Returns false if the extents don't fit (each must
 * individually satisfy Format 1's limits - see
 * ods2_encode_retrieval_pointer_format1()) or there isn't room for
 * all of them in the Map Area. */
bool ods2_build_file_header(uint8_t *header_out, const ods2_header_spec_t *spec);

#endif /* ODS2_HEADER_BUILD_H */
