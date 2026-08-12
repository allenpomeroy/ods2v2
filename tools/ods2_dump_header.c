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

/* ods2_dump_header.c - dumps a specific file number's header bytes,
 * hex-formatted, directly from a real disk image.
 *
 * Usage: ods2_dump_header <disk-image> <file-number>
 */
#include <stdio.h>
#include <stdlib.h>
#include "ods2_volume.h"

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    uint8_t header[512];
    unsigned file_number;
    unsigned off;
    ods2_head_core_t *core;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <disk-image> <file-number>\n", argv[0]);
        return 1;
    }
    file_number = (unsigned) atoi(argv[2]);

    r = ods2_mount(argv[1], &vol);
    if (!r.ok) {
        fprintf(stderr, "mount failed: %s\n", r.problem);
        return 1;
    }

    r = ods2_read_header(&vol, file_number, header);
    if (!r.ok) {
        fprintf(stderr, "could not read header for file %u: %s\n", file_number, r.problem);
        return 1;
    }

    core = (ods2_head_core_t *) header;
    printf("File %u header:\n", file_number);
    printf("  idoffset=%u mpoffset=%u acoffset=%u rsoffset=%u\n",
           core->idoffset, core->mpoffset, core->acoffset, core->rsoffset);
    printf("  seg_num=%u struclev=0x%04x\n", core->seg_num, core->struclev);
    printf("  fid=(%u,%u,%u,%u)\n", core->fid.fid_num, core->fid.fid_nmx,
           core->fid.fid_seq, core->fid.fid_rvn);
    printf("  ext_fid=(%u,%u,%u,%u)\n", core->ext_fid.fid_num, core->ext_fid.fid_nmx,
           core->ext_fid.fid_seq, core->ext_fid.fid_rvn);
    printf("  rtype=0x%02x rattrib=0x%02x rsize=%u\n",
           core->recattr.rtype, core->recattr.rattrib, core->recattr.rsize);
    printf("  hiblk(raw)=%u hiblk(swapped)=%u\n",
           core->recattr.hiblk, ods2_word_swap32(core->recattr.hiblk));
    printf("  efblk(raw)=%u efblk(swapped)=%u\n",
           core->recattr.efblk, ods2_word_swap32(core->recattr.efblk));
    printf("  ffbyte=%u maxrec=%u\n", core->recattr.ffbyte, core->recattr.maxrec);
    printf("  filechar=0x%08x\n", core->filechar);
    printf("  recprot=0x%04x map_inuse=%u acc_mode=%u\n",
           core->recprot, core->map_inuse, core->acc_mode);
    printf("  fileowner_uic=(%u,%u) fileprot=0x%04x\n",
           core->fileowner_uic[0], core->fileowner_uic[1], core->fileprot);
    printf("  backlink=(%u,%u,%u,%u)\n", core->backlink.fid_num, core->backlink.fid_nmx,
           core->backlink.fid_seq, core->backlink.fid_rvn);
    printf("  journal=%u highwater=%u\n\n", core->journal, core->highwater);

    printf("Full raw bytes:\n");
    for (off = 0; off < 512; off += 16) {
        unsigned j;
        printf("%4u: ", off);
        for (j = 0; j < 16; j++) printf("%02x ", header[off + j]);
        printf(" ");
        for (j = 0; j < 16; j++) {
            unsigned char c = header[off + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("\n");
    }

    ods2_dismount(&vol);
    return 0;
}
