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
make clean && make all           # builds everything, runs the full test suite
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
| `DIR [*...] [wildcard]` | `DIR [*...]*.COM` — wildcards in directory names (see below) |
| `CREATE/DIRECTORY path` | `CREATE/DIRECTORY [DECUS.NETLIB020]` — a directory name cannot start with `-`: VAX VMS (V7.3) reads a leading `-` as the parent directory and cannot reach files inside (`%RMS-F-SYN`; ODS-2 has no `^` escape), so it is refused |
| `COPY <local-file> <path>` | `COPY readme.txt [DECUS]README.TXT` |
| `COPY/ATTRIBUTES=(...) <local-file> <path>` | `COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) kit.pcsi [KITS]KIT.PCSI` |
| `COPY/CONVERT <local-file> <path>` | `COPY/CONVERT notes.txt [DOC]NOTES.TXT` — text as VAR records |
| `TYPE path` | `TYPE [DECUS]README.TXT;2` |
| `DELETE path` | `DELETE [DECUS]OLDFILE.TXT;1` |
| `SET DEFAULT path` | `SET DEFAULT [DECUS.NETLIB020]` |
| `SHOW DEFAULT` | |
| `SET VOLUME/LABEL=name` | `SET VOLUME/LABEL=ITANKITS` — relabel the volume |
| `SET FILE/ATTRIBUTES=(MRS:0) spec` | `SET FILE/ATTRIBUTES=(MRS:0) [DECUS...]*.*` — repairs Stream, Stream_LF and Stream_CR files made with a maximum record size or with an LRL below their longest record (older ods2v2 gave them MRS = LRL = 512, and VMS cannot read such a file with a longer line in record mode: `%RMS-W-RTB`): MRS 0, LRL the longest record; the only attribute change implemented |
| `SET FILE/VERSION_LIMIT=n spec` | `SET FILE/VERSION_LIMIT=32767 [DECUS...]*.*` — the version limit of the entries a spec names (wildcards and `[dir...]` as in `DIR`); 32767 or 0 is no limit, what VMS writes. For volumes written by an older ods2v2, whose entries carry 1: VMS purges version 1 of such a file when it makes version 2 |
| `SHOW VOLUME` | label, owner, cluster size, maximum files, free blocks |
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

`DIR` accepts VMS directory wildcards, as `DIRECTORY` does on VMS:
`*` and `%` in any directory name, and the ellipsis `...` (any
number of levels, including none) anywhere in the directory spec.

| Spec | Lists |
|---|---|
| `[*]` | every top-level directory |
| `[*...]` | every directory on the disk except root's own files |
| `[000000...]` | everything, root included |
| `[...]` | the current default directory and everything below it |
| `[DECUS.*]` | every directory directly under `[DECUS]` |
| `[...SALES]` | every directory named `SALES` at any depth below the default |
| `[DECUS...SRC]` | every `SRC` anywhere below `[DECUS]` |
| `[TEST%...]` | `[TEST1]`, `[TESTA]` ... and everything below them |

Like VMS, a multi-directory listing shows only directories that hold
something matching, ends with a grand total, and says "no files
found" when nothing matches. Quote these specs on the shell command
line (`./ods2 transfer.dsk DIR "[*...]*.*"`), since `[`, `*` and `?`
are shell wildcards too. The other commands act on one directory and
refuse a wildcard one.

File names follow ODS-2's rules: letters, digits, `$`, `_` and `-`,
with one `.` before the type, and up to 39 characters on each side
(names are uppercased for you). A host path won't do as a name -
`COPY src/x.c [TEST1]src/x.c` is refused; create `[TEST1.SRC]` and
copy to `[TEST1.SRC]X.C` instead. A name without a type is stored as
VMS stores it, with the dot (`MAKEFILE.`), and can be typed either
way.

File versions follow VMS rules too. `NAME;3` is version 3; `NAME`,
`NAME;` and `NAME;0` mean the highest version; `NAME;-1` is the next
lower existing version; `NAME;*` means every version (`DIR` and
`DELETE`). `DIR` lists every version of every file unless you give a
version (`DIR [DECUS]*.*;` shows just the newest of each). `DELETE`
removes one version - the one given, or the highest if none is - and
`DELETE NAME;*` removes them all. `COPY` onto the disk always creates
version 1 and refuses a name that already exists.

### Record formats

`COPY` makes a Stream_LF file, which suits text. Some files must have a
particular RMS record format - OpenVMS PCSI kit files, for one, are
sequential files of fixed 8192-byte records, and `PRODUCT` won't treat
a Stream_LF copy of the same bytes as a kit. Give the attributes with
`/ATTRIBUTES` on `COPY`, in the syntax of VMS's `SET FILE/ATTRIBUTES`:

```
COPY/ATTRIBUTES=(RFM:FIX,MRS:8192) dec-axpvms-foo-v0100--1.pcsi [KITS]DEC-AXPVMS-FOO-V0100--1.PCSI
COPY/ATTRIBUTES=(RFM:FIX) backup.bck [SAVE]BACKUP.BCK          ! fixed 512-byte records
COPY/ATTRIBUTES=(RFM:STMLF,RAT:CR) notes.txt [DOC]NOTES.TXT
```

- `RFM:` record format - `FIX`, `STMLF`, `STM`, `STMCR` or `UDF`, or
  `VAR` with `/CONVERT` (below). On disk each `VAR` or `VFC` record
  starts with a byte count a host file doesn't have, so `VAR` needs the
  conversion; `VFC` can't be imported.
- `MRS:` the record size (for `FIX`; `LRL:` means the same there).
  `RFM:FIX` with no size is 512-byte records. The file must be a whole
  number of records, and the size even (2-32766).
- `RAT:` record attributes - `NONE`, `CR`, `FTN`, `PRN`, `BLK`, `NOBLK`;
  repeat `RAT:` for more than one.
- A stream file (`STMLF`, `STM`, `STMCR`; also a plain `COPY`) or `UDF` file gets
  MRS 0 and its own longest record as `LRL` (the longest record as RMS reads
  it: `STMLF` ends at LF, `STMCR` at CR, `STM` at LF, FF or VT - that byte
  stays in the record - or CR LF; the length for `UDF`; at most 32767), unless
  `MRS:` or `LRL:` is given. A record longer than a given `MRS:` or `LRL:` is
  refused (`%ODS2-E-RECTOOLONG`): VMS could not read the file in record mode
  (`%RMS-W-RTB`, record too large for the user's buffer). (Before, MRS and LRL
  were 512 whatever the lines; a file with a longer line failed `TYPE`, `COPY`
  and `CHECKSUM` on VMS. `SET FILE/ATTRIBUTES=(MRS:0) spec` repairs such a
  file: MRS 0 and LRL the longest record, the data untouched.)

VMS's native text format is variable-length records with carriage-return
carriage control (`RFM:VAR`, `RAT:CR`) - what VMS FTP writes in ASCII
mode, and what some older utilities and kit procedures expect.
`/CONVERT` makes that from a host text file, one record per line:

```
COPY/CONVERT notes.txt [DOC]NOTES.TXT
COPY/ATTRIBUTES=(RFM:VAR,RAT:CR)/CONVERT notes.txt [DOC]NOTES.TXT   ! the same
```

- Lines end at LF; a CR just before the LF is dropped, so CRLF text
  converts the same. A last line with no LF is still a record.
- The longest record length (`LRL`) is set from the file; `MRS` is 0
  (no limit) unless given. A line over 32,767 bytes is refused.
- `RAT:` defaults to `CR`; `RAT:NONE`, `FTN` or `PRN` can be given
  instead. `RAT:BLK` isn't supported (the records run on across
  blocks, as VMS FTP writes them).
- `TYPE` of such a file shows the records as they are on disk, byte
  counts included.

The qualifier goes on `COPY` itself, not after the file names (a host
path has `/` in it too). `ods2_put` takes the same as numbers:
`ods2_put disk kit.pcsi KITS KIT.PCSI 1 8192` (record format, record
size, record attributes).

### Volume labels

A copy of a blank volume carries the blank's label, and two volumes
with the same label can't both be mounted `/SYSTEM` on one node - they
clash on the `DISK$label` logical name. Relabel a copy on the host, as
VMS's `SET VOLUME/LABEL` would, while VMS doesn't have it mounted:

```
./ods2 kits.dsk SET VOLUME/LABEL=ITANKITS
./ods2 kits.dsk SHOW VOLUME
```

- The label is 1-12 characters - letters, digits, `$`, `_` and `-` -
  and is uppercased.
- Every copy of the home block is rewritten: the home block and its
  cluster fillers, and the backup home block's cluster (8 blocks on
  the starter volume, cluster factor 3). Each keeps its own position
  fields, with both checksums recomputed; the primary is written last.
- Nothing is written unless every one of those copies is first found
  valid and consistent with the primary. A volume whose copies don't
  agree should go through `ANALYZE/DISK` on VMS first.
- A volume set member is refused: relabel a set on VMS.

### File dates and time zones

VMS keeps file dates in local time, so `ods2` stamps new files (and
revised directories) with the time in the zone your VMS system runs
on, not in UTC. By default that's the host's own zone, which honours
`TZ`. If the guest's zone differs from the host's, set `ODS2_TZ` to
the guest's zone; it takes precedence over `TZ` and affects only
`ods2`:

```bash
ODS2_TZ=America/New_York ./ods2 transfer.dsk COPY build.com [CONVERT]
export ODS2_TZ=UTC        # for a VMS system that really runs on UTC
```

Any value `TZ` accepts works, including POSIX rules such as
`EST5EDT,M3.2.0,M11.1.0`. A zone name the host doesn't recognise
quietly falls back to UTC, so check the spelling if dates still look
off. If they're wrong, `ANALYZE/DISK_STRUCTURE` on VMS reports
`FUTCREDAT` / `FUTREVDAT` for files dated ahead of its clock.

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

`make test` builds and runs 20 test suites and five scripts that check
the `ods2` command line, with `-fsanitize=address,undefined` on by default for development
builds (`make release` builds without them once things are stable).
Most tests run entirely offline against a synthetic disk
image assembled from real, VMS-written byte fragments captured during
development with VMS generated volumes so there was genuine on-disk bytes
in every structural test.

## Known limitations

- `COPY` takes any size of file the volume has room for. Its content
  goes in as few pieces as the free space allows (one, on a volume
  with a long enough free run), described by Format 1, 2 or 3
  retrieval pointers, continuing into extension headers on fragmented
  free space - up to 1,024 pieces. `COPY` and `TYPE` stream the
  content in 1 MiB chunks, so memory use stays flat however large the
  file. Input with no length to go by (a pipe, `/dev/stdin`) is first
  copied to a temporary file (in `$TMPDIR`, or `/tmp`), so that needs
  room for it. Directories still grow only within one header.
- No INITIALIZE command (formatting a *new* volume from scratch) - see
  `samples/README.md` for why that's a deliberate choice (it's complicated)
  and how to get a real, ready-to-use volume instead.
- `COPY` can't add a new version of an existing file - it creates
  version 1 of a new name only.
- `TYPE` and `DELETE` don't take wildcards yet, in file names or
  directories (VMS's do); only `DIR` does.
- INDEXF.SYS is extended as new files need it, anywhere on a volume
  of any size (Format 1 retrieval pointers below LBN 4,194,304,
  Format 2 above it), up to what its own header's map area holds -
  a few dozen extensions of up to 256 blocks (one header each), so
  on the order of ten thousand files beyond the headers INITIALIZE
  preallocated (`/HEADERS`). INDEXF.SYS extension headers aren't
  written.
- File numbers stop at 65,535 (the FID_NMX extension isn't used): a directory entry that names a file with an
  extended file number, or on another volume of a set (a relative volume number), is refused, never read as some
  other file. A file with more than 1,024 extents (header chains included) is refused for reading and deleting.
- One writer or any number of readers: the image is `flock()`ed (`ODS2_NOLOCK=1` turns that off); a command that
  finds it locked by another reader mounts it read-only.
- The reserved files (INDEXF.SYS ... BADLOG.SYS, and SECURITY.SYS) can't be deleted, and a delete is written in the
  order VMS uses, so that an interruption never leaves a listed file whose blocks are free.
- A directory is limited to 256 blocks (a few thousand entries), the
  most one contiguous extent can map here.

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

