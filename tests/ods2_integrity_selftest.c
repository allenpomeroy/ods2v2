/*
 * MIT License
 *
 * Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer)
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


/* ods2_integrity_selftest - what a delete, a directory insert and a damaged volume may and may not do, on a copy of
 * the synthetic disk. Built with ODS2_FAULT_INJECTION: ods2_fault_after_writes makes the (n+1)th write and every one
 * after it fail, as if the machine stopped there, and the journal puts the image back after each try.
 *
 *  - The volume's own files (file numbers 1-9, and SECURITY.SYS, 10) cannot be deleted, whichever directory lists them
 *    (VMS took such entries out with SET FILE/REMOVE and left LOSTHEADER; deleting one frees clusters the volume
 *    depends on). A refusal writes nothing.
 *  - A delete is written in VMS's order (the header marked for delete with its FID and checksum intact, the directory
 *    entry removed, the final deleted headers, the blocks freed, the bits cleared), so that in no state after any
 *    number of writes does a directory list a file whose blocks are free. The earlier order (blocks freed first, the
 *    entry removed last) did in every state but the last.
 *  - A directory block that splits, with blocks after it that move down one, is written in an order in which no name
 *    is ever unlisted (a name may be listed twice for a moment, never zero times).
 *  - A file with more than 1,024 extents is refused (read and delete), not handled with the first 1,024.
 *  - A header whose checksum or offsets are wrong, and a directory block whose records run past the block, are not
 *    rewritten with a fresh checksum: the change is refused, with nothing written. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ods2_volume.h"
#include "ods2_checksum.h"
#include "ods2_header_build.h"

#define SOURCE_DISK_PATH "samples/synthetic_disk.img"
#define DISK_PATH        "samples/integrity_disk.img"

extern long ods2_fault_after_writes;
extern bool ods2_fault_journal_on;
void ods2_fault_rollback(ods2_volume_t *vol);
void ods2_fault_forget(void);   /* drops the journal: the writes since the last arm() stay */

static void copy_sparse(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    static const uint8_t zero[512];
    uint8_t buf[512];
    size_t n;
    long total = 0;

    assert(in != NULL && out != NULL);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (n == sizeof(buf) && memcmp(buf, zero, sizeof(buf)) == 0) {
            assert(fseek(out, (long) n, SEEK_CUR) == 0);
        } else {
            assert(fwrite(buf, 1, n, out) == n);
        }
        total += (long) n;
    }
    assert(fseek(out, total - 1, SEEK_SET) == 0);
    assert(fputc(0, out) != EOF);
    fclose(in);
    fclose(out);
}

#define NOLIMIT 1000000L

/* Starts counting the writes of the next operation (and keeping what they replace). */
static void arm(long fail_after)
{
    ods2_fault_after_writes = fail_after;
    ods2_fault_journal_on = true;
}

/* Stops, and returns how many writes the operation made. */
static long disarm(long armed_with)
{
    long made = armed_with >= 0 ? armed_with - ods2_fault_after_writes : 0;
    ods2_fault_after_writes = -1;
    ods2_fault_journal_on = false;
    return made;
}

/* Is the cluster holding `lbn` free in BITMAP.SYS? */
static bool cluster_free(ods2_volume_t *vol, uint32_t lbn)
{
    uint8_t h[512], b[512];
    unsigned cluster = lbn / vol->home.cluster;

    assert(ods2_read_header(vol, 2, h).ok);
    assert(ods2_read_file_block(vol, h, 2 + cluster / 4096, b).ok);
    return (b[(cluster % 4096) / 8] >> (cluster % 8)) & 1;
}

/* The invariant of a delete: a name that is listed names a header (a live file's, or one marked for delete) whose
   blocks are allocated. */
static void check_listed_file_intact(ods2_volume_t *vol, unsigned dir_num, const char *name)
{
    uint8_t dh[512], fh[512];
    ods2_fid_t fid;
    ods2_extent_t ex[ODS2_MAX_EXTENTS];
    int n, i;

    assert(ods2_read_header(vol, dir_num, dh).ok);
    if (!ods2_lookup_name(vol, dh, name, &fid).ok) return;              /* not listed: nothing to hold */
    assert(ods2_read_header(vol, fid.fid_num, fh).ok);
    /* Listed, and the header is a live file or one marked for delete (the delete's first write: the entry is the next
       to go): either way the blocks it maps must still be allocated. A header zeroed or marked and unlisted is fine. */
    assert(ods2_decode_all_extents(vol, fh, ex, ODS2_MAX_EXTENTS, &n).ok);
    for (i = 0; i < n; i++) {
        assert(!cluster_free(vol, ex[i].lbn));                           /* listed, but its blocks are free */
    }
}

static unsigned count_named(ods2_volume_t *vol, unsigned dir_num, const char *name)
{
    uint8_t dh[512];
    ods2_dir_entry_t *e = NULL;
    int count = 0, i;
    unsigned found = 0;

    assert(ods2_read_header(vol, dir_num, dh).ok);
    assert(ods2_list_directory_alloc(vol, dh, &e, &count).ok);
    for (i = 0; i < count; i++) if (strcmp(e[i].name, name) == 0) found++;
    free(e);
    return found;
}

int main(void)
{
    ods2_volume_t vol;
    uint8_t root[512];
    ods2_fid_t fid, dfid, other;
    ods2_result_t r;
    unsigned dnum;
    long k;
    int i, tested = 0;
    static const char *reserved[] = { "INDEXF.SYS", "BITMAP.SYS", "BADBLK.SYS", "000000.DIR", "CORIMG.SYS", "VOLSET.SYS",
                                      "CONTIN.SYS", "BACKUP.SYS", "BADLOG.SYS", "SECURITY.SYS" };

    copy_sparse(SOURCE_DISK_PATH, DISK_PATH);
    assert(ods2_mount_write(DISK_PATH, &vol).ok);
    assert(ods2_read_header(&vol, 4, root).ok);

    /* ---- the reserved files ---- */
    assert(ods2_create_directory(&vol, root, "ALIASES", &dfid).ok);
    dnum = dfid.fid_num;
    for (i = 0; i < 10; i++) {
        uint8_t h[512];
        ods2_fid_t rf;
        char name[32];
        unsigned num = (unsigned) i + 1;

        snprintf(name, sizeof(name), "R%d.SYS", i + 1);
        if (!ods2_read_header(&vol, num, h).ok) continue;
        rf.fid_num = (uint16_t) num; rf.fid_seq = ((const ods2_head_core_t *) h)->fid.fid_seq; rf.fid_rvn = 0; rf.fid_nmx = 0;
        if (!ods2_header_is_file(h, rf)) continue;                      /* the starter has no file 10, say */
        assert(ods2_insert_into_directory(&vol, dnum, name, 1, rf).ok);   /* an entry in another directory */
        arm(NOLIMIT);
        r = ods2_delete_version(&vol, dnum, name, 1, NULL);
        assert(!r.ok && strstr(r.problem, "reserved") != NULL);
        assert(disarm(NOLIMIT) == 0);
        ods2_fault_forget();
        arm(NOLIMIT);
        r = ods2_delete_version(&vol, 4, reserved[i], 1, NULL);
        assert(!r.ok && strstr(r.problem, "reserved") != NULL);
        assert(disarm(NOLIMIT) == 0);
        ods2_fault_forget();
        tested++;
    }
    assert(tested >= 9);
    printf("PASS: INDEXF.SYS ... SECURITY.SYS cannot be deleted, from [000000] or through an entry in another directory; "
           "nothing is written\n");

    /* ---- the order of a delete ---- */
    {
        static uint8_t content[512 * 5];
        long full = -1;

        memset(content, 'v', sizeof(content));
        assert(ods2_create_file(&vol, root, "VICTIM.DAT", content, sizeof(content), 2, &fid).ok);
        assert(ods2_create_file(&vol, root, "NEIGHBOUR.DAT", content, 512, 2, &other).ok);
        for (k = 0; k < 64 && full < 0; k++) {
            arm(k);
            r = ods2_delete_version(&vol, 4, "VICTIM.DAT", 1, NULL);
            if (r.ok) {
                full = disarm(k);                                       /* the delete took fewer than k writes */
                ods2_fault_forget();
                assert(count_named(&vol, 4, "VICTIM.DAT") == 0);
            } else {
                disarm(k);
                check_listed_file_intact(&vol, 4, "VICTIM.DAT");
                check_listed_file_intact(&vol, 4, "NEIGHBOUR.DAT");
                ods2_fault_rollback(&vol);
            }
        }
        assert(full > 0 && k < 64);
        printf("PASS: a delete interrupted after each of its writes (0 to %ld) never leaves a listed file whose blocks "
               "are free\n", full);
    }

    /* ---- a directory block that splits ---- */
    {
        uint8_t d[512];
        ods2_fid_t d2;
        char name[64];
        int step, trials = 0, splits = 0;
        ods2_dir_entry_t *before = NULL;
        int nbefore = 0;

        assert(ods2_create_directory(&vol, root, "BIG", &d2).ok);
        assert(ods2_read_header(&vol, d2.fid_num, d).ok);
        /* names of 39 characters: nine records to a block. First fill three blocks from the top of the alphabet down,
           so that later names fall into the first block, which is full, with blocks after it. */
        for (step = 0; step < 30; step++) {
            snprintf(name, sizeof(name), "Z%03d_PADDING_TO_MAKE_A_LONG_NAME.DAT", 300 - step);
            assert(ods2_insert_into_directory(&vol, d2.fid_num, name, 1, fid).ok);
        }
        for (step = 0; step < 40; step++) {
            uint8_t dd[512];
            long made = -1;
            unsigned used_before, used_after;
            int c2 = 0, j, tries = 0;

            snprintf(name, sizeof(name), "A%03d_PADDING_TO_MAKE_A_LONG_NAME.DAT", 100 + step);
            assert(ods2_read_header(&vol, d2.fid_num, dd).ok);
            used_before = ods2_word_swap32(((const ods2_head_core_t *) dd)->recattr.efblk);
            assert(ods2_list_directory_alloc(&vol, dd, &before, &nbefore).ok);
            for (k = 0; k < 40 && made < 0; k++) {
                ods2_dir_entry_t *after = NULL;
                int nafter = 0;
                arm(k);
                r = ods2_insert_into_directory(&vol, d2.fid_num, name, 1, fid);
                if (r.ok) {
                    made = disarm(k);
                    ods2_fault_rollback(&vol);                         /* (the real insert follows) */
                    break;
                }
                disarm(k);
                tries++;
                assert(ods2_read_header(&vol, d2.fid_num, dd).ok);
                assert(ods2_list_directory_alloc(&vol, dd, &after, &nafter).ok);
                for (c2 = 0; c2 < nbefore; c2++) {                       /* every name listed before is still listed */
                    int found = 0;
                    for (j = 0; j < nafter && !found; j++) {
                        found = strcmp(before[c2].name, after[j].name) == 0 && before[c2].version == after[j].version;
                    }
                    if (!found) {
                        fprintf(stderr, "step %d, %ld writes: %s is no longer listed\n", step, k, before[c2].name);
                    }
                    assert(found);
                }
                free(after);
                ods2_fault_rollback(&vol);
            }
            trials += tries;
            free(before);
            before = NULL;
            assert(ods2_insert_into_directory(&vol, d2.fid_num, name, 1, fid).ok);
            assert(ods2_read_header(&vol, d2.fid_num, dd).ok);
            used_after = ods2_word_swap32(((const ods2_head_core_t *) dd)->recattr.efblk);
            if (used_after > used_before) splits++;
        }
        assert(splits >= 3);
        printf("PASS: %d inserts, %d of them splitting a block with blocks after it, each interrupted after every write "
               "(%d trials): no name was ever unlisted\n", 40, splits, trials);
    }

    /* ---- more than 1,024 extents ---- */
    {
        /* A file whose header chain describes 1,079 one-block extents (the primary header's one and 14 x 77 in 14 extension headers, in
           free header slots, made by hand). The first 1,024 used to be handled and the rest dropped without a word: a
           file read short, or deleted with 55 of its clusters left allocated. Nothing reads the blocks, so the LBNs
           only have to be representable. */
        enum { SEGMENTS = 15, PER = 77 };
        ods2_fid_t head, chain[SEGMENTS];
        uint8_t h[512], ext_hdr[512];
        ods2_extent_t ext[PER], got[ODS2_MAX_EXTENTS];
        ods2_header_spec_t spec;
        int seg, e, n;
        uint16_t sum;
        char problem_seen[200];

        assert(ods2_create_file(&vol, root, "FRAG.DAT", (const uint8_t *) "x", 1, 2, &head).ok);
        chain[0] = head;
        for (seg = 1; seg < SEGMENTS; seg++) {
            /* header slots inside INDEXF.SYS: real small files made for the purpose, whose headers are overwritten
               below with the extension headers */
            char nm[32];
            snprintf(nm, sizeof(nm), "SLOT%02d.DAT", seg);
            assert(ods2_create_file(&vol, root, nm, (const uint8_t *) "x", 1, 2, &chain[seg]).ok);
        }
        for (seg = SEGMENTS - 1; seg >= 1; seg--) {          /* the extension headers, last to first */
            for (e = 0; e < PER; e++) { ext[e].lbn = 2000000u + (uint32_t) ((unsigned) seg * PER + (unsigned) e) * 2u; ext[e].block_count = 1; }
            memset(&spec, 0, sizeof(spec));
            spec.fid = chain[seg];
            spec.backlink = head;
            spec.seg_num = (uint16_t) seg;
            if (seg + 1 < SEGMENTS) spec.ext_fid = chain[seg + 1];
            spec.extents = ext; spec.extent_count = PER; spec.hiblk = PER; spec.efblk = 1;
            spec.ident_name = "FRAG.DAT;1";
            assert(ods2_build_file_header(ext_hdr, &spec));
            assert(ods2_write_header(&vol, chain[seg].fid_num, ext_hdr).ok);
        }
        assert(ods2_read_header(&vol, head.fid_num, h).ok);
        {
            ods2_head_core_t *core = (ods2_head_core_t *) h;
            uint8_t blk[512];
            core->ext_fid = chain[1];
            sum = ods2_checksum(h, 255);
            h[510] = (uint8_t) (sum & 0xff); h[511] = (uint8_t) (sum >> 8);
            assert(ods2_write_header(&vol, head.fid_num, h).ok);
            r = ods2_decode_all_extents(&vol, h, got, ODS2_MAX_EXTENTS, &n);
            assert(!r.ok && strstr(r.problem, "more extents") != NULL);
            snprintf(problem_seen, sizeof(problem_seen), "%s", r.problem);
            r = ods2_read_file_block(&vol, h, 1000, blk);
            assert(!r.ok);
            arm(NOLIMIT);
            r = ods2_delete_version(&vol, 4, "FRAG.DAT", 1, NULL);
            assert(!r.ok && strstr(r.problem, "more extents") != NULL);
            assert(disarm(NOLIMIT) == 0);
            ods2_fault_forget();
            assert(count_named(&vol, 4, "FRAG.DAT") == 1);
        }
        printf("PASS: a file with 1,079 extents is refused for reading and deleting (%s), nothing written\n", problem_seen);
    }

    /* ---- headers and directory blocks that are damaged are not rewritten ---- */
    {
        uint8_t h[512], blk[512];
        ods2_fid_t f1, d1, d2;
        uint16_t sum;
        ods2_head_core_t *core;
        ods2_extent_t ex[4];
        int n;

        /* a file header with a wrong checksum: DELETE would rewrite it with a right one */
        assert(ods2_create_file(&vol, root, "BADSUM.DAT", (const uint8_t *) "x", 1, 2, &f1).ok);
        assert(ods2_read_header(&vol, f1.fid_num, h).ok);
        h[300] ^= 0x5a;                                      /* in the map area's unused tail: only the checksum shows it */
        assert(ods2_write_header(&vol, f1.fid_num, h).ok);
        arm(NOLIMIT);
        r = ods2_delete_version(&vol, 4, "BADSUM.DAT", 1, NULL);
        assert(!r.ok && strstr(r.problem, "damaged") != NULL);
        assert(disarm(NOLIMIT) == 0);
        ods2_fault_forget();
        assert(count_named(&vol, 4, "BADSUM.DAT") == 1);

        /* a directory header whose IDOFFSET is not a header's: the revision count and date are stored at IDOFFSET*2 + 20
           and + 30, i.e. at the wrong place, so the insert is refused */
        assert(ods2_create_directory(&vol, root, "ODDHDR", &d1).ok);
        assert(ods2_read_header(&vol, d1.fid_num, h).ok);
        core = (ods2_head_core_t *) h;
        core->idoffset = 0;
        sum = ods2_checksum(h, 255);
        h[510] = (uint8_t) (sum & 0xff); h[511] = (uint8_t) (sum >> 8);
        assert(ods2_write_header(&vol, d1.fid_num, h).ok);
        {
            /* entries that fit are written into the block without touching the header; the insert that needs the
               directory to grow rewrites the header (and stores into it at IDOFFSET), and is refused */
            char nm[64];
            int inserted = 0;
            for (n = 0; n < 40; n++) {
                snprintf(nm, sizeof(nm), "Q%03d_PADDING_TO_MAKE_A_LONG_NAME.DAT", n);
                arm(NOLIMIT);
                r = ods2_insert_into_directory(&vol, d1.fid_num, nm, 1, f1);
                if (!r.ok) {
                    assert(strstr(r.problem, "damaged") != NULL);
                    assert(disarm(NOLIMIT) == 0);
                    ods2_fault_forget();
                    break;
                }
                disarm(NOLIMIT);
                ods2_fault_forget();
                inserted++;
            }
            assert(inserted > 0 && n < 40);
        }

        /* a directory block with a record whose name count runs past the block */
        assert(ods2_create_directory(&vol, root, "ODDBLK", &d2).ok);
        assert(ods2_read_header(&vol, d2.fid_num, h).ok);
        assert(ods2_decode_all_extents(&vol, h, ex, 4, &n).ok && n >= 1);
        memset(blk, 0, sizeof(blk));
        blk[0] = 20; blk[1] = 0;                             /* record size 20 */
        blk[2] = 1; blk[3] = 0;                              /* version limit */
        blk[5] = 255;                                        /* name count 255: far past the record */
        blk[22] = 0xff; blk[23] = 0xff;
        assert(ods2_write_block(&vol, ex[0].lbn, blk).ok);
        arm(NOLIMIT);
        r = ods2_insert_into_directory(&vol, d2.fid_num, "Y.TXT", 1, f1);
        assert(!r.ok && strstr(r.problem, "damaged") != NULL);
        assert(disarm(NOLIMIT) == 0);
        ods2_fault_forget();
        printf("PASS: a header with a wrong checksum, a directory header with a wrong IDOFFSET and a directory block with "
               "a record that runs past it are not rewritten: the change is refused, nothing is written\n");
    }

    ods2_dismount(&vol);
    remove(DISK_PATH);
    printf("\nods2_integrity_selftest: all checks passed\n");
    return 0;
}
