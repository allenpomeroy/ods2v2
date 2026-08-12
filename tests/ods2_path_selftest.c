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
#include <string.h>
#include <assert.h>
#include "ods2_path.h"

static void check_full(const char *input, const char *expected_dir, const char *expected_file,
                        bool expected_had_brackets, bool expected_relative, bool expected_recursive,
                        int expected_up_levels)
{
    ods2_parsed_path_t p;
    bool ok = ods2_parse_path(input, &p);
    assert(ok);
    if (strcmp(p.dir_path, expected_dir) != 0 || strcmp(p.filename, expected_file) != 0 ||
        p.had_brackets != expected_had_brackets || p.relative != expected_relative ||
        p.recursive != expected_recursive || p.up_levels != expected_up_levels) {
        printf("FAIL: parse(%s) = dir=%s file=%s brackets=%d relative=%d recursive=%d up=%d, "
               "expected dir=%s file=%s brackets=%d relative=%d recursive=%d up=%d\n",
               input, p.dir_path, p.filename, p.had_brackets, p.relative, p.recursive, p.up_levels,
               expected_dir, expected_file, expected_had_brackets, expected_relative,
               expected_recursive, expected_up_levels);
        assert(0);
    }
}

static void check(const char *input, const char *expected_dir, const char *expected_file,
                   bool expected_had_brackets, bool expected_relative, bool expected_recursive)
{
    check_full(input, expected_dir, expected_file, expected_had_brackets,
               expected_relative, expected_recursive, 0);
}

int main(void)
{
    /* Basic cases - had_brackets=true, relative=false, recursive=false. */
    check("[000000]", "", "", true, false, false);
    check("[]", "", "", true, false, false);
    check("[DECUS]", "DECUS", "", true, false, false);
    check("[DECUS.NETLIB020]", "DECUS.NETLIB020", "", true, false, false);
    check("[DECUS]FILE.TXT", "DECUS", "FILE.TXT", true, false, false);
    check("[DECUS.NETLIB020]FILE.TXT", "DECUS.NETLIB020", "FILE.TXT", true, false, false);
    check("[DECUS]*.*", "DECUS", "*.*", true, false, false);
    check("[DECUS.NETLIB020.SRC]*.C", "DECUS.NETLIB020.SRC", "*.C", true, false, false);
    printf("PASS: basic absolute-bracketed cases parse correctly\n");

    /* No brackets at all - had_brackets=false, relative=true (implicitly). */
    check("FILE.TXT", "", "FILE.TXT", false, true, false);
    check("*.*", "", "*.*", false, true, false);
    printf("PASS: no-brackets cases correctly marked relative\n");

    /* Relative notation: leading '.' inside brackets. */
    check("[.NETLIB020]", "NETLIB020", "", true, true, false);
    check("[.NETLIB020]FILE.TXT", "NETLIB020", "FILE.TXT", true, true, false);
    printf("PASS: leading-'.' relative notation parses correctly\n");

    /* Recursive notation: trailing "..." inside brackets. */
    check("[...]", "", "", true, false, true);
    check("[...]*.*", "", "*.*", true, false, true);
    check("[DECUS...]", "DECUS", "", true, false, true);
    check("[DECUS...]*.*", "DECUS", "*.*", true, false, true);
    check("[DECUS.NETLIB020...]*.*", "DECUS.NETLIB020", "*.*", true, false, true);
    printf("PASS: trailing-'...' recursive notation parses correctly\n");

    /* Combined: relative AND recursive. */
    check("[.DECUS...]*.*", "DECUS", "*.*", true, true, true);
    printf("PASS: combined relative+recursive notation parses correctly\n");

    /* Up-level notation: leading '-' (VMS's "go up N levels"). */
    check_full("[-]", "", "", true, true, false, 1);
    check_full("[-.-]", "", "", true, true, false, 2);
    check_full("[-.-.-]", "", "", true, true, false, 3);
    check_full("[-.SUBDIR]", "SUBDIR", "", true, true, false, 1);
    check_full("[-.-.SUBDIR]", "SUBDIR", "", true, true, false, 2);
    check_full("[-]FILE.TXT", "", "FILE.TXT", true, true, false, 1);
    printf("PASS: up-level '[-]' notation parses correctly\n");

    /* Malformed input is rejected, not silently mishandled. */
    {
        ods2_parsed_path_t p;
        assert(!ods2_parse_path("[UNCLOSED", &p));
        assert(!ods2_parse_path("[A][B]FILE.TXT", &p));
        printf("PASS: malformed bracket syntax is correctly rejected\n");
    }

    /* Overlong input is rejected rather than silently truncated. */
    {
        char huge[600];
        ods2_parsed_path_t p;
        memset(huge, 'A', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = '\0';
        assert(!ods2_parse_path(huge, &p));
        printf("PASS: overlong input is correctly rejected\n");
    }

    printf("\nods2_path_selftest: all checks passed\n");
    return 0;
}
