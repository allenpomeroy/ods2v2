#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_dir_wildcard_check.sh - runs the ods2 CLI's DIR against a small
# directory tree and checks VMS's directory wildcards: '*' and '%' in
# any directory name, the ellipsis anywhere in the spec ("[*...]",
# "[...SALES]", "[A...B]"), relative forms after SET DEFAULT, and
# VMS DIRECTORY's output rules (directories with nothing to show are
# left out; a grand total only across several directories; "no files
# found" when nothing matches). Also checks that commands acting on a
# single directory refuse a wildcard one.
# Called by `make test`; run from the project root.

DISK=samples/cli_wildcard_disk.img
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

# The directories a command listed, in order, on one line.
dirs() { $ODS2 $DISK "$@" 2>&1 | sed -n 's/^Directory \[\(.*\)\]$/\1/p' | tr '\n' ' ' | sed 's/ $//'; }

cp samples/synthetic_disk.img $DISK || exit 1
printf 'hello\n' > samples/cli_wildcard_tmp.txt

# [TEST1] -> [TEST1.TEST2] -> [TEST1.TEST2.SALES] (empty)
# [JONES] -> [JONES.SALES] -> [JONES.SALES.EAST]
# [TESTX] (empty)
for d in TEST1 TEST1.TEST2 TEST1.TEST2.SALES JONES JONES.SALES JONES.SALES.EAST TESTX; do
    $ODS2 $DISK "CREATE/DIRECTORY [$d]" >/dev/null
done
for f in TEST1.TEST2]NOTE.TXT JONES.SALES]FEDERAL.LIS JONES.SALES.EAST]FEES.DAT JONES]STAFF.DIS; do
    $ODS2 $DISK "COPY samples/cli_wildcard_tmp.txt [$f" >/dev/null
done

check "DIR [*...] lists every directory below root, not root itself, skipping empty ones" \
    "JONES JONES.SALES JONES.SALES.EAST TEST1 TEST1.TEST2" "$(dirs 'DIR [*...]*.*')"
check "DIR [*...] ends with a grand total" \
    "Grand total of 5 directories, 8 files." "$($ODS2 $DISK 'DIR [*...]*.*' | tail -1)"
check "DIR [000000...] includes root itself" \
    "000000 JONES JONES.SALES JONES.SALES.EAST TEST1 TEST1.TEST2" "$(dirs 'DIR [000000...]')"
check "DIR [*] lists top-level directories with something to show" \
    "JONES TEST1" "$(dirs 'DIR [*]')"
check "DIR [TEST%] matches one character" "TEST1" "$(dirs 'DIR [TEST%]')"
check "DIR [*.*] matches exactly two levels down" \
    "JONES.SALES TEST1.TEST2" "$(dirs 'DIR [*.*]')"
check "DIR [...SALES] finds SALES at any depth, only where files match" \
    "JONES.SALES" "$(dirs 'DIR [...SALES]')"
check "DIR [JONES...EAST] - ellipsis in the middle" \
    "JONES.SALES.EAST" "$(dirs 'DIR [JONES...EAST]')"
check "DIR [*...]*.DAT shows only directories holding a match" \
    "JONES.SALES.EAST" "$(dirs 'DIR [*...]*.DAT')"
check "one directory shown: no grand total" \
    "Total of 1 file." "$($ODS2 $DISK 'DIR [*...]*.DAT' | tail -1)"
check "DIR [*...]NOPE.* reports no files found" \
    "%ODS2-W-NOFILES, no files found" "$($ODS2 $DISK 'DIR [*...]NOPE.*' 2>&1)"
check "DIR of an empty directory reports no files found" \
    "%ODS2-W-NOFILES, no files found" "$($ODS2 $DISK 'DIR [TESTX]' 2>&1)"
check "DIR [NOSUCH.*] reports the missing directory" \
    "%ODS2-E-DIRERR, [NOSUCH]: name not found in directory" "$($ODS2 $DISK 'DIR [NOSUCH.*]' 2>&1)"
check "DIR [A..B] is rejected as malformed" \
    "%ODS2-E-BADPATH, could not parse [A..B]" "$($ODS2 $DISK 'DIR [A..B]' 2>&1)"

# Relative forms: "[...]" is the default directory and below.
printf 'SET DEFAULT [JONES]\nDIR [...]\n' > samples/cli_wildcard_tmp.cmd
check "after SET DEFAULT [JONES], DIR [...] stays within [JONES]" \
    "JONES JONES.SALES JONES.SALES.EAST" "$(dirs @samples/cli_wildcard_tmp.cmd)"
printf 'SET DEFAULT [JONES]\nDIR [.*...]\n' > samples/cli_wildcard_tmp.cmd
check "DIR [.*...] is every subdirectory of the default" \
    "JONES.SALES JONES.SALES.EAST" "$(dirs @samples/cli_wildcard_tmp.cmd)"
printf 'SET DEFAULT [JONES.SALES]\nDIR [-.-.*]\n' > samples/cli_wildcard_tmp.cmd
check "DIR [-.-.*] goes up two levels, then every directory there" \
    "JONES TEST1" "$(dirs @samples/cli_wildcard_tmp.cmd)"

# Commands that act on one directory refuse a wildcard one.
for c in "TYPE [*]NOTE.TXT" "DELETE [TEST1...]NOTE.TXT" "SET DEFAULT [*]" \
         "CREATE/DIRECTORY [*.NEW]" "COPY samples/cli_wildcard_tmp.txt [JONES...]X.TXT"; do
    check "$c is refused" "1" "$($ODS2 $DISK "$c" 2>&1 | grep -c WILDDIR)"
done
check "the refused DELETE left NOTE.TXT in place" \
    "TEST1.TEST2" "$(dirs 'DIR [TEST1...]NOTE.TXT')"

rm -f samples/cli_wildcard_tmp.txt samples/cli_wildcard_tmp.cmd "$DISK"
if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails CLI directory wildcard check(s) failed"
    exit 1
fi
