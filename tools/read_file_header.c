/*
 * MIT License
 *
 * Copyright (c) 2026 Allen Pomeroy
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include "ods2_ondisk.h"

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "samples/indexf_headers.bin";
    long header_byte_offset = (argc > 2) ? atol(argv[2]) : (90L * 512); /* header 1, right after the 90-block bitmap extract */
    FILE *f = fopen(path, "rb");
    ods2_head_core_t head;

    if (!f) {
        perror("fopen");
        return 1;
    }
    if (fseek(f, header_byte_offset, SEEK_SET) != 0) {
        perror("fseek");
        return 1;
    }
    if (fread(&head, 1, sizeof(head), f) != sizeof(head)) {
        fprintf(stderr, "short read\n");
        return 1;
    }
    fclose(f);

    printf("--- File header at byte offset %ld ---\n", header_byte_offset);
    printf("mpoffset  = %u (word offset to Map Area)\n", head.mpoffset);
    printf("idoffset  = %u (word offset to Ident Area)\n", head.idoffset);
    printf("rsoffset  = %u\n", head.rsoffset);
    printf("acoffset  = %u\n", head.acoffset);
    printf("struclev  = 0x%04x\n", head.struclev);
    printf("fid       = (%u,%u,%u,%u)\n",
           head.fid.fid_num, head.fid.fid_nmx, head.fid.fid_seq, head.fid.fid_rvn);
    printf("ext_fid   = (%u,%u,%u,%u)\n",
           head.ext_fid.fid_num, head.ext_fid.fid_nmx, head.ext_fid.fid_seq, head.ext_fid.fid_rvn);
    printf("\n-- Record Attributes (FAT) --\n");
    printf("rtype     = 0x%02x %s\n", head.recattr.rtype,
           (head.recattr.rtype == 1) ? "(FAB$C_FIX - matches spec's \"512 byte fixed length records\")" : "");
    printf("rattrib   = 0x%02x\n", head.recattr.rattrib);
    printf("rsize     = %u %s\n", head.recattr.rsize,
           (head.recattr.rsize == 512) ? "(matches expected 512-byte records)" : "");
    printf("hiblk     = %u (word-swap corrected)\n", ods2_word_swap32(head.recattr.hiblk));
    printf("efblk     = %u (word-swap corrected)\n", ods2_word_swap32(head.recattr.efblk));
    printf("ffbyte    = %u\n", head.recattr.ffbyte);
    printf("maxrec    = %u\n", head.recattr.maxrec);
    printf("\nfilechar  = 0x%08x\n", head.filechar);
    printf("map_inuse = %u\n", head.map_inuse);
    printf("backlink  = (%u,%u,%u,%u)\n",
           head.backlink.fid_num, head.backlink.fid_nmx,
           head.backlink.fid_seq, head.backlink.fid_rvn);

    return 0;
}
