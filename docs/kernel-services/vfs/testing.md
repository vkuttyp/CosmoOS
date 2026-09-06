# VFS and storage: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| Host | `test_cosmofs` (on-disk layout sizes, inode/imap index arithmetic, extent mapping, the version-3 snapshot structures and the snapshot inode-tag arithmetic) and the CRC32C vectors in `test_crypto` | `make host-test` |
| Target | Seven self-tests: `crc32c`, `pagecache`, `vfs-ramfs`, `pool`, `cosmofs-format`, `cosmofs-ops`, `cosmofs-crash`; since audit milestone 7 `cosmofs-holes`, `cosmofs-csum`, `cosmofs-fsync`, `cosmofs-reserve`, `cosmofs-fallback`, `cosmofs-writeback`, `cosmofs-badmap` on RAM devices and `blk-queue` for the block layer's pending queue and bio flags; since the verification milestone `cosmofs-replay` (crash consistency over every prefix of the write stream) and `fault-blk` (device errors) on a RAM block device; since the storage milestone `cosmofs-snapshot` (history kept while the live tree moves, writes refused, exact reclaim when a snapshot is deleted) and `cosmofs-snapshot-remount` (a snapshot survives an unmount); since audit milestone 6 `cache-limits` and `cache-budget-race` (the ramfs page budget, also under two concurrent writers, and the global page-cache limit with reclaim, `docs/kernel/security/testing.md`) | `make test` |
| Host fuzz | `fuzz_cosmofs`: mount, walk and read mutated images under ASan/UBSan (`docs/verification/`) | `make fuzz` |
| User mode | `init --selftest` runs `fs_selftest()` against ramfs and then mounts the cosmofs the kernel tests left on the scratch disk (`USERTEST: PASS` required) | `make test` |
| Shell | `/etc/rc.test` mounts the cosmofs the `nvme` self-test leaves on `nvme0n1`, takes a snapshot with `mkdir .snapshots/shell`, reads the old contents back through it, checks a write is refused, and deletes it with `rmdir` (`SNAPTEST: PASS` required on both machines; `SNAPTEST: skipped` is a forbidden marker) | `make test` |

The boot test's total was `SELFTEST: PASS (51 tests)` at the end of
Phase 7 (58 since Phase 8 added the network tests). The process tests
(`process-reject`, `process-user`, `process-fault`) are the **last**
entries of the self-test table so that `init --selftest`, which they
run, finds the scratch disk already formatted and populated by the
cosmofs tests.

## Host tests

`tests/host/test_cosmofs.c` compiles only `cosmofs_format.h` (no kernel
code) and checks: `struct cfs_mhdr` 32 bytes, `struct cfs_inode` 256,
`struct cfs_dirent` 64, `struct cfs_extent` 16, the superblock fits a
block, 508 pointers per block, 15 inodes per block, 32512 bits per
bitmap, 254 extents per indirect block, 64 dirents per block, 264
extents per file; inode→block/slot and imap L0/L1 index arithmetic
including the first inode of L1 index 1 and the last representable
inode; `cfs_map_block` over three runs including the hole beyond them.
`tests/host/test_crypto.c` gained `crc32c`: the `123456789` vector,
the empty string, 32 zero bytes, and incremental equals one-shot.

## Self-tests (`kernel-services/vfs/vfstest.c`, `kernel-services/filesystem/cosmofs/cosmofstest.c`)

**`crc32c`**: the standard vector and incremental update.

**`pagecache`**: on a fresh `/tmp/pc-test`: a 100-byte write at offset
`2*PAGE+50` grows the file to `2*PAGE+150`; a full read returns zeros for
the two hole pages and the written bytes; after that read the cache
holds 3 pages of which 1 is dirty (holes read in clean); a 4000-byte
write crossing a page boundary reads back; `file_sync` clears dirty;
truncate to `PAGE+10` leaves 2 pages, reads stop at the new size, and a
later write at `PAGE+100` sees zeros in the truncated tail.

**`vfs-ramfs`**: the root is a directory with ino 1; `/boot/init` and
`/boot/modules/hello.ko` exist; paths with repeated slashes, `.` and
`..` resolve; a trailing slash on a file is `-ENOTDIR`; a file is
created, written twice, read back through a second open (write-only and
read-only handles refuse the wrong direction, `O_APPEND` appends,
`O_EXCL` on an existing file is `-EEXIST`, `O_DIRECTORY` on a file is
`-ENOTDIR`, opening a directory for writing is `-EISDIR`, `O_ACCMODE`
alone is `-EINVAL`); directories: `mkdir` chain with `nlink` counts,
readdir entry count, renames across directories, moving a directory
beneath itself (`-EINVAL`), replacing renames, `rmdir` of a non-empty
directory (`-ENOTEMPTY`), `rmdir` on a file (`-ENOTDIR`), `unlink` on a
directory (`-EISDIR`), `unlink("/")` (`-EEXIST`), `rmdir("/tmp/..")`
(`-EINVAL`); an open file survives `unlink` (readable, `nlink` 0,
`VNODE_DEAD`); a second ramfs mounted on `/mnt`: stacking (`-EBUSY`),
missing target (`-ENOENT`), unknown filesystem (`-ENODEV`), `..` from
the mount root reaches the global root, cross-mount rename (`-EXDEV`),
unmount while a file is open (`-EBUSY`), `rmdir` of a mountpoint
(`-EBUSY`), unmount, unmount again (`-EINVAL`), unmount `/` (`-EBUSY`);
the mount count and vnode count return to their starting values.

The cosmofs tests on `vda` skip with a log line when no `vda` exists.
They format the scratch disk (`make test` creates a fresh 8 MiB one per
run: 2048 blocks, 2041 free after format; format version 2 since
milestone 7, so a disk formatted by an older kernel is refused with a
message naming the version). The milestone 7 tests use RAM devices and
run everywhere.

**`pool`**: `pool_open` on `vda` (4096-byte blocks, 8 sectors each,
`nblocks = capacity / 8`); write, flush and read back the last block;
`-EINVAL` past the end.

**`cosmofs-format`**: mounting the zeroed disk is `-EIO`; after
`cosmofs_format`, mount succeeds at generation 1 with 1 inode and
`total - 7` free blocks; the root is a directory, ino 1, `nlink` 2;
lookups miss; unmount and remount keep generation 1 (nothing was
committed).

**`cosmofs-ops`**: creates `/mnt/hello.txt` ("hello from the kernel"),
`/mnt/dir`, `/mnt/dir/nested.txt`; checks `nlink` bookkeeping and `..`
from a subdirectory; writes a 45-block-plus-123-byte file, rewrites 5000
bytes in the middle (splitting and merging runs), syncs and reads it
back exactly; replacing rename, rename to the parent, `rmdir`, renaming
a file onto a removed directory's name, `unlink`; `vfs_sync` advances
the generation by one; unmount and remount show every file with its
content and `..` still correct; deleting the big file and committing
returns more than 40 blocks and one inode; `O_TRUNC` re-extends
`hello.txt`; a 48-byte name is `-ENAMETOOLONG`, `rmdir` of a non-empty
directory `-ENOTEMPTY`, cross-mount rename `-EXDEV`. It leaves
`/mnt/hello.txt` and `/mnt/dir/nested.txt` on the disk.

**`cosmofs-crash`**: creates a file and a directory, unlinks
`hello.txt`, overwrites `dir/nested.txt`, then arms
`cosmofs_test_discard_on_unmount` and unmounts: the remount shows the
same generation and free count as before, none of the changes, and the
original contents. Then it corrupts one byte of the superblock slot
holding the newer generation through the pool: mount falls back to the
older slot (generation minus one) and every file still reads; a commit
rewrites the torn slot and a final remount verifies the two fixture
files. The disk is left holding `hello.txt` and `dir/nested.txt` for
init.

**`cosmofs-replay`** (`kernel-services/filesystem/cosmofs/cosmofscrash.c`,
`docs/verification/design.md`): formats a 512-block RAM device, records
every write and flush of a workload with five sync points (creates,
rewrites, renames, unlinks), then for every sampled prefix of the log,
intact and with the last write torn, restores the formatted image,
replays the prefix, mounts, and requires every file committed by the
last sync whose superblock write is in the prefix to read back exactly,
and every directory and file to walk and read cleanly. 75 writes, 139
prefix images per boot; the property is the commit rule in `design.md`
made a machine check.

**The transaction-engine tests** (milestone 7, `cosmofstest.c`, each on
its own RAM device mounted at `/mnt/eng` with the writeback thread off
unless the test is about it): **`cosmofs-holes`** writes four bytes
200 MiB into a file on a 4 MiB device (five blocks consumed, no zero
fill), reads zeros in the holes and the data at its offsets, fills a
block in the middle of the hole and the first block, remounts, and
truncates (the checksum tree of the emptied file is freed too).
**`cosmofs-csum`** finds a file's data block by its pattern through the
pool, flips a byte, and the read is `-EIO` while another file reads
fine and the counter rises; a rewrite repairs it; then the block of a
one-entry directory is flipped and the lookup is `-EIO`.
**`cosmofs-fsync`** writes and `file_sync`s one file (the generation and
the commit count advance by one), writes another without, discards the
transaction at unmount: the first survives, the second does not.
**`cosmofs-reserve`** fills a 256-block device with 4 KiB writes until
`-ENOSPC` (at the sync that allocates), checks the free count stopped at
the 32-block reserve, unlinks two files, commits, and writes a new file.
**`cosmofs-fallback`** commits generations 2 and 3, corrupts generation
3's inode-map root, and mounts: generation 2 with a warning, the file of
generation 3 absent; a commit and remount show the pair healthy again.
**`cosmofs-writeback`** turns the thread on with a 50 ms interval,
writes one file, and sees the generation and the thread's commit count
advance without any sync call; nothing further commits while nothing is
dirty; a discarded unmount and remount still show the file.

**`cosmofs-badmap`** writes a two-run file (blocks 0 and 5), walks
superblock → IMAP1 → IMAP0 → INODES through the pool, swaps the inode's
two direct runs and re-seals the block, and mounts: both reads are
`-EIO`; before the direct runs were validated on the map fast path the
first block read as a hole of zeros (Greptile on PR #22).

**`blk-queue`** (`kernel/block/blktest.c`): a RAM device in deferred
mode with two slots takes eight concurrent writes through `blk_submit`
without one `-EAGAIN`; all complete in order and six waited in the
pending queue; a `BIO_PREFLUSH | BIO_FUA` write is recorded as flush,
write, flush (and as write, flush with `BIO_FUA` alone); a flagged read
and a flagged write past the end are `-EINVAL` before anything is
submitted; an asynchronous flagged write returns with the caller's `done`
and `arg` intact (the sequence borrows the field and gives it back).

**`fault-blk`** (`kernel/core/faulttest.c`): completion and submission
errors injected under a cosmofs workload; every write and sync returns
`-EIO` or succeeds, a forced unmount and a clean remount read every
visible file back.

## User-mode test (`userland/init/init.c`, `fs_selftest`)

Run by `process-user` (as `init --selftest`): `stat` of `/boot/init` and
`/boot`, `-ENOENT`, `-ENOTDIR`; create/write/`fstat`/`lseek`/read/EOF on
`/tmp/usertest.txt`, negative seek `-EINVAL`, `SEEK_END`; close twice
(`-EBADF`); read-only handle refuses `write` (`-EBADF`); `O_EXCL`
`-EEXIST`, missing `-ENOENT`, directory for writing `-EISDIR`, bad path
pointer `-EFAULT`; `mkdir` (twice: `-EEXIST`), `rename` into it,
`getdents` sees `.`, `..` and the file then returns 0; `rmdir` non-empty
`-ENOTEMPTY`, `unlink` on a directory `-EISDIR`, `unlink`, `rmdir`,
`sync`; `mount("vda", "/mnt", "cosmofs", 0)` then reads the 21 bytes of
`/mnt/hello.txt`, stats `/mnt/dir/nested.txt`, unmounts, and the path
is gone; `umount("/")` is `-EBUSY`. If no disk is present the mount
must fail with `-ENODEV` or `-EIO` and the disk part is skipped. Expected
log line: `usertest: cosmofs mounted and read from user mode`.

## Running

```sh
make host-test                       # 5 binaries incl. test_cosmofs
make test                            # all self-tests (58 since Phase 8) + USERTEST on the scratch disk
make BUILD=release test              # cosmofs still formats and mounts, no self-tests
QEMU_TESTDISK=/tmp/d.img make run    # keep a formatted disk between runs
```

`make run` attaches the same devices; a disk formatted by a previous
`make test` mounts with `mount("vda", "/mnt", "cosmofs", 0)` from init.

## Gaps

- No fuzzing of on-disk images beyond one corrupted superblock byte;
  no fault injection inside the commit sequence.
- No concurrency stress (every test is single-threaded); lock order is
  reviewed, not checked.
- No host `mkfs`; `cosmofs_format` runs only in the kernel.
- `-EPERM` on `mount` has no test until a non-root process exists.
- Files with more than 4096 runs (`CFS_MAX_EXTENTS`, the implementation's
  fragmentation bound) are not exercised; full disks are
  (`cosmofs-reserve`). `cache-limits` still syncs its 2 MiB file every
  64 pages, a habit from the 264-run cap that no longer exists.
