#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_large_file_check.sh - large files through the ods2 CLI, on the
# disk build/ods2_large_file_selftest leaves behind
# (samples/large_file_disk.img). Covers issue #2 and the regression
# report after it: COPY of files past the old 12 MiB read buffer and
# past one header's Format-1-only map capacity (~9.6 MiB), read back
# byte for byte with TYPE, from a regular file and from a pipe.
# Called by `make test`; run from the project root.

DISK=samples/large_file_disk.img
ODS2=./ods2
TMP=samples/cli_large_tmp
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

[ -f "$DISK" ] || { echo "FAIL: $DISK missing (run build/ods2_large_file_selftest first)"; exit 1; }
mkdir -p "$TMP"

# Deterministic, incompressible-looking content of an exact size.
make_file() { # make_file <path> <bytes> <seed>
    awk -v n="$2" -v s="$3" 'BEGIN { srand(s); for (i = 0; i < n / 16; i++) printf "%08x%08x", int(rand()*4294967295), i }' |
        head -c "$2" > "$1"
}

make_file "$TMP/ten.bin" 10485760 10
make_file "$TMP/thirteen.bin" 13631488 13
make_file "$TMP/odd.bin" 27000001 27

check "COPY of a 10 MiB file (was: map area too small)" \
    "%ODS2-I-COPIED, 10485760 bytes to TEN.BIN;1" \
    "$($ODS2 $DISK COPY $TMP/ten.bin '[000000]TEN.BIN' 2>&1)"
check "COPY of a 13 MiB file (was: TOOBIG, 12 MiB buffer)" \
    "%ODS2-I-COPIED, 13631488 bytes to THIRTEEN.BIN;1" \
    "$($ODS2 $DISK COPY $TMP/thirteen.bin '[000000]THIRTEEN.BIN' 2>&1)"
check "COPY of a 27 MB file of odd length" \
    "%ODS2-I-COPIED, 27000001 bytes to ODD.BIN;1" \
    "$($ODS2 $DISK COPY $TMP/odd.bin '[000000]ODD.BIN' 2>&1)"
check "COPY from a pipe (no file size to go by)" \
    "%ODS2-I-COPIED, 13631488 bytes to PIPED.BIN;1" \
    "$(cat $TMP/thirteen.bin | $ODS2 $DISK COPY /dev/stdin '[000000]PIPED.BIN' 2>&1)"

for pair in ten.bin:TEN.BIN thirteen.bin:THIRTEEN.BIN odd.bin:ODD.BIN thirteen.bin:PIPED.BIN; do
    src=${pair%%:*}
    vms=${pair##*:}
    $ODS2 $DISK "TYPE [000000]$vms" > "$TMP/out.bin" 2>/dev/null
    if cmp -s "$TMP/$src" "$TMP/out.bin"; then same=identical; else same=different; fi
    check "TYPE $vms reads back byte for byte" identical "$same"
done

check "DELETE of a large file" "" \
    "$($ODS2 $DISK 'DELETE [000000]ODD.BIN;1' 2>&1 | grep -v '^%ODS2-I')"
check "COPY again into the space it freed" \
    "%ODS2-I-COPIED, 27000001 bytes to ODD2.BIN;1" \
    "$($ODS2 $DISK COPY $TMP/odd.bin '[000000]ODD2.BIN' 2>&1)"

rm -rf "$TMP"
[ "$fails" -eq 0 ] || { echo "$fails CLI large-file check(s) failed"; exit 1; }
