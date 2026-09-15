# VFS and storage: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| Host | `test_cosmofs` (on-disk layout sizes, inode/imap index arithmetic, extent mapping, the version-3 snapshot structures and the snapshot inode-tag arithmetic), `test_lz4` (round trips including overlapping matches and continued lengths, and malformed streams that must be refused rather than copied past a buffer), `test_chacha20` (RFC 8439's own vectors for ChaCha20 and Poly1305, plus the properties the filesystem depends on: the tag is over ciphertext, a forgery never becomes plaintext, and the same key and nonce give the same bytes) and the CRC32C vectors in `test_crypto` | `make host-test` |
| Target | Seven self-tests: `crc32c`, `pagecache`, `vfs-ramfs`, `pool`, `cosmofs-format`, `cosmofs-ops`, `cosmofs-crash`; since audit milestone 7 `cosmofs-holes`, `cosmofs-csum`, `cosmofs-fsync`, `cosmofs-reserve`, `cosmofs-fallback`, `cosmofs-writeback`, `cosmofs-badmap` on RAM devices and `blk-queue` for the block layer's pending queue and bio flags; since the verification milestone `cosmofs-replay` (crash consistency over every prefix of the write stream) and `fault-blk` (device errors) on a RAM block device; since the storage milestone `cosmofs-snapshot` (history kept while the live tree moves, writes refused, exact reclaim when a snapshot is deleted, a deletion refused with `-EBUSY` while a file inside the snapshot is open, and `..` staying inside history), `cosmofs-pool2` (a pool of two members: allocation across both, per-member free counts, assembly by label on remount, a snapshot spanning both) , `cosmofs-v3` (the previous on-disk format still mounts and is written), `cosmofs-badmembers` (a member table that cannot be true is refused at mount), `cosmofs-mirror-stale` (two members of two copies each, with the stale device being the one labelled first: the mount passes over it, comes up with three devices and one degraded copy, and reads correctly), `cosmofs-crypt` (an encrypted filesystem: the plaintext is not on the disk, a wrong key is refused, a tampered block is refused rather than returned, a scrub with no key reads and repairs everything, rotation rewrites one block, and a keyless remount refuses to walk a path), `cosmofs-compress` (a compressible file stored in a fraction of its blocks and an incompressible one stored as it is, a page rewritten inside a record, a partial page that has to read the record first, truncation into the middle of a record and re-extension reading zeros) and `cosmofs-mirror` (two copies of a member: rot one copy of a data block and the read still answers and repairs it, scrub finds rot on a copy no read would have touched, both copies gone is `-EIO` for that file alone, and a device that missed a commit is left out of the mirror rather than serving old blocks), `cosmofs-pool2` (a pool of two members: allocation across both, per-member free counts, assembly by label on remount, a snapshot spanning both) and `cosmofs-v3` (the previous on-disk format still mounts and is written) and `cosmofs-snapshot-remount` (a snapshot survives an unmount); since audit milestone 6 `cache-limits` and `cache-budget-race` (the ramfs page budget, also under two concurrent writers, and the global page-cache limit with reclaim, `docs/kernel/security/testing.md`); since the file-path unit `read-bounce`, `wb-error-fsync`, `wb-error-once`, `wb-error-close`, `wb-error-lost`, `read-bench`, `write-bench` | `make test` |
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

**`vfs-put-race`**: one bare vnode on the root mount per round, one
reference per racer, the racers pinned to CPUs 1..n−1 and spinning on a
generation counter so they reach `vnode_put` together; 4 000 rounds or
500 ms, whichever first, then the vnode census must equal what it was
before. The version of `vnode_put` that read the count before deciding
whether to unhash panics in the release assertion within seconds under
this test (two drops from 2 both read 2); the decrement-and-lock version
runs 4 000 rounds in 320 ms on x86_64 and 150 ms on aarch64 with every
vnode released once. The racers must not occupy every CPU: the first
version did, and the driving thread ran only on preemption ticks.
Skipped below three CPUs.

**`vfs-chrdev-open`**: a synthetic character device with the per-open
lifecycle. Two opens get distinct per-open instances (each read returns its
own instance's id); `release` runs exactly once, on the last reference and
not on an earlier `file_put`; a refused `open` leaves no file and runs no
`release`. Proved by removing the `dev_open` gate — the refused open's file
then runs `release` and the count is wrong.

### The file path (`docs/audit/next-subsystem-file-path.md`)

**`read-bounce`**: the syscall bounce's sizing (the stack chunk for 512
bytes and 1 KiB, the heap for 64 KiB, the 64 KiB ceiling for 100 000);
with `FI_KMALLOC` refusing the allocation, the stack chunk and the
fallback counter, never an error; a 200 KiB ramfs file of a known
pattern answers one `file_read` of 64 KiB with 64 KiB of the right bytes
and 3 KiB from its end with 3 KiB; a pipe holding 300 bytes answers a
64 KiB request with 300. The syscall's wiring (a user address) is
`fs_selftest`'s, below.

**`wb-error-fsync`**, **`wb-error-once`**, **`wb-error-close`**,
**`wb-error-lost`**: cosmofs on a RAM block device, which completes a
bio in its submitter's context, so `FI_BLK_COMPLETE` scoped to the test
thread refuses exactly the next `budget` write-backs the test issues
(`faultinject_stats().hits` equals the budget after every phase, so a
run in which nothing was refused fails on that count, not on a 0 that
meant "nothing was tried"). fsync: three dirty pages, one refusal,
`-EIO`, the pages still dirty, `wb_errors` +1, the next `fsync` 0, a
fresh mount reads the data. once: files A and B on one vnode, A's
attempt refused (`-EIO`, A told), B's attempt writes and B is told once
(`-EIO` then 0), A's second is 0, C opened afterwards is 0. close: a file
in a handle table, one refusal, `handle_close` returns `-EIO` and the
slot is `-EBADF` after it; the release's retry writes the page, so
`dropped_dirty` is unchanged and a fresh mount reads it; a clean close
returns 0. lost: three refusals (the flush, the file release's retry,
the vnode release's last attempt): `-EIO`, `dropped_dirty` +1,
`wb_errors` +3, the `lost` line logged once, and the mount still writes
and syncs a new file.

**`read-bench`**, **`write-bench`**: print, assert nothing. A 1 MiB file
read and written through `file_read`/`file_write` with kernel buffers of
1, 4 and 64 KiB -- the object path per request size -- on ramfs and, for
reads, on cosmofs over the RAM block device cold (after a remount,
through the device) and warm (the page cache). The syscall side is
`fs_selftest`'s `USERBENCH` lines.

### Symbolic links

| test | what it asserts |
| --- | --- |
| `vfs-symlink` | create, `readlink` returns the exact bytes **with no terminator**, `lstat` reports the link and its target's length, `stat` reports the target's type and size, removing the link leaves the target |
| `vfs-symlink-walk` | a link to a directory is walked through; a relative target resolves against the link's own directory; `..` after an expansion names the target's parent; an absolute target; a link to a link |
| `vfs-symlink-loop` | `a -> b -> a` and a self-link are `ELOOP`; a chain of 8 resolves and one of 9 does not; the budget is pinned at 8 so raising it fails the test rather than moving it; `lstat` and `readlink` still answer on a looping link |
| `vfs-symlink-nofollow` | `O_NOFOLLOW` is `ELOOP` on a link named last, takes a file, and says nothing about links in between; a dangling link opens `ENOENT` while `lstat` and `readlink` succeed; an over-long target is refused, and so is a target that fits alone but not with the remainder after it |
| `cosmofs-symlink` | a link and its target survive unmount and remount with a cold cache; one block per link, given back when it goes |
| `cosmofs-symlink-version` | a version-7 filesystem mounts, works, and refuses `symlink` with `-EOPNOTSUPP` |
| `fs_selftest` (user mode) | `symlink`, `readlink`, `lstat` and `O_NOFOLLOW` through the libc wrappers, and what `ls -l` prints for a link: the type column and the arrow |
| the jail test (user mode) | a child rooted at `/tmp/jail` writes through an absolute-target link and lands inside its own root, while the file that target names outside is untouched |
| `lxtest` | `readlink`, `readlinkat`, `symlink`/`symlinkat`, `lstat` and `newfstatat` with `AT_SYMLINK_NOFOLLOW` each report the link, not the target |

### The operator's channel (`docs/audit/next-subsystem-fsctl.md`)

| test | what it asserts |
| --- | --- |
| `vfs-mount-id` | two mounts get two ids; a mount at a path an unmount just freed gets a **new** id, which an index would not; the root has one and it is neither |
| `vfs-mount-pin` | an unmount begun while a pass is held does not complete; a second acquisition is refused once it has begun, which is what makes the drain terminate; a second unmount is refused; releasing wakes the drain and the unmount takes the mount |
| `fsctl-list` | every mount the namespace holds appears once, at its path, with its type and passes; the root is there at `/`; `count == total`; a mount dropped from this namespace leaves the listing while the machine still counts it |
| `fsctl-check` | a leak found by block number through the device, with the same numbers the pass reports when called directly; repaired through the device; `-EOPNOTSUPP` for a filesystem with no such pass, `-ENOENT` for a name nothing holds, `-EINVAL` for a version or a size the channel does not know |
| `fsctl-result-per-open` | two open files run two commands and each reads its own; a file that has asked nothing reads zero bytes; a result survives being read twice; a buffer too small is refused rather than truncated |
| `fsctl_selftest` (user mode) | the tool lists, is refused on a filesystem with no passes, and checks, scrubs and repairs a real cosmofs -- the first time either pass has run from userland |

**What is not asserted**, and is an inventory row rather than a
comment: no test attempts an unprivileged open, because kernel
self-tests and the user-mode suite both run as root; and no test fires
`vfs_umount2`'s second-unmount guard, because the path walk refuses
first and the door that reaches it is a relative path resolved from
inside the mount.

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
- Little concurrency stress beyond `vfs-concurrency`, `cache-budget-race` and `vfs-put-race`; lock order is
  reviewed, not checked.
- No host `mkfs`; `cosmofs_format` runs only in the kernel.
- `-EPERM` on `mount` has no test until a non-root process exists.
- Files with more than 4096 runs (`CFS_MAX_EXTENTS`, the implementation's
  fragmentation bound) are not exercised; full disks are
  (`cosmofs-reserve`). `cache-limits` still syncs its 2 MiB file every
  64 pages, a habit from the 264-run cap that no longer exists.
