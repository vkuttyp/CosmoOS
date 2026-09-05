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
the newer valid superblock slot; there is no journal to replay. In the
constitution's words: designed to provide crash consistency and a
recoverable structure without journal replay, not "immune to
corruption".

## Responsibilities

- The on-disk format (version 2): two superblock slots, a two-level
  inode map, 256-byte inodes with 10 direct extents and a chain of
  extent blocks, hole-capable extents that carry their logical position,
  a per-inode checksum tree over data and directory blocks, a bitmap
  allocator behind an allocation index, directories as 64-byte entries
  in file data, CRC32C and self-numbering on every metadata block.
- Transactions: one open generation per mount; copy-on-write of
  metadata (`cfs_buf_cow`), always-new blocks for data and directory
  writes, deferred frees, the reserve-then-write bitmap fixpoint, the
  commit into the alternate superblock slot with a flush before and
  after (`BIO_PREFLUSH | BIO_FUA`), a metadata reserve, `fsync` as a
  commit, a writeback thread with dirty and age thresholds, and the
  older-slot fallback when the newer root's tree does not load.
- Formatting a device (`cosmofs_format`), mounting (slot selection,
  bitmap load, free-count reconciliation), unmounting (commit, or
  discard under the test hook), statistics.
- Inode semantics: types regular and directory, link counts, sizes,
  times, owner ids stored, parent pointers for `..`, freeing of blocks
  and inode slots when an unlinked inode's last reference goes.

## Non-responsibilities

- A pool-wide checksum tree (the superblock's `csum_root` stays
  reserved; checksums are per inode), snapshots (`snap_root` reserved;
  every committed root is already immutable), multi-device pools and
  redundancy (`members` reserved, the pool is the seam), compression,
  quotas, symbolic or hard links, inode number reuse, transaction groups
  pipelined behind an open one, a host `mkfs` or `fsck`, and any
  performance work beyond contiguity-aware allocation (linear
  directories, one lock per filesystem).

## Interfaces at a glance

| Interface | Where | Used by |
|---|---|---|
| `cosmofs_fs_type`, `cosmofs_init` | `kernel/cosmofs.h` | `kernel_main`, the VFS registry |
| `cosmofs_format`, `cosmofs_stats`, `cosmofs_test_discard_on_unmount`, `cosmofs_test_set_writeback`, `cosmofs_test_set_writeback_interval` | `kernel/cosmofs.h` | self-tests |
| `struct cfs_super`, `cfs_mhdr`, `cfs_inode`, `cfs_extent`, `cfs_dirent`, index helpers | `cosmofs_format.h` | the implementation, `tests/host/test_cosmofs.c` |
| `pool_*` | `kernel/storage.h` | cosmofs (its only I/O path) |

See `design.md` for the layout and the transaction model, and
`docs/kernel-services/vfs/{api,invariants,testing}.md` for the API,
the rules (V2–V6, V15–V18) and the tests.
