# ods2v2

This is a from-scratch implementation of DEC Files-11 (ODS-2) utility
to copy local files from a Linux (likely a Mac too) system to an
ODS-2 disk volume that is usable by SIMH simulator running a
VAX/VMS (vax8600) OpenVMS 7.1 system.

This utility can run a minimal set of VMS DCL type commands on the
disk file, with the intent of transferring files into or out of the
VMS system.  Reads and writes real ODS-2 disk images - the same format
real VMS's own `MOUNT`, `INITIALIZE`, `COPY`, and `DIRECTORY` commands
use.  No VMS installation, SIMH emulator, or license required to use it.

Built directly from the Files-11 On-Disk Structure Specification, not
from copying an existing implementation, and validated repeatedly
against real OpenVMS 7.1 (via SIMH) throughout development with
real  MOUNT / DIR / TYPE / COPY and ANALYZE/DISK commands on a
vax8600 running OpenVMS 7.1 in simh. Every significant bug this
project found (a word-swapped 32-bit field, an inverted bitmap
convention, directory records needing to be storage-sorted) was found
with analysis on an OpenVMS system.

## Quick start

```bash
tar xzf ods2v2.tgz && cd ods2v2  # expand and cd into ods2v2 cwd
make clean && make ods2          # only builds ods2
make starter-volume              # creates an empty ODS2 RA92 volume
                                 # by decompressing a real VMS
                                 # INITIALIZEd disk file: transfer.dsk
./ods2 transfer.dsk DIR          # starts ods2v2 and executes a DIR command
```

## Full test suite

```bash
tar xzf ods2v2.tgz && cd ods2v2  # expand and cd into ods2v2 cwd
make clean && make test          # builds everything, runs the full test suite
make starter-volume              # creates an empty ODS2 RA92 volume
                                 # by decompressing a real VMS
                                 # INITIALIZEd disk file: transfer.dsk
./ods2 transfer.dsk DIR          # starts ods2v2 and executes a DIR command
```

No external dependencies beyond a C compiler and standard C dev tools.
Development was on Rocky Linux 10.2 (same system as the simh vax8600 is
running on). There is external code included for the interactive shell
command history (see Dependencies below).

## Using `ods2`

`ods2` is the actual, intended interface - one binary, DCL-style
commands, VMS bracket-notation paths throughout. Interactive mode
implements basic command recall to ease repetitive command sequences.

```bash
./ods2 transfer.dsk                          # interactive shell
./ods2 transfer.dsk DIR [DECUS.NETLIB020]    # single-shot
./ods2 transfer.dsk @script.cmd              # run commands from a file
```

### Commands

| Command | Example |
|---|---|
| `DIR [path] [wildcard]` | `DIR [DECUS] *.TXT` |
| `DIR [path...] [wildcard]` | `DIR [...]*.*` — recursive, VMS's `...` notation |
| `CREATE/DIRECTORY path` | `CREATE/DIRECTORY [DECUS.NETLIB020]` |
| `COPY <local-file> <path>` | `COPY readme.txt [DECUS]README.TXT` |
| `TYPE path` | `TYPE [DECUS]README.TXT` |
| `DELETE path` | `DELETE [DECUS]OLDFILE.TXT` |
| `SET DEFAULT path` | `SET DEFAULT [DECUS.NETLIB020]` |
| `SHOW DEFAULT` | |
| `HELP` | |
| `EXIT` / `QUIT` | |

You can probably deduce I was trying to get some freeware networking
running (such as CMU066IP) and developed this tool to help with the
initial transfer of savesets onto the OpenVMS system.

Paths follow real VMS conventions: `[.SUBDIR]` and bare filenames with
no brackets are relative to the current default directory (see `SET
DEFAULT`); `[-]` means "up one level" (`[-.-]` for more); other
bracketed paths are absolute from root. Names are automatically
uppercased on creation, matching real VMS storage.

### A simple example

```
$ ./ods2 transfer.dsk
ODS2> CREATE/DIRECTORY [DECUS]
%ODS2-I-CREATED, created [DECUS]
ODS2> SET DEFAULT [DECUS]
%ODS2-I-DEFSET, default set to [DECUS]
ODS2> COPY /tmp/readme.txt README.TXT
%ODS2-I-COPIED, 59 bytes to README.TXT;1
ODS2> DIR
Directory [DECUS]
README.TXT           ;1
ODS2> type readme.txt
Use this to get VMS savesets onto your VAX/VMS sim system.
ODS2> EXIT
```

## Development/diagnostic tools

Ten additional single-purpose tools exist from this project's own
development:

One operation each, no DCL syntax:
`ods2_cat`
`ods2_ls`
`ods2_mkdir`
`ods2_put`
`ods2_rm` 

Raw on-disk structure inspection:
`ods2_check_headers`
`ods2_dump_header`
`ods2_dump_root`
`read_file_header`
`read_home_block`

Fully functional, but not needed for normal use - `ods2` already covers
everything these commands do.

Build them with:

```bash
make dev-tools
or
make all
```

## Project structure

```
include/    Headers for the core library (ods2_volume.h is main)
src/        Core library - reading, writing, validating ODS-2 structures
tools/      ods2.c (the CLI) plus the dev-tools individual source files
tests/      One test suite per src file, run via `make test`
samples/    Real, VMS-written fragments used by the test suite, plus a
            ready-to-use starter volume (see samples/README.md)
third_party/linenoise/  BSD licensed - interactive-mode command history
            with zero external dependencies
```

## Testing

`make test` builds and runs 14 test suites, 122 individual assertions,
with `-fsanitize=address,undefined` on by default for development
builds (`make release` builds without them once things are stable).
Most tests run entirely offline against a synthetic disk
image assembled from real, VMS-written byte fragments captured during
development with VMS generated volumes so there was genuine on-disk bytes
in every structural test.

## Known limitations

- Files/directories needing more than roughly 77 retrieval-pointer
  extents in a single header (extension headers) aren't yet
  implemented - very large or badly fragmented files/directories
  would need this eventually.
- No INITIALIZE command (formatting a *new* volume from scratch) - see
  `samples/README.md` for why that's a deliberate choice (it's complicated)
  and how to get a real, ready-to-use volume instead.
- Files are capped around 9.5MB (a consequence of the extension-header
  limitation above).

## Dependencies

None beyond a C compiler and `make`. The interactive shell's command
history (up/down arrows) uses a BSD licensed copy of linenoise rather
than linking against a system library like GNU Readline - keeps `make`
working identically on every platform with no separate install step.
FYI, the vanilla linenoise doesn't appear to support Ctrl-R reverse search.

## License

MIT - see LICENSE. Applies to this project's own code; the BSD copy of
linenoise (third_party/linenoise) has its own, separate license.

## Acknowledgements

I started this project from scratch after seeing the great work
from various people on "ods2".  I determined the best way to get
a solid working ODS2 utility was to go back to the original 
DEC Files-11 Specification, versus attempting patches or fixes to 
the original ods2 source code.

That ods2 code had these references in it:

 Jul-2003, v1.3hb, some extensions by Hartmut Becker
 Ods2.c v1.3   Mainline ODS2 program

 This is part of ODS2 written by Paul Nankervis,
 email address:  Paulnank@au1.ibm.com

 ODS2 is distributed freely for all members of the
 VMS community to use. However all derived works
 must maintain comments in their source to acknowledge
 the contibution of the original author.

 Modified by:
 31-AUG-2001 01:04 Hunter Goatley <goathunter@goatley.com>
 For VMS, added routine getcmd() to read commands with full
 command recall capabilities.

 This is the top level set of routines. It is fairly
 simple minded asking the user for a command, doing some
 primitive command parsing, and then calling a set of routines
 to perform whatever function is required (for example COPY).
 Some routines are implemented in different ways to test the
 underlying routines - for example TYPE is implemented without
 a NAM block meaning that it cannot support wildcards...
 (sorry! - could be easily fixed though!)

