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

    # INDEXF.SYS's own header (file 1) has a fixed, real map area with
    # only 4 extents, covering file numbers up to ~18 - a genuine limit
    # of the real disk this sample was extracted from (extending
    # INDEXF.SYS itself, the bootstrap file, is a separate, more
    # delicate feature not yet implemented). To let tests exercise
    # DIRECTORY extension (allocating many files in one directory)
    # without immediately hitting that separate, unrelated limit, I
    # patch a 5th, synthetic extent onto INDEXF.SYS's own header here -
    # test-fixture-only, giving room for ~120 more file numbers at a
    # safe, otherwise-unused LBN range (cluster 2000+, well clear of
    # every other region this fixture uses).
    indexf_header_offset = (1470720 + 90) * 512  # header(1)'s absolute position in the target file
    with open(TARGET, 'r+b') as tf:
        tf.seek(indexf_header_offset)
        header = bytearray(tf.read(512))

        mpoffset_words = header[1]
        map_inuse_words = header[58]
        map_area_start = mpoffset_words * 2
        new_extent_offset = map_area_start + map_inuse_words * 2

        # Format 1 retrieval pointer: word0 = [format=01][high_lbn:6][count:8],
        # word1 = low_lbn. LBN 6000, 120 blocks (count field = 119).
        synthetic_lbn = 6000
        synthetic_blocks = 120
        word0 = (1 << 14) | ((synthetic_lbn >> 16) << 8) | (synthetic_blocks - 1)
        word1 = synthetic_lbn & 0xffff
        header[new_extent_offset:new_extent_offset + 4] = bytes([
            word0 & 0xff, (word0 >> 8) & 0xff, word1 & 0xff, (word1 >> 8) & 0xff
        ])
        header[58] = map_inuse_words + 2  # +1 extent = +2 words

        # Recompute the additive checksum (last word, covers the 255
        # words before it) - same algorithm used throughout this
        # project, confirmed against real VMS-computed checksums.
        checksum = 0
        for i in range(0, 510, 2):
            checksum = (checksum + header[i] + (header[i + 1] << 8)) & 0xffff
        header[510] = checksum & 0xff
        header[511] = (checksum >> 8) & 0xff

        tf.seek(indexf_header_offset)
        tf.write(bytes(header))

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
