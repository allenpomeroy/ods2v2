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

#include "ods2_path.h"
#include <string.h>

bool ods2_parse_path(const char *input, ods2_parsed_path_t *out)
{
    const char *open_bracket;
    const char *close_bracket;
    size_t len;

    memset(out, 0, sizeof(*out));

    if (strlen(input) >= ODS2_PATH_MAX) {
        return false;
    }

    open_bracket = strchr(input, '[');
    if (open_bracket == NULL) {
        /* No brackets at all - the whole thing is a filename/wildcard,
           implicitly relative to whatever the caller's current
           default directory is. */
        strcpy(out->filename, input);
        out->had_brackets = false;
        out->relative = true;
        return true;
    }
    out->had_brackets = true;

    close_bracket = strchr(open_bracket, ']');
    if (close_bracket == NULL) {
        return false; /* unmatched '[' */
    }
    if (strchr(close_bracket + 1, '[') != NULL) {
        return false; /* more than one bracketed section - not supported */
    }

    len = (size_t) (close_bracket - open_bracket - 1);
    if (len >= ODS2_PATH_MAX) {
        return false;
    }
    memcpy(out->dir_path, open_bracket + 1, len);
    out->dir_path[len] = '\0';

    /* VMS's recursive-subtree wildcard: a trailing "..." means "this
       directory and everything beneath it". Checked before the
       leading-'.' relative check below, since "[DECUS...]" has both a
       trailing "..." and (after stripping it) no leading '.' at all -
       order matters for a case like "[...]" itself, which is ONLY the
       trailing "...", nothing else. */
    {
        size_t dlen = strlen(out->dir_path);
        if (dlen >= 3 &&
            out->dir_path[dlen - 1] == '.' &&
            out->dir_path[dlen - 2] == '.' &&
            out->dir_path[dlen - 3] == '.') {
            out->recursive = true;
            out->dir_path[dlen - 3] = '\0';
        }
    }

    /* VMS's up-level notation: a leading '-' (optionally repeated via
       "-.-.-..." for multiple levels) means "go up N levels from the
       current default before applying whatever follows" - e.g. "[-]"
       alone, "[-.-]" for two levels up, or "[-.SUBDIR]" for "up one
       level, then into SUBDIR" (a sibling of the current default).
       Checked before the leading-'.' relative check below, since '-'
       isn't '.', but both mean relative=true - going "up" only makes
       sense relative to something. */
    if (out->dir_path[0] == '-') {
        size_t pos = 0;
        size_t dlen = strlen(out->dir_path);
        while (pos < dlen && out->dir_path[pos] == '-') {
            out->up_levels++;
            pos++;
            if (pos < dlen && out->dir_path[pos] == '.') {
                pos++;
            }
        }
        out->relative = true;
        memmove(out->dir_path, out->dir_path + pos, dlen - pos + 1); /* +1 for the nul */
    }

    /* VMS's relative-to-default notation: a leading '.' means "under
       the current default directory" rather than "from root". */
    if (out->dir_path[0] == '.') {
        out->relative = true;
        memmove(out->dir_path, out->dir_path + 1, strlen(out->dir_path));
    }

    /* "[000000]" and "[]" both mean root - our internal convention
       for root is an empty dir_path string. Checked after the
       recursive/relative stripping above, so "[...]" (dir_path="..."
       before stripping) correctly reduces to root, not to "000000"
       -handling specifically. */
    if (strcmp(out->dir_path, "000000") == 0) {
        out->dir_path[0] = '\0';
    }

    strcpy(out->filename, close_bracket + 1);
    return true;
}
