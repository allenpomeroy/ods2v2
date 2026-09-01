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
    ods2_fid_t     backlink;   /* parent directory's FID - EXCEPT for an
                                   extension header (is_extension==true),
                                   where spec 3.5.2.16 requires this to
                                   be the FID of the file's PRIMARY
                                   header instead. */
    ods2_fid_t     ext_fid;    /* FH2$W_EXT_FID - FID of this header's own
                                   extension header, or zero (the default,
                                   from a memset spec) if this is the
                                   last (or only) header of the file. */
    uint16_t       seg_num;    /* FH2$W_SEG_NUM - 0 for the primary
                                   header, 1, 2, 3... for successive
                                   extension headers (spec 3.5.2.5). */
    bool           is_extension; /* false: normal, full-size Ident Area
                                   (120 bytes) as before. true: Ident
                                   Area truncated to zero length (spec
                                   3.5.3: "customarily truncated in
                                   extension headers"), freeing an
                                   extra 120 bytes/30 words for map
                                   pointers - see
                                   ODS2_EXT_HEADER_MPOFFSET_BYTES. */
    uint32_t       filechar;   /* FH2$L_FILECHAR bits */
    uint8_t        rtype;      /* FAT$B_RTYPE */
    uint8_t        rattrib;    /* FAT$B_RATTRIB */
    uint16_t       rsize;      /* FAT$W_RSIZE */
    uint16_t       maxrec;     /* FAT$W_MAXREC */
    const ods2_extent_t *extents; /* only the extents THIS header segment
                                   maps - the caller splits the file's
                                   full extent list across segments. */
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
                                   happen in practice). Ignored entirely
                                   when is_extension is true (no Ident
                                   Area exists to put it in). */
} ods2_header_spec_t;

/* Word/byte offsets for the two header "shapes" this code produces.
 * Primary headers always use the full-size Ident Area (unchanged from
 * before); extension headers truncate it to zero length per spec
 * 3.5.3, starting the Map Area right after the fixed Header Area
 * instead. Both are exposed here so callers (ods2_volume.c's
 * multi-header allocation loop) can compute how many extents fit in
 * each kind of segment without duplicating the arithmetic. */
#define ODS2_HEADER_IDOFFSET_BYTES     80
#define ODS2_HEADER_IDENT_BYTES        120
#define ODS2_HEADER_MPOFFSET_BYTES     (ODS2_HEADER_IDOFFSET_BYTES + ODS2_HEADER_IDENT_BYTES)
#define ODS2_EXT_HEADER_MPOFFSET_BYTES ODS2_HEADER_IDOFFSET_BYTES

/* Format 1 retrieval pointers are 4 bytes each; the Map Area runs up
 * to byte 510 (the last 2 bytes are the header checksum). */
#define ODS2_HEADER_MAX_EXTENTS     (((510 - ODS2_HEADER_MPOFFSET_BYTES) / 4))
#define ODS2_EXT_HEADER_MAX_EXTENTS (((510 - ODS2_EXT_HEADER_MPOFFSET_BYTES) / 4))

/* Builds a complete, checksummed 512-byte header into `header_out`
 * from `spec`. Returns false if the extents don't fit (each must
 * individually satisfy Format 1's limits - see
 * ods2_encode_retrieval_pointer_format1()) or there isn't room for
 * all of them in the Map Area (see ODS2_HEADER_MAX_EXTENTS /
 * ODS2_EXT_HEADER_MAX_EXTENTS for the per-shape capacity). */
bool ods2_build_file_header(uint8_t *header_out, const ods2_header_spec_t *spec);

#endif /* ODS2_HEADER_BUILD_H */

