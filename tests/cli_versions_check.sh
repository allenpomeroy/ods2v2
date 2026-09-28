#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_versions_check.sh - runs the ods2 CLI against the multi-version
# fixture that build/ods2_versions_selftest leaves behind
# (samples/versions_cli_disk.img) and checks the user-visible results
# for issues #4 (DIR lists every version), #5 (NAME;VERSION works with
# TYPE/DIR/DELETE) and #6 (end-of-file handling via TYPE).
# Called by `make test`; run from the project root.

DISK=samples/versions_cli_disk.img
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

[ -f "$DISK" ] || { echo "FAIL: $DISK missing (run build/ods2_versions_selftest first)"; exit 1; }

# Issue #4: every version listed, including a name split over records.
check "DIR [VERS] lists all 68 versions" \
    "Total of 68 files." "$($ODS2 $DISK 'DIR [VERS]' | tail -1)"
check "DIR shows all 64 versions of SAVAGE.LIS (was 2)" \
    "64" "$($ODS2 $DISK 'DIR [VERS]SAVAGE.LIS' | grep -c '^SAVAGE.LIS')"
check "DIR *.*; lists only the highest version of each name" \
    "NOTE.TXT;3 SAVAGE.LIS;64 YOW.ELC;1" \
    "$($ODS2 $DISK 'DIR [VERS]*.*;' | grep ';' | tr -d ' ' | tr '\n' ' ' | sed 's/ $//')"
check "DIR SAVAGE.LIS;-1 selects the next lower version" \
    "SAVAGE.LIS;63" "$($ODS2 $DISK 'DIR [VERS]SAVAGE.LIS;-1' | grep ';' | tr -d ' ')"

# Issue #5: explicit versions.
yow="Yow! Are we having fun yet?"
check "TYPE YOW.ELC (no version)" "$yow" "$($ODS2 $DISK 'TYPE [VERS]YOW.ELC' 2>&1)"
check "TYPE YOW.ELC;1" "$yow" "$($ODS2 $DISK 'TYPE [VERS]YOW.ELC;1' 2>&1)"
check "TYPE YOW.ELC; (highest)" "$yow" "$($ODS2 $DISK 'TYPE [VERS]YOW.ELC;' 2>&1)"
check "TYPE NOTE.TXT;1 reads the old version" \
    "note version one" "$($ODS2 $DISK 'TYPE [VERS]NOTE.TXT;1' 2>&1)"
check "TYPE NOTE.TXT;-1 reads the next lower version" \
    "note version two" "$($ODS2 $DISK 'TYPE [VERS]NOTE.TXT;-1' 2>&1)"
check "TYPE of an absent version reports it" \
    "%ODS2-E-FNF, NOTE.TXT;9 not found" "$($ODS2 $DISK 'TYPE [VERS]NOTE.TXT;9' 2>&1)"

# Issue #6 / EOF mark: a real VMS-written empty file TYPEs as 0 bytes.
check "TYPE BADBLK.SYS (real empty VMS file) outputs 0 bytes" \
    "0" "$($ODS2 $DISK 'TYPE [000000]BADBLK.SYS' 2>/dev/null | wc -c | tr -d ' ')"

# Issue #5: DELETE one version leaves the others.
check "DELETE NOTE.TXT;2 deletes just that version" \
    "%ODS2-I-DELETED, deleted NOTE.TXT;2" "$($ODS2 $DISK 'DELETE [VERS]NOTE.TXT;2' 2>&1)"
check "NOTE.TXT;3 and ;1 remain" \
    "NOTE.TXT;3 NOTE.TXT;1" \
    "$($ODS2 $DISK 'DIR [VERS]NOTE.TXT' | grep ';' | tr -d ' ' | tr '\n' ' ' | sed 's/ $//')"
check "NOTE.TXT;1 still reads correctly" \
    "note version one" "$($ODS2 $DISK 'TYPE [VERS]NOTE.TXT;1' 2>&1)"
check "DELETE NOTE.TXT;* deletes the remaining versions" \
    "2" "$($ODS2 $DISK 'DELETE [VERS]NOTE.TXT;*' 2>&1 | grep -c DELETED)"
check "SAVAGE.LIS and YOW.ELC untouched by those deletes" \
    "Total of 65 files." "$($ODS2 $DISK 'DIR [VERS]' | tail -1)"

# COPY never stores ";version" in a name.
echo "hello" > samples/cli_versions_tmp.txt
check "COPY to NAME;3 is refused" \
    "1" "$($ODS2 $DISK 'COPY samples/cli_versions_tmp.txt [VERS]NEW.TXT;3' 2>&1 | grep -c BADVER)"
check "COPY to NAME;1 creates NEW.TXT;1" \
    "NEW.TXT;1" \
    "$($ODS2 $DISK 'COPY samples/cli_versions_tmp.txt [VERS]NEW.TXT;1' >/dev/null 2>&1; \
       $ODS2 $DISK 'DIR [VERS]NEW.TXT' | grep ';' | tr -d ' ')"
rm -f samples/cli_versions_tmp.txt

rm -f "$DISK"
if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails CLI version check(s) failed"
    exit 1
fi
