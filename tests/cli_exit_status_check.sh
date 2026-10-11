#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_exit_status_check.sh - the ods2 exit status tells success from
# failure, single-shot and @script: a command that reports a %ODS2-E-
# error exits 1 and a failed COPY leaves no destination entry; success
# and %ODS2-W- warnings exit 0. (A failed COPY - a missing source, a
# size error - used to exit 0, so automation saw success with no file.)
# Called by `make test`; run from the project root.

SRC_DISK=samples/synthetic_disk.img
DISK=samples/cli_status_disk.img
ODS2=./ods2
TMP=samples/cli_status_tmp
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

# status <command...> - runs it, output discarded, prints the exit status
status() { "$@" >/dev/null 2>&1; echo $?; }

[ -f "$SRC_DISK" ] || { echo "FAIL: $SRC_DISK missing"; exit 1; }
rm -f "$DISK"
cp "$SRC_DISK" "$DISK"
mkdir -p "$TMP"
printf 'WRITE SYS$OUTPUT "OK"\n' > "$TMP/ok.com"

check "COPY of a missing source exits 1" 1 \
    "$(status $ODS2 $DISK COPY /nonexistent/FOO.COM '[000000]FOO.COM')"
check "...and leaves no destination entry" "%ODS2-W-NOFILES, no files found" \
    "$($ODS2 $DISK 'DIR [000000]FOO.COM' 2>&1)"
check "COPY into a missing directory exits 1" 1 \
    "$(status $ODS2 $DISK COPY $TMP/ok.com '[NOSUCH]OK.COM')"
check "COPY to a bad version exits 1" 1 \
    "$(status $ODS2 $DISK COPY $TMP/ok.com '[000000]OK.COM;5')"
check "COPY with a missing argument exits 1" 1 "$(status $ODS2 $DISK COPY $TMP/ok.com)"
check "successful COPY exits 0" 0 "$(status $ODS2 $DISK COPY $TMP/ok.com '[000000]OK.COM')"
check "COPY over an existing name exits 1" 1 \
    "$(status $ODS2 $DISK COPY $TMP/ok.com '[000000]OK.COM')"

check "TYPE of an existing file exits 0" 0 "$(status $ODS2 $DISK 'TYPE [000000]OK.COM')"
check "TYPE of a missing file exits 1" 1 "$(status $ODS2 $DISK 'TYPE [000000]NOPE.TXT')"
if [ -w /dev/full ]; then
    check "TYPE that cannot write its output exits 1" 1 \
        "$($ODS2 $DISK 'TYPE [000000]OK.COM' >/dev/full 2>/dev/null; echo $?)"
fi
check "DIR finding nothing is a warning: exits 0" 0 "$(status $ODS2 $DISK 'DIR [000000]NOPE.*')"
check "DELETE of a missing file exits 1" 1 "$(status $ODS2 $DISK 'DELETE [000000]NOPE.TXT')"
check "CREATE/DIRECTORY of an existing name exits 1" 1 "$(status $ODS2 $DISK 'CREATE/DIRECTORY [DECUS]')"
check "SET DEFAULT to a missing directory exits 1" 1 "$(status $ODS2 $DISK 'SET DEFAULT [NOSUCH]')"
check "an unknown command exits 1" 1 "$(status $ODS2 $DISK BOGUS)"
check "a disk that cannot be mounted exits 1" 1 "$(status $ODS2 /nonexistent.dsk DIR)"

printf '! all good\nDIR [000000]OK.COM\nTYPE [000000]OK.COM\n' > "$TMP/good.com"
printf 'COPY /nonexistent/X.COM [000000]X.COM\nCOPY %s [000000]AFTER.COM\n' "$TMP/ok.com" > "$TMP/bad.com"
check "a script whose commands all succeed exits 0" 0 "$(status $ODS2 $DISK @$TMP/good.com)"
check "a script with a failing command exits 1" 1 "$(status $ODS2 $DISK @$TMP/bad.com)"
check "...after still running the rest of the script" "AFTER.COM           ;1" \
    "$($ODS2 $DISK 'DIR [000000]AFTER.COM' 2>&1 | grep AFTER)"
check "a missing script exits 1" 1 "$(status $ODS2 $DISK @/nonexistent.com)"

rm -rf "$TMP" "$DISK"
[ "$fails" -eq 0 ] || { echo "$fails CLI exit status check(s) failed"; exit 1; }
