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

/* ods2_wildcard.h - VMS-style wildcard matching for filenames.
 * Supports '*' (matches any sequence, including empty) and '%'
 * (matches exactly one character) - the two standard VMS wildcard
 * characters. Matching is case-insensitive, matching how directory
 * names are conventionally stored (uppercase) but users type either
 * case.
 */
#ifndef ODS2_WILDCARD_H
#define ODS2_WILDCARD_H

#include <stdbool.h>
#include <stddef.h>

/* Returns true if `name` matches `pattern`. Both are plain C strings
   (no version number component - callers strip/append that
   separately, since version matching has its own rules e.g. ";*"
   meaning "any version"). */
bool ods2_wildcard_match(const char *pattern, const char *name);

/* --- Directory specifications ---
 *
 * A directory spec here is the text inside VMS's brackets, already in
 * this project's internal form: dot-separated names with no brackets,
 * "" for the starting directory itself. Each name may use '*' and '%'
 * as above, and VMS's ellipsis "..." stands for any number of
 * directory levels, including none, anywhere in the spec:
 *
 *   "*"         every directory one level down
 *   "*..."      every directory one level down, and all below them
 *   "..."       the starting directory and every directory below it
 *   "A...B"     every directory named B anywhere below A (A.B, A.X.B)
 *   "...SALES"  every directory named SALES at any depth
 *
 * A concrete path is plain dot-separated names ("A.B.C"), matched
 * name by name. */

/* Is `spec` well formed? Rejects empty names ("A..B", "A.", ".A")
   and runs of dots that are neither a separator nor an ellipsis
   ("A.....", "......"). "" is valid (the starting directory). */
bool ods2_dir_spec_valid(const char *spec);

/* Does `spec` contain '*', '%' or "..." - i.e. could it name more
   than one directory? */
bool ods2_dir_spec_has_wildcard(const char *spec);

/* Does the concrete path `path` match `spec` exactly? */
bool ods2_dir_spec_match(const char *spec, const char *path);

/* Could some path strictly deeper than `path` (path plus one or more
   further names) match `spec`? Lets a tree walk skip subtrees that
   cannot contain a match. */
bool ods2_dir_spec_could_descend(const char *spec, const char *path);

/* Splits `spec` into its leading run of plain names (no wildcard, no
   ellipsis), copied to `prefix_out` ("" if there is none), and
   returns a pointer into `spec` at the remainder - "" if the whole
   spec is plain. The remainder starts at a name or at an ellipsis,
   never at a separating dot:
     "A.B.*.C" -> prefix "A.B", rest "*.C"
     "A...B"   -> prefix "A",   rest "...B"
     "*..."    -> prefix "",    rest "*..."
   Returns NULL if `prefix_out_size` is too small. */
const char *ods2_dir_spec_split_prefix(const char *spec, char *prefix_out,
                                       size_t prefix_out_size);

#endif /* ODS2_WILDCARD_H */
