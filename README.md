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

I have followed the excellent SIMH processor simulator package
(https://github.com/simh/simh.git)
implementation guide here 
(https://www.openvmshobby.com/vax-vms/openvms-on-vax-simh/)
along with an OpenVMS 7.1 "ISO" image from here
(http://vaxhaven.com/cd-image/AG-QSBWB-BE.iso.zip)
to get a VAX8600 running VMS 7.1 on a Ubuntu 24 Linux system.

I also have hosted a copy of that ISO on my website here
(https://pomeroy.us/vms-software/VAXVMS071.iso)

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

- No INITIALIZE command (formatting a *new* volume from scratch) - see
  `samples/README.md` for why that's a deliberate choice (it's complicated)
  and how to get a real, ready-to-use volume instead.

## Chained extension headers & Format 2/3 retrieval pointers

Originally, any file needing more retrieval-pointer extents than a
single header's Map Area could hold (roughly 77 Format 1 extents, or
~9.5MB on this project's test volume) just failed outright:

```
%ODS2-E-COPYERR, content far too large for a single header's map area
(needs a second, extension header - not yet implemented)
```

Since this utility's purpose is to enable transfer of large file
volumes into an OpenVMS system to help build functionality that
is needed, this version supports Format 2 and 3 extents now.
It is now possible to copy CD image files (600-700MB) into a
ODS-2 filesystem.

Two things had to be added to extend the functionality:

### 1. Chained extension headers (spec 3.3)

A file whose retrieval pointers don't fit in one header can span a
*chain* of headers, each with its own file number, linked via
`FH2$W_EXT_FID`. I already had the read side
(`ods2_decode_all_extents()`) already wired to walk that chain
correctly, since it has to handle any real VMS-written file the
same way. Only the write side (`ods2_create_file()`, and the
equivalent directory-growth path in `ods2_insert_into_directory()`)
were still on my original Format 1 code.

Two spec facts needed to update the design:

- **Extension headers truncate their own Ident Area to zero length**
  (spec 3.5.3: "customarily truncated in extension headers"). That
  frees the 120 bytes the Ident Area would otherwise take, giving an
  extension header ~107 extents of Map Area instead of a primary
  header's ~77.
- **An extension header's `FH2$W_BACKLINK` is the file's *primary*
  header's FID, not the parent directory's** (spec 3.5.2.16). Every
  other header field (backlink included) works exactly like a normal
  file's.

`ods2_create_file()` now allocates as many header segments as needed
(pre-allocating every segment's file number up front, since each
segment's `ext_fid` has to name the *next* one), up to
`ODS2_MAX_HEADER_SEGMENTS` (64, shared with the read side so nothing
either side produces exceeds what the other can follow).
`ods2_delete()` was updated to match: it now walks the whole chain to
free every segment's file number, not just the primary's - otherwise a
multi-header file's extension headers would leak (permanently marked
"in use" in the index bitmap) on delete.

### 2. Format 2/3 retrieval pointer encoding (spec 3.5.4.3/3.5.4.4)

Chaining headers alone wasn't enough. Format 1 retrieval pointers cap
every extent at 256 blocks (an 8-bit count field) regardless of how
contiguous the underlying free space actually is - a structural limit
of the *encoding*, not a reflection of real fragmentation. A 700MB
image needs 5,000+ Format 1 extents even on an empty volume with one
giant free run, which chews through header-chain file numbers fast:
INDEXF.SYS's own header-storage region is a fixed size set at
`INITIALIZE` time, and a single large file eating 50+ file numbers for
its own extension headers can exhaust it on a modestly-sized volume.

The ODS2 disk spec already defines Format 2 (14-bit count, up to 16,384
blocks/extent) and Format 3 (30-bit count, up to ~2^30 blocks/extent);
the decoder already handled all three formats, but only Format 1 had
an encoder. `ods2_encode_retrieval_pointer()` now picks the smallest
of the three that actually fits a given extent, so ordinary small
extents stay exactly as compact as before (unchanged 4-byte Format 1)
while a large contiguous allocation compresses into one 6- or 8-byte
pointer instead of dozens of 4-byte ones.

That only pays off if the allocator actually *tries* for large
contiguous runs, so `ods2_create_file()`'s allocation loop no longer
artificially chunks every request down to ≤256 blocks - it requests
the full remaining amount each time. On a genuinely fragmented volume
that could fail outright with no fallback, so `ods2_allocate_blocks()`
gained a best-effort path (`ods2_bitmap_find_largest_free()`): if no
single run is big enough for the whole request, it takes the largest
run actually available instead of failing, and the loop continues with
more (but still as few as possible) extents for the remainder.

Net effect: a 100MB file that used to need ~8 header segments
(and failed outright on a volume without that much header-storage
headroom) now needs exactly 1 header and 1 extent - Format 3 encodes
the whole contiguous allocation as a single retrieval pointer.

### Testing the Format 2/3 

Make the dev-tools

```
$ make dev-tools
```

Make large files to test (or use real large files)

```
dd if=/dev/urandom of=../data/t1.bin bs=1M count=11   # ~11MB
dd if=/dev/urandom of=../data/t2.bin bs=1M count=30   # ~30MB
dd if=/dev/urandom of=../data/t3.bin bs=1M count=100  # ~100MB
```

Copy large files into ODS2 volume

```
$ ./ods2 transfer.dsk
ods2v2 - type HELP for commands, EXIT to leave
ODS2> 
ODS2> dir

Directory [000000]

000000.DIR          ;1
BACKUP.SYS          ;1
BADBLK.SYS          ;1
BADLOG.SYS          ;1
BITMAP.SYS          ;1
CONTIN.SYS          ;1
CORIMG.SYS          ;1
INDEXF.SYS          ;1
SECURITY.SYS        ;1
VOLSET.SYS          ;1

Total of 10 files.
ODS2> 
ODS2> create/dir [test1]
%ODS2-I-CREATED, created [TEST1]
ODS2> set def [test1]
%ODS2-I-DEFSET, default set to [TEST1]
ODS2> copy ../data/t3.bin t3.bin
%ODS2-I-COPIED, 104857600 bytes to t3.bin;1
ODS2> exit
```

Validate they extract and are identical

```
$ ./ods2_cat transfer.dsk TEST1 T3.BIN | cmp - ../data/t3.bin && echo IDENTICAL
IDENTICAL
```

### What this doesn't fix

The header-storage region itself (INDEXF.SYS's own allocation, set at
`INITIALIZE` time) is still a fixed size - a *sufficiently* large or
fragmented copy job can still exhaust it, just far less easily than
before. There's still no rollback if an allocation succeeds but a
later step in the same operation fails partway through (a pre-existing
property of this codebase, not something introduced here) - blocks and
file numbers already committed to disk at that point aren't
automatically freed again.

### Testing

Verified end-to-end against a real ODS-2 disk image (the RA92-geometry
synthetic test volume, same shape as `transfer.dsk`), not just the unit
test suite: files at several size tiers (11MB, 30MB, 100MB) were copied
in via the actual `ods2` CLI, read back via `ods2_cat`, and confirmed
byte-for-byte identical via checksum. Header chains were dumped and
checked directly against spec (correct `seg_num`, `ext_fid` linkage,
and `backlink` pointing at the primary FID on extension headers). The
existing `ods2_check_headers`/`ods2_validate_head` diagnostics had a
couple of false-positive checks that assumed every file fit in one
header (comparing a single header's own `HIBLK` against just its own
extents) - both were updated to recognize a chained header instead of
reporting a spurious mismatch.

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
