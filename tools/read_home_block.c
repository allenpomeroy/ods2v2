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
#include <string.h>
#include "ods2_ondisk.h"
#include "ods2_validate.h"

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "samples/sample.bin";
    FILE *f = fopen(path, "rb");
    long offset;
    int block_num;
    int copies_found = 0, copies_ok = 0;

    if (!f) {
        perror("fopen");
        return 1;
    }

    /* Scan the first 32 blocks looking for redundant home block
       copies (spec: multiple copies exist in early clusters for
       redundancy). A real home block's own homelbn field should
       self-reference the block it's actually stored at. */
    for (block_num = 0; block_num < 32; block_num++) {
        ods2_home_t home;
        ods2_validate_result_t r;

        offset = (long) block_num * 512;
        if (fseek(f, offset, SEEK_SET) != 0) break;
        if (fread(&home, 1, sizeof(home), f) != sizeof(home)) break;

        if (memcmp(home.format, "DECFILE11B  ", 12) != 0) continue;

        copies_found++;
        printf("--- Home block copy at block %d (byte offset %ld) ---\n",
               block_num, offset);
        printf("  self-reference homelbn = %u (%s)\n", home.homelbn,
               (home.homelbn == (uint32_t) block_num) ? "MATCHES position" : "MISMATCH");
        printf("  volname   = \"%.12s\"\n", home.volname);
        printf("  struclev  = 0x%04x\n", home.struclev);
        printf("  maxfiles  = %u\n", home.maxfiles);
        printf("  altidxlbn = %u\n", home.altidxlbn);

        r = ods2_validate_home(&home);
        printf("  ods2_validate_home(): %s\n", r.ok ? "OK" : "FAILED");
        if (!r.ok) {
            printf("    reason: %s\n", r.problem);
        } else {
            copies_ok++;
        }
        printf("\n");
    }

    fclose(f);
    printf("=== %d home block copies found, %d validated OK ===\n",
           copies_found, copies_ok);
    return (copies_found > 0 && copies_found == copies_ok) ? 0 : 1;
}
