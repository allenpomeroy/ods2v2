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

/* ods2_record_attrs_selftest - RMS record attributes of imported files
 * (FH2$W_RECATTR, spec 3.5.2.9), on a copy of the synthetic disk:
 *
 *  - ods2_check_record_attrs() accepts fixed records of any even size
 *    the content is a whole number of, and refuses the rest (odd sizes,
 *    MRS != record size, partial last record, VAR/VFC, bad RAT bits);
 *  - a file created with fixed 8192-byte records - as OpenVMS PCSI kit
 *    files are - has RTYPE FIX, RSIZE = MRS = 8192 in its header, the
 *    same shape VMS gives its own fixed files (INDEXF.SYS: FIX, 512,
 *    512), a correct end-of-file mark, and reads back unchanged;
 *  - other formats and record attributes land in the header as given;
 *  - refused attributes create nothing and allocate nothing;
 *  - ods2_create_file() is unchanged: RSIZE = MRS = 512, RAT 0. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_validate.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/record_attrs_disk.img"

static void copy_sparse(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    static char buf[1 << 20];
    static const char zero[1 << 20];
    size_t n;
    long total = 0;
    char last;
    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (n == sizeof(buf) && memcmp(buf, zero, n) == 0) {
            assert(fseek(out, (long) n, SEEK_CUR) == 0);
        } else {
            assert(fwrite(buf, 1, n, out) == n);
        }
        total += (long) n;
    }
    assert(fseek(in, total - 1, SEEK_SET) == 0 && fread(&last, 1, 1, in) == 1);
    assert(fseek(out, total - 1, SEEK_SET) == 0 && fwrite(&last, 1, 1, out) == 1);
    fclose(in);
    fclose(out);
}

static ods2_record_attrs_t attrs(uint8_t rtype, uint8_t rattrib, uint16_t rsize, uint16_t maxrec)
{
    ods2_record_attrs_t a;
    a.rtype = rtype;
    a.rattrib = rattrib;
    a.rsize = rsize;
    a.maxrec = maxrec;
    return a;
}

static bool accepted(uint8_t rtype, uint8_t rattrib, uint16_t rsize, uint16_t maxrec, size_t len)
{
    ods2_record_attrs_t a = attrs(rtype, rattrib, rsize, maxrec);
    return ods2_check_record_attrs(&a, len) == NULL;
}

static uint32_t swap32(uint32_t v) { return (v << 16) | (v >> 16); }

/* Creates `name` in the root with `a`, and checks its header carries
   exactly those attributes and the right end-of-file mark. */
static ods2_fid_t create_and_check(ods2_volume_t *vol, const char *name,
                                   const uint8_t *content, size_t len, ods2_record_attrs_t a)
{
    uint8_t root[512], h[512], *back;
    const ods2_head_core_t *core = (const ods2_head_core_t *) h;
    ods2_fid_t fid;
    size_t got;
    ods2_result_t r;

    assert(ods2_read_header(vol, 4, root).ok);
    r = ods2_create_file_attrs(vol, root, name, content, len, &a, &fid);
    if (!r.ok) printf("create %s: %s\n", name, r.problem);
    assert(r.ok);
    assert(ods2_read_header(vol, fid.fid_num, h).ok);
    assert(ods2_validate_head(h).ok);
    assert(core->recattr.rtype == a.rtype);
    assert(core->recattr.rattrib == a.rattrib);
    assert(core->recattr.rsize == a.rsize);
    assert(core->recattr.maxrec == a.maxrec);
    assert(swap32(core->recattr.efblk) == len / 512 + 1);
    assert(core->recattr.ffbyte == len % 512);
    back = malloc(len + 1);
    assert(back != NULL);
    assert(ods2_read_file(vol, h, back, len + 1, &got).ok && got == len);
    assert(memcmp(back, content, len) == 0);
    free(back);
    return fid;
}

int main(void)
{
    ods2_volume_t vol;
    uint8_t *kit;
    size_t kit_len = 3u * 8192u;
    size_t i;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* --- What ods2_check_record_attrs() accepts --- */
    assert(accepted(ODS2_RFM_FIX, 0, 8192, 8192, 16384));
    assert(accepted(ODS2_RFM_FIX, 0, 8192, 8192, 0));          /* empty: zero records */
    assert(accepted(ODS2_RFM_FIX, 0, 512, 512, 1024));
    assert(accepted(ODS2_RFM_FIX, 0, 80, 80, 800));
    assert(accepted(ODS2_RFM_FIX, ODS2_RAT_CR, 80, 80, 160));
    assert(accepted(ODS2_RFM_FIX, 0, 32766, 32766, 32766));
    assert(!accepted(ODS2_RFM_FIX, 0, 8192, 8192, 16000));     /* partial last record */
    assert(!accepted(ODS2_RFM_FIX, 0, 8192, 512, 16384));      /* MRS != record size */
    assert(!accepted(ODS2_RFM_FIX, 0, 133, 133, 133));         /* odd: RMS pads it */
    assert(!accepted(ODS2_RFM_FIX, 0, 0, 0, 0));               /* no size */
    assert(!accepted(ODS2_RFM_FIX, 0, 32768, 32768, 32768));   /* too big */
    assert(accepted(ODS2_RFM_STMLF, ODS2_RAT_CR, 0, 0, 123));
    assert(accepted(ODS2_RFM_STMLF, 0, 512, 512, 123));
    assert(accepted(ODS2_RFM_UDF, 0, 512, 512, 7));
    assert(accepted(ODS2_RFM_STM, ODS2_RAT_CR, 0, 0, 7));
    assert(accepted(ODS2_RFM_STMCR, ODS2_RAT_CR | ODS2_RAT_BLK, 0, 0, 7));
    assert(!accepted(ODS2_RFM_VAR, ODS2_RAT_CR, 0, 0, 7));     /* needs count words */
    assert(!accepted(ODS2_RFM_VFC, ODS2_RAT_PRN, 0, 0, 7));
    assert(!accepted(7, 0, 0, 0, 7));                          /* no such format */
    assert(!accepted(0x11, 0, 512, 512, 512));                 /* not sequential */
    assert(!accepted(ODS2_RFM_STMLF, ODS2_RAT_CR | ODS2_RAT_FTN, 0, 0, 7));
    assert(!accepted(ODS2_RFM_STMLF, 0x10, 0, 0, 7));          /* unknown RAT bit */
    assert(!accepted(ODS2_RFM_STMLF, 0, 40000, 0, 7));
    printf("PASS: record attributes are checked against the content: fixed records "
           "whole and even-sized, MRS = record size, no VAR/VFC, valid RAT\n");

    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);

    /* --- Fixed 8192-byte records, as in a PCSI kit --- */
    kit = malloc(kit_len);
    assert(kit != NULL);
    for (i = 0; i < kit_len; i++) kit[i] = (uint8_t) (i * 7 + i / 8192);
    create_and_check(&vol, "KIT.PCSI", kit, kit_len, attrs(ODS2_RFM_FIX, 0, 8192, 8192));
    printf("PASS: KIT.PCSI created with fixed 8192-byte records: RTYPE FIX, "
           "RSIZE = MRS = 8192, RAT none, EOF after 3 records, content intact\n");

    /* --- The shape VMS gives its own fixed files --- */
    {
        uint8_t ix[512], h[512];
        const ods2_head_core_t *vms = (const ods2_head_core_t *) ix;
        const ods2_head_core_t *ours = (const ods2_head_core_t *) h;
        create_and_check(&vol, "FIX512.DAT", kit, 1024, attrs(ODS2_RFM_FIX, 0, 512, 512));
        assert(ods2_read_header(&vol, 1, ix).ok); /* INDEXF.SYS, written by VMS */
        assert(vms->recattr.rtype == ODS2_RFM_FIX && vms->recattr.rsize == 512 &&
               vms->recattr.maxrec == 512 && vms->recattr.rattrib == 0);
        {
            uint8_t root[512];
            ods2_fid_t fid;
            assert(ods2_read_header(&vol, 4, root).ok);
            assert(ods2_lookup_name(&vol, root, "FIX512.DAT", &fid).ok);
            assert(ods2_read_header(&vol, fid.fid_num, h).ok);
        }
        assert(ours->recattr.rtype == vms->recattr.rtype);
        assert(ours->recattr.rattrib == vms->recattr.rattrib);
        assert(ours->recattr.rsize == vms->recattr.rsize);
        assert(ours->recattr.maxrec == vms->recattr.maxrec);
        printf("PASS: a fixed 512-byte import has the same record attributes as "
               "VMS's own INDEXF.SYS\n");
    }

    /* --- Other formats and attributes, as given --- */
    create_and_check(&vol, "NOTES.TXT", (const uint8_t *) "one\ntwo\n", 8,
                     attrs(ODS2_RFM_STMLF, ODS2_RAT_CR, 0, 0));
    create_and_check(&vol, "RAW.DAT", kit, 777, attrs(ODS2_RFM_UDF, 0, 512, 512));
    create_and_check(&vol, "CARDS.DAT", kit, 800, attrs(ODS2_RFM_FIX, ODS2_RAT_CR, 80, 80));
    create_and_check(&vol, "EMPTY.PCSI", kit, 0, attrs(ODS2_RFM_FIX, 0, 8192, 8192));
    printf("PASS: Stream_LF with CR, UDF, fixed 80 with CR and an empty fixed 8192 "
           "file all get exactly the attributes asked for\n");

    /* --- Refused attributes create and allocate nothing --- */
    {
        uint8_t root[512];
        ods2_fid_t fid;
        ods2_record_attrs_t bad = attrs(ODS2_RFM_FIX, 0, 8192, 8192);
        uint8_t bitmap_before[512], bitmap_after[512];
        ods2_result_t r;
        assert(ods2_read_header(&vol, 4, root).ok);
        assert(fseek(vol.fp, (long) vol.home.ibmaplbn * 512, SEEK_SET) == 0);
        assert(fread(bitmap_before, 1, 512, vol.fp) == 512);
        r = ods2_create_file_attrs(&vol, root, "SHORT.PCSI", kit, 8192 + 100, &bad, &fid);
        assert(!r.ok);
        assert(strstr(r.problem, "whole number") != NULL);
        assert(ods2_read_header(&vol, 4, root).ok);
        assert(!ods2_lookup_name(&vol, root, "SHORT.PCSI", &fid).ok);
        assert(fseek(vol.fp, (long) vol.home.ibmaplbn * 512, SEEK_SET) == 0);
        assert(fread(bitmap_after, 1, 512, vol.fp) == 512);
        assert(memcmp(bitmap_before, bitmap_after, 512) == 0);
        printf("PASS: a file that isn't a whole number of records is refused before "
               "anything is allocated (%s)\n", r.problem);
    }

    /* --- ods2_create_file() unchanged --- */
    {
        uint8_t root[512], h[512];
        const ods2_head_core_t *core = (const ods2_head_core_t *) h;
        ods2_fid_t fid;
        assert(ods2_read_header(&vol, 4, root).ok);
        assert(ods2_create_file(&vol, root, "LEGACY.TXT", kit, 10, ODS2_RFM_STMLF, &fid).ok);
        assert(ods2_read_header(&vol, fid.fid_num, h).ok);
        assert(core->recattr.rtype == ODS2_RFM_STMLF && core->recattr.rattrib == 0 &&
               core->recattr.rsize == 512 && core->recattr.maxrec == 512);
        printf("PASS: ods2_create_file() still writes RSIZE = MRS = 512, RAT none\n");
    }

    ods2_dismount(&vol);
    free(kit);
    remove(DISK_PATH);
    printf("\nods2_record_attrs_selftest: all checks passed\n");
    return 0;
}
