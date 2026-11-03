#!/bin/sh
# MIT License - Copyright (c) 2026 Allen Pomeroy (see LICENSE)
#
# cli_volume_label_check.sh - SET VOLUME/LABEL and SHOW VOLUME
# (issue #13), on the real VMS-INITIALIZEd starter volume
# (samples/empty_ods2_volume.dsk.gz): its eight home block copies (LBN
# 1-5: the home block and cluster fillers; LBN 1032-1034: the backup
# cluster) are all relabelled, each keeping its own HOMELBN/HOMEVBN,
# with both checksums valid, and no other block changed. Labels that
# aren't allowed, a damaged copy, a volume set member, and a volume
# missing its backup home block are refused with exit status 1 and
# the image left byte for byte as it was. Called by `make test`; run
# from the project root.

GZ=samples/empty_ods2_volume.dsk.gz
DISK=samples/cli_label_disk.img
ORIG=samples/cli_label_orig.img
SYNTH=samples/synthetic_disk.img
ODS2=./ods2
TMP=samples/cli_label_tmp
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
same() { if cmp -s "$1" "$2"; then echo unchanged; else echo changed; fi; }

# Checks every home block copy of <image>: prints "<label> ok" if all
# eight carry that label, name their own LBN/VBN and have valid
# checksums, and no block other than them differs from <original>.
home_check() { # home_check <image> <original>
    python3 -I - "$1" "$2" <<'EOF'
import struct, sys
img, orig = open(sys.argv[1], 'rb'), open(sys.argv[2], 'rb')
copies = {1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 1032: 7, 1033: 8, 1034: 9}  # LBN: VBN
def csum(b, words): return sum(struct.unpack_from('<%dH' % words, b, 0)) & 0xffff
labels = set()
for lbn, vbn in copies.items():
    img.seek(lbn * 512); b = img.read(512)
    if struct.unpack_from('<I', b, 0)[0] != lbn or struct.unpack_from('<H', b, 16)[0] != vbn:
        print('LBN %d names the wrong position' % lbn); sys.exit()
    if struct.unpack_from('<H', b, 58)[0] != csum(b, 29) or \
       struct.unpack_from('<H', b, 510)[0] != csum(b, 255):
        print('LBN %d checksum bad' % lbn); sys.exit()
    labels.add(b[472:484].decode('ascii'))
changed = []
img.seek(0); orig.seek(0); pos = 0
while True:
    x, y = img.read(1 << 20), orig.read(1 << 20)
    if not x and not y: break
    if x != y:
        for i in range(0, max(len(x), len(y)), 512):
            if x[i:i + 512] != y[i:i + 512]: changed.append(pos // 512 + i // 512)
    pos += 1 << 20
extra = [c for c in changed if c not in copies]
if extra: print('other blocks changed: %s' % extra[:10]); sys.exit()
print(' / '.join(sorted(labels)).rstrip() + ' ok')
EOF
}

[ -f "$GZ" ] || { echo "FAIL: $GZ missing"; exit 1; }
mkdir -p "$TMP"
gunzip -c "$GZ" | dd of="$ORIG" bs=1M conv=sparse status=none
cp --sparse=always "$ORIG" "$DISK"

# --- SHOW VOLUME ---
show=$($ODS2 $DISK 'SHOW VOLUME' 2>&1)
check "SHOW VOLUME: label" "  Volume label:       TRANSFER" \
    "$(echo "$show" | grep 'Volume label')"
check "SHOW VOLUME: cluster size, maximum files, total blocks" \
    "  Cluster size:       3|  Maximum files:      367618|  Total blocks:       2940951" \
    "$(echo "$show" | grep -E 'Cluster size|Maximum files|Total blocks' | paste -sd'|' -)"
check "SHOW VOLUME: not a volume set" "  Volume set:         none" \
    "$(echo "$show" | grep 'Volume set')"
check "SHOW VOLUME changes nothing" unchanged "$(same $DISK $ORIG)"

# --- SET VOLUME/LABEL ---
check "SET VOLUME/LABEL=itankits succeeds, uppercased" \
    "%ODS2-I-RELABELED, volume label changed from TRANSFER to ITANKITS (8 home block copies)" \
    "$($ODS2 $DISK 'SET VOLUME/LABEL=itankits' 2>&1)"
check "...all 8 copies relabelled in place, checksums valid, nothing else touched" \
    "ITANKITS ok" "$(home_check $DISK $ORIG)"
check "...and SHOW VOLUME reports it" "  Volume label:       ITANKITS" \
    "$($ODS2 $DISK 'SHOW VOL' 2>&1 | grep 'Volume label')"
check "SET VOL /LABEL=... (abbreviated, qualifier as its own word), 12 characters" \
    "%ODS2-I-RELABELED, volume label changed from ITANKITS to KITS_2026-\$1 (8 home block copies)" \
    "$($ODS2 $DISK 'SET VOL /LABEL=KITS_2026-$1' 2>&1)"
check "...all copies again" 'KITS_2026-$1 ok' "$(home_check $DISK $ORIG)"
printf 'not much\n' > "$TMP/f.txt"
check "the relabelled volume still takes files" "%ODS2-I-COPIED, 9 bytes to F.TXT;1" \
    "$($ODS2 $DISK COPY $TMP/f.txt '[000000]F.TXT' 2>&1)"

# --- refusals: exit 1, nothing written ---
cp --sparse=always "$DISK" "$TMP/before.img"
check "an empty label exits 1" 1 "$(status $ODS2 $DISK 'SET VOLUME/LABEL=')"
check "a 13-character label exits 1" 1 "$(status $ODS2 $DISK 'SET VOLUME/LABEL=ABCDEFGHIJKLM')"
check "a '.' in a label exits 1" 1 "$(status $ODS2 $DISK 'SET VOLUME/LABEL=KITS.1')"
check "...saying which characters are allowed" \
    "%ODS2-E-SETVOL, a volume label may contain only letters, digits, '\$', '_' and '-'" \
    "$($ODS2 $DISK 'SET VOLUME/LABEL=KITS.1' 2>&1)"
check "SET VOLUME without /LABEL exits 1" 1 "$(status $ODS2 $DISK 'SET VOLUME')"
check "an unknown SET VOLUME qualifier exits 1" 1 "$(status $ODS2 $DISK 'SET VOLUME/OWNER=ME')"
check "none of those changed the image" unchanged "$(same $DISK $TMP/before.img)"

# A damaged copy: one byte of the backup cluster's middle block.
cp --sparse=always "$DISK" "$TMP/damaged.img"
printf 'X' | dd of="$TMP/damaged.img" bs=1 seek=$((1033 * 512 + 100)) conv=notrunc status=none
cp --sparse=always "$TMP/damaged.img" "$TMP/damaged_before.img"
check "a damaged home block copy is refused (exit 1)" 1 \
    "$(status $ODS2 $TMP/damaged.img 'SET VOLUME/LABEL=OTHER')"
check "...naming it" \
    "%ODS2-E-SETVOL, the block at LBN 1033 (INDEXF.SYS VBN 8) isn't a valid copy of the home block - nothing changed; check the volume with ANALYZE/DISK on VMS" \
    "$($ODS2 $TMP/damaged.img 'SET VOLUME/LABEL=OTHER' 2>&1)"
check "...and nothing written" unchanged "$(same $TMP/damaged.img $TMP/damaged_before.img)"

# A volume set member: a structure name in every copy, checksums fixed.
cp --sparse=always "$DISK" "$TMP/volset.img"
python3 -I - "$TMP/volset.img" <<'EOF'
import struct, sys
f = open(sys.argv[1], 'r+b')
for lbn in (1, 2, 3, 4, 5, 1032, 1033, 1034):
    f.seek(lbn * 512); b = bytearray(f.read(512))
    b[460:472] = b'KITSET      '
    struct.pack_into('<H', b, 58, sum(struct.unpack_from('<29H', b, 0)) & 0xffff)
    struct.pack_into('<H', b, 510, sum(struct.unpack_from('<255H', b, 0)) & 0xffff)
    f.seek(lbn * 512); f.write(b)
EOF
cp --sparse=always "$TMP/volset.img" "$TMP/volset_before.img"
check "a volume set member is refused (exit 1)" 1 \
    "$(status $ODS2 $TMP/volset.img 'SET VOLUME/LABEL=OTHER')"
check "...and nothing written" unchanged "$(same $TMP/volset.img $TMP/volset_before.img)"
check "SHOW VOLUME shows the set" "  Volume set:         KITSET, volume 0 of 0" \
    "$($ODS2 $TMP/volset.img 'SHOW VOLUME' 2>&1 | grep 'Volume set')"

# The synthetic disk has the home block cluster but not the backup
# cluster's blocks (they were never extracted): refused, not half done.
if [ -f "$SYNTH" ]; then
    cp --sparse=always "$SYNTH" "$TMP/synth.img"
    cp --sparse=always "$SYNTH" "$TMP/synth_before.img"
    check "a volume whose backup home block cluster isn't valid is refused (exit 1)" 1 \
        "$(status $ODS2 $TMP/synth.img 'SET VOLUME/LABEL=OTHER')"
    check "...and nothing written" unchanged "$(same $TMP/synth.img $TMP/synth_before.img)"
    # Its storage control block is a placeholder (all 0xFF): SHOW VOLUME
    # says so rather than print a size its bitmap can't describe.
    check "SHOW VOLUME doesn't believe an SCB its bitmap doesn't cover (exit 1)" 1 \
        "$(status $ODS2 $TMP/synth.img 'SHOW VOLUME')"
fi

rm -rf "$TMP" "$DISK" "$ORIG"
[ "$fails" -eq 0 ] || { echo "$fails CLI volume label check(s) failed"; exit 1; }
