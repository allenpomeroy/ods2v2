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

#include "ods2_header_build.h"
#include "ods2_checksum.h"
#include <string.h>
#include <time.h>

/* Header layout matching real VMS exactly, confirmed against two
   independently-examined real headers (root's own, INDEXF.SYS's own):
   fixed core fields end at byte 80 (idoffset), then a FIXED 120-byte
   Ident Area (space-padded, holding the file's own "NAME.TYPE;VER"
   string) through byte 200 (mpoffset), then the Map Area.

   An earlier version of this code used an empty (zero-length) Ident
   Area, reasoning it wasn't required for structural correctness.
   That held for a regular file (a real file written and read back
   with TYPE worked fine) but not for a directory - ANALYZE/DISK's
   BAD_DIRHEADER finding on a real, created-with-this-code directory
   pointed at exactly this difference from every real header examined.
   Populating it properly, matching real VMS's fixed-size convention
   rather than sizing it to the actual name length.

   Extension headers (spec->is_extension) deliberately DON'T follow
   this layout: spec 3.5.3 explicitly permits (and this code always
   takes advantage of) truncating the Ident Area to zero length in an
   extension header, i.e. idoffset==mpoffset, starting the Map Area
   right after the fixed Header Area at byte 80 instead of byte 200.
   That recovers the full 120 bytes the Ident Area would otherwise
   have used, for another ~30 Format 1 retrieval pointers - see
   ODS2_EXT_HEADER_MPOFFSET_BYTES / ODS2_EXT_HEADER_MAX_EXTENTS in
   ods2_header_build.h. Primary headers are unaffected and keep the
   full-size Ident Area exactly as before. */

bool ods2_build_file_header(uint8_t *header_out, const ods2_header_spec_t *spec)
{
    ods2_head_core_t *core;
    /* idoffset is always byte 80 (the fixed Header Area's real size) -
       only mpoffset moves, so an extension header's Ident Area has
       zero length (idoffset==mpoffset) rather than not existing at a
       different offset. */
    size_t idoffset_bytes = ODS2_HEADER_IDOFFSET_BYTES;
    size_t map_offset_bytes = spec->is_extension ? ODS2_EXT_HEADER_MPOFFSET_BYTES
                                                  : ODS2_HEADER_MPOFFSET_BYTES;
    size_t map_bytes = 0; /* accumulated below - each extent's actual
                              encoded size varies (4/6/8 bytes,
                              whichever format it needs), so this can't
                              be precomputed as extent_count * 4 the
                              way it could when only Format 1 existed */
    int i;
    uint16_t checksum;

    memset(header_out, 0, 512);
    core = (ods2_head_core_t *) header_out;

    core->idoffset = (uint8_t) (idoffset_bytes / 2);
    core->mpoffset = (uint8_t) (map_offset_bytes / 2);
    core->acoffset = 0xff; /* no ACL area */
    core->rsoffset = 0xff; /* no reserved area */
    core->seg_num = spec->seg_num;
    core->struclev = 0x0201; /* level 2, version 1 - confirmed convention */
    core->fid = spec->fid;
    core->ext_fid = spec->ext_fid; /* zero (no further extension header)
                                       unless the caller set one */
    core->recattr.rtype = spec->rtype;
    core->recattr.rattrib = spec->rattrib;
    core->recattr.rsize = spec->rsize;
    core->recattr.hiblk = ods2_word_swap32(spec->hiblk);
    core->recattr.efblk = ods2_word_swap32(spec->efblk);
    core->recattr.ffbyte = spec->ffbyte;
    core->recattr.maxrec = spec->maxrec;
    core->filechar = spec->filechar;
    /* core->map_inuse is set below, once the encode loop has computed
       the actual accumulated map_bytes (each extent's real byte size
       varies by format, so this can't be known up front). */
    /* FH2$L_FILEOWNER, FH2$W_FILEPROT, FH2$W_RECPROT: confirmed via
       ods2_dump_header against real, VMS-written headers that real
       files always carry non-zero values here - fileowner_uic=(4,1),
       fileprot=0xfa00, recprot=0xfe00 (root itself shows a slightly
       different fileprot=0xba00, likely just a different default
       protection class for directories vs. plain files - using the
       more common 0xfa00 value here, seen on both INDEXF.SYS and our
       own file 11). */
    core->fileowner_uic[0] = 4;
    core->fileowner_uic[1] = 1;
    core->fileprot = 0xfa00;
    core->recprot = 0xfe00;
    core->backlink = spec->backlink;

    /* Ident Area (spec 3.5.3) Layout:
         FI2$T_FILENAME    20 bytes  - name;version, space-padded
         FI2$W_REVISION     2 bytes  - binary revision count
         FI2$Q_CREDATE      8 bytes  - binary VMS timestamp
         FI2$Q_REVDATE      8 bytes  - binary VMS timestamp
         FI2$Q_EXPDATE      8 bytes  - binary VMS timestamp
         FI2$Q_BAKDATE      8 bytes  - binary VMS timestamp
         FI2$T_FILENAMEXT  66 bytes  - name continuation
       Total 120 bytes - found while chasing ANALYZE/DISK's BADDIRENT/
       BAD_DIRHEADER findings. Skipped entirely for extension headers -
       there's no Ident Area to write into (idoffset==mpoffset above),
       and spec->ident_name is documented as ignored in that case. */
    if (!spec->is_extension) {
        uint8_t *ident = header_out + ODS2_HEADER_IDOFFSET_BYTES;
        size_t name_len;
        uint64_t vms_now;

        memset(ident, ' ', 20); /* FI2$T_FILENAME: space-padded text */
        if (spec->ident_name != NULL) {
            name_len = strlen(spec->ident_name);
            if (name_len > 20) name_len = 20; /* overflow goes to FILENAMEXT below */
            memcpy(ident, spec->ident_name, name_len);
        }

        /* FI2$W_REVISION: binary revision count. 1, matching every
           real freshly-created file examined - not 0 (this file has
           been "revised" once, by being written). */
        ident[20] = 1;
        ident[21] = 0;

        /* FI2$Q_CREDATE / FI2$Q_REVDATE: binary 64-bit VMS timestamp -
           (spec 3.5.3.3) Both set to "now" for a freshly
           created file that hasn't been revised since. */
        vms_now = ((uint64_t) time(NULL) + 3506716800ULL) * 10000000ULL;
        {
            int b;
            for (b = 0; b < 8; b++) {
                uint8_t byte = (uint8_t) ((vms_now >> (b * 8)) & 0xff);
                ident[22 + b] = byte; /* CREDATE */
                ident[30 + b] = byte; /* REVDATE */
            }
        }

        /* FI2$Q_EXPDATE / FI2$Q_BAKDATE: zero - "no expiration set" /
           "never backed up", a normal, valid state for a new file
           (not a missing/invalid value - VMS itself leaves these
           zero until the corresponding operation actually happens). */
        memset(ident + 38, 0, 8);  /* EXPDATE */
        memset(ident + 46, 0, 8);  /* BAKDATE */

        /* FI2$T_FILENAMEXT: continuation of the name if it didn't
           fit in the first 20 bytes, space-padded otherwise. */
        memset(ident + 54, ' ', 66);
        if (spec->ident_name != NULL && strlen(spec->ident_name) > 20) {
            size_t ext_len = strlen(spec->ident_name) - 20;
            if (ext_len > 66) ext_len = 66;
            memcpy(ident + 54, spec->ident_name + 20, ext_len);
        }
    }

    for (i = 0; i < spec->extent_count; i++) {
        uint8_t encoded[8];
        size_t bytes_written;

        if (!ods2_encode_retrieval_pointer(encoded, spec->extents[i].lbn,
                                            spec->extents[i].block_count, &bytes_written)) {
            return false; /* extent's LBN/block_count don't fit even Format 3 */
        }
        if (map_offset_bytes + map_bytes + bytes_written > 510) {
            return false; /* wouldn't leave room for the checksum word */
        }
        memcpy(header_out + map_offset_bytes + map_bytes, encoded, bytes_written);
        map_bytes += bytes_written;
    }
    core->map_inuse = (uint8_t) (map_bytes / 2);

    /* FH2$W_CHECKSUM occupies the last word of the block, covering
       the 255 words before it - same algorithm/coverage as every
       other checksum in this project. */
    checksum = ods2_checksum(header_out, 255);
    header_out[510] = (uint8_t) (checksum & 0xff);
    header_out[511] = (uint8_t) (checksum >> 8);

    return true;
}
