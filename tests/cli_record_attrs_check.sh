#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_record_attrs_check.sh - RMS record attributes of host imports:
# `ods2 COPY/ATTRIBUTES=(...)` and `ods2_put ... rtype rsize rattrib`
# set the record format, record size and record attributes in the new
# file's header (read back with ods2_dump_header), e.g. the fixed
# 8192-byte records OpenVMS PCSI kit files need; plain COPY is still
# Stream_LF; attributes that don't fit the content are refused with
# exit status 1 and no file. Called by `make test`; run from the
# project root.

SRC_DISK=samples/synthetic_disk.img
DISK=samples/cli_attrs_disk.img
ODS2=./ods2
PUT=./ods2_put
DUMP=./ods2_dump_header
TMP=samples/cli_attrs_tmp
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

status() { "$@" >/dev/null 2>&1; echo $?; }

[ -f "$SRC_DISK" ] || { echo "FAIL: $SRC_DISK missing"; exit 1; }
for t in $PUT $DUMP; do [ -x $t ] || { echo "FAIL: $t not built"; exit 1; }; done
rm -f "$DISK"
cp "$SRC_DISK" "$DISK"
mkdir -p "$TMP"

head -c 16384 /dev/zero | tr '\0' 'K' > "$TMP/kit.pcsi"     # 2 records of 8192
head -c 16000 /dev/zero | tr '\0' 'S' > "$TMP/short.pcsi"   # not whole records
printf 'line one\nline two\n' > "$TMP/notes.txt"

# File numbers are handed out in order on this fresh disk, so each
# successful create below takes the next one; read them from the COPY
# itself where possible: ods2_put prints FID=(num,...).
next_fid() { $PUT "$@" | sed -n 's/.*FID=(\([0-9]*\),.*/\1/p'; }
hdr() { $DUMP $DISK "$1" | tr -s ' ' '\n' | grep -E '^(rtype|rattrib|rsize|maxrec)=' | tr '\n' ' ' | sed 's/ $//'; }

# --- ods2_put ---
n=$(next_fid $DISK $TMP/kit.pcsi '' PUT8192.PCSI 1 8192)
check "ods2_put 1 8192: fixed 8192-byte records" \
    "rtype=0x01 rattrib=0x00 rsize=8192 maxrec=8192" "$(hdr $n)"
n=$(next_fid $DISK $TMP/kit.pcsi '' PUT512.DAT 1)
check "ods2_put 1: fixed 512-byte records by default" \
    "rtype=0x01 rattrib=0x00 rsize=512 maxrec=512" "$(hdr $n)"
n=$(next_fid $DISK $TMP/notes.txt '' PUTCR.TXT 5 512 2)
check "ods2_put 5 512 2: Stream_LF with CR carriage control" \
    "rtype=0x05 rattrib=0x02 rsize=512 maxrec=512" "$(hdr $n)"
check "ods2_put refuses content that isn't whole records (exit 1)" 1 \
    "$(status $PUT $DISK $TMP/short.pcsi '' SHORT.PCSI 1 8192)"
check "ods2_put refuses VAR (exit 1)" 1 "$(status $PUT $DISK $TMP/notes.txt '' V.TXT 2)"
check "ods2_put refuses a non-numeric size (exit 1)" 1 \
    "$(status $PUT $DISK $TMP/kit.pcsi '' X.PCSI 1 8k)"

# --- ods2 COPY ---
# Each successful COPY takes the next file number after the last one.
check "COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) succeeds" \
    "%ODS2-I-COPIED, 16384 bytes to KIT.PCSI;1" \
    "$($ODS2 $DISK "COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) $TMP/kit.pcsi [000000]KIT.PCSI" 2>&1)"
n=$((n + 1))
check "...with fixed 8192-byte records in the header" \
    "rtype=0x01 rattrib=0x00 rsize=8192 maxrec=8192" "$(hdr $n)"
$ODS2 $DISK "TYPE [000000]KIT.PCSI" > "$TMP/out.bin" 2>/dev/null
if cmp -s "$TMP/kit.pcsi" "$TMP/out.bin"; then same=identical; else same=different; fi
check "...and reads back byte for byte" identical "$same"

$ODS2 $DISK "COPY/ATTR=(RFM:FIXED,LRL:8192) $TMP/kit.pcsi [000000]LRL.PCSI" >/dev/null 2>&1
n=$((n + 1))
check "LRL alone sets a fixed record size too; /ATTR abbreviation" \
    "rtype=0x01 rattrib=0x00 rsize=8192 maxrec=8192" "$(hdr $n)"
$ODS2 $DISK "copy/attributes=rfm:fix $TMP/kit.pcsi [000000]F512.DAT" >/dev/null 2>&1
n=$((n + 1))
check "RFM:FIX with no size: 512-byte records; lowercase, no parentheses" \
    "rtype=0x01 rattrib=0x00 rsize=512 maxrec=512" "$(hdr $n)"
$ODS2 $DISK "COPY/ATTRIBUTES=(RFM:STMLF,RAT:CR) $TMP/notes.txt [000000]NOTES.TXT" >/dev/null 2>&1
n=$((n + 1))
check "RFM:STMLF,RAT:CR" "rtype=0x05 rattrib=0x02 rsize=512 maxrec=512" "$(hdr $n)"
$ODS2 $DISK "COPY/ATTRIBUTES=(RFM:UDF,RAT:NONE,MRS:0,LRL:0) $TMP/notes.txt [000000]RAW.DAT" >/dev/null 2>&1
n=$((n + 1))
check "RFM:UDF with sizes 0" "rtype=0x00 rattrib=0x00 rsize=0 maxrec=0" "$(hdr $n)"
$ODS2 $DISK "COPY $TMP/notes.txt [000000]PLAIN.TXT" >/dev/null 2>&1
n=$((n + 1))
check "plain COPY is still Stream_LF, 512, no RAT" \
    "rtype=0x05 rattrib=0x00 rsize=512 maxrec=512" "$(hdr $n)"

check "COPY of content that isn't whole records exits 1" 1 \
    "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) $TMP/short.pcsi [000000]SHORT.PCSI")"
check "...and creates no file" "%ODS2-W-NOFILES, no files found" \
    "$($ODS2 $DISK 'DIR [000000]SHORT.PCSI' 2>&1)"
check "MRS different from LRL for FIX exits 1" 1 \
    "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:FIX,MRS:8192,LRL:512) $TMP/kit.pcsi [000000]REFUSED1.PCSI")"
check "an odd fixed size exits 1" 1 \
    "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:FIX,MRS:8191) $TMP/kit.pcsi [000000]REFUSED2.PCSI")"
check "RFM:VAR exits 1" 1 \
    "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:VAR) $TMP/notes.txt [000000]REFUSED3.TXT")"
check "an unknown record format exits 1" 1 \
    "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:XYZ) $TMP/notes.txt [000000]REFUSED4.TXT")"
check "an unknown qualifier exits 1" 1 \
    "$(status $ODS2 $DISK "COPY/BOGUS $TMP/notes.txt [000000]REFUSED5.TXT")"
check "two carriage controls exit 1" 1 \
    "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:STMLF,RAT:CR,RAT:FTN) $TMP/notes.txt [000000]REFUSED6.TXT")"
check "a missing ')' exits 1" 1 \
    "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:FIX $TMP/kit.pcsi [000000]REFUSED7.PCSI")"
check "none of the refused copies created a file" "%ODS2-W-NOFILES, no files found" \
    "$($ODS2 $DISK 'DIR [000000]REFUSED*.*' 2>&1)"

rm -rf "$TMP" "$DISK"
[ "$fails" -eq 0 ] || { echo "$fails CLI record attribute check(s) failed"; exit 1; }
