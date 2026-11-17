#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_off64_check.sh - a source-level guard for issue #25: nothing in the program may address an image or a host file with a
# `long`. On a 64-bit Unix host `long` is 64 bits, so no run can tell the two apart; on a 32-bit host or on Windows (LLP64) a
# `long` stops at 2 GiB, and a volume or a file has blocks past it (a VMS volume may hold 2^32 blocks, 2 TiB, and a file as
# many). tests/ods2_off64_selftest does what a run can do on this host: it puts blocks past 2 GiB and past 4 GiB on a
# sparse image and checks them with pread(). This script checks what no run here can:
#
#  - the Makefile compiles with -D_FILE_OFFSET_BITS=64, so that off_t is 64 bits on a 32-bit Unix host;
#  - src/ods2_volume.c asserts at compile time that off_t is at least 64 bits (a build that forgets the flag stops);
#  - no source in src/, include/, tools/ and tests/*.c calls fseek() or ftell() (they take and return a `long`): seeking is
#    fseeko()/ftello() with an off_t. The test is for the whole words `fseek(` and `ftell(` in code: comments, string and
#    character literals are left out, and fseeko/ftello do not match, so a change to the code that keeps to the rule
#    passes. Called by `make test`; run from the project root.

fails=0

check() { # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"
    else
        echo "FAIL: $1"
        echo "      expected: $2"
        echo "      got:      $3"
        fails=$((fails + 1))
    fi
}

check "the Makefile's CFLAGS has -D_FILE_OFFSET_BITS=64" "1" \
    "$(grep -E '^CFLAGS[[:space:]]*=' Makefile | grep -c -e '-D_FILE_OFFSET_BITS=64')"

check "src/ods2_volume.c asserts sizeof(off_t) >= 8 at compile time" "1" \
    "$(grep -c '_Static_assert(sizeof(off_t) >= 8' src/ods2_volume.c)"

# the C sources, minus comments and string/character literals, searched for fseek( and ftell( as whole words
found=$(python3 -I - src include tools tests <<'PY'
import os, re, sys

def code_only(text):
    """The text with comments and string/character literals blanked out (newlines kept)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == '/*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif two == '//':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)

word = re.compile(r'\b(fseek|ftell)\s*\(')
for top in sys.argv[1:]:
    for dirpath, dirs, files in os.walk(top):
        dirs[:] = [d for d in dirs if d not in ('third_party',)]
        if top == 'tests' and dirpath != 'tests':
            continue                                   # tests/*.c only
        for name in sorted(files):
            if not name.endswith(('.c', '.h')):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding='utf-8', errors='replace') as f:
                code = code_only(f.read())
            for lineno, line in enumerate(code.split('\n'), 1):
                m = word.search(line)
                if m:
                    print('%s:%d: %s(' % (path, lineno, m.group(1)))
PY
)
check "no fseek() or ftell() in src/, include/, tools/ or tests/*.c (fseeko/ftello with an off_t)" "" "$found"

if [ $fails -ne 0 ]; then
    echo "cli_off64_check: $fails FAILED"
    exit 1
fi
echo "cli_off64_check: all checks passed"
