/*
 * MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
 *
 * ods2_text_var.h - host text to RMS variable-length records
 * (issue #15).
 *
 * VMS's native text format is a sequential file of variable-length
 * records (RFM:VAR), usually with carriage-return carriage control
 * (RAT:CR) - what VMS FTP writes in ASCII mode. On disk each record is
 * a 16-bit little-endian byte count, then that many bytes, then one
 * pad byte if the count is odd, so every record starts on a word
 * boundary. Records run on across block boundaries (the file is not
 * RAT:BLK); the end of the data is the file's end-of-file mark (EFBLK/
 * FFBYTE), with no marker after the last record.
 *
 * A host text file becomes one record per line:
 *  - lines end at LF; a CR just before the LF is dropped (CRLF text);
 *    any other CR is kept as data;
 *  - a last line with no LF is still a record; a file ending in LF has
 *    no empty record after it; an empty file has no records;
 *  - a line longer than ODS2_VAR_MAX_RECORD bytes can't be a record.
 *
 * Converting takes two passes over the host file: a scan, giving the
 * converted size and longest record before anything is allocated on
 * the volume, then an encoder that produces the records a chunk at a
 * time as COPY writes them, so memory use stays flat (issue #16).
 */
#ifndef ODS2_TEXT_VAR_H
#define ODS2_TEXT_VAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ODS2_VAR_MAX_RECORD 32767u

/* Pass 1: feed the whole host file, in pieces of any size, then
   finish. */
typedef struct {
    size_t records;          /* records the file converts to */
    size_t converted_length; /* bytes, count words and pads included */
    size_t longest;          /* longest record (FAT$W_RSIZE), bytes */
    size_t too_long_line;    /* first line over ODS2_VAR_MAX_RECORD, 1-based; 0 if none */
    /* internal */
    size_t cur_len;
    bool last_cr;
} ods2_text_var_scan_t;

void ods2_text_var_scan_init(ods2_text_var_scan_t *s);
void ods2_text_var_scan_feed(ods2_text_var_scan_t *s, const uint8_t *data, size_t n);
void ods2_text_var_scan_finish(ods2_text_var_scan_t *s);

/* Pass 2: an encoder reading the host text through `read_some` (which
   returns up to `n` bytes, 0 at the end of the file, and sets
   *error_out on a read error) and producing the records. */
typedef struct ods2_text_var_encoder ods2_text_var_encoder_t;

ods2_text_var_encoder_t *ods2_text_var_encoder_new(
    size_t (*read_some)(void *ctx, uint8_t *buf, size_t n, bool *error_out), void *ctx);
void ods2_text_var_encoder_free(ods2_text_var_encoder_t *e);

/* Fills exactly `n` bytes of `buf` with the next part of the converted
   content, for use as an ods2_content_source_t read function (its ctx
   is the encoder). Returns false if it can't: a read error, a line too
   long, or the input ending early - each meaning the file changed
   since it was scanned, or couldn't be read. ods2_text_var_problem()
   then says which. */
bool ods2_text_var_read(void *encoder, uint8_t *buf, size_t n);
const char *ods2_text_var_problem(const ods2_text_var_encoder_t *e);

#endif /* ODS2_TEXT_VAR_H */
