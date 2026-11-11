#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_volume_end_check.sh - nothing is ever allocated at or past the end
# of the volume (SCB$L_VOLSIZE). The storage bitmap's last block has
# bits for clusters past the last one, and a volume whose size is not a
# multiple of its cluster size has a partial last cluster; VMS marks
# both allocated, but ods2v2 used to trust whatever the bits said. On a
# SIMH container the block just past the volume is the simulator's
# metadata footer, so a run there overwrote it and grew the image.
#
# On a copy of the real starter volume (samples/empty_ods2_volume.dsk.gz,
# a SIMH container: 2,940,951 blocks + a 512-byte footer) every real
# cluster is marked in use and only the bits past the end are left free.
# COPY must then fail for lack of space, the image must keep its size
# and its footer, and the storage bitmap must not change. Called by
# `make test`; run from the project root.

GZ=samples/empty_ods2_volume.dsk.gz
DISK=samples/cli_volend_disk.img
ODS2=./ods2
TMP=samples/cli_volend_tmp
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

rm -rf "$TMP"; mkdir -p "$TMP"
gunzip -c "$GZ" > "$DISK"
printf 'hello\n' > "$TMP/hello.txt"

# Fill the volume: every real cluster in use, only the bits past the last cluster free.
python3 -I - "$DISK" <<'EOF'
import struct, sys
f = open(sys.argv[1], 'r+b')
f.seek(512); h = f.read(512)
cluster, = struct.unpack_from('<H', h, 14)
ibmaplbn, = struct.unpack_from('<I', h, 24); ibmapsize, = struct.unpack_from('<H', h, 32)
f.seek((ibmaplbn + ibmapsize + 1) * 512); bh = f.read(512)        # BITMAP.SYS header (file 2)
mp = bh[1] * 2; w0, low = struct.unpack_from('<HH', bh, mp)
assert w0 >> 14 == 1
lbn = ((w0 >> 8) & 0x3f) << 16 | low; blocks = (w0 & 0xff) + 1
f.seek(lbn * 512); volsize, = struct.unpack_from('<I', f.read(512), 4)
clusters = (volsize + cluster - 1) // cluster
nbits = (blocks - 1) * 4096
bits = bytearray((blocks - 1) * 512)
for k in range(clusters, nbits):
    bits[k // 8] |= 1 << (k % 8)
f.seek((lbn + 1) * 512); f.write(bits)
open(sys.argv[1] + '.bitmap', 'wb').write(bits)
print('volume of %d blocks, %d clusters; %d bits past the end left free' % (volsize, clusters, nbits - clusters))
EOF

SIZE_BEFORE=$(wc -c < "$DISK")
tail -c 512 "$DISK" > "$TMP/footer.before"

"$ODS2" "$DISK" "COPY $TMP/hello.txt HELLO.TXT" >/dev/null 2>&1
check "COPY onto a volume with only out-of-volume bits free is refused" 1 $?

check "the image keeps its size (nothing written past the volume)" "$SIZE_BEFORE" "$(wc -c < "$DISK")"
tail -c 512 "$DISK" > "$TMP/footer.after"
if cmp -s "$TMP/footer.before" "$TMP/footer.after"; then r=intact; else r=overwritten; fi
check "the SIMH metadata footer is intact" intact "$r"

r=$(python3 -I - "$DISK" <<'EOF'
import struct, sys
f = open(sys.argv[1], 'rb')
f.seek(512); h = f.read(512)
ibmaplbn, = struct.unpack_from('<I', h, 24); ibmapsize, = struct.unpack_from('<H', h, 32)
f.seek((ibmaplbn + ibmapsize + 1) * 512); bh = f.read(512)
mp = bh[1] * 2; w0, low = struct.unpack_from('<HH', bh, mp)
lbn = ((w0 >> 8) & 0x3f) << 16 | low; blocks = (w0 & 0xff) + 1
f.seek((lbn + 1) * 512)
print('unchanged' if f.read((blocks - 1) * 512) == open(sys.argv[1] + '.bitmap', 'rb').read() else 'changed')
EOF
)
check "the storage bitmap is unchanged" unchanged "$r"

"$ODS2" "$DISK" "CREATE/DIRECTORY [NEWDIR]" >/dev/null 2>&1
check "CREATE/DIRECTORY on the same volume is refused too" 1 $?
check "...and the image still keeps its size" "$SIZE_BEFORE" "$(wc -c < "$DISK")"

rm -rf "$TMP" "$DISK" "$DISK.bitmap"
if [ "$fails" -ne 0 ]; then
    echo "cli_volume_end_check: $fails check(s) FAILED"
    exit 1
fi
echo "cli_volume_end_check: all checks passed"
