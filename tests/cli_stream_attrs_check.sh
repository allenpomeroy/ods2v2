#!/bin/sh
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
#
# cli_stream_attrs_check.sh - a stream (Stream_LF, Stream, Stream_CR) or undefined-format file has MRS 0 and its own longest record
# as LRL, whatever the lines: COPY (plain and /ATTRIBUTES), ods2_put and ods2_create_file() used to give RSIZE = MRS = 512, and VMS
# could not read such a file in record mode when a line was longer (%RMS-W-RTB, "record too large for the user's buffer", found on
# VAX V7.3 and V8.4). The record ends are RMS's (measured on V8.4, V7.3-2 and VAX V7.3): Stream_LF at LF, Stream_CR at CR, Stream at
# LF, FF or VT (the byte stays in the record) or CR LF (stripped). A record longer than a given MRS or LRL is refused. SET
# FILE/ATTRIBUTES=(MRS:0) repairs files made the old way. Called by `make test`; run from the project root.

SRC_DISK=samples/synthetic_disk.img
DISK=samples/cli_stream_disk.img
ODS2=./ods2
PUT=./ods2_put
DUMP=./ods2_dump_header
TMP=samples/cli_stream_tmp
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
rm -rf "$TMP" "$DISK"
cp "$SRC_DISK" "$DISK"
mkdir -p "$TMP"
python3 -c "
open('$TMP/long.txt','wb').write(b'a' * 1500 + b'\n' + b'b' * 20 + b'\n')
open('$TMP/crlong.txt','wb').write(b'x' * 700 + b'\r' + b'y\r')
open('$TMP/crlf.txt','wb').write(b'q' * 900 + b'\r\n' + b'r\r\n')
open('$TMP/lf600.txt','wb').write(b'a' * 600 + b'\n' + b'b')
open('$TMP/mixed.txt','wb').write(b'x' * 10 + b'\r\n' + b'y' * 700 + b'\n' + b'z')
open('$TMP/ff.txt','wb').write(b'p' * 300 + b'\f' + b'q' * 5 + b'\v' + b'r' * 299 + b'\r' + b's')
open('$TMP/big.bin','wb').write(b'B' * 40000)
open('$TMP/mid.bin','wb').write(b'C' * 20000)
open('$TMP/short.txt','wb').write(b'short\nlines\n')
"

next_fid() { $PUT "$@" | sed -n 's/.*FID=(\([0-9]*\),.*/\1/p'; }
hdr() { $DUMP $DISK "$1" | tr -s ' ' '\n' | grep -E '^(rtype|rattrib|rsize|maxrec)=' | tr '\n' ' ' | sed 's/ $//'; }
# file numbers are handed out in order on this fresh disk: the first from ods2_put's output, each successful create the next
n=$(next_fid $DISK $TMP/long.txt '' PUT1.TXT 5)
check "ods2_put rtype 5 (no size): MRS 0, LRL 1500" "rtype=0x05 rattrib=0x00 rsize=1500 maxrec=0" "$(hdr $n)"
copy() { # copy <qualifiers> <host file> <name>: the next file number is used when it succeeds
    if $ODS2 $DISK "COPY$1 $2 [000000]$3" >/dev/null 2>&1; then n=$((n + 1)); else :; fi
}

copy "/ATTRIBUTES=(RFM:STMLF,RAT:CR)" $TMP/long.txt A.TXT
check "COPY/ATTRIBUTES=(RFM:STMLF,RAT:CR) of a 1500-byte line: MRS 0, LRL 1500" "rtype=0x05 rattrib=0x02 rsize=1500 maxrec=0" "$(hdr $n)"
copy "/ATTRIBUTES=(RFM:STMCR,RAT:CR)" $TMP/crlong.txt B.TXT
check "RFM:STMCR: lines end at CR, LRL 700" "rtype=0x06 rattrib=0x02 rsize=700 maxrec=0" "$(hdr $n)"
copy "/ATTRIBUTES=(RFM:STM,RAT:CR)" $TMP/crlf.txt C.TXT
check "RFM:STM: lines end at CR LF (the CR not counted), LRL 900" "rtype=0x04 rattrib=0x02 rsize=900 maxrec=0" "$(hdr $n)"
copy "/ATTRIBUTES=(RFM:UDF)" $TMP/long.txt D.DAT
check "RFM:UDF: LRL the length (1522), MRS 0" "rtype=0x00 rattrib=0x00 rsize=1522 maxrec=0" "$(hdr $n)"
copy "/ATTRIBUTES=(RFM:STMLF,MRS:4000)" $TMP/long.txt E.TXT
check "an MRS that is given is kept: 4000, LRL 1500" "rtype=0x05 rattrib=0x00 rsize=1500 maxrec=4000" "$(hdr $n)"
before=$n
check "MRS:512 with a 1500-byte line is refused (exit 1)" 1 "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:STMLF,MRS:512) $TMP/long.txt [000000]F.TXT")"
check "LRL:100 with a 1500-byte line is refused (exit 1)" 1 "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:STMLF,LRL:100) $TMP/long.txt [000000]G.TXT")"
check "... nothing was created" "%ODS2-W-NOFILES, no files found" "$($ODS2 $DISK 'DIR [000000]F.TXT' 2>&1 | grep NOFILES | sed 's/^ *//')"
copy "" $TMP/long.txt H.TXT
check "plain COPY: MRS 0, LRL 1500" "rtype=0x05 rattrib=0x00 rsize=1500 maxrec=0" "$(hdr $n)"
copy "" $TMP/big.bin BIG.BIN
check "plain COPY of a 40,000-byte file with no line end: MRS 0, LRL 32767 (VMS's own state for such a binary file)" "rtype=0x05 rattrib=0x00 rsize=32767 maxrec=0" "$(hdr $n)"

# Stream (STM) record ends as RMS applies them: LF, FF and VT end a record and stay in it, CR LF ends one and is stripped, a lone CR is
# data. COPY wrote LRL 600 for a 600-byte line ended by a lone LF, which is a 601-byte record (COPY and CHECKSUM then failed on VMS).
copy "/ATTRIBUTES=(RFM:STM,RAT:CR)" $TMP/lf600.txt LF600.TXT
check "RFM:STM, a 600-byte line ended by a lone LF is a 601-byte record: LRL 601" "rtype=0x04 rattrib=0x02 rsize=601 maxrec=0" "$(hdr $n)"
lf600=$n
copy "/ATTRIBUTES=(RFM:STM)" $TMP/mixed.txt MIXED.TXT
check "RFM:STM of CR LF and lone LF ends: 10, 701 (LF stays), 1: LRL 701" "rtype=0x04 rattrib=0x00 rsize=701 maxrec=0" "$(hdr $n)"
copy "/ATTRIBUTES=(RFM:STM)" $TMP/ff.txt FF.TXT
check "RFM:STM, FF and VT end a record and stay in it, a lone CR is data: 301, 6, 301: LRL 301" "rtype=0x04 rattrib=0x00 rsize=301 maxrec=0" "$(hdr $n)"
copy "/ATTRIBUTES=(RFM:STMLF)" $TMP/lf600.txt STMLF.TXT
check "Stream_LF: the same file is 600 (the LF is not in the record)" "rtype=0x05 rattrib=0x00 rsize=600 maxrec=0" "$(hdr $n)"
check "an LRL of 600 for the 601-byte Stream record is refused (exit 1)" 1 "$(status $ODS2 $DISK "COPY/ATTRIBUTES=(RFM:STM,LRL:600) $TMP/lf600.txt [000000]TOOSHORT.TXT")"
copy "/CONVERT" $TMP/mixed.txt VARFILE.TXT
check "COPY/CONVERT: LRL the longest line (one record per LF, a CR before it dropped: 10, 700, 1)" "rtype=0x02 rattrib=0x02 rsize=700 maxrec=0" "$(hdr $n)"

# SET FILE/ATTRIBUTES=(MRS:0): repairs files made the old way (MRS = LRL = 512) and files with an LRL below their longest record
cat > "$TMP/damage.py" <<'PY'
import struct, sys
img, n = sys.argv[1], int(sys.argv[2])
lrl = int(sys.argv[3]); mrs = int(sys.argv[4]); rtype = int(sys.argv[5]) if len(sys.argv) > 5 else None
f = open(img, 'r+b'); f.seek(512); home = f.read(512)
ibm = struct.unpack_from('<I', home, 24)[0]; size = struct.unpack_from('<H', home, 32)[0]; cl = struct.unpack_from('<H', home, 14)[0]
f.seek((ibm + size) * 512); h1 = f.read(512)
p = h1[1] * 2; q = 0; ex = []
while q < h1[58]:
    w0 = struct.unpack_from('<H', h1, p)[0]; fmt = (w0 >> 14) & 3
    if fmt == 1: cnt = (w0 & 0xff) + 1; lbn = (((w0 >> 8) & 0x3f) << 16) | struct.unpack_from('<H', h1, p + 2)[0]; p += 4; q += 2
    elif fmt == 2: cnt = (w0 & 0x3fff) + 1; lbn = struct.unpack_from('<I', h1, p + 2)[0]; p += 6; q += 3
    else: cnt = (((w0 & 0x3fff) << 16) | struct.unpack_from('<H', h1, p + 2)[0]) + 1; lbn = struct.unpack_from('<I', h1, p + 4)[0]; p += 8; q += 4
    ex.append((lbn, cnt))
vbn = cl * 4 + size + n; cur = 1
for l, c in ex:
    if cur <= vbn < cur + c: at = l + vbn - cur
    cur += c
f.seek(at * 512); h = bytearray(f.read(512))
if rtype is not None: h[20] = rtype
struct.pack_into('<H', h, 22, lrl); struct.pack_into('<H', h, 36, mrs)      # RSIZE (LRL), MAXREC (MRS)
struct.pack_into('<H', h, 510, sum(struct.unpack_from('<255H', h, 0)) & 0xffff)
f.seek(at * 512); f.write(h)
PY
repair() { $ODS2 $DISK "SET FILE/ATTRIBUTES=(MRS:0) [000000]$1" 2>&1; }
copy "" $TMP/long.txt OLD1.TXT; old1=$n
python3 "$TMP/damage.py" $DISK $old1 512 512
check "a file made the old way: Stream_LF, LRL 512, MRS 512" "rtype=0x05 rattrib=0x00 rsize=512 maxrec=512" "$(hdr $old1)"
out=$(repair OLD1.TXT); rc=$?
case "$out" in *"RECSIZES, 1 file looked at, 1 stream file changed"*) msg=repaired ;; *) msg="$out" ;; esac
check "SET FILE/ATTRIBUTES=(MRS:0) [000000]OLD1.TXT repairs it: MRS 0, LRL 1500" "repaired rtype=0x05 rattrib=0x00 rsize=1500 maxrec=0" "$msg $(hdr $old1)"
out=$(repair OLD1.TXT)
case "$out" in *", 0 stream files changed"*) msg=nothing-more ;; *) msg="$out" ;; esac
check "run again: nothing to change" "nothing-more" "$msg"
python3 "$TMP/damage.py" $DISK $lf600 600 0
check "an STM file with LRL 600 and a 601-byte record (the old COPY's output)" "rtype=0x04 rattrib=0x02 rsize=600 maxrec=0" "$(hdr $lf600)"
repair LF600.TXT >/dev/null
check "... the repair makes LRL 601" "rtype=0x04 rattrib=0x02 rsize=601 maxrec=0" "$(hdr $lf600)"
copy "" $TMP/short.txt SHORT.TXT; sh1=$n
python3 "$TMP/damage.py" $DISK $sh1 4000 0
out=$(repair SHORT.TXT)
case "$out" in *", 0 stream files changed"*) msg=unchanged ;; *) msg="$out" ;; esac
check "MRS 0 and an LRL (4000) that is enough: left alone" "unchanged rtype=0x05 rattrib=0x00 rsize=4000 maxrec=0" "$msg $(hdr $sh1)"
python3 "$TMP/damage.py" $DISK $sh1 0 0
repair SHORT.TXT >/dev/null
check "LRL 0 is repaired too: the longest record (5)" "rtype=0x05 rattrib=0x00 rsize=5 maxrec=0" "$(hdr $sh1)"
# a binary Stream_LF file with a 40,000-byte "record": VMS's own state is MRS 0 and LRL 32767: skipped silently; MRS or a short LRL: made so
copy "/ATTRIBUTES=(RFM:UDF)" $TMP/big.bin BIGUDF.DAT; bg=$n
python3 "$TMP/damage.py" $DISK $bg 32767 0 5
out=$(repair BIGUDF.DAT); rc=$?
case "$out" in *"0 stream files changed"*) msg=skipped ;; *) msg="$out" ;; esac
check "Stream_LF, MRS 0, LRL 32767, a 40,000-byte record (VMS's state): the repair skips it silently, exit 0" "skipped 0 0" "$msg $(echo "$out" | grep -c 'E-RECSIZES') $rc"
python3 "$TMP/damage.py" $DISK $bg 512 512 5
repair BIGUDF.DAT >/dev/null
check "the same file with MRS 512 gets MRS 0, LRL 32767" "rtype=0x05 rattrib=0x00 rsize=32767 maxrec=0" "$(hdr $bg)"
copy "/ATTRIBUTES=(RFM:UDF)" $TMP/mid.bin MID.DAT; md=$n
python3 "$TMP/damage.py" $DISK $md 16384 0 5
repair MID.DAT >/dev/null
check "MRS 0 and LRL 16384 with a 20,000-byte record: LRL 20000" "rtype=0x05 rattrib=0x00 rsize=20000 maxrec=0" "$(hdr $md)"
# the repair leaves a fixed file and a directory alone, and other attribute changes are refused
copy "/ATTRIBUTES=(RFM:FIX)" $TMP/short.txt FIXED.DAT 2>/dev/null; copy "/ATTRIBUTES=(RFM:FIX,MRS:2)" $TMP/short.txt FIXED2.DAT
out=$(repair '*.*'); case "$out" in *"RECSIZES"*) msg=ran ;; *) msg="$out" ;; esac
check "SET FILE/ATTRIBUTES=(MRS:0) [000000]*.*: runs over everything without an error" "ran 0" "$msg $(echo "$out" | grep -c 'E-RECSIZES')"
check "any other attribute change is refused (exit 1)" 1 "$(status $ODS2 $DISK 'SET FILE/ATTRIBUTES=(MRS:100) [000000]*.TXT')"

# ods2_put rtype 5 with a size that is too small: refused
check "ods2_put rtype 5 with size 100 for a 1500-byte line: refused, exit 1" 1 "$(status $PUT $DISK $TMP/long.txt '' PUT3.TXT 5 100)"

rm -rf "$TMP" "$DISK"
if [ $fails -ne 0 ]; then
    echo "cli_stream_attrs_check: $fails FAILED"
    exit 1
fi
echo "cli_stream_attrs_check: all checks passed"
