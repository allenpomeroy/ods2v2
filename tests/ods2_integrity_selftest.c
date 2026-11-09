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

/* The invariant of review 7 (R7-1): no header that could be finished or recovered maps a free cluster. Every header
   slot that holds a header marked for delete with its FID still intact must map only allocated clusters. */
static void check_no_marked_header_maps_free(ods2_volume_t *vol)
{
    unsigned num;
    uint8_t h[512];
    ods2_extent_t ex[ODS2_MAX_EXTENTS];
    int n, i;

    for (num = 1; num < 80; num++) {
        const ods2_head_core_t *core = (const ods2_head_core_t *) h;
        if (!ods2_read_header(vol, num, h).ok) continue;
        if (!(core->filechar & 0x8000u) || core->fid.fid_num == 0) continue;
        if (!ods2_decode_all_extents(vol, h, ex, ODS2_MAX_EXTENTS, &n).ok) continue;
        for (i = 0; i < n; i++) assert(!cluster_free(vol, ex[i].lbn));
    }
}

static ods2_fid_t root_fid_of(const uint8_t *root_hdr)
{
    return ((const ods2_head_core_t *) root_hdr)->fid;
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
                check_no_marked_header_maps_free(&vol);
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
        /* even an entry that fits is refused: a directory is checked as a whole before any change (a refusal only
           where the header is first rewritten would leave the volume half changed) */
        arm(NOLIMIT);
        r = ods2_insert_into_directory(&vol, d1.fid_num, "X.TXT", 1, f1);
        assert(!r.ok && strstr(r.problem, "damaged") != NULL);
        assert(disarm(NOLIMIT) == 0);
        ods2_fault_forget();

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

    /* ---- a refusal leaves nothing behind (review 15) ---- */
    {
        uint8_t h[512], blk[512], pb[512];
        ods2_fid_t victim, pfid, tail, big;
        ods2_extent_t ex[4];
        int n;
        ods2_head_core_t *core;
        uint16_t sum;
        unsigned pos;

        /* a directory whose first block holds A.TXT and then a record the read side accepts but the write side
           refuses (a name count of 0): the lookup finds A.TXT, so a delete used to mark its header before the
           directory was first checked for writing */
        assert(ods2_create_file(&vol, root, "VICTIM2.DAT", (const uint8_t *) "victim\n", 7, 5, &victim).ok);
        assert(ods2_create_directory(&vol, root, "PREF", &pfid).ok);
        assert(ods2_insert_into_directory(&vol, pfid.fid_num, "A.TXT", 1, victim).ok);
        assert(ods2_read_header(&vol, pfid.fid_num, pb).ok);
        assert(ods2_decode_all_extents(&vol, pb, ex, 4, &n).ok && n >= 1);
        assert(ods2_read_file_block(&vol, pb, 1, blk).ok);
        for (pos = 0; pos + 6 <= 512; ) {                    /* the end marker after the last real record */
            uint16_t size = (uint16_t) (blk[pos] | (blk[pos + 1] << 8));
            if (size == 0xffff || size == 0) break;
            pos += (unsigned) size + 2;
        }
        assert(pos + 16 <= 512);
        blk[pos] = 12; blk[pos + 1] = 0;                     /* record size 12 (14 with the size word) ... */
        blk[pos + 2] = 1; blk[pos + 3] = 0;
        blk[pos + 4] = 0; blk[pos + 5] = 0;                  /* ... a name count of 0 */
        memset(blk + pos + 6, 0, 8);
        blk[pos + 14] = 0xff; blk[pos + 15] = 0xff;
        assert(ods2_write_block(&vol, ex[0].lbn, blk).ok);
        {
            ods2_fid_t found;
            assert(ods2_read_header(&vol, pfid.fid_num, pb).ok);
            assert(ods2_lookup_name(&vol, pb, "A.TXT", &found).ok);   /* the read side lists it */
        }

        /* R15-1: DELETE [PREF]A.TXT is refused, and the header is not marked for delete */
        arm(NOLIMIT);
        r = ods2_delete_version(&vol, pfid.fid_num, "A.TXT", 1, NULL);
        assert(!r.ok && strstr(r.problem, "damaged") != NULL);
        assert(disarm(NOLIMIT) == 0);
        ods2_fault_forget();
        assert(ods2_read_header(&vol, victim.fid_num, h).ok);
        assert(!(((const ods2_head_core_t *) h)->filechar & 0x8000u));
        assert(ods2_header_is_file(h, victim));

        /* R15-2: COPY and CREATE/DIRECTORY into it are refused before anything is allocated: no lost file */
        {
            ods2_fid_t nf;
            arm(NOLIMIT);
            r = ods2_create_file(&vol, pb, "NEW.TXT", (const uint8_t *) "x", 1, 5, &nf);
            assert(!r.ok);
            assert(disarm(NOLIMIT) == 0);
            ods2_fault_forget();
            arm(NOLIMIT);
            r = ods2_create_directory(&vol, pb, "NEWDIR", &nf);
            assert(!r.ok);
            assert(disarm(NOLIMIT) == 0);
            ods2_fault_forget();
            /* and the file number it would have taken is still free (nothing lost) */
            {
                unsigned num1, num2;
                uint16_t sq;
                assert(ods2_find_free_file_number(&vol, 1, 4000, &num1, &sq).ok);
                assert(ods2_create_file(&vol, root, "CHECK.DAT", (const uint8_t *) "x", 1, 5, &nf).ok);
                assert(nf.fid_num == num1);
                (void) num2;
            }
        }

        /* the same with a parent directory header that has a wrong checksum */
        assert(ods2_read_header(&vol, pfid.fid_num, h).ok);
        h[300] ^= 0x33;
        assert(ods2_write_header(&vol, pfid.fid_num, h).ok);
        {
            ods2_fid_t nf;
            arm(NOLIMIT);
            r = ods2_create_file(&vol, h, "NEW2.TXT", (const uint8_t *) "x", 1, 5, &nf);
            assert(!r.ok);
            assert(disarm(NOLIMIT) == 0);
            ods2_fault_forget();
            h[300] ^= 0x33;
            assert(ods2_write_header(&vol, pfid.fid_num, h).ok);
        }
        printf("PASS: DELETE, COPY and CREATE/DIRECTORY into a directory that cannot be written are refused before the "
               "first write: no marked header, no lost file\n");

        /* R15-3: a header chain whose EXT_FID names an unrelated live file: the delete is refused, the file is not
           destroyed */
        assert(ods2_create_file(&vol, root, "CHAINED.DAT", (const uint8_t *) "chain\n", 6, 5, &big).ok);
        assert(ods2_create_file(&vol, root, "TAIL.TXT", (const uint8_t *) "tail\n", 5, 5, &tail).ok);
        assert(ods2_read_header(&vol, big.fid_num, h).ok);
        core = (ods2_head_core_t *) h;
        core->ext_fid = tail;
        sum = ods2_checksum(h, 255);
        h[510] = (uint8_t) (sum & 0xff); h[511] = (uint8_t) (sum >> 8);
        assert(ods2_write_header(&vol, big.fid_num, h).ok);
        arm(NOLIMIT);
        r = ods2_delete_version(&vol, 4, "CHAINED.DAT", 1, NULL);
        assert(!r.ok && strstr(r.problem, "extension") != NULL);
        assert(disarm(NOLIMIT) == 0);
        ods2_fault_forget();
        assert(ods2_read_header(&vol, tail.fid_num, h).ok && ods2_header_is_file(h, tail));
        assert(ods2_read_header(&vol, big.fid_num, pb).ok);
        assert(!ods2_decode_all_extents(&vol, pb, ex, 4, &n).ok);
        printf("PASS: an extension chain that is not this file's (EXT_FID names another live file) is refused for "
               "reading and deleting: the other file is not destroyed\n");

        /* R15-4: a directory with an extension header is not rewritten (the move would leave the extension behind) */
        {
            ods2_fid_t dx;
            assert(ods2_create_directory(&vol, root, "WITHEXT", &dx).ok);
            assert(ods2_read_header(&vol, dx.fid_num, h).ok);
            core = (ods2_head_core_t *) h;
            core->ext_fid = tail;                               /* any other header: the chain is not followed */
            sum = ods2_checksum(h, 255);
            h[510] = (uint8_t) (sum & 0xff); h[511] = (uint8_t) (sum >> 8);
            assert(ods2_write_header(&vol, dx.fid_num, h).ok);
            arm(NOLIMIT);
            r = ods2_insert_into_directory(&vol, dx.fid_num, "Z.TXT", 1, victim);
            assert(!r.ok);
            assert(disarm(NOLIMIT) == 0);
            ods2_fault_forget();
        }
        printf("PASS: a directory whose header has an extension header is not changed\n");

        /* R15-6: a file whose header maps an extent beyond the storage bitmap: the delete is refused before the
           header is marked (the free would have refused after the headers were final) */
        {
            ods2_fid_t bf;
            ods2_header_spec_t spec;
            ods2_extent_t beyond;
            assert(ods2_create_file(&vol, root, "BEYOND.DAT", (const uint8_t *) "b", 1, 5, &bf).ok);
            assert(ods2_read_header(&vol, bf.fid_num, h).ok);
            memset(&spec, 0, sizeof(spec));
            spec.fid = bf; spec.backlink = root_fid_of(root);
            beyond.lbn = 4194300u - (4194300u % vol.home.cluster); beyond.block_count = vol.home.cluster;
            spec.extents = &beyond; spec.extent_count = 1; spec.hiblk = vol.home.cluster; spec.efblk = 2;
            spec.rtype = 5; spec.rattrib = 2;
            spec.ident_name = "BEYOND.DAT;1";
            assert(ods2_build_file_header(h, &spec));
            assert(ods2_write_header(&vol, bf.fid_num, h).ok);
            arm(NOLIMIT);
            r = ods2_delete_version(&vol, 4, "BEYOND.DAT", 1, NULL);
            assert(!r.ok);
            assert(disarm(NOLIMIT) == 0);
            ods2_fault_forget();
            assert(ods2_read_header(&vol, bf.fid_num, h).ok && !(((const ods2_head_core_t *) h)->filechar & 0x8000u));
        }
        printf("PASS: a file with an extent beyond the storage bitmap is refused before its header is marked\n");

        /* R15-5: INDEXF.SYS's own header with a damaged IDOFFSET (checksum refreshed): a COPY is refused */
        {
            ods2_fid_t nf;
            uint8_t ih[512];
            assert(ods2_read_header(&vol, 1, ih).ok);
            ((ods2_head_core_t *) ih)->idoffset = 250;
            sum = ods2_checksum(ih, 255);
            ih[510] = (uint8_t) (sum & 0xff); ih[511] = (uint8_t) (sum >> 8);
            arm(NOLIMIT);                                       /* the journal puts header 1 back after */
            assert(ods2_write_header(&vol, 1, ih).ok);
            {
                long before = ods2_fault_after_writes;
                r = ods2_create_file(&vol, root, "INDEXDAMAGED.TXT", (const uint8_t *) "x", 1, 5, &nf);
                assert(!r.ok && strstr(r.problem, "damaged") != NULL);
                assert(before == ods2_fault_after_writes);       /* no write at all after the damage */
            }
            disarm(NOLIMIT);
            ods2_fault_rollback(&vol);
        }
        printf("PASS: a damaged INDEXF.SYS header is not rewritten with a fresh checksum\n");

        /* header_ok_for_write: offsets out of order, MPOFFSET at the end of the block (a header whose FID is not the entry's is
           a stale entry, not damage: it is taken out alone, tested in ods2_fid_selftest) */
        {
            ods2_fid_t hf;
            int variant;
            assert(ods2_create_file(&vol, root, "HOK.DAT", (const uint8_t *) "x", 1, 5, &hf).ok);
            for (variant = 0; variant < 2; variant++) {
                assert(ods2_read_header(&vol, hf.fid_num, h).ok);
                core = (ods2_head_core_t *) h;
                if (variant == 0) core->acoffset = (uint8_t) (core->mpoffset - 1);          /* offsets out of order */
                if (variant == 1) core->mpoffset = 255;                                      /* MPOFFSET at the very end */
                sum = ods2_checksum(h, 255);
                h[510] = (uint8_t) (sum & 0xff); h[511] = (uint8_t) (sum >> 8);
                assert(ods2_write_header(&vol, hf.fid_num, h).ok);
                arm(NOLIMIT);
                r = ods2_delete_version(&vol, 4, "HOK.DAT", 1, NULL);
                assert(!r.ok);
                assert(disarm(NOLIMIT) == 0);
                ods2_fault_forget();
                assert(ods2_read_header(&vol, hf.fid_num, h).ok);
                assert(!(((const ods2_head_core_t *) h)->filechar & 0x8000u));
                /* put it right again for the next variant */
                assert(ods2_read_header(&vol, hf.fid_num, h).ok);
                core = (ods2_head_core_t *) h;
                if (variant == 0) core->acoffset = core->rsoffset;
                if (variant == 1) core->mpoffset = 100;
                sum = ods2_checksum(h, 255);
                h[510] = (uint8_t) (sum & 0xff); h[511] = (uint8_t) (sum >> 8);
                assert(ods2_write_header(&vol, hf.fid_num, h).ok);
            }
        }
        /* SET FILE/VERSION_LIMIT on the damaged directory of the first test: refused, nothing written */
        {
            unsigned changed = 0;
            arm(NOLIMIT);
            r = ods2_set_entry_version_limit(&vol, pfid.fid_num, "A.TXT", 3, &changed);
            assert(!r.ok && strstr(r.problem, "damaged") != NULL);
            assert(disarm(NOLIMIT) == 0);
            ods2_fault_forget();
        }
        printf("PASS: a header with offsets out of order or an MPOFFSET at the block's end is not "
               "rewritten; SET FILE/VERSION_LIMIT refuses a damaged directory\n");
    }

    ods2_dismount(&vol);
    remove(DISK_PATH);
    printf("\nods2_integrity_selftest: all checks passed\n");
    return 0;
}
