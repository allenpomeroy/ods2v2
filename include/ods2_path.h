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

/* ods2_path.h - parses VMS-style path strings (e.g.
 * "[DECUS.NETLIB020]FILE.TXT", "[DECUS]*.*", "[000000]", "FILE.TXT",
 * "[.NETLIB020]", "[DECUS...]") into a directory path (our internal
 * dot-separated, no-brackets form) and a filename/wildcard part, plus
 * flags for VMS's relative-path and recursive-wildcard notations.
 * This is purely string manipulation - no disk access - kept separate
 * from ods2_volume.h so it's trivially testable in isolation.
 */
#ifndef ODS2_PATH_H
#define ODS2_PATH_H

#include <stdbool.h>

#define ODS2_PATH_MAX 256

typedef struct {
    char dir_path[ODS2_PATH_MAX];  /* dot-separated, no brackets, no
                                       leading '.' or trailing "...",
                                       "" for root */
    char filename[ODS2_PATH_MAX];  /* e.g. "FILE.TXT", "*.*", or "" if
                                       the input was just a directory
                                       reference */
    bool had_brackets;             /* was a '[...]' section present at
                                       all? Distinguishes an explicit
                                       "[000000]"/"[]" (absolute root)
                                       from a bare "FILE.TXT" with no
                                       brackets at all (relative to
                                       whatever the caller's current
                                       default directory is) - both
                                       produce dir_path="", but they
                                       mean different things. */
    bool relative;                 /* true if the bracketed content
                                       started with '.' (VMS's
                                       "[.SUBDIR]" relative-to-default
                                       notation), OR if there were no
                                       brackets at all (also implicitly
                                       relative to the current
                                       default) - false only for an
                                       explicit absolute bracketed
                                       path like "[DECUS]". */
    bool recursive;                /* true if the bracketed content
                                       ended with "..." (VMS's
                                       recursive-subtree wildcard,
                                       e.g. "[DECUS...]*.*" or
                                       "[...]*.*"). */
    int up_levels;                 /* VMS's "[-]" notation: number of
                                       levels to go up from the current
                                       default before applying dir_path
                                       - "[-]" is up_levels=1,
                                       dir_path=""; "[-.-]" is
                                       up_levels=2; "[-.SUBDIR]" is
                                       up_levels=1, dir_path="SUBDIR"
                                       (a sibling of the current
                                       default). Implies relative=true,
                                       since going "up" only makes
                                       sense relative to something. 0
                                       when no '-' notation was used. */
} ods2_parsed_path_t;

/* Parses `input` into `out`. Recognizes:
 *   [000000]              -> dir_path="", had_brackets=true, relative=false, recursive=false
 *   []                     -> dir_path="", had_brackets=true, relative=false, recursive=false
 *   [DECUS]                -> dir_path="DECUS", had_brackets=true, relative=false
 *   [DECUS.NETLIB020]       -> dir_path="DECUS.NETLIB020", had_brackets=true, relative=false
 *   [.NETLIB020]             -> dir_path="NETLIB020", had_brackets=true, relative=true
 *   [DECUS...]                 -> dir_path="DECUS", recursive=true
 *   [...]                        -> dir_path="", recursive=true
 *   [-]                            -> up_levels=1, dir_path="", relative=true
 *   [-.-]                           -> up_levels=2, dir_path="", relative=true
 *   [-.SUBDIR]                       -> up_levels=1, dir_path="SUBDIR", relative=true
 *   [DECUS]FILE.TXT                -> dir_path="DECUS", filename="FILE.TXT"
 *   FILE.TXT                         -> dir_path="", filename="FILE.TXT", had_brackets=false, relative=true
 *   [DECUS]*.*                         -> dir_path="DECUS", filename="*.*"
 *   *.*                                  -> dir_path="", filename="*.*", had_brackets=false, relative=true
 * Returns false if `input` is too long to fit ODS2_PATH_MAX, or the
 * bracket syntax is malformed (unmatched '[' or ']', or more than one
 * bracketed section).
 */
bool ods2_parse_path(const char *input, ods2_parsed_path_t *out);

#endif /* ODS2_PATH_H */
