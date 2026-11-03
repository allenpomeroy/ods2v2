/*
 * MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
 *
 * ods2_text_var.c - host text to RMS variable-length records
 * (issue #15). See ods2_text_var.h for the format and the rules.
 */
#include "ods2_text_var.h"
#include <stdlib.h>
#include <string.h>

/* --- pass 1: scan --- */

void ods2_text_var_scan_init(ods2_text_var_scan_t *s)
{
    memset(s, 0, sizeof(*s));
}

static void scan_record(ods2_text_var_scan_t *s, size_t len)
{
    s->records++;
    s->converted_length += 2 + len + (len & 1u);
    if (len > s->longest) s->longest = len;
    if (len > ODS2_VAR_MAX_RECORD && s->too_long_line == 0) {
        s->too_long_line = s->records;
    }
}

void ods2_text_var_scan_feed(ods2_text_var_scan_t *s, const uint8_t *data, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (data[i] == '\n') {
            scan_record(s, s->cur_len - (s->last_cr ? 1u : 0u));
            s->cur_len = 0;
            s->last_cr = false;
        } else {
            s->cur_len++;
            s->last_cr = (data[i] == '\r');
        }
    }
}

void ods2_text_var_scan_finish(ods2_text_var_scan_t *s)
{
    /* A last line with no LF is a record, its CR (if any) kept: only
       a CR right before an LF is a line ending. */
    if (s->cur_len > 0) scan_record(s, s->cur_len);
    s->cur_len = 0;
    s->last_cr = false;
}

/* --- pass 2: encode --- */

#define INBUF_SIZE 65536u

struct ods2_text_var_encoder {
    size_t (*read_some)(void *ctx, uint8_t *buf, size_t n, bool *error_out);
    void *ctx;
    uint8_t in[INBUF_SIZE];
    size_t in_pos, in_len;
    bool in_eof;
    /* the record being handed out: count word, data, pad */
    uint8_t rec[2 + ODS2_VAR_MAX_RECORD + 2];
    size_t rec_len, rec_pos;
    const char *problem;
};

ods2_text_var_encoder_t *ods2_text_var_encoder_new(
    size_t (*read_some)(void *ctx, uint8_t *buf, size_t n, bool *error_out), void *ctx)
{
    ods2_text_var_encoder_t *e = calloc(1, sizeof(*e));
    if (e == NULL) return NULL;
    e->read_some = read_some;
    e->ctx = ctx;
    return e;
}

void ods2_text_var_encoder_free(ods2_text_var_encoder_t *e)
{
    free(e);
}

const char *ods2_text_var_problem(const ods2_text_var_encoder_t *e)
{
    return e->problem;
}

/* Next input byte into *c; false at the end of the input or on error
   (e->problem set). */
static bool next_byte(ods2_text_var_encoder_t *e, uint8_t *c)
{
    if (e->in_pos == e->in_len) {
        bool error = false;
        if (e->in_eof) return false;
        e->in_len = e->read_some(e->ctx, e->in, sizeof(e->in), &error);
        e->in_pos = 0;
        if (error) {
            e->problem = "error reading the file";
            e->in_eof = true;
            return false;
        }
        if (e->in_len == 0) {
            e->in_eof = true;
            return false;
        }
    }
    *c = e->in[e->in_pos++];
    return true;
}

/* Builds the next record in e->rec. Returns false at the end of the
   input (no more records) or on a problem (e->problem set). */
static bool next_record(ods2_text_var_encoder_t *e)
{
    uint8_t *data = e->rec + 2;
    size_t len = 0;
    bool got_any = false, ended_by_lf = false;
    uint8_t c;

    while (next_byte(e, &c)) {
        got_any = true;
        if (c == '\n') {
            ended_by_lf = true;
            break;
        }
        /* Room for one byte past the limit: a CR that an LF then drops. */
        if (len == ODS2_VAR_MAX_RECORD + 1) {
            e->problem = "a line is longer than a record can be (32767 bytes) - "
                         "the file changed after it was checked";
            return false;
        }
        data[len++] = c;
    }
    if (e->problem != NULL) return false;
    if (!got_any) return false; /* end of input: no more records */
    if (ended_by_lf && len > 0 && data[len - 1] == '\r') len--;
    if (len > ODS2_VAR_MAX_RECORD) {
        e->problem = "a line is longer than a record can be (32767 bytes) - "
                     "the file changed after it was checked";
        return false;
    }
    e->rec[0] = (uint8_t) (len & 0xffu);
    e->rec[1] = (uint8_t) (len >> 8);
    e->rec_len = 2 + len;
    if (len & 1u) {
        data[len] = 0; /* pad to a word boundary */
        e->rec_len++;
    }
    e->rec_pos = 0;
    return true;
}

bool ods2_text_var_read(void *encoder, uint8_t *buf, size_t n)
{
    ods2_text_var_encoder_t *e = (ods2_text_var_encoder_t *) encoder;
    while (n > 0) {
        size_t piece;
        if (e->rec_pos == e->rec_len) {
            if (!next_record(e)) {
                if (e->problem == NULL) {
                    e->problem = "the file became shorter while being copied";
                }
                return false;
            }
        }
        piece = e->rec_len - e->rec_pos;
        if (piece > n) piece = n;
        memcpy(buf, e->rec + e->rec_pos, piece);
        e->rec_pos += piece;
        buf += piece;
        n -= piece;
    }
    return true;
}
