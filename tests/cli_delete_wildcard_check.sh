#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_delete_wildcard_check.sh - DELETE with '*' and '%' in the file
# name. DELETE used to look a wildcard up as a literal name ("*.*: name
# not found in directory"). Every match in the one directory is now
# deleted, with the same version rules as for one file: no version the
# highest, ;n that version, ;-n the n-th lower, ;* every version.
# Reserved files are still refused, one by one, and a spec with no
# match fails. Runs on copies of the multi-version fixture that
# build/ods2_versions_selftest makes ([VERS]: 64 versions of
# SAVAGE.LIS, 3 of NOTE.TXT, 1 of OTHER.DAT - 68 in all) and of the
# starter volume. Called by `make test`; run from the project root.

FIXTURE=samples/versions_cli_disk.img
GZ=samples/empty_ods2_volume.dsk.gz
DISK=samples/cli_delwild_disk.img
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

# cli_versions_check.sh uses the fixture up and removes it: make it again if it is gone.
[ -f "$FIXTURE" ] || ./build/ods2_versions_selftest >/dev/null 2>&1
[ -f "$FIXTURE" ] || { echo "FAIL: $FIXTURE missing (build/ods2_versions_selftest did not make it)"; exit 1; }
fresh() { rm -f "$DISK"; cp "$FIXTURE" "$DISK"; }
total() { "$ODS2" "$DISK" "DIR [VERS]" 2>/dev/null | sed -n 's/^Total of \([0-9]*\) files*\./\1/p'; }
deleted() { "$ODS2" "$DISK" "DELETE $1" 2>/dev/null | grep -c '^%ODS2-I-DELETED'; }

fresh
check "DELETE [VERS]S*.LIS deletes only the highest version" 1 "$(deleted '[VERS]S*.LIS')"
check "...SAVAGE.LIS;64 is the one gone" 0 "$("$ODS2" "$DISK" 'DIR [VERS]SAVAGE.LIS;64' 2>/dev/null | grep -c ';64')"
check "...67 entries remain" 67 "$(total)"

fresh
check "DELETE [VERS]*.*;-1 deletes the next lower version of each name that has one" 2 "$(deleted '[VERS]*.*;-1')"

fresh
check "DELETE [VERS]%%%%%%.LIS;60 deletes that one version" 1 "$(deleted '[VERS]%%%%%%.LIS;60')"

fresh
check "DELETE [VERS]*.*;* deletes all 68 versions" 68 "$(deleted '[VERS]*.*;*')"
check "...the directory is empty" "" "$(total)"
"$ODS2" "$DISK" "DELETE [VERS]*.*" >/dev/null 2>&1
check "DELETE of a wildcard that matches nothing fails (exit 1)" 1 $?

fresh
"$ODS2" "$DISK" "DELETE [VERS]N*.TXT;*" >/dev/null 2>&1
check "a wildcard leaves non-matching names alone" 65 "$(total)"

rm -f "$DISK"
gunzip -c "$GZ" > "$DISK"
out=$("$ODS2" "$DISK" "DELETE [000000]*.SYS;*" 2>&1)
check "DELETE [000000]*.SYS;* refuses each reserved file" 9 "$(echo "$out" | grep -c 'reserved files')"
check "...and deletes none of them" 0 "$(echo "$out" | grep -c '^%ODS2-I-DELETED')"
"$ODS2" "$DISK" "DELETE [000000]*.SYS;*" >/dev/null 2>&1
check "...exit status 1" 1 $?

rm -f "$DISK" "$FIXTURE"
if [ "$fails" -ne 0 ]; then
    echo "cli_delete_wildcard_check: $fails check(s) FAILED"
    exit 1
fi
echo "cli_delete_wildcard_check: all checks passed"
