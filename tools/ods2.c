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
 *   COPY <local-file> <path>      write a local file onto the disk
 *   TYPE path                     print a file's content
 *   DELETE path                   delete a file (one version) or an
 *                                 empty directory
 *   SET DEFAULT path              set the current default directory
 *   SHOW DEFAULT                  show the current default directory
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
#include "ods2_volume.h"
#include "ods2_path.h"
#include "ods2_wildcard.h"

#define MAX_LINE 1024
#define MAX_TOKENS 8

/* Current default directory for this session (SET DEFAULT), dot-
   separated with no brackets - "" means root. Bare filenames (no
   brackets at all) and VMS's "[.SUBDIR]" relative notation both
   resolve against this. A plain global is fine here: this is a
   single-threaded, single-volume interactive tool - there is exactly
   one "current session" for it to belong to. */
static char g_default_dir[ODS2_PATH_MAX] = "";

/* Case-insensitive prefix/equality check. */
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
    fprintf(stderr, "%%ODS2-E-WILDDIR, %s: %s does not accept a wildcard or \"...\" "
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
            if (shown == 0) {
                printf("\nDirectory [%s%s]\n\n", (dir_path[0] == '\0') ? "000000" : "",
                       dir_path);
            }
            printf("%-20s;%u\n", entries[i].name, entries[i].version);
            shown++;
        }
    }
    if (shown > 0) {
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
        fprintf(stderr, "%%ODS2-E-TOODEEP, recursion limit reached, skipping "
                        "subtree under [%s]\n", abs_path);
        w->errors++;
        return;
    }

    r = resolve_dir(w->vol, abs_path, dir_header, &dir_fid);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-DIRERR, [%s]: %s\n", abs_path, r.problem);
        w->errors++;
        return;
    }
    r = ods2_list_directory_alloc(w->vol, dir_header, &entries, &count);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-LISTERR, [%s]: %s\n", abs_path, r.problem);
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
            fprintf(stderr, "%%ODS2-E-PATHTOOLONG, path too deep to represent, "
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
        fprintf(stderr, "%%ODS2-E-BADPATH, [%s]: invalid directory specification\n", spec);
        return;
    }

    memset(&w, 0, sizeof(w));
    w.vol = vol;
    w.spec = rest;
    w.pattern = (p->filename[0] != '\0') ? p->filename : "*";
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
        fprintf(stderr, "%%ODS2-E-BADPATH, no directory name given\n");
        return;
    }
    split_last_component(effective_path, dir_copy, &parent_path, &new_name);

    r = resolve_dir(vol, parent_path, parent_header, &parent_fid);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    r = ods2_create_directory(vol, parent_header, new_name, &new_fid);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-CREATEERR, %s\n", r.problem);
        return;
    }
    printf("%%ODS2-I-CREATED, created [%s]\n", effective_path);
}

static void cmd_copy(ods2_volume_t *vol, const char *local_file, const ods2_parsed_path_t *p)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid, new_fid;
    FILE *f;
    uint8_t *buf;
    size_t buf_size = 12u * 1024u * 1024u;
    size_t content_len;
    ods2_result_t r;
    const char *name;
    uint8_t rtype = 5; /* FAB$C_STMLF - reasonable default for COPY */

    if (p->filename[0] == '\0') {
        fprintf(stderr, "%%ODS2-E-BADPATH, no destination filename given\n");
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
        fprintf(stderr, "%%ODS2-E-BADVER, %s: COPY creates version 1 only - "
                        "give NAME, NAME; or NAME;1\n", spec);
        return;
    }
    name = p->filename;

    f = fopen(local_file, "rb");
    if (f == NULL) {
        fprintf(stderr, "%%ODS2-E-OPENIN, could not open %s\n", local_file);
        return;
    }
    buf = malloc(buf_size);
    if (buf == NULL) {
        fprintf(stderr, "%%ODS2-E-NOMEM, could not allocate read buffer\n");
        fclose(f);
        return;
    }
    content_len = fread(buf, 1, buf_size, f);
    if (!feof(f)) {
        fprintf(stderr, "%%ODS2-E-TOOBIG, %s is too large for a single header's map area\n",
                local_file);
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);

    {
        char effective_dir[ODS2_PATH_MAX];
        resolve_effective_path(p, effective_dir, sizeof(effective_dir));
        r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    }
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-DIRERR, %s\n", r.problem);
        free(buf);
        return;
    }
    r = ods2_create_file(vol, dir_header, name, buf, content_len, rtype, &new_fid);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-COPYERR, %s\n", r.problem);
        free(buf);
        return;
    }
    printf("%%ODS2-I-COPIED, %zu bytes to %s;1\n", content_len, name);
    free(buf);
}

static void cmd_type(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    uint8_t dir_header[512], file_header[512];
    ods2_fid_t dir_fid, file_fid;
    ods2_result_t r;
    uint8_t *buf;
    size_t buf_size;
    size_t bytes_read = 0;
    char spec[ODS2_PATH_MAX + 16];

    if (p->filename[0] == '\0') {
        fprintf(stderr, "%%ODS2-E-BADPATH, no filename given\n");
        return;
    }
    format_file_spec(p, spec, sizeof(spec));
    if (p->version_wildcard) {
        fprintf(stderr, "%%ODS2-E-BADVER, %s: TYPE takes one version, not ;*\n", spec);
        return;
    }
    {
        char effective_dir[ODS2_PATH_MAX];
        resolve_effective_path(p, effective_dir, sizeof(effective_dir));
        r = resolve_dir(vol, effective_dir, dir_header, &dir_fid);
    }
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-DIRERR, %s\n", r.problem);
        return;
    }
    /* p->version is 0 (highest) when no version was given. */
    r = ods2_lookup_name_version(vol, dir_header, p->filename, p->version, &file_fid, NULL);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-FNF, %s not found\n", spec);
        return;
    }
    r = ods2_read_header(vol, file_fid.fid_num, file_header);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-READERR, %s\n", r.problem);
        return;
    }
    /* Sized from the file's own end-of-file mark rather than a fixed
       guess, so any size of file can be typed. */
    buf_size = ods2_file_content_length(file_header);
    if (buf_size > ods2_file_allocated_bytes(file_header)) {
        fprintf(stderr, "%%ODS2-E-READERR, %s: end-of-file mark lies beyond the "
                        "file's allocated blocks (corrupt header?)\n", spec);
        return;
    }
    buf = malloc(buf_size > 0 ? buf_size : 1);
    if (buf == NULL) {
        fprintf(stderr, "%%ODS2-E-NOMEM, could not allocate %zu-byte read buffer\n", buf_size);
        return;
    }
    r = ods2_read_file(vol, file_header, buf, buf_size, &bytes_read);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-READERR, %s\n", r.problem);
        free(buf);
        return;
    }
    fwrite(buf, 1, bytes_read, stdout);
    free(buf);
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
            fprintf(stderr, "%%ODS2-E-BADPATH, cannot delete the root directory\n");
            return;
        }
        split_last_component(effective_path, split_buf, &parent_path, &last_name);
        snprintf(name_buf, sizeof(name_buf), "%s.DIR", last_name);
        dir_path = parent_path;
        name = name_buf;
    }

    r = resolve_dir(vol, dir_path, dir_header, &dir_fid);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-DIRERR, %s\n", r.problem);
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
        fprintf(stderr, "%%ODS2-E-DELETEERR, %s: %s\n", spec, r.problem);
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
           "  COPY <local-file> <path>   write a local file onto the disk\n"
           "  TYPE path                  print a file's content\n"
           "  DELETE path                delete a file (one version) or empty directory\n"
           "  SET DEFAULT path           set the current default directory\n"
           "  SHOW DEFAULT               show the current default directory\n"
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
    int n = tokenize(line, tokens);
    if (n == 0) return true; /* blank line */

    if (matches(tokens[0], "EXIT") || matches(tokens[0], "QUIT")) {
        return false;
    }
    if (matches(tokens[0], "HELP") || matches(tokens[0], "?")) {
        print_help();
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
            fprintf(stderr, "%%ODS2-E-NOPARM, usage: SET DEFAULT <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[2], &p)) {
            fprintf(stderr, "%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
            return true;
        }
        if (refuse_wild_dir(&p, "SET DEFAULT", tokens[2])) return true;
        if (p.filename[0] != '\0') {
            fprintf(stderr, "%%ODS2-E-BADPATH, SET DEFAULT takes a directory, "
                            "not a file (%s)\n", tokens[2]);
            return true;
        }
        resolve_effective_path(&p, effective, sizeof(effective));
        /* Validate the target actually exists before accepting it -
           an invalid SET DEFAULT should never silently leave the
           session pointed at a directory that doesn't exist. */
        r = ods2_lookup_path(vol, effective, &fid);
        if (!r.ok) {
            fprintf(stderr, "%%ODS2-E-DIRERR, %s\n", r.problem);
            return true;
        }
        r = ods2_read_header(vol, fid.fid_num, header);
        if (r.ok && !(((ods2_head_core_t *) header)->filechar & 0x2000u)) {
            fprintf(stderr, "%%ODS2-E-NOTDIR, %s is not a directory\n", tokens[2]);
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
            fprintf(stderr, "%%ODS2-E-BADPATH, could not parse %s\n", arg);
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
                fprintf(stderr, "%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
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
            fprintf(stderr, "%%ODS2-E-NOPARM, usage: CREATE/DIRECTORY <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            fprintf(stderr, "%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        if (refuse_wild_dir(&p, "CREATE/DIRECTORY", tokens[1])) return true;
        cmd_create_directory(vol, &p);
        return true;
    }
    if (matches(tokens[0], "COPY") || matches(tokens[0], "PUT") || matches(tokens[0], "IMPORT")) {
        ods2_parsed_path_t p;
        if (n < 3) {
            fprintf(stderr, "%%ODS2-E-NOPARM, usage: COPY <local-file> <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[2], &p)) {
            fprintf(stderr, "%%ODS2-E-BADPATH, could not parse %s\n", tokens[2]);
            return true;
        }
        if (refuse_wild_dir(&p, "COPY", tokens[2])) return true;
        cmd_copy(vol, tokens[1], &p);
        return true;
    }
    if (matches(tokens[0], "TYPE") || matches(tokens[0], "CAT")) {
        ods2_parsed_path_t p;
        if (n < 2) {
            fprintf(stderr, "%%ODS2-E-NOPARM, usage: TYPE <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            fprintf(stderr, "%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        if (refuse_wild_dir(&p, "TYPE", tokens[1])) return true;
        cmd_type(vol, &p);
        return true;
    }
    if (matches(tokens[0], "DELETE") || matches(tokens[0], "RM")) {
        ods2_parsed_path_t p;
        if (n < 2) {
            fprintf(stderr, "%%ODS2-E-NOPARM, usage: DELETE <path>\n");
            return true;
        }
        if (!ods2_parse_path(tokens[1], &p)) {
            fprintf(stderr, "%%ODS2-E-BADPATH, could not parse %s\n", tokens[1]);
            return true;
        }
        if (refuse_wild_dir(&p, "DELETE", tokens[1])) return true;
        cmd_delete(vol, &p);
        return true;
    }

    fprintf(stderr, "%%ODS2-E-ILLCMD, unrecognized command \"%s\" - try HELP\n", tokens[0]);
    return true;
}

static int run_script(ods2_volume_t *vol, const char *script_path)
{
    FILE *f = fopen(script_path, "r");
    char line[MAX_LINE];
    if (f == NULL) {
        fprintf(stderr, "%%ODS2-E-OPENIN, could not open script %s\n", script_path);
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
    return 0;
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
    }

    ods2_dismount(&vol);
    return exit_code;
}
