#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_verlimit_check.sh - SET FILE/VERSION_LIMIT=n <spec>: the version limit (DIR$W_VERLIMIT) of the directory entries a
# specification names, with wildcards and [dir...] as in DIRECTORY. The remedy for volumes written before ods2v2 gave its
# entries VMS's version limit (32767): with limit 1 VMS purges version 1 of a file when it makes version 2. Called by
# `make test`; run from the project root.

SYNTH=samples/synthetic_disk.img
DISK=samples/cli_verlimit_disk.img
ODS2=./ods2
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

cp "$SYNTH" "$DISK"
echo "text" > "$DISK.txt"
$ODS2 "$DISK" "CREATE/DIRECTORY [VL]" >/dev/null 2>&1
$ODS2 "$DISK" "CREATE/DIRECTORY [VL.SUB]" >/dev/null 2>&1
$ODS2 "$DISK" "COPY $DISK.txt [VL]A.TXT" >/dev/null 2>&1
$ODS2 "$DISK" "COPY $DISK.txt [VL]B.TXT" >/dev/null 2>&1
$ODS2 "$DISK" "COPY $DISK.txt [VL.SUB]C.TXT" >/dev/null 2>&1

out=$($ODS2 "$DISK" "SET FILE/VERSION_LIMIT=3 [VL]A.TXT" 2>&1); rc=$?
# the entry already carries 32767: one record changes to 3
check "SET FILE/VERSION_LIMIT=3 [VL]A.TXT: one name, one record changed, exit 0" "1 name looked at, 1 directory record changed 0" \
    "$(echo "$out" | sed -n 's/.*VERLIMIT, \(1 name looked at, 1 directory record changed\) .*/\1/p') $rc"
out=$($ODS2 "$DISK" "SET FILE/VERSION_LIMIT=3 [VL]A.TXT" 2>&1)
check "... again: nothing to change" "1 name looked at, 0 directory records changed" \
    "$(echo "$out" | sed -n 's/.*VERLIMIT, \(1 name looked at, 0 directory records changed\) .*/\1/p')"
out=$($ODS2 "$DISK" "SET FILE/VERSION_LIMIT=7 [VL...]*.*" 2>&1); rc=$?
# [VL]: A.TXT, B.TXT, SUB.DIR; [VL.SUB]: C.TXT
check "SET FILE/VERSION_LIMIT=7 [VL...]*.*: four names, all four records changed (SUB.DIR too), exit 0" "4 4 0" \
    "$(echo "$out" | sed -n 's/.*VERLIMIT, \([0-9]*\) names looked at, \([0-9]*\) directory records changed.*/\1 \2/p') $rc"
out=$($ODS2 "$DISK" "SET FILE/VERSION_LIMIT=0 [VL...]*.*" 2>&1)
check "SET FILE/VERSION_LIMIT=0 is no limit (32767): all four back" "version limit 32767" "$(echo "$out" | sed -n 's/.*changed to \(version limit 32767\).*/\1/p')"
out=$($ODS2 "$DISK" "SET FILE/VERSION_LIMIT=3 [000000]*.*" 2>&1)
check "the reserved files' entries (limit 1) are left alone: only DECUS.DIR and VL.DIR are looked at" "yes" \
    "$(echo "$out" | grep -q 'VERLIMIT, 2 names looked at, 2 directory records changed' && echo yes)"
$ODS2 "$DISK" "SET FILE/VERSION_LIMIT=3 [VL]NOSUCH.TXT" >/dev/null 2>&1
check "no match: warning, exit 0 as DIRECTORY" 0 "$?"
$ODS2 "$DISK" "SET FILE/VERSION_LIMIT=40000 [VL]A.TXT" >/dev/null 2>&1
check "a limit above 32767 is refused (exit 1)" 1 "$?"
$ODS2 "$DISK" "SET FILE/VERSION_LIMIT=3" >/dev/null 2>&1
check "no specification is refused (exit 1)" 1 "$?"

rm -f "$DISK" "$DISK.txt"
if [ $fails -ne 0 ]; then
    echo "cli_verlimit_check: $fails FAILED"
    exit 1
fi
echo "cli_verlimit_check: all checks passed"
