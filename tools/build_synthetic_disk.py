#!/usr/bin/env python3

# MIT License
#
# Copyright (c) 2026 Allen Pomeroy
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Assembles samples/synthetic_disk.img: a sparse file at the real RA92
size (2,940,951 blocks), with the real sample data written at the real
byte offsets it was extracted from. Lets the volume-level integration
test exercise real file I/O (seeking to real offsets, reading real
extents) without needing to ship or download a full 1.5GB disk image.
"""
import os

SAMPLES_DIR = os.path.join(os.path.dirname(__file__), '..', 'samples')
TARGET = os.path.join(SAMPLES_DIR, 'synthetic_disk.img')
TOTAL_BLOCKS = 2940951
TOTAL_BYTES = TOTAL_BLOCKS * 512

def write_at(offset_blocks, src_name):
    src_path = os.path.join(SAMPLES_DIR, src_name)
    with open(src_path, 'rb') as sf:
        data = sf.read()
    with open(TARGET, 'r+b') as tf:
        tf.seek(offset_blocks * 512)
        tf.write(data)


def main():
    with open(TARGET, 'wb') as f:
        f.truncate(TOTAL_BYTES)

    write_at(0, 'sample.bin')                  # home block area
    write_at(1470720, 'indexf_headers.bin')    # bitmap + first 16 headers
    write_at(1470474, 'root_dir.bin')          # root directory content

    # INDEXF.SYS's own header (file 1) is left exactly as VMS wrote it:
    # 4 extents, HIBLK 120, room for file numbers up to 18. Tests that
    # create more files than that exercise ods2v2's real INDEXF.SYS
    # extension. (An earlier version of this script patched a fifth,
    # synthetic extent on here instead, without raising HIBLK - a
    # stopgap from before extension existed, and a header that
    # disagreed with itself.)

    # BITMAP.SYS's own content lives at LBN 1470477, 243 blocks (decoded
    # from its real header's own extents, in indexf_headers.bin). We
    # never extracted its actual real content, so this is a SYNTHETIC
    # test fixture, not verified real data.
    #
    # Per spec 5.2.1: VBN 1 of BITMAP.SYS is the Storage Control Block,
    # NOT bitmap data - the real bitmap bits start at VBN 2. The first
    # 512 bytes here are left as 0xff (harmless placeholder content,
    # never read as bitmap bits by ods2_allocate_blocks(), which
    # correctly skips VBN 1 entirely). Clusters 0-999 of the REAL
    # bitmap data (i.e. starting at byte 512, VBN 2) are marked
    # allocated as a stand-in for the real reserved regions identified
    # throughout this project (boot/home/filler blocks, backup home
    # block, index file bitmap+headers, root directory, alternate
    # index header), everything from cluster 1000 on is free.
    bitmap_lbn = 1470477
    bitmap_blocks = 243
    bitmap_bytes = bytearray(b'\xff' * (bitmap_blocks * 512))
    for cluster in range(0, 1000):
        byte_i, bit_i = divmod(cluster, 8)
        bitmap_bytes[512 + byte_i] &= ~(1 << bit_i) & 0xff  # +512 skips the SCB block
    with open(TARGET, 'r+b') as tf:
        tf.seek(bitmap_lbn * 512)
        tf.write(bytes(bitmap_bytes))

    print(f'Assembled {TARGET} ({TOTAL_BYTES} bytes apparent, sparse)')


if __name__ == '__main__':
    main()
