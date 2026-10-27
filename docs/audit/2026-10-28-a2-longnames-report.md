# Roadmap 1.2 A2, first PR: long names in cosmofs (format version 15), 2026-10-28

A2 (`docs/roadmap-1.2.md`): "on the installed system GNU make builds
itself ...; then `make` and `cc` build BusyBox 1.37.0 from source ... and
that BusyBox passes ... BusyBox's testsuite". An experiment on 2026-10-28
(an uncommitted harness that installed a release build, served the
tarballs and ran each step on the system) stopped at the first command:
`tar` could not unpack BusyBox's tarball onto the root,
`Filename too long`.

## Scope and baseline

Branch `a2-long-names` from main at `ba52d94c`. The owner chose, on
2026-10-28, to lift the limit in cosmofs within A2 rather than build in a
ramfs. A new feature, proven by its tests; no probe (`AGENTS.md`: the
probe convention is for defect fixes).

## Why

A cosmofs directory entry was one 64-byte slot with a name of at most 47
bytes (`CFS_NAME_MAX`). The VFS, the native `readdir` record and the
Linux personality all take 255. BusyBox 1.37.0's tarball has 35 names
over 47 bytes, all in its testsuite, the longest 73
(`var-do-not-expand-tilde-in-parameter-expansion-in-quotes.right`); GNU
make 4.4.1's has none. The limit was in no backlog line.

## The design

`docs/kernel-services/filesystem/cosmofs/design.md`, "Format version 15:
long names"; CF13 in `invariants.md`.

- **Several slots, one block.** A name of up to 48 bytes is one slot. A
  longer one is a head (the inode, type, `namelen` up to 255, the first
  48 bytes and `extra`, the count of continuations, in a byte that was
  padding) and `extra` continuations of 56 bytes each, marked with an
  inode number no inode has (`CFS_DIRENT_CONT`, ~0). A head and its
  continuations are consecutive slots of one directory block, so adding
  or removing a name is still one copy-on-write write of one block.
- **Raised on use.** As 12, 13 and 14 are: the first create, `mkdir`,
  `symlink` or rename that makes a name over 47 bytes raises the pool to
  15, through 12 (`cfs_snap_raise`), and says so on the console. A pool
  with no long name stays mountable by v1.0.14. A pool below 11, or one
  encrypted with a version-1 key block, cannot be raised and answers
  `-ENAMETOOLONG`, the old answer.
- **One set of helpers.** `cfs_dirent_check` (the slots an entry takes,
  or 0 for any malformed shape, before a byte of the name is read),
  `cfs_dirent_name`, `cfs_dirent_name_eq`, `cfs_dirent_free_run`,
  `cfs_dirent_put` and `cfs_dirent_clear` in `cosmofs_format.h`, used by
  lookup, create, rename, unlink, `readdir`, the checker and the fault
  injectors, and tested on the host.
- **`readdir`** still counts positions in slots; it returns a long name
  whole (assembled in a buffer beside its block, not on the stack) and
  resumes past it.
- **The checker.** A head's `namelen` and `extra` must agree, its
  continuations must follow it in its block, each marked, and a
  continuation reached on its own is `dir_bad`. Duplicates compare whole
  names.
- **The format marker** is `cosmofs-format-max=15`, so `cosmo-upgrade`
  still refuses a release whose kernel could not mount the root.
- **`tools/cosmofs-damage.py`** reads long names.

## Tests

- `cosmofs-longnames` (host): the helpers at 48, 49, 104, 105 and 255
  bytes, malformed shapes, free runs, clearing.
- `cosmofs-longname` (kernel): a version-11 pool stays 11 for short names
  and lookups and is raised to 15 by its first long name; files, a
  directory and a symbolic link with names of 48 to 255 bytes are
  created, read, listed whole through a buffer that holds one long entry
  at a time, renamed long to long, long to short and short to long, and
  removed; a 255-byte name that does not fit its block's last four slots
  goes to the next block; all of it survives a remount and the checker
  calls the pool clean. A version-10 pool refuses long names and stays 10.
- `cosmofs-check-longname`: a head with one continuation too few, and
  continuations whose head is gone, are each `dir_bad`; a 200-byte name
  twice is `dir_dup_name`.
- `cosmofs-ops`'s error case now asserts `-ENAMETOOLONG` for 256 bytes:
  64 is a valid name from version 15.

The first boot of the branch failed `cosmofs-longname` with a report
that was not clean (`dangling` naming inode `0xffffffffffffffff`): the
checker's walk stepped past a long name's continuations before it read
the entry, and so read a continuation as the entry. Fixed before the
validation below.

## Validation (2026-10-28, macOS host)

| Check | x86_64 | aarch64 |
|---|---|---|
| `make host-test` | PASS (`cosmofs-longnames` ok) | (host) |
| debug `make test` (4 CPUs) | PASS (192.5 s); `cosmofs-longname` 425 ms, `cosmofs-check-longname` 172 ms | PASS (189.9 s); 273 ms, 165 ms |
| debug `QEMU_SMP=1 make test` | PASS (174.4 s) | PASS (186.1 s) |
| `make test-smp2` | PASS (187.5 s) | PASS (182.9 s) |
| `make test-chaos` | PASS (200.2 s) | PASS (202.4 s) |
| `make test-harness-retry` | PASS (200.7 s) | PASS (189.3 s) |
| `BUILD=release make test` | PASS (19.1 s) | PASS (21.3 s) |
| `make analyze` | clean: 7 baselined, 0 unexpected | clean: 7 baselined, 0 unexpected |
| `make fuzz` | PASS | (host) |

## What is left of A2

Hard links (VFS-4) and the native `chmod` (VFS-6); the build tools
(`patch`, an `ld` that GNU make's `configure` will accept, an `xz`
compressor, `ar`); GNU make built on the system; BusyBox rebuilt there
with its testsuite, the acceptance test. The same experiment found that
GNU make's `configure` stops at "no acceptable ld found": its gettext
macro wants an `ld` on `PATH` even with `--disable-nls`, and tcc links by
itself.
