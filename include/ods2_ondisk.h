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

/* ods2_ondisk.h - On-disk structure definitions for ODS-2 (Files-11
 * Structure Level 2), written directly from the Files-11 On-Disk
 * Structure Specification (references below are to that document's
 * section numbers), not inherited from any prior implementation.
 *
 * Design decisions, and why:
 *
 *  - All multi-byte fields are little-endian on disk (VAX/x86 native).
 *    Exact-width types (uint16_t/uint32_t) + #pragma pack(1) are used
 *    throughout instead of the old codebase's "vmsword"/VMSWORD()
 *    macro pattern. That pattern existed to guard against struct
 *    padding, but it was applied inconsistently - it's exactly what
 *    caused the DIRREC_HDRSIZE bug (sizeof(struct dir$rec) was 8 due
 *    to compiler padding, but the real on-disk header is 6 bytes).
 *    pack(1) removes the entire bug class at the type level instead
 *    of requiring every call site to remember a separate constant.
 *
 *  - The file header (HEAD, section 3.6) has areas at VARIABLE offsets
 *    (ident/map/ACL/reserved), located via FH2$B_IDOFFSET/MPOFFSET/
 *    ACOFFSET/RSOFFSET (each a word-offset from the start of the
 *    header). This is modeled as a fixed-layout "core" struct for the
 *    portion that's genuinely fixed-position, plus accessor functions
 *    for the variable areas - not a single flat struct for the whole
 *    512 bytes, which would be actively wrong.
 *
 *  - Every structure whose size is asserted here (HOME, the HEAD core)
 *    is checked at compile time in ods2_ondisk_selftest.c against the
 *    spec's own stated sizes/offsets, so a layout mistake is a build
 *    failure in our own sandbox, not a mount failure on real VMS three
 *    steps later.
 */

#ifndef ODS2_ONDISK_H
#define ODS2_ONDISK_H

#include <stdint.h>

/* Some longword fields in the on-disk structures (confirmed so far:
   FAT$L_HIBLK, FAT$L_EFBLK) use PDP-11-heritage "word-swapped"
   encoding: the value's two 16-bit halves are stored in the opposite
   order from a normal little-endian longword (high-order word at the
   lower byte address, low-order word at the higher address) - NOT the
   same thing as the whole value being big-endian; each 16-bit word is
   still itself little-endian internally, just the two words are
   swapped relative to each other. */
static inline uint32_t ods2_word_swap32(uint32_t v)
{
    return ((v & 0xffffu) << 16) | ((v >> 16) & 0xffffu);
}

#pragma pack(push, 1)

/* ----------------------------------------------------------------
 * 48-bit File ID (FID). Spec section 2.3: three words - number,
 * sequence, and a byte pair (nmx/rvn) packed into the third word's
 * position. We keep num/seq as their own words and nmx/rvn as two
 * bytes explicitly, matching every FID reference in the spec's
 * layout diagrams (e.g. FH2$W_FID / FH2$W_FID_NUM.. FH2$B_FID_RVN).
 * ---------------------------------------------------------------- */
typedef struct {
    uint16_t fid_num;   /* FH2$W_FID_NUM equivalent */
    uint16_t fid_seq;   /* FH2$W_FID_SEQ equivalent */
    uint8_t  fid_rvn;   /* relative volume number */
    uint8_t  fid_nmx;   /* extension of fid_num (files > 65535) */
} ods2_fid_t;

/* ----------------------------------------------------------------
 * Home Block (HOME2) - spec section 5.1.9. Exactly 512 bytes.
 * Two checksums: HM2$W_CHECKSUM1 covers the first 29 words (the
 * volume-identification portion only); HM2$W_CHECKSUM2 covers the
 * first 255 words (i.e. everything before itself). Both must be
 * kept correct independently - this is the exact field the old
 * project got wrong first (only recomputed checksum2, leaving
 * checksum1 stale the moment any covered field changed).
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t homelbn;        /* HM2$L_HOMELBN */
    uint32_t alhomelbn;      /* HM2$L_ALHOMELBN */
    uint32_t altidxlbn;      /* HM2$L_ALTIDXLBN */
    uint16_t struclev;       /* HM2$W_STRUCLEV: high byte=level, low=version */
    uint16_t cluster;        /* HM2$W_CLUSTER */
    uint16_t homevbn;        /* HM2$W_HOMEVBN */
    uint16_t alhomevbn;      /* HM2$W_ALHOMEVBN */
    uint16_t altidxvbn;      /* HM2$W_ALTIDXVBN */
    uint16_t ibmapvbn;       /* HM2$W_IBMAPVBN */
    uint32_t ibmaplbn;       /* HM2$L_IBMAPLBN */
    uint32_t maxfiles;       /* HM2$L_MAXFILES */
    uint16_t ibmapsize;      /* HM2$W_IBMAPSIZE (blocks) */
    uint16_t resfiles;       /* HM2$W_RESFILES */
    uint16_t devtype;        /* HM2$W_DEVTYPE */
    uint16_t rvn;            /* HM2$W_RVN */
    uint16_t setcount;       /* HM2$W_SETCOUNT */
    uint16_t volchar;        /* HM2$W_VOLCHAR */
    uint16_t volowner_uic[2];/* HM2$L_VOLOWNER (mem,grp as words) */
    uint32_t reserved1;
    uint16_t protect;        /* HM2$W_PROTECT */
    uint16_t fileprot;       /* HM2$W_FILEPROT */
    uint16_t reserved2;
    uint16_t checksum1;      /* HM2$W_CHECKSUM1 - covers words 0-28 only */
    uint64_t credate;        /* HM2$Q_CREDATE */
    uint8_t  window;         /* HM2$B_WINDOW */
    uint8_t  lru_lim;        /* HM2$B_LRU_LIM */
    uint16_t extend;         /* HM2$W_EXTEND */
    uint64_t retainmin;      /* HM2$Q_RETAINMIN */
    uint64_t retainmax;      /* HM2$Q_RETAINMAX */
    uint64_t revdate;        /* HM2$Q_REVDATE */
    uint8_t  min_class[20];  /* HM2$R_MIN_CLASS */
    uint8_t  max_class[20];  /* HM2$R_MAX_CLASS */
    uint8_t  reserved3[320];
    uint32_t serialnum;      /* HM2$L_SERIALNUM */
    char     strucname[12];  /* HM2$T_STRUCNAME */
    char     volname[12];    /* HM2$T_VOLNAME */
    char     ownername[12];  /* HM2$T_OWNERNAME */
    char     format[12];     /* HM2$T_FORMAT - "DECFILE11B  " */
    uint16_t reserved4;
    uint16_t checksum2;      /* HM2$W_CHECKSUM2 - covers words 0-254 */
} ods2_home_t;

/* ----------------------------------------------------------------
 * File Attributes Table (FAT) - spec section 6.2. This is the exact
 * 32-byte structure occupying FH2$W_RECATTR. Confirmed field-by-field
 * against a real INDEXF.SYS header (file 1) read from an actual
 * disk: FAT$B_RTYPE=1 (FAB$C_FIX) and FAT$W_RSIZE=512,
 * FAT$W_MAXREC=512 all matched the spec's own description of
 * INDEXF.SYS as "512 byte fixed length records" exactly.
 * ---------------------------------------------------------------- */
typedef struct {
    uint8_t  rtype;      /* FAT$B_RTYPE - record format + organization nibbles */
    uint8_t  rattrib;    /* FAT$B_RATTRIB - carriage control / NOSPAN flags */
    uint16_t rsize;       /* FAT$W_RSIZE */
    uint32_t hiblk;       /* FAT$L_HIBLK - highest VBN allocated. WORD-SWAPPED
                              encoding - use ods2_word_swap32() when reading
                              or writing this field, not a plain LE read. */
    uint32_t efblk;       /* FAT$L_EFBLK - end-of-file VBN. Also WORD-SWAPPED -
                              see ods2_word_swap32(). */
    uint16_t ffbyte;      /* FAT$W_FFBYTE - first free byte in last block */
    uint8_t  bktsize;     /* FAT$B_BKTSIZE */
    uint8_t  vfcsize;     /* FAT$B_VFCSIZE */
    uint16_t maxrec;       /* FAT$W_MAXREC */
    uint16_t defext;       /* FAT$W_DEFEXT */
    uint16_t gbc;          /* FAT$W_GBC */
    uint8_t  not_used[8];
    uint16_t versions;     /* FAT$W_VERSIONS - directory default version limit */
} ods2_fat_t;

/* ----------------------------------------------------------------
 * File Header (HEAD) - fixed-position "core" portion only, spec
 * section 3.6, up through the end of the Header Area. The Ident,
 * Map, ACL, and Reserved areas that follow are NOT part of this
 * struct - see ods2_head_ident_area()/ods2_head_map_area() etc.
 * (to be added alongside the accessor implementation).
 * ---------------------------------------------------------------- */
typedef struct {
    uint8_t     idoffset;    /* FH2$B_IDOFFSET - word offset to Ident Area.
                                 CONFIRMED against real data: this byte's
                                 value*2 exactly matched the byte offset
                                 where a real header's literal filename
                                 text ("INDEXF.SYS;1") was found. The
                                 spec's side-by-side diagram layout puts
                                 the lower byte address under the
                                 RIGHT-hand label, not the left - easy to
                                 get backwards, verified here the hard way. */
    uint8_t     mpoffset;    /* FH2$B_MPOFFSET - word offset to Map Area */
    uint8_t     acoffset;    /* FH2$B_ACOFFSET - word offset to ACL Area */
    uint8_t     rsoffset;    /* FH2$B_RSOFFSET - word offset to Reserved Area */
    uint16_t    seg_num;     /* FH2$W_SEG_NUM */
    uint16_t    struclev;    /* FH2$W_STRUCLEV */
    ods2_fid_t  fid;         /* FH2$W_FID */
    ods2_fid_t  ext_fid;     /* FH2$W_EXT_FID */
    ods2_fat_t  recattr;     /* FH2$W_RECATTR - Record Attributes Area (32 bytes) */
    uint32_t    filechar;    /* FH2$L_FILECHAR */
    uint16_t    recprot;     /* FH2$W_RECPROT */
    uint8_t     map_inuse;   /* FH2$B_MAP_INUSE */
    uint8_t     acc_mode;    /* FH2$B_ACC_MODE */
    uint16_t    fileowner_uic[2]; /* FH2$L_FILEOWNER */
    uint16_t    fileprot;    /* FH2$W_FILEPROT */
    ods2_fid_t  backlink;    /* FH2$W_BACKLINK */
    uint16_t    journal;     /* FH2$W_JOURNAL */
    uint16_t    not_used_1;
    uint32_t    highwater;   /* FH2$L_HIGHWATER */
    uint8_t     not_used_2[12];
    uint8_t     class_prot[20]; /* FH2$R_CLASS_PROT */
} ods2_head_core_t;

/* ----------------------------------------------------------------
 * Directory Record - spec section 4.2/4.3. Fixed 6-byte header
 * (DIR$W_SIZE, DIR$W_VERLIMIT, DIR$B_FLAGS, DIR$B_NAMECOUNT) followed
 * by a variable-length name (DIR$T_NAME, namecount bytes, padded to
 * an even boundary) and then one or more 8-byte per-version entries
 * (DIR$W_VERSION + a 6-byte FID). This is genuinely variable-length,
 * so it's modeled as a fixed header struct plus a separate fixed
 * per-entry struct, navigated with accessor functions rather than one
 * flat struct - same reasoning as the file header's variable areas.
 *
 * DIR$B_FLAGS (section 4.2.3): low 3 bits are a type code, currently
 * only DIR$C_FID (0 - "value field is a list of version/FID pairs")
 * is used in practice; DIR$C_LINKNAME is documented but "not yet
 * supported by any implementation". The Level 0 Subset table (section
 * 7.4) states this byte must be exactly 0 - confirmed against a real,
 * VMS-native directory record read back from an untouched volume
 * during the old project's investigation, so this is empirically
 * verified, not just spec-inferred.
 */
typedef struct {
    uint16_t size;       /* DIR$W_SIZE - bytes in this record, header+name */
    uint16_t verlimit;   /* DIR$W_VERLIMIT - version limit; spec: "ignored"
                             per the Level 0 subset table */
    uint8_t  flags;      /* DIR$B_FLAGS - must be 0 (see above) */
    uint8_t  namecount;  /* DIR$B_NAMECOUNT - length of name that follows */
    /* DIR$T_NAME follows here: `namecount` bytes, then padding to an
       even byte boundary, then one or more dir_ent_t entries. */
} ods2_dir_rec_header_t;

typedef struct {
    uint16_t   version;  /* DIR$W_VERSION */
    ods2_fid_t fid;      /* file ID this version points to */
} ods2_dir_ent_t;

#pragma pack(pop)

#endif /* ODS2_ONDISK_H */
