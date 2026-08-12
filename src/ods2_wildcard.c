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

#include "ods2_wildcard.h"
#include <ctype.h>
#include <stddef.h>

static int upper(char c)
{
    return toupper((unsigned char) c);
}

bool ods2_wildcard_match(const char *pattern, const char *name)
{
    const char *p = pattern;
    const char *n = name;
    const char *star_p = NULL; /* pattern position right after the last '*' seen */
    const char *star_n = NULL; /* name position to resume trying from */

    while (*n) {
        if (*p == '%' || (*p && upper(*p) == upper(*n))) {
            /* '%' matches exactly one character, or literal match */
            p++;
            n++;
        } else if (*p == '*') {
            /* Remember this position; try matching zero characters
               first, backtrack to consume one more from `name` on
               failure. */
            star_p = p + 1;
            star_n = n;
            p++;
        } else if (star_p != NULL) {
            /* Mismatch, but we have a previous '*' to backtrack to:
               let it consume one more character from name. */
            p = star_p;
            star_n++;
            n = star_n;
        } else {
            return false;
        }
    }
    /* Consume any trailing '*' characters in the pattern - they can
       always match the empty remainder. */
    while (*p == '*') p++;

    return *p == '\0';
}
