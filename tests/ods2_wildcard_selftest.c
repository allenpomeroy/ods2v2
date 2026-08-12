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

#include <stdio.h>
#include <assert.h>
#include "ods2_wildcard.h"

int main(void)
{
    /* Exact matches */
    assert(ods2_wildcard_match("DECUS.DIR", "DECUS.DIR"));
    assert(ods2_wildcard_match("decus.dir", "DECUS.DIR")); /* case-insensitive */
    assert(!ods2_wildcard_match("DECUS.DIR", "NETLIB020.DIR"));
    printf("PASS: exact match (case-insensitive)\n");

    /* '*' wildcard */
    assert(ods2_wildcard_match("*", "ANYTHING.AT.ALL"));
    assert(ods2_wildcard_match("*.DIR", "DECUS.DIR"));
    assert(ods2_wildcard_match("*.DIR", "NETLIB020.DIR"));
    assert(!ods2_wildcard_match("*.DIR", "BACKUP.SYS"));
    assert(ods2_wildcard_match("DECUS.*", "DECUS.DIR"));
    assert(!ods2_wildcard_match("DECUS.*", "NETLIB020.DIR"));
    printf("PASS: '*' wildcard matching\n");

    /* Multiple '*' */
    assert(ods2_wildcard_match("*.SYS", "BACKUP.SYS"));
    assert(ods2_wildcard_match("*A*.SYS", "BACKUP.SYS"));
    assert(ods2_wildcard_match("B*P.SYS", "BACKUP.SYS"));
    assert(!ods2_wildcard_match("B*Q.SYS", "BACKUP.SYS"));
    printf("PASS: multiple/embedded '*' wildcards\n");

    /* '%' single-character wildcard */
    assert(ods2_wildcard_match("%ACKUP.SYS", "BACKUP.SYS"));
    assert(!ods2_wildcard_match("%ACKUP.SYS", "XACKUP2.SYS")); /* wrong length */
    assert(ods2_wildcard_match("BACKUP.%YS", "BACKUP.SYS"));
    assert(ods2_wildcard_match("%%%%%%.SYS", "BACKUP.SYS")); /* 6 chars before .SYS */
    printf("PASS: '%%' single-character wildcard\n");

    /* Empty pattern / empty name edge cases */
    assert(ods2_wildcard_match("*", ""));
    assert(!ods2_wildcard_match("", "SOMETHING"));
    assert(ods2_wildcard_match("", ""));
    printf("PASS: empty pattern/name edge cases\n");

    /* Real-world case from our own test disk. */
    assert(ods2_wildcard_match("*.*", "NETLIB020.DIR"));
    assert(ods2_wildcard_match("NETLIB020.DIR", "netlib020.dir"));
    assert(!ods2_wildcard_match("SRC.DIR", "NETLIB020.DIR"));
    printf("PASS: real filenames from our test disk\n");

    printf("\nods2_wildcard_selftest: all checks passed\n");
    return 0;
}
