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

/* ods2.c - a unified, DCL-style command-line interface for reading
 * and writing ODS-2 disk images. Wraps everything built in ods2v2
 * into one easy-to-use tool, rather than the separate single-purpose
 * tools (ods2_ls, ods2_mkdir, ods2_put, ods2_cat, ods2_rm) used
 * during development - those remain as diagnostic building blocks,
 * this is the intended day-to-day interface.
 *
 * Usage:
 *   ods2 <disk-image>                       interactive shell
 *   ods2 <disk-image> <command...>          run one command, exit
 *   ods2 <disk-image> @<script-file>        run commands from a file,
 *                                            one per line
 *
 * Exit status: 0 on success; 1 if the disk image couldn't be mounted,
 * or if the command (single-shot) or any command in the script failed
 * - i.e. reported a %ODS2-E- error. A script runs to the end either
 * way. Warnings (%ODS2-W-, e.g. DIR finding no files) don't count as
 * failure.
 *
 * Commands (case-insensitive; VMS bracket-notation paths throughout,
 * e.g. [DECUS.NETLIB020]FILE.TXT):
 *   DIR [path] [wildcard]         list a directory
 *   DIR [path...] [wildcard]      list a directory and everything
 *                                 beneath it, recursively (VMS's
 *                                 "..." notation)
 *   DIR [*...] [wildcard]         directory wildcards: '*' and '%' in
 *                                 any directory name, "..." anywhere
 *                                 (e.g. [*], [*.SRC], [...SALES],
 *                                 [DECUS...SRC]) - as VMS DIRECTORY
 *   CREATE/DIRECTORY path         create a directory
 *   COPY <local-file> <path>      write a local file onto the disk,
 *                                 as a Stream_LF file
 *   COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) <local-file> <path>
 *                                 ... with chosen RMS record attributes,
 *                                 in SET FILE/ATTRIBUTES syntax (see
 *                                 parse_copy_qualifiers())
 *   COPY/CONVERT <local-file> <path>
 *                                 a text file as variable-length
 *                                 records (RFM:VAR, RAT:CR), one per
 *                                 line - VMS's native text format
 *   TYPE path                     print a file's content
 *   DELETE path                   delete a file (one version) or an
 *                                 empty directory
 *   SET DEFAULT path              set the current default directory
 *   SHOW DEFAULT                  show the current default directory
 *   SET VOLUME/LABEL=name         relabel the volume: every home block
 *                                 copy, checksums recomputed
 *   SET FILE/VERSION_LIMIT=n spec the version limit of the entries a spec
 *                                 names ([dir...] and wildcards work as in
 *                                 DIR); 32767 or 0 = no limit, as VMS writes
 *   SHOW VOLUME                   label, owner, cluster size, maximum
 *                                 files, total and free blocks
 *   HELP                          show this command list
 *   EXIT / QUIT                   leave the interactive shell
 *
 * Paths starting with '.' or '-' inside brackets (e.g. [.NETLIB020],
 * [-], [-.SIBLING]) and bare filenames with no brackets at all are
 * relative to the current default directory (see SET DEFAULT); '-'
 * means "go up one level" (repeat as "-.-" for more); other
 * bracketed paths are always absolute from root, matching real VMS.
 *
 * A directory spec beginning with "..." is relative too: [...] is the
 * default directory and everything below it; [*...] is every
 * directory on the disk except root's own (use [000000...] for that).
 * Only DIR accepts wildcard directories; the other commands act on
 * one directory and refuse them.
 *
 * File versions follow VMS rules: NAME;3 is version 3, NAME; or NAME;0
 * or plain NAME is the highest version, NAME;-1 the next lower
 * existing version, and NAME;* every version (DIR and DELETE only).
 * DIR lists every version unless a version is given.
 *
 * Interactive mode uses a vendored copy of linenoise (BSD licensed,
 * single .c/.h file, see third_party/linenoise/) for line editing and
 * command history (up/down arrows, Ctrl-A/E/W, etc.) - chosen over
 * GNU Readline (GPL - licensing friction) and over libedit (needs a
 * separate system install: `apt install libedit-dev` on Debian/
 * Ubuntu, which isn't preinstalled everywhere and reintroduces a
 * build-environment dependency this project has otherwise avoided
 * completely). Vendoring the source directly keeps `make` working
 * with zero external dependencies on every platform, matching the
 * rest of this project - `gcc file.c -o binary` has always just
 * worked here, and this keeps that true. The one real trade-off:
 * vanilla linenoise does not support Ctrl-R reverse search (GNU
 * Readline and libedit both do) - accepted deliberately in exchange
 * for zero-dependency builds. */
#include "linenoise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include "ods2_volume.h"
#include "ods2_path.h"
#include "ods2_wildcard.h"
#include "ods2_directory_write.h"
#include "ods2_text_var.h"

#define MAX_LINE 1024
#define MAX_TOKENS 8

/* Current default directory for this session (SET DEFAULT), dot-
   separated with no brackets - "" means root. Bare filenames (no
   brackets at all) and VMS's "[.SUBDIR]" relative notation both
   resolve against this. A plain global is fine here: this is a
   single-threaded, single-volume interactive tool - there is exactly
   one "current session" for it to belong to. */
static char g_default_dir[ODS2_PATH_MAX] = "";

/* Set by report_error(): some command since the last reset failed.
   main() turns it into the exit status of a single-shot command or an
   @script, so scripts and automation can tell success from failure
   without parsing messages. */
static bool g_command_failed = false;

/* Prints an error message (%ODS2-E-...) to stderr and records that the
   current command failed. Every error a command reports goes through
   here; warnings (-W-) and informational messages (-I-) do not. */
static void report_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    g_command_failed = true;
}

/* Case-insensitive equality check. */
static bool matches(const char *token, const char *name)
{
    size_t i;
    for (i = 0; token[i] && name[i]; i++) {
        if (toupper((unsigned char) token[i]) != toupper((unsigned char) name[i])) {
            return false;
        }
    }
    return token[i] == '\0' && name[i] == '\0';
}

/* Case-insensitive equality of two directory entry names. */
static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (toupper((unsigned char) *a) != toupper((unsigned char) *b)) return false;
    }
    return *a == '\0' && *b == '\0';
}

/* Formats a parsed path's filename plus its ";version" exactly as
   typed (";", ";*", ";3", ";-1"), for messages. */
static void format_file_spec(const ods2_parsed_path_t *p, char *out, size_t out_size)
{
    if (!p->has_version) {
        snprintf(out, out_size, "%s", p->filename);
    } else if (p->version_wildcard) {
        snprintf(out, out_size, "%s;*", p->filename);
    } else if (p->version == 0) {
        snprintf(out, out_size, "%s;", p->filename);
    } else {
        snprintf(out, out_size, "%s;%d", p->filename, p->version);
    }
}

/* Commands that act on one directory refuse a wildcard or ellipsis
   in it with a clear message, rather than looking up "*" as a
   literal name or quietly ignoring a "...". Returns true if refused. */
static bool refuse_wild_dir(const ods2_parsed_path_t *p, const char *command, const char *arg)
{
    if (!p->dir_wildcard) return false;
    report_error("%%ODS2-E-WILDDIR, %s: %s does not accept a wildcard or \"...\" "
                    "in the directory\n", arg, command);
    return true;
}

/* Combines a parsed path's dir_path/relative flag with the current
 * session default into the actual, absolute (from-root) path to look
 * up - e.g. with g_default_dir="DECUS": a bare "FILE.TXT" (relative,
 * empty dir_path) resolves to "DECUS"; "[.NETLIB020]" (relative,
 * dir_path="NETLIB020") resolves to "DECUS.NETLIB020"; an explicit
 * "[SOMEWHERE]" (not relative) resolves to itself unchanged,
 * regardless of the current default - matching real VMS, where a
 * plain bracketed path is always absolute unless it starts with '.'.
 */
/* Uppercases a path string in place - real VMS storage is always
   uppercase, but a user can type a path in any case. Kept as a small,
   local duplicate of ods2_volume.c's own uppercase_str() rather than
   exported/shared across the library/CLI boundary for something this
   trivial. */
static void uppercase_display_path(char *s)
{
    for (; *s; s++) {
        *s = (char) toupper((unsigned char) *s);
    }
}

/* Combines a parsed path's dir_path/relative/up_levels flags with the
 * current session default into the actual, absolute (from-root) path
 * to look up - e.g. with g_default_dir="DECUS": a bare "FILE.TXT"
 * (relative, empty dir_path) resolves to "DECUS"; "[.NETLIB020]"
 * (relative, dir_path="NETLIB020") resolves to "DECUS.NETLIB020"; an
 * explicit "[SOMEWHERE]" (not relative) resolves to itself unchanged,
 * regardless of the current default - matching real VMS, where a
 * plain bracketed path is always absolute unless it starts with '.'
 * or '-'. VMS's "[-]" up-level notation strips that many trailing
 * dot-separated components from the current default first (going up
 * past root simply clamps at root).
 *
 * The result is always uppercased before returning: real VMS storage
 * is always uppercase (see uppercase_str() in ods2_volume.c), but a
 * user can type a path in any case - without this, a lowercase-typed
 * path would display and recurse in whatever case was typed instead
 * of the actual, canonical stored form. 
 */
static void resolve_effective_path(const ods2_parsed_path_t *p, char *out, size_t out_size)
{
    char base[ODS2_PATH_MAX];
    strncpy(base, g_default_dir, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    if (p->up_levels > 0) {
        int levels_to_strip = p->up_levels;
        while (levels_to_strip > 0 && base[0] != '\0') {
            char *last_dot = strrchr(base, '.');
            if (last_dot != NULL) {
                *last_dot = '\0';
            } else {
                base[0] = '\0'; /* only one component left - up from here is root */
            }
            levels_to_strip--;
        }
        /* levels_to_strip still > 0 here means "up" was requested
           past root - base is already "" (root), which is the
           correct, clamped result. */
    }

    if (p->relative && base[0] != '\0') {
        if (p->dir_path[0] != '\0') {
            snprintf(out, out_size, "%s.%s", base, p->dir_path);
        } else {
            snprintf(out, out_size, "%s", base);
        }
    } else {
        /* Either an explicit absolute path, or relative but the
           current default is already root - both cases just use
           dir_path directly. */
        snprintf(out, out_size, "%s", p->dir_path);
    }

    uppercase_display_path(out);
}

/* Resolves a parsed path's directory component to a header. Every
 * command needs at least the directory part resolved. */
static ods2_result_t resolve_dir(ods2_volume_t *vol, const char *dir_path,
                                  uint8_t *dir_header_out, ods2_fid_t *dir_fid_out)
{
    ods2_result_t r = ods2_lookup_path(vol, dir_path, dir_fid_out);
    if (!r.ok) return r;
    return ods2_read_header(vol, dir_fid_out->fid_num, dir_header_out);
}

/* Splits a dot-separated dir_path into its parent path and its own
 * last component name - e.g. "DECUS.NETLIB020" -> parent="DECUS",
 * name="NETLIB020". Used both when creating a directory (its own
 * name is the last component of where it will live) and when
 * deleting one referenced as a bare path like "[TESTDIR]" (no
 * filename part - the directory itself is the target, one level up
 * from where ods2_lookup_path would otherwise resolve it). `out_buf`
 * must be at least ODS2_PATH_MAX bytes; `*parent_out` and `*name_out`
 * point into it after the call.
 */
static void split_last_component(const char *dir_path, char *out_buf,
                                  const char **parent_out, const char **name_out)
{
    char *last_dot;
    strcpy(out_buf, dir_path);
    last_dot = strrchr(out_buf, '.');
    if (last_dot != NULL) {
        *last_dot = '\0';
        *parent_out = out_buf;
        *name_out = last_dot + 1;
    } else {
        *parent_out = "";
        *name_out = out_buf;
    }
}

/* Does entry `i` of a directory listing pass a DIR version filter?
   `rank` is its position among its name's versions (0 = highest,
   since they are stored newest first). */
static bool version_selected(const ods2_parsed_path_t *p, const ods2_dir_entry_t *e, int rank)
{
    if (!p->has_version || p->version_wildcard) return true; /* all versions */
    if (p->version > 0) return e->version == (unsigned) p->version;
    return rank == -p->version; /* 0 = highest, -1 = next lower, ... */
}

/* Lists the entries of one directory that match `pattern` and the
 * version filter, VMS DIRECTORY style: the "Directory [path]" heading,
 * the entries and a "Total of N files." line. Like VMS, prints nothing
 * at all for a directory with no matching entries, so a listing across
 * many directories shows only the ones that have something to show.
 * `dir_path` is absolute (from root). Returns the number shown. */
/* A command that walks directories like DIR but does something to each matching entry (SET FILE/VERSION_LIMIT)
   sets this: it is called, once per entry, in place of printing it. */
typedef void (*entry_hook_fn)(void *ctx, ods2_volume_t *vol, const char *dir_path, const ods2_dir_entry_t *entry);
static entry_hook_fn g_entry_hook = NULL;
static void *g_entry_hook_ctx = NULL;
static ods2_volume_t *g_entry_hook_vol = NULL;

static int show_directory(const char *dir_path, const ods2_dir_entry_t *entries, int count,
                          const char *pattern, const ods2_parsed_path_t *p)
{
    int i, shown = 0, rank = 0;

    /* Every version of a name is its own entry, newest first and
       contiguous (a name's versions may span several records). */
    for (i = 0; i < count; i++) {
        rank = (i > 0 && same_name(entries[i].name, entries[i - 1].name)) ? rank + 1 : 0;
        if (ods2_wildcard_match(pattern, entries[i].name) &&
            version_selected(p, &entries[i], rank)) {
            if (g_entry_hook != NULL) {
                g_entry_hook(g_entry_hook_ctx, g_entry_hook_vol, dir_path, &entries[i]);
                shown++;
                continue;
            }
            if (shown == 0) {
                printf("\nDirectory [%s%s]\n\n", (dir_path[0] == '\0') ? "000000" : "",
                       dir_path);
            }
            printf("%-20s;%u\n", entries[i].name, entries[i].version);
            shown++;
        }
    }
    if (shown > 0 && g_entry_hook == NULL) {
        printf("\nTotal of %d file%s.\n", shown, (shown == 1) ? "" : "s");
    }
    return shown;
}

/* Real VMS allows 8 directory levels; this is a generous but finite
   backstop against a directory cycle on a corrupted disk image. */
#define MAX_RECURSION_DEPTH 50

/* State for one DIR across a wildcard directory spec. */
typedef struct {
    ods2_volume_t *vol;
    const char *spec;              /* what remains to match below the start
                                      directory, e.g. "*...", "...SALES" */
    const char *pattern;           /* file name pattern, e.g. "*.TXT" */
    const ods2_parsed_path_t *p;   /* for the version filter */
    int dirs_shown;
    int files_shown;
    int errors;
} dir_walk_t;

/* Walks the tree below the start directory depth first, in directory
 * order (alphabetical, as VMS stores them), listing every directory
 * whose path relative to the start matches w->spec. `abs_path` is the
 * directory's absolute path, `rel_path` the same directory relative to
 * where the walk began ("" for the start itself). Subtrees that cannot
 * contain a match are never read. This one walk serves every DIR:
 * "[A]" (spec "" - just the start), "[A...]" (spec "..."), "[*]",
 * "[*...]", "[A...B]" and so on. */
static void walk_directories(dir_walk_t *w, const char *abs_path, const char *rel_path,
                             int depth)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid;
    ods2_dir_entry_t *entries = NULL;
    int count = 0, i;
    ods2_result_t r;
    bool here = ods2_dir_spec_match(w->spec, rel_path);
    bool deeper = ods2_dir_spec_could_descend(w->spec, rel_path);

    if (!here && !deeper) return;
    if (depth > MAX_RECURSION_DEPTH) {
        report_error("%%ODS2-E-TOODEEP, recursion limit reached, skipping "
                        "subtree under [%s]\n", abs_path);
        w->errors++;
        return;
    }

    r = resolve_dir(w->vol, abs_path, dir_header, &dir_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, [%s]: %s\n", abs_path, r.problem);
        w->errors++;
        return;
    }
    r = ods2_list_directory_alloc(w->vol, dir_header, &entries, &count);
    if (!r.ok) {
        report_error("%%ODS2-E-LISTERR, [%s]: %s\n", abs_path, r.problem);
        w->errors++;
        return;
    }

    if (here) {
        int shown = show_directory(abs_path, entries, count, w->pattern, w->p);
        if (shown > 0) {
            w->dirs_shown++;
            w->files_shown += shown;
        }
    }

    for (i = 0; deeper && i < count; i++) {
        uint8_t sub_header[512];
        char name[ODS2_PATH_MAX];
        char child_abs[ODS2_PATH_MAX];
        char child_rel[ODS2_PATH_MAX];
        size_t len;
        int a, b;

        /* Skip self-referential entries - root's own directory always
           contains a "000000.DIR" entry pointing straight back at
           itself (FID (4,4), present on every ODS-2 volume). This is
           also why "[*]" never matches [000000], as on VMS. */
        if (entries[i].fid.fid_num == dir_fid.fid_num &&
            entries[i].fid.fid_seq == dir_fid.fid_seq) {
            continue;
        }
        /* Only a name's highest version can be the subdirectory a
           path like [A.B] refers to. */
        if (i > 0 && same_name(entries[i].name, entries[i - 1].name)) {
            continue;
        }
        /* Directories are NAME.DIR; the name is the path component. */
        len = strlen(entries[i].name);
        if (len <= 4 || len >= sizeof(name) || !same_name(entries[i].name + len - 4, ".DIR")) {
            continue;
        }
        memcpy(name, entries[i].name, len - 4);
        name[len - 4] = '\0';

        a = snprintf(child_abs, sizeof(child_abs), "%s%s%s", abs_path,
                     abs_path[0] ? "." : "", name);
        b = snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel_path,
                     rel_path[0] ? "." : "", name);
        if (a < 0 || (size_t) a >= sizeof(child_abs) || b < 0 ||
            (size_t) b >= sizeof(child_rel)) {
            report_error("%%ODS2-E-PATHTOOLONG, path too deep to represent, "
                            "skipping subtree under [%s]\n", abs_path);
            w->errors++;
            continue;
        }
        /* Cheap name test before reading the entry's header. */
        if (!ods2_dir_spec_match(w->spec, child_rel) &&
            !ods2_dir_spec_could_descend(w->spec, child_rel)) {
            continue;
        }
        /* Is it really a directory? Checked via its own header's
           FH2$M_DIRECTORY bit (0x2000), not just the .DIR suffix. */
        r = ods2_read_header(w->vol, entries[i].fid.fid_num, sub_header);
        if (!r.ok || !(((ods2_head_core_t *) sub_header)->filechar & 0x2000u)) {
            continue;
        }
        walk_directories(w, child_abs, child_rel, depth + 1);
    }
    free(entries);
}

/* DIR, following VMS DIRECTORY: '*' and '%' may appear in any
 * directory name and the ellipsis anywhere in the directory spec, so
 * one command can list many directories. As on VMS, directories with
 * no matching files are not shown, a "Grand total" follows when more
 * than one directory was listed, and a listing that finds nothing at
 * all reports "no files found". */
static void cmd_dir(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    char effective_dir[ODS2_PATH_MAX];
    char spec[ODS2_PATH_MAX + 4];
    char start[ODS2_PATH_MAX];
    const char *rest;
    char file_pattern[ODS2_PATH_MAX + 4];
    dir_walk_t w;

    resolve_effective_path(p, effective_dir, sizeof(effective_dir));
    /* A trailing ellipsis was split off as `recursive`; put it back,
       so the spec says everything the walk has to match. */
    snprintf(spec, sizeof(spec), "%s%s", effective_dir, p->recursive ? "..." : "");

    /* Start at the longest plain prefix ("A.B" of "A.B.*...") - it is
       looked up directly, so a missing directory there is reported
       as such - and match only what follows it. */
    rest = ods2_dir_spec_split_prefix(spec, start, sizeof(start));
    if (rest == NULL) {
        report_error("%%ODS2-E-BADPATH, [%s]: invalid directory specification\n", spec);
        return;
    }

    memset(&w, 0, sizeof(w));
    w.vol = vol;
    w.spec = rest;
    /* As in VMS DIRECTORY, a name with no type means any type:
       "MAKEFILE" is "MAKEFILE.*", "*" is "*.*". (Names are stored with
       their dot, "MAKEFILE.", so the bare pattern would match nothing.) */
    if (p->filename[0] == '\0') {
        snprintf(file_pattern, sizeof(file_pattern), "*.*");
    } else if (strchr(p->filename, '.') == NULL) {
        snprintf(file_pattern, sizeof(file_pattern), "%s.*", p->filename);
    } else {
        snprintf(file_pattern, sizeof(file_pattern), "%s", p->filename);
    }
    w.pattern = file_pattern;
    w.p = p;
    walk_directories(&w, start, "", 0);

    if (w.dirs_shown > 1) {
        printf("\nGrand total of %d director%s, %d file%s.\n",
               w.dirs_shown, (w.dirs_shown == 1) ? "y" : "ies",
               w.files_shown, (w.files_shown == 1) ? "" : "s");
    } else if (w.dirs_shown == 0 && w.errors == 0) {
        fprintf(stderr, "%%ODS2-W-NOFILES, no files found\n");
    }
}

/* SET FILE/VERSION_LIMIT=n <spec>: the version limit of the directory entries the spec names (wildcards and [dir...]
   work as in DIRECTORY). ods2v2 wrote 1 for the entries until it was fixed; VMS then purges version 1 when it makes
   version 2 of such a file (COPY, EDIT). 32767 is "no limit", what VMS writes. */
typedef struct {
    uint16_t limit;
    unsigned entries, records;     /* names looked at, directory records changed */
    char last_dir[ODS2_PATH_MAX];
    char last_name[ODS2_DIR_MAX_NAME + 8];
    bool have_last;
} verlimit_ctx_t;

static void verlimit_entry(void *ctx_, ods2_volume_t *vol, const char *dir_spelling, const ods2_dir_entry_t *e)
{
    verlimit_ctx_t *x = (verlimit_ctx_t *) ctx_;
    ods2_fid_t dfid;
    unsigned changed = 0;
    ods2_result_t r;

    /* the reserved files and the master directory's own entry keep the limit VMS gave them (1) */
    if (e->fid.fid_num >= 1 && e->fid.fid_num <= 10 && e->fid.fid_nmx == 0) return;
    /* one call per name (the walk calls the hook once per version) */
    if (x->have_last && strcmp(x->last_dir, dir_spelling) == 0 && same_name(x->last_name, e->name)) return;
    snprintf(x->last_dir, sizeof(x->last_dir), "%s", dir_spelling);
    snprintf(x->last_name, sizeof(x->last_name), "%s", e->name);
    x->have_last = true;
    x->entries++;
    r = ods2_lookup_path(vol, dir_spelling, &dfid);
    if (r.ok) r = ods2_set_entry_version_limit(vol, dfid.fid_num, e->name, x->limit, &changed);
    if (!r.ok) {
        report_error("%%ODS2-E-VERLIMIT, [%s]%s: %s\n", dir_spelling, e->name, r.problem);
        return;
    }
    x->records += changed;
}

static void cmd_set_version_limit(ods2_volume_t *vol, const ods2_parsed_path_t *p, uint16_t limit)
{
    verlimit_ctx_t x;
    char effective[ODS2_PATH_MAX], spec[ODS2_PATH_MAX + 4], start[ODS2_PATH_MAX], pattern[ODS2_PATH_MAX + 4];
    const char *rest;
    dir_walk_t w;

    memset(&x, 0, sizeof(x));
    x.limit = limit;
    resolve_effective_path(p, effective, sizeof(effective));
    snprintf(spec, sizeof(spec), "%s%s", effective, p->recursive ? "..." : "");
    rest = ods2_dir_spec_split_prefix(spec, start, sizeof(start));
    if (rest == NULL) {
        report_error("%%ODS2-E-BADPATH, [%s]: invalid directory specification\n", spec);
        return;
    }
    if (p->filename[0] == '\0') snprintf(pattern, sizeof(pattern), "*.*");
    else if (strchr(p->filename, '.') == NULL) snprintf(pattern, sizeof(pattern), "%s.*", p->filename);
    else snprintf(pattern, sizeof(pattern), "%s", p->filename);
    memset(&w, 0, sizeof(w));
    w.vol = vol;
    w.spec = rest;
    w.pattern = pattern;
    w.p = p;
    g_entry_hook = verlimit_entry;
    g_entry_hook_ctx = &x;
    g_entry_hook_vol = vol;
    walk_directories(&w, start, "", 0);
    g_entry_hook = NULL;
    g_entry_hook_ctx = NULL;
    if (x.entries == 0 && w.errors == 0) {
        fprintf(stderr, "%%ODS2-W-NOFILES, no files found\n");
    } else {
        printf("%%ODS2-I-VERLIMIT, %u name%s looked at, %u directory record%s changed to version limit %u\n",
               x.entries, x.entries == 1 ? "" : "s", x.records, x.records == 1 ? "" : "s", (unsigned) limit);
    }
}

static void cmd_create_directory(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    /* CREATE/DIRECTORY [PARENT.NEWNAME] - the new directory's own
       name is the LAST component of the (effective, SET-DEFAULT-
       resolved) bracketed path, its parent is everything before
       that. */
    char effective_path[ODS2_PATH_MAX];
    char dir_copy[ODS2_PATH_MAX];
    const char *parent_path;
    const char *new_name;
    uint8_t parent_header[512];
    ods2_fid_t parent_fid, new_fid;
    ods2_result_t r;

    resolve_effective_path(p, effective_path, sizeof(effective_path));
    if (effective_path[0] == '\0') {
        report_error("%%ODS2-E-BADPATH, no directory name given\n");
        return;
    }
    split_last_component(effective_path, dir_copy, &parent_path, &new_name);

    r = resolve_dir(vol, parent_path, parent_header, &parent_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    r = ods2_create_directory(vol, parent_header, new_name, &new_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-CREATEERR, %s\n", r.problem);
        return;
    }
    printf("%%ODS2-I-CREATED, created [%s]\n", effective_path);
}

/* A host file opened for COPY, read in order a chunk at a time
   rather than all at once (issue #16). `length` is known before
   anything is allocated on the volume: from the file itself if it is
   a regular file, otherwise (a pipe, /dev/stdin) by first copying it
   to a temporary file, which keeps memory use flat either way. */
typedef struct {
    FILE *f;
    const char *path;
    size_t length;
    char problem[256];
} host_input_t;

static bool host_input_read(void *ctx_, uint8_t *buf, size_t n)
{
    host_input_t *in = (host_input_t *) ctx_;
    size_t got = fread(buf, 1, n, in->f);
    if (got == n) return true;
    if (ferror(in->f)) {
        snprintf(in->problem, sizeof(in->problem), "error reading %s", in->path);
    } else {
        snprintf(in->problem, sizeof(in->problem),
                 "%s became shorter while being copied (expected %zu bytes)",
                 in->path, in->length);
    }
    return false;
}

/* Opens `path` for COPY, finding its length. Returns false and prints
   why on failure. */
static bool host_input_open(const char *path, host_input_t *in)
{
    struct stat st;
    FILE *f = fopen(path, "rb");

    memset(in, 0, sizeof(*in));
    in->path = path;
    if (f == NULL) {
        report_error("%%ODS2-E-OPENIN, could not open %s\n", path);
        return false;
    }
    if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode)) {
        in->f = f;
        in->length = (size_t) st.st_size;
        return true;
    }
    /* No length to go by: spool it to a temporary file first. */
    {
        static uint8_t buf[1 << 16];
        FILE *spool = tmpfile();
        size_t n, total = 0;

        if (spool == NULL) {
            report_error("%%ODS2-E-OPENIN, could not create a temporary file to "
                         "hold %s: %s\n", path, strerror(errno));
            fclose(f);
            return false;
        }
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (fwrite(buf, 1, n, spool) != n) {
                report_error("%%ODS2-E-WRITEERR, could not write the temporary copy of "
                             "%s (is the temporary directory full?)\n", path);
                fclose(f);
                fclose(spool);
                return false;
            }
            total += n;
        }
        if (ferror(f)) {
            report_error("%%ODS2-E-READERR, error reading %s\n", path);
            fclose(f);
            fclose(spool);
            return false;
        }
        fclose(f);
        if (fflush(spool) != 0 || fseek(spool, 0, SEEK_SET) != 0) {
            report_error("%%ODS2-E-READERR, could not reread the temporary copy of %s\n",
                         path);
            fclose(spool);
            return false;
        }
        in->f = spool;
        in->length = total;
        return true;
    }
}

/* Case-insensitive: is `word` (length `len`) `name`, or an
   abbreviation of it at least `min` characters long, as VMS allows? */
static bool keyword(const char *word, size_t len, const char *name, size_t min)
{
    size_t i;
    if (len < min || len > strlen(name)) return false;
    for (i = 0; i < len; i++) {
        if (toupper((unsigned char) word[i]) != toupper((unsigned char) name[i])) return false;
    }
    return true;
}

/* COPY's options beyond the record attributes themselves. */
typedef struct {
    bool convert;   /* /CONVERT: host text lines -> VAR records */
    bool mrs_given; /* MRS:n was given */
} copy_options_t;

/* Parses COPY's qualifiers - everything after "COPY" in the command
 * word - into the record attributes of the new file. /ATTRIBUTES is in
 * the syntax of VMS's SET FILE/ATTRIBUTES:
 *
 *   COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) kit.pcsi [000000]KIT.PCSI
 *
 *   RFM:fmt   record format - FIX, STMLF, STM, STMCR or UDF, or VAR
 *             with /CONVERT
 *   MRS:n     maximum record size; for FIX, the record size
 *   LRL:n     longest record length (for FIX, the same as MRS)
 *   RAT:x     record attributes - NONE, CR, FTN, PRN, BLK, NOBLK;
 *             repeat RAT for more than one (RAT:CR,RAT:BLK)
 *
 * /CONVERT (issue #15) turns a host text file into variable-length
 * records, one per line (see ods2_text_var.h) - VMS's native text
 * format, as VMS FTP writes it in ASCII mode:
 *
 *   COPY/CONVERT notes.txt [DOC]NOTES.TXT
 *   COPY/ATTRIBUTES=(RFM:VAR,RAT:CR)/CONVERT notes.txt [DOC]NOTES.TXT
 *
 * With /CONVERT the format is VAR and, unless a RAT was given, RAT:CR;
 * the longest record length comes from the file, and MRS is 0 (no
 * limit) unless given.
 *
 * With no qualifier the file is Stream_LF, as always. A fixed format
 * with no size given is 512-byte records. Returns false after
 * reporting the problem. */
static bool parse_copy_qualifiers(const char *quals, ods2_record_attrs_t *a,
                                  copy_options_t *opts)
{
    bool mrs_given = false, lrl_given = false, rfm_given = false, rat_given = false;
    const char *q = quals;

    while (*q == '/') {
        const char *name = ++q;
        const char *value, *end;
        size_t name_len;

        while (*q && *q != '=' && *q != '/') q++;
        name_len = (size_t) (q - name);
        if (keyword(name, name_len, "CONVERT", 4)) {
            if (*q == '=') {
                report_error("%%ODS2-E-NOVALUE, /CONVERT takes no value\n");
                return false;
            }
            opts->convert = true;
            continue;
        }
        if (!keyword(name, name_len, "ATTRIBUTES", 4)) {
            report_error("%%ODS2-E-IVQUAL, unrecognized qualifier /%.*s - COPY takes "
                         "/ATTRIBUTES=(RFM:fmt,MRS:size,RAT:attr) and /CONVERT\n",
                         (int) name_len, name);
            return false;
        }
        if (*q != '=') {
            report_error("%%ODS2-E-VALREQ, /ATTRIBUTES needs a value, e.g. "
                         "/ATTRIBUTES=(RFM:FIX,MRS:8192)\n");
            return false;
        }
        value = ++q;
        if (*value == '(') {
            end = strchr(value, ')');
            if (end == NULL) {
                report_error("%%ODS2-E-BADVALUE, /ATTRIBUTES: missing ')'\n");
                return false;
            }
            value++;
            q = end + 1;
        } else {
            end = value;
            while (*end && *end != '/') end++;
            q = end;
        }

        /* comma-separated KEY:VALUE items between value and end */
        while (value < end) {
            const char *item = value, *colon, *item_end = value;
            size_t key_len, val_len;
            while (item_end < end && *item_end != ',') item_end++;
            value = (item_end < end) ? item_end + 1 : end;
            colon = memchr(item, ':', (size_t) (item_end - item));
            if (colon == NULL) colon = memchr(item, '=', (size_t) (item_end - item));
            if (colon == NULL) {
                report_error("%%ODS2-E-BADVALUE, /ATTRIBUTES: \"%.*s\" is not KEY:VALUE\n",
                             (int) (item_end - item), item);
                return false;
            }
            key_len = (size_t) (colon - item);
            val_len = (size_t) (item_end - colon - 1);

            if (keyword(item, key_len, "RFM", 3)) {
                const char *v = colon + 1;
                rfm_given = true;
                if (keyword(v, val_len, "FIXED", 3)) a->rtype = ODS2_RFM_FIX;
                else if (keyword(v, val_len, "STMLF", 5) || keyword(v, val_len, "STREAM_LF", 8))
                    a->rtype = ODS2_RFM_STMLF;
                else if (keyword(v, val_len, "STMCR", 5) || keyword(v, val_len, "STREAM_CR", 8))
                    a->rtype = ODS2_RFM_STMCR;
                else if (keyword(v, val_len, "STM", 3) || keyword(v, val_len, "STREAM", 6))
                    a->rtype = ODS2_RFM_STM;
                else if (keyword(v, val_len, "UDF", 3) || keyword(v, val_len, "UNDEFINED", 3))
                    a->rtype = ODS2_RFM_UDF;
                else if (keyword(v, val_len, "VARIABLE", 3)) a->rtype = ODS2_RFM_VAR;
                else if (keyword(v, val_len, "VFC", 3)) a->rtype = ODS2_RFM_VFC;
                else {
                    report_error("%%ODS2-E-BADVALUE, RFM:%.*s - record format must be FIX, "
                                 "STMLF, STM, STMCR or UDF, or VAR with /CONVERT\n",
                                 (int) val_len, v);
                    return false;
                }
            } else if (keyword(item, key_len, "MRS", 3) || keyword(item, key_len, "LRL", 3)) {
                char digits[16];
                char *stop;
                unsigned long n;
                if (val_len == 0 || val_len >= sizeof(digits)) {
                    n = 0;
                    stop = digits;
                    digits[0] = 'x';
                } else {
                    memcpy(digits, colon + 1, val_len);
                    digits[val_len] = '\0';
                    n = strtoul(digits, &stop, 10);
                }
                if (*stop != '\0' || digits[0] == '-' || n > 65535) {
                    report_error("%%ODS2-E-BADVALUE, %.*s needs a record size in bytes\n",
                                 (int) (item_end - item), item);
                    return false;
                }
                if (toupper((unsigned char) item[0]) == 'M') {
                    a->maxrec = (uint16_t) n;
                    mrs_given = true;
                } else {
                    a->rsize = (uint16_t) n;
                    lrl_given = true;
                }
            } else if (keyword(item, key_len, "RAT", 3)) {
                const char *v = colon + 1;
                rat_given = true;
                if (keyword(v, val_len, "NONE", 4)) a->rattrib &= (uint8_t) ~0x07u;
                else if (keyword(v, val_len, "CR", 2)) a->rattrib |= ODS2_RAT_CR;
                else if (keyword(v, val_len, "FTN", 3)) a->rattrib |= ODS2_RAT_FTN;
                else if (keyword(v, val_len, "PRN", 3)) a->rattrib |= ODS2_RAT_PRN;
                else if (keyword(v, val_len, "BLK", 3)) a->rattrib |= ODS2_RAT_BLK;
                else if (keyword(v, val_len, "NOBLK", 5)) a->rattrib &= (uint8_t) ~ODS2_RAT_BLK;
                else {
                    report_error("%%ODS2-E-BADVALUE, RAT:%.*s - record attributes are NONE, "
                                 "CR, FTN, PRN, BLK or NOBLK\n", (int) val_len, v);
                    return false;
                }
            } else {
                report_error("%%ODS2-E-BADVALUE, /ATTRIBUTES: unknown keyword \"%.*s\" - "
                             "use RFM, MRS, LRL or RAT\n", (int) key_len, item);
                return false;
            }
        }
    }
    if (*q != '\0') {
        report_error("%%ODS2-E-IVQUAL, could not parse qualifiers \"%s\"\n", quals);
        return false;
    }

    opts->mrs_given = mrs_given;
    if (opts->convert) {
        if (!rfm_given) a->rtype = ODS2_RFM_VAR;
        if (a->rtype != ODS2_RFM_VAR) {
            report_error("%%ODS2-E-CONFQUAL, /CONVERT makes variable-length records: "
                         "use it with RFM:VAR, or no RFM at all\n");
            return false;
        }
        if (!rat_given) a->rattrib = ODS2_RAT_CR;
        if (lrl_given) {
            report_error("%%ODS2-E-CONFQUAL, with /CONVERT the longest record length "
                         "(LRL) comes from the file - leave LRL out\n");
            return false;
        }
    } else if (a->rtype == ODS2_RFM_VAR) {
        report_error("%%ODS2-E-CONFQUAL, RFM:VAR needs /CONVERT: each variable-length "
                     "record starts with a byte count the host file doesn't have, so "
                     "COPY/CONVERT makes one record per line\n");
        return false;
    }

    /* A fixed record's size is both RSIZE and MRS: either one sets
       both, as VMS keeps them equal; neither means 512. */
    if (a->rtype == ODS2_RFM_FIX) {
        if (mrs_given && !lrl_given) a->rsize = a->maxrec;
        else if (lrl_given && !mrs_given) a->maxrec = a->rsize;
        else if (!mrs_given && !lrl_given) a->rsize = a->maxrec = 512;
    }
    return true;
}

/* /CONVERT's second pass: the encoder reads the host file through
   this, and its problem is passed on to the content source. */
typedef struct {
    host_input_t *in;
    ods2_text_var_encoder_t *enc;
    ods2_content_source_t *src;
} convert_ctx_t;

static size_t convert_read_some(void *ctx_, uint8_t *buf, size_t n, bool *error_out)
{
    host_input_t *in = (host_input_t *) ctx_;
    size_t got = fread(buf, 1, n, in->f);
    if (got == 0 && ferror(in->f)) *error_out = true;
    return got;
}

static bool convert_read(void *ctx_, uint8_t *buf, size_t n)
{
    convert_ctx_t *c = (convert_ctx_t *) ctx_;
    if (ods2_text_var_read(c->enc, buf, n)) return true;
    snprintf(c->in->problem, sizeof(c->in->problem), "%s: %s", c->in->path,
             ods2_text_var_problem(c->enc));
    c->src->problem = c->in->problem;
    return false;
}

/* /CONVERT's first pass: reads the whole host file through the
   scanner, then rewinds it. Returns false after reporting a problem
   (a read error, or a line too long to be a record). */
static bool convert_scan(host_input_t *in, ods2_text_var_scan_t *scan)
{
    static uint8_t buf[1 << 16];
    size_t n;

    ods2_text_var_scan_init(scan);
    while ((n = fread(buf, 1, sizeof(buf), in->f)) > 0) {
        ods2_text_var_scan_feed(scan, buf, n);
    }
    if (ferror(in->f)) {
        report_error("%%ODS2-E-READERR, error reading %s\n", in->path);
        return false;
    }
    ods2_text_var_scan_finish(scan);
    if (scan->too_long_line != 0) {
        report_error("%%ODS2-E-RECTOOBIG, %s: line %zu is longer than a variable-length "
                     "record can be (%u bytes)\n", in->path, scan->too_long_line,
                     ODS2_VAR_MAX_RECORD);
        return false;
    }
    if (fseek(in->f, 0, SEEK_SET) != 0) {
        report_error("%%ODS2-E-READERR, could not reread %s\n", in->path);
        return false;
    }
    return true;
}

static void cmd_copy(ods2_volume_t *vol, const char *local_file, const ods2_parsed_path_t *p,
                     const ods2_record_attrs_t *attrs_in, const copy_options_t *opts)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid, new_fid;
    host_input_t in;
    ods2_content_source_t src;
    ods2_record_attrs_t attrs = *attrs_in;
    ods2_text_var_scan_t scan;
    convert_ctx_t conv;
    ods2_result_t r;
    const char *name;

    if (p->filename[0] == '\0') {
        report_error("%%ODS2-E-BADPATH, no destination filename given\n");
        return;
    }
    /* A new file is always created as version 1 here. Accept the
       spellings that mean that on a new file ("NAME", "NAME;",
       "NAME;0", "NAME;1") and refuse the rest, rather than ever
       storing ";version" as part of the name. */
    if (p->has_version &&
        (p->version_wildcard || p->version < 0 || p->version > 1)) {
        char spec[ODS2_PATH_MAX + 16];
        format_file_spec(p, spec, sizeof(spec));
        report_error("%%ODS2-E-BADVER, %s: COPY creates version 1 only - "
                        "give NAME, NAME; or NAME;1\n", spec);
        return;
    }
    name = p->filename;

    if (!host_input_open(local_file, &in)) {
        return;
    }
    memset(&src, 0, sizeof(src));
    memset(&conv, 0, sizeof(conv));
    if (opts->convert) {
        if (!convert_scan(&in, &scan)) {
            fclose(in.f);
            return;
        }
        attrs.rsize = (uint16_t) scan.longest;
        if (!opts->mrs_given) attrs.maxrec = 0;
        conv.in = &in;
        conv.src = &src;
        conv.enc = ods2_text_var_encoder_new(convert_read_some, &in);
        if (conv.enc == NULL) {
            report_error("%%ODS2-E-NOMEM, could not allocate the record converter\n");
            fclose(in.f);
            return;
        }
    }

    {
        char effective_dir[ODS2_PATH_MAX];
        resolve_effective_path(p, effective_dir, sizeof(effective_dir));
        r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    }
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        ods2_text_var_encoder_free(conv.enc);
        fclose(in.f);
        return;
    }
    if (opts->convert) {
        src.length = scan.converted_length;
        src.read = convert_read;
        src.ctx = &conv;
        src.var_records = true;
    } else {
        src.length = in.length;
        src.read = host_input_read;
        src.ctx = &in;
        src.problem = in.problem;
    }
    r = ods2_create_file_source(vol, dir_header, name, &src, &attrs, &new_fid);
    if (r.ok) {
        /* More content than was measured means the host file grew
           while it was copied: what's on the volume is its earlier
           length, and with /CONVERT may end part way through a record. */
        uint8_t extra;
        bool grew = opts->convert ? ods2_text_var_read(conv.enc, &extra, 1)
                                  : (fgetc(in.f) != EOF);
        if (grew && opts->convert) {
            report_error("%%ODS2-E-CHANGED, %s changed while being converted: its last "
                         "record may be cut short - delete it and copy again\n",
                         local_file);
        } else if (grew) {
            fprintf(stderr, "%%ODS2-W-CHANGED, %s grew while being copied; only its "
                    "first %zu bytes were written - copy it again\n",
                    local_file, in.length);
        }
    }
    ods2_text_var_encoder_free(conv.enc);
    fclose(in.f);
    if (!r.ok) {
        report_error("%%ODS2-E-COPYERR, %s\n", r.problem);
        return;
    }
    {
        /* Report the name as stored: uppercase, with its dot. */
        char stored[ODS2_PATH_MAX];
        const char *why;
        if (!ods2_make_file_name(name, stored, sizeof(stored), &why)) {
            snprintf(stored, sizeof(stored), "%s", name);
        }
        if (opts->convert) {
            printf("%%ODS2-I-COPIED, %zu bytes to %s;1 as %zu variable-length "
                   "record%s (longest %zu)\n", in.length, stored, scan.records,
                   scan.records == 1 ? "" : "s", scan.longest);
        } else {
            printf("%%ODS2-I-COPIED, %zu bytes to %s;1\n", in.length, stored);
        }
    }
}

/* "VOLUME", abbreviated to at least "VOL", with any qualifiers after
   it ("VOLUME/LABEL=X"). */
static bool is_volume_word(const char *token)
{
    const char *slash = strchr(token, '/');
    size_t len = slash ? (size_t) (slash - token) : strlen(token);
    return keyword(token, len, "VOLUME", 3);
}

/* A 12-byte, space-padded name field, without its padding. */
static void print_name(const char *what, const char *field)
{
    int len = 12;
    while (len > 0 && (field[len - 1] == ' ' || field[len - 1] == '\0')) len--;
    printf("  %-20s%.*s\n", what, len, field);
}

/* SET VOLUME/LABEL=name (issue #13), as on VMS; the qualifier may also
   come as a separate word ("SET VOLUME /LABEL=name"). */
static void cmd_set_volume(ods2_volume_t *vol, char **tokens, int n)
{
    const char *label = NULL;
    int i;

    for (i = 1; i < n; i++) {
        const char *q = (i == 1) ? strchr(tokens[1], '/') : tokens[i];
        while (q != NULL && *q == '/') {
            const char *name = ++q, *value = NULL;
            size_t name_len;
            while (*q && *q != '=' && *q != '/') q++;
            name_len = (size_t) (q - name);
            if (*q == '=') {
                value = ++q;
                while (*q && *q != '/') q++;
            }
            if (!keyword(name, name_len, "LABEL", 3)) {
                report_error("%%ODS2-E-IVQUAL, unrecognized qualifier /%.*s - SET VOLUME "
                             "takes /LABEL=name\n", (int) name_len, name);
                return;
            }
            if (value == NULL || value == q) {
                report_error("%%ODS2-E-VALREQ, /LABEL needs a value, e.g. "
                             "SET VOLUME/LABEL=ITANKITS\n");
                return;
            }
            {
                static char buf[64];
                size_t len = (size_t) (q - value);
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                memcpy(buf, value, len);
                buf[len] = '\0';
                label = buf;
            }
        }
        if (i > 1 && (tokens[i][0] != '/')) {
            report_error("%%ODS2-E-MAXPARM, SET VOLUME takes no parameters here - the "
                         "volume is the disk image (unexpected \"%s\")\n", tokens[i]);
            return;
        }
    }
    if (label == NULL) {
        report_error("%%ODS2-E-NOPARM, usage: SET VOLUME/LABEL=name\n");
        return;
    }
    {
        unsigned copies = 0;
        char old[13];
        ods2_result_t r;
        memcpy(old, vol->home.volname, 12);
        old[12] = '\0';
        r = ods2_set_volume_label(vol, label, &copies);
        if (!r.ok) {
            report_error("%%ODS2-E-SETVOL, %s\n", r.problem);
            return;
        }
        {
            int len = 12;
            while (len > 0 && old[len - 1] == ' ') len--;
            old[len] = '\0';
        }
        {
            int len = 12;
            while (len > 0 && vol->home.volname[len - 1] == ' ') len--;
            printf("%%ODS2-I-RELABELED, volume label changed from %s to %.*s "
                   "(%u home block copies)\n", old, len, vol->home.volname, copies);
        }
    }
}

/* SHOW VOLUME (issue #13): the home block's identification, and space. */
static void cmd_show_volume(ods2_volume_t *vol)
{
    const ods2_home_t *h = &vol->home;
    uint64_t total = 0, free_blocks = 0;
    ods2_result_t r = ods2_volume_space(vol, &total, &free_blocks);
    bool in_set = false;
    int i;

    for (i = 0; i < 12; i++) {
        if (h->strucname[i] != ' ' && h->strucname[i] != '\0') in_set = true;
    }
    print_name("Volume label:", h->volname);
    print_name("Owner name:", h->ownername);
    print_name("Format:", h->format);
    printf("  %-20sODS-%u, version %u\n", "Structure level:", h->struclev >> 8,
           h->struclev & 0xffu);
    printf("  %-20s%u\n", "Cluster size:", h->cluster);
    printf("  %-20s%u\n", "Maximum files:", (unsigned) h->maxfiles);
    printf("  %-20s[%o,%o]\n", "Volume owner UIC:", h->volowner_uic[1], h->volowner_uic[0]);
    if (in_set || h->rvn != 0) {
        int len = 12;
        while (len > 0 && (h->strucname[len - 1] == ' ' || h->strucname[len - 1] == '\0')) len--;
        printf("  %-20s%.*s, volume %u of %u\n", "Volume set:", len, h->strucname,
               h->rvn, h->setcount);
    } else {
        printf("  %-20snone\n", "Volume set:");
    }
    if (r.ok) {
        printf("  %-20s%llu\n", "Total blocks:", (unsigned long long) total);
        printf("  %-20s%llu\n", "Free blocks:", (unsigned long long) free_blocks);
    } else {
        report_error("%%ODS2-E-READERR, could not count free space: %s\n", r.problem);
    }
}

static bool stdout_sink(void *ctx, const uint8_t *data, size_t n)
{
    (void) ctx;
    return fwrite(data, 1, n, stdout) == n;
}

static void cmd_type(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    uint8_t dir_header[512], file_header[512];
    ods2_fid_t dir_fid, file_fid;
    ods2_result_t r;
    char spec[ODS2_PATH_MAX + 16];

    if (p->filename[0] == '\0') {
        report_error("%%ODS2-E-BADPATH, no filename given\n");
        return;
    }
    format_file_spec(p, spec, sizeof(spec));
    if (p->version_wildcard) {
        report_error("%%ODS2-E-BADVER, %s: TYPE takes one version, not ;*\n", spec);
        return;
    }
    {
        char effective_dir[ODS2_PATH_MAX];
        resolve_effective_path(p, effective_dir, sizeof(effective_dir));
        r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    }
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    /* p->version is 0 (highest) when no version was given. */
    r = ods2_lookup_name_version(vol, dir_header, p->filename, p->version, &file_fid, NULL);
    if (!r.ok) {
        report_error("%%ODS2-E-FNF, %s not found\n", spec);
        return;
    }
    r = ods2_read_header(vol, file_fid.fid_num, file_header);
    if (!r.ok) {
        report_error("%%ODS2-E-READERR, %s\n", r.problem);
        return;
    }
    /* Streamed a chunk at a time, so any size of file can be typed
       without holding it in memory (issue #16). A short write (a full
       disk, a closed pipe) is a failed TYPE: whatever was extracting
       the file got less than all of it. */
    {
        bool sink_failed = false;
        size_t bytes = 0;
        r = ods2_read_file_stream(vol, file_header, stdout_sink, NULL, &bytes, &sink_failed);
        if (r.ok && fflush(stdout) != 0) sink_failed = true;
        if (sink_failed) {
            report_error("%%ODS2-E-WRITEERR, %s: could not write all %zu bytes to output\n",
                         spec, ods2_file_content_length(file_header));
        } else if (!r.ok) {
            report_error("%%ODS2-E-READERR, %s: %s\n", spec, r.problem);
        }
    }
}

static void cmd_delete(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid;
    ods2_result_t r;
    const char *dir_path;
    const char *name;
    char effective_path[ODS2_PATH_MAX];
    char split_buf[ODS2_PATH_MAX];
    char name_buf[ODS2_PATH_MAX];

    int version = p->version; /* 0 (highest) when none was given */
    uint16_t deleted_version = 0;
    int deleted = 0;

    resolve_effective_path(p, effective_path, sizeof(effective_path));

    if (p->filename[0] != '\0') {
        /* Normal case: "[SOMEDIR]FILE.TXT" - delete FILE.TXT from
           SOMEDIR directly. */
        dir_path = effective_path;
        name = p->filename;
    } else {
        /* Bare directory reference like "[TESTDIR]" - the directory
           itself is the target, one level up from where
           ods2_lookup_path would otherwise resolve it. Append .DIR,
           matching how it's actually stored as a directory entry. */
        const char *parent_path;
        const char *last_name;
        if (effective_path[0] == '\0') {
            report_error("%%ODS2-E-BADPATH, cannot delete the root directory\n");
            return;
        }
        split_last_component(effective_path, split_buf, &parent_path, &last_name);
        snprintf(name_buf, sizeof(name_buf), "%s.DIR", last_name);
        dir_path = parent_path;
        name = name_buf;
    }

    r = resolve_dir(vol, dir_path, dir_header, &dir_fid);
    if (!r.ok) {
        report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }

    /* One version (the one selected, the highest by default), or with
       ";*" every version - highest first, until none remain. */
    do {
        r = ods2_delete_version(vol, dir_fid.fid_num, name,
                                p->version_wildcard ? ODS2_VERSION_HIGHEST : version,
                                &deleted_version);
        if (!r.ok) break;
        printf("%%ODS2-I-DELETED, deleted %s;%u\n", name, deleted_version);
        deleted++;
    } while (p->version_wildcard);

    if (!r.ok && !(p->version_wildcard && deleted > 0)) {
        char spec[ODS2_PATH_MAX + 16];
        if (p->filename[0] != '\0') {
            format_file_spec(p, spec, sizeof(spec));
        } else {
            snprintf(spec, sizeof(spec), "%s", name);
        }
        report_error("%%ODS2-E-DELETEERR, %s: %s\n", spec, r.problem);
    }
}

static void print_help(void)
{
    printf("Commands (VMS bracket-notation paths, e.g. [DECUS.NETLIB020]FILE.TXT):\n"
           "  DIR [path] [wildcard]      list a directory\n"
           "  DIR [path...] [wildcard]   list a directory and everything beneath it\n"
           "  DIR [*...] [wildcard]      '*' '%%' in directory names, '...' anywhere\n"
           "                             ([*], [*.SRC], [...SALES], [DECUS...SRC])\n"
           "  CREATE/DIRECTORY path      create a directory\n"
           "  COPY <local-file> <path>   write a local file onto the disk (Stream_LF)\n"
           "  COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) <local-file> <path>\n"
           "                             ... with chosen record attributes: RFM: FIX,\n"
           "                             STMLF, STM, STMCR or UDF; MRS: record size;\n"
           "                             RAT: NONE, CR, FTN, PRN, BLK (one per RAT:)\n"
           "  COPY/CONVERT <local-file> <path>\n"
           "                             a text file as variable-length records with\n"
           "                             CR carriage control (VMS's native text), one\n"
           "                             per line; with /ATTRIBUTES for other RAT/MRS\n"
           "  TYPE path                  print a file's content\n"
           "  DELETE path                delete a file (one version) or empty directory\n"
           "  SET DEFAULT path           set the current default directory\n"
           "  SHOW DEFAULT               show the current default directory\n"
           "  SET VOLUME/LABEL=name      relabel the volume (all home block copies)\n"
           "  SET FILE/VERSION_LIMIT=n spec   the version limit of the entries a spec names (32767 or 0 = no limit,\n"
           "                             what VMS writes; volumes written before carry 1, and VMS then purges\n"
           "                             version 1 when it makes version 2)\n"
           "  SHOW VOLUME                label, owner, cluster size, files, free space\n"
           "  HELP                       show this command list\n"
           "  EXIT / QUIT                leave the interactive shell\n"
           "\n"
           "Paths starting with '.' or '-' inside brackets (e.g. [.NETLIB020],\n"
           "[-], [-.SIBLING]) and bare filenames with no brackets at all are\n"
           "relative to the current default directory (see SET DEFAULT); '-'\n"
           "means \"go up one level\" (repeat as \"-.-\" for more); other bracketed\n"
           "paths are always absolute from root, matching real VMS. [...] is the\n"
           "default directory and below; [*...] every directory on the disk.\n"
           "\n"
           "Versions: NAME;3 is version 3; NAME, NAME; or NAME;0 the highest;\n"
           "NAME;-1 the next lower existing version; NAME;* every version\n"
           "(DIR and DELETE). DIR lists every version unless one is given.\n");
}

/* Splits `line` into up to MAX_TOKENS whitespace-separated tokens,
   modifying `line` in place (inserting nuls) - tokens[] point into it. */
static int tokenize(char *line, char *tokens[MAX_TOKENS])
{
    int count = 0;
    char *p = line;
    while (*p && count < MAX_TOKENS) {
        while (*p && isspace((unsigned char) *p)) p++;
        if (!*p) break;
        tokens[count++] = p;
        while (*p && !isspace((unsigned char) *p)) p++;
        if (*p) *p++ = '\0';
    }
    return count;
}

/* Executes one command line against the mounted volume. Returns
   false if the command was EXIT/QUIT (caller should stop looping). */
static bool execute_line(ods2_volume_t *vol, char *line)
{
    char *tokens[MAX_TOKENS];
    char verb[32];          /* the command word without qualifiers */
    const char *quals;      /* its qualifiers ("/ATTRIBUTES=..."), or NULL */
    size_t verb_len;
    int n = tokenize(line, tokens);
    if (n == 0) return true; /* blank line */

    /* COPY takes qualifiers on the command word itself
       (COPY/ATTRIBUTES=...) - not on its arguments, since a host path
       contains '/' too. CREATE/DIRECTORY is matched whole, below. */
    quals = strchr(tokens[0], '/');
    verb_len = quals ? (size_t) (quals - tokens[0]) : strlen(tokens[0]);
    if (verb_len >= sizeof(verb)) verb_len = 0; /* too long to be any command */
    memcpy(verb, tokens[0], verb_len);
    verb[verb_len] = '\0';

    if (matches(tokens[0], "EXIT") || matches(tokens[0], "QUIT")) {
        return false;
    }
    if (matches(tokens[0], "HELP") || matches(tokens[0], "?")) {
        print_help();
        return true;
    }
    if (matches(tokens[0], "SET") && n > 1 && strncasecmp(tokens[1], "FILE/VERSION_LIMIT=", 19) == 0) {
        /* SET FILE/VERSION_LIMIT=n <spec> */
        ods2_parsed_path_t p;
        char *end;
        long limit = strtol(tokens[1] + 19, &end, 10);
        if (n < 3 || *end != '\0' || limit < 0 || limit > 32767) {
            report_error("%%ODS2-E-NOPARM, usage: SET FILE/VERSION_LIMIT=n <spec>   (n 0 to 32767; 0 is no limit)\n");
            return true;
        }
        if (!ods2_parse_path(tokens[2], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
            return true;
        }
        cmd_set_version_limit(vol, &p, (uint16_t) (limit == 0 ? 32767 : limit));
        return true;
    }
    if (matches(tokens[0], "SET") && n > 1 && is_volume_word(tokens[1])) {
        cmd_set_volume(vol, tokens, n);
        return true;
    }
    if (matches(tokens[0], "SHOW") && n > 1 && is_volume_word(tokens[1])) {
        cmd_show_volume(vol);
        return true;
    }
    if (matches(tokens[0], "SET") && n > 1 &&
        (matches(tokens[1], "DEFAULT") || matches(tokens[1], "DEF"))) {
        ods2_parsed_path_t p;
        char effective[ODS2_PATH_MAX];
        uint8_t header[512];
        ods2_fid_t fid;
        ods2_result_t r;
        if (n < 3) {
            report_error("%%ODS2-E-NOPARM, usage: SET DEFAULT <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[2], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
            return true;
        }
        if (refuse_wild_dir(&p, "SET DEFAULT", tokens[2])) return true;
        if (p.filename[0] != '\0') {
            report_error("%%ODS2-E-BADPATH, SET DEFAULT takes a directory, "
                            "not a file (%s)\n", tokens[2]);
            return true;
        }
        resolve_effective_path(&p, effective, sizeof(effective));
        /* Validate the target actually exists before accepting it -
           an invalid SET DEFAULT should never silently leave the
           session pointed at a directory that doesn't exist. */
        r = ods2_lookup_path(vol, effective, &fid);
        if (!r.ok) {
            report_error("%%ODS2-E-DIRERR, %s\n", r.problem);
            return true;
        }
        r = ods2_read_header(vol, fid.fid_num, header);
        if (r.ok && !(((ods2_head_core_t *) header)->filechar & 0x2000u)) {
            report_error("%%ODS2-E-NOTDIR, %s is not a directory\n", tokens[2]);
            return true;
        }
        strncpy(g_default_dir, effective, sizeof(g_default_dir) - 1);
        g_default_dir[sizeof(g_default_dir) - 1] = '\0';
        printf("%%ODS2-I-DEFSET, default set to [%s%s]\n",
               (g_default_dir[0] == '\0') ? "000000" : "", g_default_dir);
        return true;
    }
    if (matches(tokens[0], "SHOW") && n > 1 &&
        (matches(tokens[1], "DEFAULT") || matches(tokens[1], "DEF"))) {
        printf("  [%s%s]\n", (g_default_dir[0] == '\0') ? "000000" : "", g_default_dir);
        return true;
    }
    if (matches(tokens[0], "DIR") || matches(tokens[0], "LS")) {
        ods2_parsed_path_t p;
        const char *arg = (n > 1) ? tokens[1] : ""; /* "" resolves to the
                                                         current default
                                                         directory, via the
                                                         same relative-path
                                                         logic as a bare
                                                         filename */
        if (!ods2_parse_path(arg, &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", arg);
            return true;
        }
        /* An explicit third token overrides any wildcard already
           parsed from the path itself (e.g. "DIR [000000] *.SYS"
           rather than "DIR [000000]*.SYS" in one token). */
        if (n > 2) {
            strncpy(p.filename, tokens[2], sizeof(p.filename) - 1);
            p.filename[sizeof(p.filename) - 1] = '\0';
            if (!ods2_split_version(p.filename, &p.has_version,
                                    &p.version_wildcard, &p.version)) {
                report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
                return true;
            }
        }
        cmd_dir(vol, &p);
        return true;
    }
    if (matches(tokens[0], "CREATE/DIRECTORY") || matches(tokens[0], "CREATE/DIR") ||
        matches(tokens[0], "MKDIR")) {
        ods2_parsed_path_t p;
        if (n < 2) {
            report_error("%%ODS2-E-NOPARM, usage: CREATE/DIRECTORY <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        if (refuse_wild_dir(&p, "CREATE/DIRECTORY", tokens[1])) return true;
        cmd_create_directory(vol, &p);
        return true;
    }
    if (matches(verb, "COPY") || matches(verb, "PUT") || matches(verb, "IMPORT")) {
        ods2_parsed_path_t p;
        ods2_record_attrs_t attrs;
        copy_options_t opts;
        memset(&opts, 0, sizeof(opts));
        /* COPY's default: Stream_LF, as before qualifiers existed. */
        attrs.rtype = ODS2_RFM_STMLF;
        attrs.rattrib = 0;
        attrs.rsize = 512;
        attrs.maxrec = 512;
        if (quals != NULL && !parse_copy_qualifiers(quals, &attrs, &opts)) {
            return true;
        }
        if (n < 3) {
            report_error("%%ODS2-E-NOPARM, usage: COPY <local-file> <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[2], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
            return true;
        }
        if (refuse_wild_dir(&p, "COPY", tokens[2])) return true;
        cmd_copy(vol, tokens[1], &p, &attrs, &opts);
        return true;
    }
    if (matches(tokens[0], "TYPE") || matches(tokens[0], "CAT")) {
        ods2_parsed_path_t p;
        if (n < 2) {
            report_error("%%ODS2-E-NOPARM, usage: TYPE <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        if (refuse_wild_dir(&p, "TYPE", tokens[1])) return true;
        cmd_type(vol, &p);
        return true;
    }
    if (matches(tokens[0], "DELETE") || matches(tokens[0], "RM")) {
        ods2_parsed_path_t p;
        if (n < 2) {
            report_error("%%ODS2-E-NOPARM, usage: DELETE <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            report_error("%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        if (refuse_wild_dir(&p, "DELETE", tokens[1])) return true;
        cmd_delete(vol, &p);
        return true;
    }

    report_error("%%ODS2-E-ILLCMD, unrecognized command \"%s\" - try HELP\n", tokens[0]);
    return true;
}

static int run_script(ods2_volume_t *vol, const char *script_path)
{
    FILE *f = fopen(script_path, "r");
    char line[MAX_LINE];
    if (f == NULL) {
        report_error("%%ODS2-E-OPENIN, could not open script %s\n", script_path);
        return 1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        /* Skip comment/blank lines (VMS-ish convention: '!' starts a comment). */
        char *p = line;
        while (*p && isspace((unsigned char) *p)) p++;
        if (*p == '\0' || *p == '!') continue;
        printf("ODS2> %s", line);
        if (line[strlen(line) - 1] != '\n') printf("\n");
        if (!execute_line(vol, line)) break;
    }
    fclose(f);
    /* Every line runs, as before, but the script as a whole failed if
       any command in it did. */
    return g_command_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    ods2_volume_t vol;
    ods2_result_t r;
    int exit_code = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <disk-image> [command...]\n", argv[0]);
        fprintf(stderr, "       %s <disk-image> @<script-file>\n", argv[0]);
        fprintf(stderr, "       %s <disk-image>                (interactive)\n", argv[0]);
        return 1;
    }

    r = ods2_mount_write(argv[1], &vol);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-F-MOUNTERR, %s: %s\n", argv[1], r.problem);
        return 1;
    }

    if (argc == 2) {
        /* Interactive shell - linenoise() returns a malloc'd line with
           no trailing newline, or NULL on EOF (Ctrl-D) or a read
           error. linenoiseHistoryAdd() copies the string internally,
           so it must run before execute_line() modifies the buffer
           in place via tokenization. Blank lines aren't added to
           history, matching typical shell behavior. */
        printf("ods2v2 - type HELP for commands, EXIT to leave\n");
        for (;;) {
            char *line = linenoise("ODS2> ");
            if (line == NULL) break; /* EOF (Ctrl-D) */
            if (line[0] != '\0') {
                linenoiseHistoryAdd(line);
            }
            {
                bool keep_going = execute_line(&vol, line);
                linenoiseFree(line);
                if (!keep_going) break;
            }
        }
    } else if (argv[2][0] == '@') {
        exit_code = run_script(&vol, argv[2] + 1);
    } else {
        /* Single-shot: join all remaining argv into one command line. */
        char line[MAX_LINE];
        int i;
        line[0] = '\0';
        for (i = 2; i < argc; i++) {
            if (i > 2) strncat(line, " ", sizeof(line) - strlen(line) - 1);
            strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
        }
        execute_line(&vol, line);
        exit_code = g_command_failed ? 1 : 0;
    }

    ods2_dismount(&vol);
    return exit_code;
}
