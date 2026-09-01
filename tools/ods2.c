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
 *   CREATE/DIRECTORY path         create a directory
 *   COPY <local-file> <path>      write a local file onto the disk
 *   TYPE path                     print a file's content
 *   DELETE path                   delete a file or empty directory
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

/* Lists one directory's matching entries, printing a "Directory
 * [path]" header first - the non-recursive building block both the
 * plain DIR command and the recursive one below use. `dir_path` here
 * is already fully resolved/absolute (from root), not a raw parsed
 * path - callers handle SET DEFAULT resolution before this. */
static int list_one_directory(ods2_volume_t *vol, const char *dir_path, const char *pattern)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid;
    ods2_dir_entry_t entries[512];
    int count = 0, i, shown = 0;
    ods2_result_t r = resolve_dir(vol, dir_path, dir_header, &dir_fid);

    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-DIRERR, [%s]: %s\n", dir_path, r.problem);
        return -1;
    }
    r = ods2_list_directory(vol, dir_header, entries, 512, &count);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-LISTERR, [%s]: %s\n", dir_path, r.problem);
        return -1;
    }

    printf("\nDirectory [%s%s]\n\n", (dir_path[0] == '\0') ? "000000" : "", dir_path);
    for (i = 0; i < count; i++) {
        if (!ods2_wildcard_match(pattern, entries[i].name)) continue;
        printf("%-20s;%u\n", entries[i].name, entries[i].version);
        shown++;
    }
    printf("\nTotal of %d file%s.\n", shown, (shown == 1) ? "" : "s");
    return shown;
}

/* Recursive listing - VMS's "[DIR...]*.*" notation: lists dir_path
 * itself, then descends into every subdirectory found there
 * (regardless of whether the subdirectory's own name matches
 * `pattern` - the pattern filters what's shown at each level, not
 * which directories get descended into, matching real VMS), each
 * getting its own "Directory [...]" header, recursively. Whether an
 * entry is a subdirectory is checked via its own header's
 * FH2$M_DIRECTORY bit (0x2000, confirmed against real VMS-written
 * headers earlier in this project) rather than just a ".DIR" name
 * suffix - more robust, matches how ods2_delete() already checks
 * this same thing. */
/* Real VMS directory hierarchies are never more than a handful of
   levels deep in practice; this is a generous but finite backstop
   against stack overflow from a cycle I haven't thought of - the
   project reads arbitrary, possibly corrupted disk images. */
#define MAX_RECURSION_DEPTH 50

static void list_directory_recursive(ods2_volume_t *vol, const char *dir_path, const char *pattern,
                                      int *total_files, int *total_dirs, int depth)
{
    uint8_t dir_header[512];
    ods2_fid_t dir_fid;
    ods2_dir_entry_t entries[512];
    int count = 0, i;
    ods2_result_t r;
    int shown;

    if (depth > MAX_RECURSION_DEPTH) {
        fprintf(stderr, "%%ODS2-E-TOODEEP, recursion limit reached, skipping "
                        "subtree under [%s]\n", dir_path);
        return;
    }

    shown = list_one_directory(vol, dir_path, pattern);

    if (shown < 0) return; /* error already reported */
    *total_files += shown;
    (*total_dirs)++;

    r = resolve_dir(vol, dir_path, dir_header, &dir_fid);
    if (!r.ok) return; /* already reported by list_one_directory above */
    r = ods2_list_directory(vol, dir_header, entries, 512, &count);
    if (!r.ok) return;

    for (i = 0; i < count; i++) {
        uint8_t sub_header[512];

        /* Skip self-referential entries - root's own directory
           always contains a "000000.DIR" entry pointing straight
           back at itself (FID (4,4) - spec-mandated, present on
           every ODS-2 volume). Avoids erroneous recursive DIR
           descend into root via that entry causing an infinite loop. */
        if (entries[i].fid.fid_num == dir_fid.fid_num &&
            entries[i].fid.fid_seq == dir_fid.fid_seq) {
            continue;
        }

        r = ods2_read_header(vol, entries[i].fid.fid_num, sub_header);
        if (r.ok) {
            ods2_head_core_t *core = (ods2_head_core_t *) sub_header;
            if (core->filechar & 0x2000u) { /* FH2$M_DIRECTORY */
                char name_no_suffix[ODS2_PATH_MAX];
                char sub_path[ODS2_PATH_MAX];
                char *dot;
                strncpy(name_no_suffix, entries[i].name, sizeof(name_no_suffix) - 1);
                name_no_suffix[sizeof(name_no_suffix) - 1] = '\0';
                dot = strrchr(name_no_suffix, '.');
                if (dot != NULL) *dot = '\0'; /* strip ".DIR" */

                if (dir_path[0] == '\0') {
                    snprintf(sub_path, sizeof(sub_path), "%s", name_no_suffix);
                } else {
                    int written = snprintf(sub_path, sizeof(sub_path), "%s.%s",
                                            dir_path, name_no_suffix);
                    if (written < 0 || (size_t) written >= sizeof(sub_path)) {
                        fprintf(stderr, "%%ODS2-E-PATHTOOLONG, path too deep to represent, "
                                        "skipping subtree under [%s]\n", dir_path);
                        continue;
                    }
                }
                list_directory_recursive(vol, sub_path, pattern, total_files, total_dirs, depth + 1);
            }
        }
    }
}

static void cmd_dir(ods2_volume_t *vol, const ods2_parsed_path_t *p)
{
    char effective_dir[ODS2_PATH_MAX];
    const char *pattern = (p->filename[0] != '\0') ? p->filename : "*";
    resolve_effective_path(p, effective_dir, sizeof(effective_dir));

    if (p->recursive) {
        int total_files = 0, total_dirs = 0;
        list_directory_recursive(vol, effective_dir, pattern, &total_files, &total_dirs, 0);
        if (total_dirs > 0) {
            printf("\nGrand total of %d director%s, %d file%s.\n",
                   total_dirs, (total_dirs == 1) ? "y" : "ies",
                   total_files, (total_files == 1) ? "" : "s");
        }
        /* total_dirs==0 means the root of the requested subtree
           itself couldn't be listed - list_one_directory() already
           reported that error, printing a "Grand total" of zero here
           too would misleadingly suggest a legitimately empty
           (rather than failed) subtree. */
    } else {
        list_one_directory(vol, effective_dir, pattern);
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
    long file_size;
    size_t buf_size;
    size_t content_len;
    ods2_result_t r;
    const char *name;
    uint8_t rtype = 5; /* FAB$C_STMLF - reasonable default for COPY */

    if (p->filename[0] == '\0') {
        fprintf(stderr, "%%ODS2-E-BADPATH, no destination filename given\n");
        return;
    }
    name = p->filename;

    f = fopen(local_file, "rb");
    if (f == NULL) {
        fprintf(stderr, "%%ODS2-E-OPENIN, could not open %s\n", local_file);
        return;
    }
    /* Size the read buffer to the actual local file, rather than a
       fixed cap - ods2_create_file() itself now supports files well
       beyond the old ~9.5MB single-header limit (chained extension
       headers, spec 3.3), so a hard-coded buffer here would just
       reintroduce that ceiling at the CLI layer. */
    if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%%ODS2-E-OPENIN, could not determine size of %s\n", local_file);
        fclose(f);
        return;
    }
    buf_size = (size_t) file_size;
    buf = malloc(buf_size > 0 ? buf_size : 1);
    if (buf == NULL) {
        fprintf(stderr, "%%ODS2-E-NOMEM, could not allocate read buffer\n");
        fclose(f);
        return;
    }
    content_len = fread(buf, 1, buf_size, f);
    if (content_len != buf_size) {
        fprintf(stderr, "%%ODS2-E-READERR, could not read all of %s\n", local_file);
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

    if (p->filename[0] == '\0') {
        fprintf(stderr, "%%ODS2-E-BADPATH, no filename given\n");
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
    r = ods2_lookup_name(vol, dir_header, p->filename, &file_fid);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-FNF, %s not found\n", p->filename);
        return;
    }
    r = ods2_read_header(vol, file_fid.fid_num, file_header);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-READERR, %s\n", r.problem);
        return;
    }
    /* Size the buffer exactly to this file's own stated content
       length (from EFBLK/FFBYTE, already in the header we just read)
       rather than a fixed cap - a multi-header file can be far larger
       than the old ~9.5MB single-header limit. */
    buf_size = ods2_file_content_length(file_header);
    buf = malloc(buf_size > 0 ? buf_size : 1);
    if (buf == NULL) {
        fprintf(stderr, "%%ODS2-E-NOMEM, could not allocate read buffer\n");
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
    r = ods2_delete(vol, dir_fid.fid_num, name);
    if (!r.ok) {
        fprintf(stderr, "%%ODS2-E-DELETEERR, %s\n", r.problem);
        return;
    }
    printf("%%ODS2-I-DELETED, deleted %s\n", name);
}

static void print_help(void)
{
    printf("Commands (VMS bracket-notation paths, e.g. [DECUS.NETLIB020]FILE.TXT):\n"
           "  DIR [path] [wildcard]      list a directory\n"
           "  DIR [path...] [wildcard]   list a directory and everything beneath it\n"
           "  CREATE/DIRECTORY path      create a directory\n"
           "  COPY <local-file> <path>   write a local file onto the disk\n"
           "  TYPE path                  print a file's content\n"
           "  DELETE path                delete a file or empty directory\n"
           "  SET DEFAULT path           set the current default directory\n"
           "  SHOW DEFAULT               show the current default directory\n"
           "  HELP                       show this command list\n"
           "  EXIT / QUIT                leave the interactive shell\n"
           "\n"
           "Paths starting with '.' or '-' inside brackets (e.g. [.NETLIB020],\n"
           "[-], [-.SIBLING]) and bare filenames with no brackets at all are\n"
           "relative to the current default directory (see SET DEFAULT); '-'\n"
           "means \"go up one level\" (repeat as \"-.-\" for more); other bracketed\n"
           "paths are always absolute from root, matching real VMS.\n");
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
