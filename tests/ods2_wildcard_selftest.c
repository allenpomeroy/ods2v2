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
#include <string.h>
#include "ods2_wildcard.h"

/* Every concrete path over names {A, B, AB} up to `max_depth` levels,
   for checking the pruning function exhaustively below. */
static const char *NAMES[] = { "A", "B", "AB" };

/* True if some path strictly below `path`, at most `levels_left`
   deeper, matches `spec` - brute force, for comparison. */
static bool brute_descend(const char *spec, const char *path, int levels_left)
{
    size_t i;
    if (levels_left == 0) return false;
    for (i = 0; i < 3; i++) {
        char child[128];
        snprintf(child, sizeof child, "%s%s%s", path, path[0] ? "." : "", NAMES[i]);
        if (ods2_dir_spec_match(spec, child)) return true;
        if (brute_descend(spec, child, levels_left - 1)) return true;
    }
    return false;
}

static int check_descend_all(const char *spec, const char *path, int depth)
{
    int checked = 1;
    size_t i;
    /* 6 more levels is deeper than any spec below can need to show a
       difference, so brute force is exact for these specs. */
    if (ods2_dir_spec_could_descend(spec, path) != brute_descend(spec, path, 6)) {
        printf("FAIL: could_descend(%s, \"%s\") = %d, brute force says %d\n", spec, path,
               ods2_dir_spec_could_descend(spec, path), brute_descend(spec, path, 6));
        assert(0);
    }
    if (depth == 0) return checked;
    for (i = 0; i < 3; i++) {
        char child[128];
        snprintf(child, sizeof child, "%s%s%s", path, path[0] ? "." : "", NAMES[i]);
        checked += check_descend_all(spec, child, depth - 1);
    }
    return checked;
}

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

    /* --- Directory specs --- */
    {
        struct { const char *spec, *path; bool match; } m[] = {
            { "",          "",            true  },
            { "",          "A",           false },
            { "*",         "TEST1",       true  },
            { "*",         "TEST1.TEST2", false },
            { "*",         "",            false },
            { "...",       "",            true  }, /* [...] includes the start */
            { "...",       "A.B.C",       true  },
            { "*...",      "",            false }, /* [*...] excludes the root itself */
            { "*...",      "TEST1",       true  },
            { "*...",      "TEST1.TEST2", true  },
            { "A...B",     "A.B",         true  }, /* zero levels between */
            { "A...B",     "A.X.Y.B",     true  },
            { "A...B",     "A",           false },
            { "A...B",     "B",           false },
            { "A...B",     "A.B.C",       false },
            { "A....B",    "A.X.B",       true  }, /* "...." = ellipsis + separator */
            { "...SALES",  "SALES",       true  },
            { "...SALES",  "JONES.SALES", true  },
            { "...SALES",  "JONES.SALES.X", false },
            { "TEST%",     "TEST1",       true  },
            { "TEST%",     "TEST12",      false },
            { "A.*",       "A.B",         true  },
            { "A.*",       "A",           false },
            { "A.*",       "A.B.C",       false },
            { "*.*...",    "A",           false },
            { "*.*...",    "A.B.C",       true  },
            { "test1...",  "TEST1.TEST2", true  }, /* case-insensitive */
            { "A...B...",  "A.X.B.Y",     true  },
        };
        size_t i;
        for (i = 0; i < sizeof m / sizeof m[0]; i++) {
            if (ods2_dir_spec_match(m[i].spec, m[i].path) != m[i].match) {
                printf("FAIL: dir_spec_match(%s, \"%s\") should be %d\n",
                       m[i].spec, m[i].path, m[i].match);
                assert(0);
            }
        }
        printf("PASS: directory specs match: '*', '%%', and the ellipsis at the "
               "start, middle and end (zero or more levels)\n");
    }
    {
        const char *specs[] = { "", "*", "...", "*...", "A...B", "...B", "A.*", "*.B",
                                "A...B...", "*.*...", "A%...", "...A.B", "A....B" };
        size_t i;
        int checked = 0;
        for (i = 0; i < sizeof specs / sizeof specs[0]; i++) {
            checked += check_descend_all(specs[i], "", 4);
        }
        printf("PASS: could_descend() agrees with brute force for %zu specs at "
               "%d paths (no subtree wrongly pruned or wrongly walked)\n",
               sizeof specs / sizeof specs[0], checked);
    }
    {
        char prefix[64];
        const char *rest;
        rest = ods2_dir_spec_split_prefix("A.B.*.C", prefix, sizeof prefix);
        assert(rest && strcmp(prefix, "A.B") == 0 && strcmp(rest, "*.C") == 0);
        rest = ods2_dir_spec_split_prefix("A...B", prefix, sizeof prefix);
        assert(rest && strcmp(prefix, "A") == 0 && strcmp(rest, "...B") == 0);
        rest = ods2_dir_spec_split_prefix("*...", prefix, sizeof prefix);
        assert(rest && strcmp(prefix, "") == 0 && strcmp(rest, "*...") == 0);
        rest = ods2_dir_spec_split_prefix("...", prefix, sizeof prefix);
        assert(rest && strcmp(prefix, "") == 0 && strcmp(rest, "...") == 0);
        rest = ods2_dir_spec_split_prefix("A.B", prefix, sizeof prefix);
        assert(rest && strcmp(prefix, "A.B") == 0 && strcmp(rest, "") == 0);
        rest = ods2_dir_spec_split_prefix("A.B...", prefix, sizeof prefix);
        assert(rest && strcmp(prefix, "A.B") == 0 && strcmp(rest, "...") == 0);
        assert(ods2_dir_spec_split_prefix("LONGNAME.*", prefix, 4) == NULL);
        printf("PASS: split_prefix() separates the plain leading names from "
               "the wildcard part\n");
    }
    {
        assert(ods2_dir_spec_valid(""));
        assert(ods2_dir_spec_valid("A.B...C"));
        assert(ods2_dir_spec_valid("...A"));
        assert(!ods2_dir_spec_valid("A..B"));
        assert(!ods2_dir_spec_valid("A."));
        assert(!ods2_dir_spec_valid(".A"));
        assert(!ods2_dir_spec_valid("A......B"));
        assert(ods2_dir_spec_has_wildcard("A...B") && ods2_dir_spec_has_wildcard("A%"));
        assert(!ods2_dir_spec_has_wildcard("A.B"));
        printf("PASS: malformed directory specs are rejected\n");
    }

    printf("\nods2_wildcard_selftest: all checks passed\n");
    return 0;
}
