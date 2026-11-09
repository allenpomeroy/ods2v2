#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_lock_check.sh - one writer or any number of readers: two ods2v2 processes writing one image, or one writing an image
# another reads, would corrupt it or read a half-done change. The image is flock()ed (LOCK_EX to write, LOCK_SH to read);
# a process that finds it locked mounts it read-only for the commands that only read, and refuses the ones that write.
# (flock is advisory: it stops another ods2v2, not a simulator that has the image open; ODS2_NOLOCK=1 turns it off.)
# Called by `make test`; run from the project root.

SYNTH=samples/synthetic_disk.img
DISK=samples/cli_lock_disk.img
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

if ! command -v flock >/dev/null 2>&1; then
    echo "SKIP: no flock(1) here"
    exit 0
fi
cp --sparse=always "$SYNTH" "$DISK" 2>/dev/null || cp "$SYNTH" "$DISK"
echo "text" > "$DISK.txt"

# another process holds the image for write (an exclusive lock) for a few seconds: nothing else may read or write it
flock -x "$DISK" sleep 4 &
sleep 1
out=$($ODS2 "$DISK" "DIR [000000]" 2>&1); rc=$?
check "a writer holds the image: DIR is refused (exit 1): it is being written, its blocks are not consistent" "1 1" \
    "$(echo "$out" | grep -c 'MOUNTERR.*locked') $rc"
out=$(ODS2_NOLOCK=1 $ODS2 "$DISK" "DIR [000000]" 2>&1); rc=$?
check "ODS2_NOLOCK=1 ignores the lock" "1 0" "$(echo "$out" | grep -c 'DECUS.DIR') $rc"
wait
out=$($ODS2 "$DISK" "COPY $DISK.txt [000000]NEW.TXT" 2>&1); rc=$?
check "the lock is gone: COPY works" "1 0" "$(echo "$out" | grep -c COPIED) $rc"

# another process holds it for read (a shared lock): readers go on, a writer is refused
flock -s "$DISK" sleep 4 &
sleep 1
out=$($ODS2 "$DISK" "DIR [000000]" 2>&1); rc=$?
check "a reader holds the image: DIR works, read-only, with a warning (exit 0)" "1 1 0" \
    "$(echo "$out" | grep -c 'READONLY.*locked by another process') $(echo "$out" | grep -c 'DECUS.DIR') $rc"
out=$($ODS2 "$DISK" "COPY $DISK.txt [000000]NEW2.TXT" 2>&1); rc=$?
check "a reader holds the image: COPY is refused (exit 1), nothing is written" "1 1" "$(echo "$out" | grep -c 'not mounted for write') $rc"
wait

rm -f "$DISK" "$DISK.txt"
if [ $fails -ne 0 ]; then
    echo "cli_lock_check: $fails FAILED"
    exit 1
fi
echo "cli_lock_check: all checks passed"
