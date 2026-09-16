# cosmofs: architecture

The persistent copy-on-write filesystem of Phase 7. Constitution
sections 28 (storage stack), 29 (storage pool), 30 (transactional CoW:
"every committed root must describe a completely valid filesystem
state"), 31 (checksums), 32 (snapshots as immutable roots).

## Where it sits

```text
   VFS (struct fs_type cosmofs_fs_type, struct vnode_ops)     kernel-services/vfs/
        │
   kernel-services/filesystem/cosmofs/
        cosmofs.c        vnode operations: lookup/create/mkdir/unlink/rmdir/rename/readdir,
                         readpage/writepage/truncate, extents, directories, eviction
        cosmofs_core.c   metadata buffer cache, copy-on-write, bitmap allocator, inode map,
                         commit, format, mount/unmount, stats, the crash test hook
        cosmofs_format.h the on-disk layout (host-testable, no kernel dependencies)
        cosmofs_internal.h shared in-memory state
        │
   kernel-services/storage/pool.c   pool_read/write/flush in 4 KiB blocks
        │
   kernel/block/ → virtio_blk.ko → vda
```

cosmofs sees pool blocks only; it never names a block device. The page
cache (owned by the VFS) holds file data; cosmofs supplies `readpage`
and `writepage`. Directory blocks bypass the page cache and go through
the pool directly.

## Purpose

A filesystem whose on-disk state is always a valid tree: every mutation
lands in blocks that the last committed root does not reference, and a
commit publishes a new root with one superblock write. A crash at any
point leaves either the old root or the new one. Recovery is choosing
the newer valid superblock slot; there is no journal to replay. From
version 9 a root also names the blocks it freed, so a mount can return
the space the last commit could not write -- a list of block numbers
the root makes true, not a log of operations to replay. In the
constitution's words: designed to provide crash consistency and a
recoverable structure without journal replay, not "immune to
corruption".

## Responsibilities

- The on-disk format (`CFS_VERSION` 9, mounting back to
  `CFS_VERSION_MIN` 2; what each version added is in `design.md`): two
  superblock slots, a two-level
  inode map, 256-byte inodes with 10 direct extents and a chain of
  extent blocks, hole-capable extents that carry their logical position,
  a per-inode checksum tree over data and directory blocks, a bitmap
  allocator behind an allocation index, directories as 64-byte entries
  in file data, CRC32C and self-numbering on every metadata block.
- Transactions: one open generation per mount; copy-on-write of
  metadata (`cfs_buf_cow`), always-new blocks for data and directory
  writes, deferred frees recorded in the root that made them
  (`free_root`, so a mount can finish what the last commit started),
  the reserve-then-write bitmap fixpoint, the
  commit into the alternate superblock slot with a flush before and
  after (`BIO_PREFLUSH | BIO_FUA`), a metadata reserve, `fsync` as a
  commit, a writeback thread with dirty and age thresholds, and the
  older-slot fallback when the newer root's tree does not load.
- Formatting a device (`cosmofs_format`), mounting (slot selection,
  bitmap load, the free record's replay, free-count reconciliation),
  unmounting (commit, or
  discard under the test hook), statistics.
- Inode semantics: types regular, directory and symbolic link, link
  counts, sizes, times, owner ids stored, parent pointers for `..`,
  freeing of blocks and inode slots when an unlinked inode's last
  reference goes.
- Snapshots (version 3), many members (4), mirrored members (5),
  compressed records (6), encryption at rest (7) and symbolic links (8),
  each described under its own heading in `design.md`.
- Two maintenance passes over a mounted filesystem, in every build and
  reachable by an operator through `/dev/fsctl`: the scrub
  (`cosmofs_scrub`), which asks whether every block is still what was
  written, and the structural check (`cosmofs_check`), which asks
  whether the blocks add up. Each is offered to the VFS through
  `struct fs_type`.

## Non-responsibilities

- A pool-wide checksum tree (the superblock's `csum_root` stays
  reserved; checksums are per inode), parity or erasure coding across
  the members of a pool (mirroring is built, format version 5), quotas,
  hard links, inode number reuse, transaction groups pipelined behind an
  open one, a host `mkfs` and an *offline* checker over a
  block device (the mounted one is built — `design.md`, "The structural
  check"), a *scheduled* pass (nothing runs either one on a timer; an
  operator starts them), and any performance work beyond
  contiguity-aware allocation (linear directories, one lock per
  filesystem).

## Interfaces at a glance

| Interface | Where | Used by |
|---|---|---|
| `cosmofs_fs_type`, `cosmofs_init` | `kernel/cosmofs.h` | `kernel_main`, the VFS registry |
| `cosmofs_format`, `cosmofs_stats`, `cosmofs_test_discard_on_unmount`, `cosmofs_test_set_writeback`, `cosmofs_test_set_writeback_interval` | `kernel/cosmofs.h` | self-tests |
| `struct cfs_super`, `cfs_mhdr`, `cfs_inode`, `cfs_extent`, `cfs_dirent`, index helpers | `cosmofs_format.h` | the implementation, `tests/host/test_cosmofs.c` |
| `cosmofs_scrub`, `cosmofs_check` (+ `struct cosmofs_check_report`, `COSMOFS_CHECK_REPAIR`) | `kernel/cosmofs.h` | the self-tests, the crash suite, and `struct fs_type`'s `check`/`scrub` entries, through which `/dev/fsctl` reaches them; in every build |
| `cosmofs_test_corrupt` (nine named corruptions) | `kernel/cosmofs.h` | the checker's tests: eight of the ten finding classes have a test that manufactures exactly one. `chain_cycle` has none, and four of `dir_bad`'s six sites have none; both are inventory rows |
| `pool_*` | `kernel/storage.h` | cosmofs (its only I/O path) |

See `design.md` for the layout and the transaction model, and
`docs/kernel-services/vfs/{api,invariants,testing}.md` for the API,
the rules (V2–V6, V15–V18) and the tests.
