# VFS and storage: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| Host | `test_cosmofs` (on-disk layout sizes, inode/imap index arithmetic, extent mapping) and the CRC32C vectors in `test_crypto` | `make host-test` |
| Target | Seven self-tests: `crc32c`, `pagecache`, `vfs-ramfs`, `pool`, `cosmofs-format`, `cosmofs-ops`, `cosmofs-crash` | `make test` |
| User mode | `init --selftest` runs `fs_selftest()` against ramfs and then mounts the cosmofs the kernel tests left on the scratch disk (`USERTEST: PASS` required) | `make test` |

The boot test's total is `SELFTEST: PASS (51 tests)`. The process tests
(`process-reject`, `process-user`, `process-fault`) are now the **last**
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

The cosmofs tests skip with a log line when no `vda` exists. They
format the scratch disk (`make test` creates a fresh 8 MiB one per run:
2048 blocks, 2041 free after format).

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
make test                            # 51 self-tests + USERTEST on the scratch disk
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
- Large files (more than 264 runs) and full disks (`-ENOSPC`) are not
  exercised.
