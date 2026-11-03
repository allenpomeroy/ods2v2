/*
 * MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
 *
 * ods2_text_var_selftest - host text to variable-length records
 * (issue #15):
 *
 *  - the scan (pass 1) and the encoder (pass 2) agree on the length,
 *    and the encoder's bytes are exactly the records expected, for
 *    LF and CRLF text, empty lines, a last line with no LF, a lone CR,
 *    odd and even lengths, an empty file, and lines at the 32767-byte
 *    limit; read in pieces of awkward sizes, the result is the same;
 *  - a line over the limit is reported by the scan, and refused by the
 *    encoder if the file changed between the passes; input that ends
 *    early is refused;
 *  - end to end on a copy of the synthetic disk: the file is VAR with
 *    the carriage control asked for, LRL = the longest line, MRS 0,
 *    the end-of-file just past the last record, and its blocks hold
 *    exactly the records, which decode back to the original lines;
 *  - the volume refuses VAR content not marked as converted, and
 *    converted records with RAT:BLK or a longest record over MRS. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_text_var.h"
#include "ods2_volume.h"
#include "ods2_validate.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/text_var_disk.img"

/* An input for the encoder: a buffer, handed out `step` bytes at a
   time (to exercise reads that split lines and CRLFs). */
typedef struct {
    const uint8_t *data;
    size_t len, pos, step;
} input_t;

static size_t input_read_some(void *ctx_, uint8_t *buf, size_t n, bool *error_out)
{
    input_t *in = (input_t *) ctx_;
    size_t k = in->len - in->pos;
    (void) error_out;
    if (k > n) k = n;
    if (k > in->step) k = in->step;
    memcpy(buf, in->data + in->pos, k);
    in->pos += k;
    return k;
}

/* Converts `text` with the scan and the encoder (reading `step` bytes
   at a time, handing out `out_step` bytes at a time); returns the
   records in a malloc'd buffer, *len_out its length, after checking
   the two passes agree. */
static uint8_t *convert(const uint8_t *text, size_t len, size_t step, size_t out_step,
                        ods2_text_var_scan_t *scan_out, size_t *len_out)
{
    ods2_text_var_scan_t scan;
    ods2_text_var_encoder_t *e;
    input_t in;
    uint8_t *out;
    size_t done = 0, i;

    ods2_text_var_scan_init(&scan);
    for (i = 0; i < len; i += step) {
        ods2_text_var_scan_feed(&scan, text + i, (len - i < step) ? len - i : step);
    }
    ods2_text_var_scan_finish(&scan);
    assert(scan.too_long_line == 0);

    in.data = text;
    in.len = len;
    in.pos = 0;
    in.step = step;
    e = ods2_text_var_encoder_new(input_read_some, &in);
    assert(e != NULL);
    out = malloc(scan.converted_length + 1);
    assert(out != NULL);
    while (done < scan.converted_length) {
        size_t k = scan.converted_length - done;
        if (k > out_step) k = out_step;
        assert(ods2_text_var_read(e, out + done, k));
        done += k;
    }
    /* Nothing more: one more byte would be past the last record. */
    {
        uint8_t extra;
        assert(!ods2_text_var_read(e, &extra, 1));
    }
    ods2_text_var_encoder_free(e);
    *scan_out = scan;
    *len_out = done;
    return out;
}

static void expect(const char *what, const char *text, size_t text_len,
                   const uint8_t *records, size_t records_len, size_t n_records,
                   size_t longest)
{
    static const size_t steps[] = { 1, 2, 3, 7, 64, 65536 };
    size_t s;
    for (s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        ods2_text_var_scan_t scan;
        size_t len;
        uint8_t *out = convert((const uint8_t *) text, text_len, steps[s], steps[s] + 2,
                               &scan, &len);
        assert(scan.converted_length == records_len);
        assert(len == records_len);
        assert(memcmp(out, records, records_len) == 0);
        assert(scan.records == n_records);
        assert(scan.longest == longest);
        free(out);
    }
    printf("PASS: %s\n", what);
}

/* Decodes records back into lines joined by LF, for comparison. */
static size_t decode(const uint8_t *rec, size_t len, uint8_t *out)
{
    size_t pos = 0, o = 0;
    while (pos < len) {
        size_t n = (size_t) rec[pos] | ((size_t) rec[pos + 1] << 8);
        memcpy(out + o, rec + pos + 2, n);
        o += n;
        out[o++] = '\n';
        pos += 2 + n + (n & 1u);
    }
    return o;
}

static void copy_sparse(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    static char buf[1 << 20];
    static const char zero[1 << 20];
    size_t n;
    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (n == sizeof(buf) && memcmp(buf, zero, n) == 0) {
            assert(fseek(out, (long) n, SEEK_CUR) == 0);
        } else {
            assert(fwrite(buf, 1, n, out) == n);
        }
    }
    fclose(in);
    fclose(out);
}

static bool memory_read(void *ctx, uint8_t *buf, size_t n)
{
    const uint8_t **p = (const uint8_t **) ctx;
    memcpy(buf, *p, n);
    *p += n;
    return true;
}

int main(void)
{
    /* --- the format --- */
    {
        static const uint8_t r[] = { 3, 0, 'a', 'b', 'c', 0, 2, 0, 'd', 'e' };
        expect("LF lines: count word, data, a pad byte after an odd length",
               "abc\nde\n", 7, r, sizeof r, 2, 3);
    }
    {
        static const uint8_t r[] = { 3, 0, 'a', 'b', 'c', 0, 2, 0, 'd', 'e' };
        expect("CRLF lines: the CR before each LF is dropped",
               "abc\r\nde\r\n", 9, r, sizeof r, 2, 3);
    }
    {
        static const uint8_t r[] = { 0, 0, 0, 0, 1, 0, 'x', 0 };
        expect("empty lines are zero-length records", "\n\nx\n", 4, r, sizeof r, 3, 1);
    }
    {
        static const uint8_t r[] = { 2, 0, 'a', 'b', 3, 0, 't', 'a', 'i', 0 };
        expect("a last line with no LF is still a record", "ab\ntai", 6, r, sizeof r, 2, 3);
    }
    {
        static const uint8_t r[] = { 3, 0, 'a', '\r', 'b', 0, 2, 0, 'c', '\r' };
        expect("a CR not before an LF is kept, including at the very end",
               "a\rb\nc\r", 6, r, sizeof r, 2, 3);
    }
    {
        expect("an empty file has no records", "", 0, (const uint8_t *) "", 0, 0, 0);
    }

    /* --- the 32767-byte limit --- */
    {
        size_t len = ODS2_VAR_MAX_RECORD;
        uint8_t *text = malloc(len + 3), *rec = malloc(len + 3);
        memset(text, 'L', len);
        text[len] = '\r';
        text[len + 1] = '\n';
        rec[0] = (uint8_t) (len & 0xff);
        rec[1] = (uint8_t) (len >> 8);
        memset(rec + 2, 'L', len);
        rec[2 + len] = 0;
        expect("a 32767-byte CRLF line is one record (the CR doesn't count)",
               (const char *) text, len + 2, rec, len + 3, 1, len);
        free(text);
        free(rec);
    }
    {
        size_t len = ODS2_VAR_MAX_RECORD + 1;
        uint8_t *text = malloc(len + 4);
        ods2_text_var_scan_t scan;
        memcpy(text, "ok\n", 3);
        memset(text + 3, 'L', len);
        text[3 + len] = '\n';
        ods2_text_var_scan_init(&scan);
        ods2_text_var_scan_feed(&scan, text, len + 4);
        ods2_text_var_scan_finish(&scan);
        assert(scan.too_long_line == 2);
        {
            /* The encoder refuses it too (the file changed between the
               passes from fitting to not). */
            input_t in = { text, len + 4, 0, 4096 };
            ods2_text_var_encoder_t *e = ods2_text_var_encoder_new(input_read_some, &in);
            uint8_t *out = malloc(len + 8);
            assert(ods2_text_var_read(e, out, 4)); /* the "ok" record */
            assert(!ods2_text_var_read(e, out, len));
            assert(strstr(ods2_text_var_problem(e), "longer") != NULL);
            ods2_text_var_encoder_free(e);
            free(out);
        }
        free(text);
        printf("PASS: a 32768-byte line is reported by the scan (line 2) and refused "
               "by the encoder\n");
    }
    {
        /* Input shorter than was scanned. */
        input_t in = { (const uint8_t *) "ab\n", 3, 0, 1 };
        ods2_text_var_encoder_t *e = ods2_text_var_encoder_new(input_read_some, &in);
        uint8_t out[16];
        assert(!ods2_text_var_read(e, out, 10));
        assert(strstr(ods2_text_var_problem(e), "shorter") != NULL);
        ods2_text_var_encoder_free(e);
        printf("PASS: input ending before the length scanned is refused\n");
    }

    /* --- end to end on a volume --- */
    {
        static const char text[] =
            "$! A DCL procedure\r\n"
            "$ WRITE SYS$OUTPUT \"Hello\"\r\n"
            "\r\n"
            "$ EXIT\r\n";
        ods2_volume_t vol;
        ods2_text_var_scan_t scan;
        ods2_content_source_t src;
        ods2_record_attrs_t attrs;
        ods2_fid_t fid;
        uint8_t root[512], h[512];
        const ods2_head_core_t *core = (const ods2_head_core_t *) h;
        size_t rec_len, n;
        uint8_t *rec = convert((const uint8_t *) text, sizeof text - 1, 65536, 65536,
                               &scan, &rec_len);
        const uint8_t *cursor = rec;
        uint8_t *back, decoded[256];

        copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
        assert(ods2_mount_write(DISK_PATH, &vol).ok);

        memset(&src, 0, sizeof src);
        src.length = rec_len;
        src.read = memory_read;
        src.ctx = &cursor;
        src.var_records = true;
        attrs.rtype = ODS2_RFM_VAR;
        attrs.rattrib = ODS2_RAT_CR;
        attrs.rsize = (uint16_t) scan.longest;
        attrs.maxrec = 0;
        assert(ods2_read_header(&vol, 4, root).ok);
        assert(ods2_create_file_source(&vol, root, "HELLO.COM", &src, &attrs, &fid).ok);

        assert(ods2_read_header(&vol, fid.fid_num, h).ok);
        assert(ods2_validate_head(h).ok);
        assert(core->recattr.rtype == ODS2_RFM_VAR);
        assert(core->recattr.rattrib == ODS2_RAT_CR);
        assert(core->recattr.rsize == 26); /* $ WRITE SYS$OUTPUT "Hello" */
        assert(core->recattr.maxrec == 0);
        assert(ods2_file_content_length(h) == rec_len);
        back = malloc(rec_len);
        assert(ods2_read_file(&vol, h, back, rec_len, &n).ok && n == rec_len);
        assert(memcmp(back, rec, rec_len) == 0);
        n = decode(back, rec_len, decoded);
        assert(n == 18 + 1 + 26 + 1 + 0 + 1 + 6 + 1);
        assert(memcmp(decoded, "$! A DCL procedure\n$ WRITE SYS$OUTPUT \"Hello\"\n\n$ EXIT\n",
                      n) == 0);
        free(back);
        printf("PASS: on a volume: VAR, RAT:CR, LRL 26 (the longest line), MRS 0, EOF "
               "just past the last record; records decode back to the lines\n");

        /* --- the volume's checks --- */
        cursor = rec;
        src.var_records = false;
        assert(ods2_read_header(&vol, 4, root).ok);
        assert(!ods2_create_file_source(&vol, root, "RAW.COM", &src, &attrs, &fid).ok);
        src.var_records = true;
        attrs.rattrib = ODS2_RAT_CR | ODS2_RAT_BLK;
        assert(ods2_check_var_record_attrs(&attrs, rec_len) != NULL);
        attrs.rattrib = ODS2_RAT_CR;
        attrs.maxrec = 20;
        assert(ods2_check_var_record_attrs(&attrs, rec_len) != NULL);
        attrs.maxrec = 26;
        assert(ods2_check_var_record_attrs(&attrs, rec_len) == NULL);
        attrs.rtype = ODS2_RFM_STMLF;
        assert(ods2_check_var_record_attrs(&attrs, rec_len) != NULL);
        assert(ods2_read_header(&vol, 4, root).ok);
        assert(!ods2_lookup_name(&vol, root, "RAW.COM", &fid).ok);
        printf("PASS: VAR content not marked as converted is refused; converted records "
               "with RAT:BLK, or longer than MRS, or not VAR, are refused\n");

        ods2_dismount(&vol);
        remove(DISK_PATH);
        free(rec);
    }

    printf("\nods2_text_var_selftest: all checks passed\n");
    return 0;
}
