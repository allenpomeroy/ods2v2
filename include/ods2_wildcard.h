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

/* Returns true if `name` matches `pattern`. Both are plain C strings
   (no version number component - callers strip/append that
   separately, since version matching has its own rules e.g. ";*"
   meaning "any version"). */
bool ods2_wildcard_match(const char *pattern, const char *name);

#endif /* ODS2_WILDCARD_H */
