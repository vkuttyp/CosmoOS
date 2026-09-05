# VFS and storage: architecture

Constitution sections 37 (VFS), 28 (storage architecture: VFS → filesystem
→ page cache → block layer → storage pool → driver), 29 (unified storage
pool), 30 (copy-on-write filesystem with transactional roots), 31
(checksums), 32 (snapshots as immutable roots), invariant 4 (the VFS must
not depend on a specific filesystem), section 11 (handles), and the
Phase 7 roadmap entry: VFS, block layer, page cache, CoW filesystem,
storage pool.

## Where it sits

```text
   user process        open/read/write/stat/mkdir/unlink/rename/getdents/lseek/sync/mount
        │              (kernel/syscall/native.c → handles → struct file)
        ▼
   kernel-services/vfs/     vfs.c        mounts, path walk, vnodes, files, the fs_type registry
                            pagecache.c  per-vnode page cache: read-through, dirty pages, writeback
                            ramfs.c      in-memory filesystem (the root, /tmp, /boot from the archive)
        │
        ▼
   kernel-services/filesystem/cosmofs/   the on-disk CoW filesystem: superblock slots, inode map,
                                          extents, bitmap allocator, transactions, checksums
        │
        ▼
   kernel-services/storage/pool.c        the storage pool: block addressing over member devices
        │                                (one member in this phase)
        ▼
   kernel/block/ (Phase 6)  ──►  virtio_blk.ko  ──►  the scratch disk
```

The VFS knows filesystems only through `struct fs_type` and vnodes only
through `struct vnode_ops`. cosmofs knows the pool, never a block
device; the pool knows block devices, never a filesystem. User space
reaches files through handles on `struct file` kobjects, so the existing
`read`/`write`/`close` system calls work on files without change.

## Purpose

Give the kernel a namespace: a tree of directories and files rooted at
`/`, assembled from mounted filesystems, reachable by path from user
programs through handles, with an in-memory filesystem for the boot
namespace and a persistent copy-on-write filesystem whose every
committed root describes a completely valid state.

## Responsibilities

- **VFS core** (`kernel/include/kernel/vfs.h`): `fs_type` registry;
  `mount` objects and the mount table; `vnode` (the in-memory inode: a
  `kobject` with type, size, times, operations, a page cache and a
  lock); path resolution across mount points with `.` and `..`; `file`
  objects (a `kobject_io_type`: position, flags, rights) usable through
  handle tables; the operations `open`, `close`, `read`, `write`,
  `seek`, `stat`, `mkdir`, `unlink`, `rmdir`, `rename`, `readdir`,
  `sync`, `mount`, `umount`.
- **Page cache** (`pagecache.c`): per-vnode index-to-frame map, generic
  `read`/`write` for regular files built on the filesystem's `readpage`
  and `writepage`, dirty tracking, writeback on `sync`, truncation.
  Filesystems that cache in memory only (ramfs) use it as their store.
- **ramfs**: directories and regular files entirely in memory; the
  root filesystem at boot with `/boot` populated from the boot archive,
  the archive's `bin/`, `sbin/`, `etc/` entries placed at `/bin`,
  `/sbin`, `/etc` (programs executable, Phase 9), and `/tmp`, `/mnt`,
  `/dev` created.
- **Storage pool** (`kernel-services/storage/`): `pool_open(blkdev)`,
  `pool_read/write/flush` in 4 KiB pool blocks. One device now; the
  interface is where allocation groups and redundancy attach later.
- **cosmofs** (`kernel-services/filesystem/cosmofs/`): the persistent
  filesystem. Two superblock slots with generation numbers, an inode
  map tree, extent-mapped files, a bitmap allocator, directories as
  data, CRC32C on every metadata block, and a transaction model in
  which nothing committed is overwritten: modified blocks are copied,
  the new root is written last, frees are deferred to the next
  generation. `cosmofs_format()` creates a filesystem on a pool.
- **System calls**: the native personality grows from 11 to 23 calls;
  numbers stay stable. Files carry handle rights derived from the open
  flags; `mount`/`umount` require uid 0.
- **CRC32C** (`kernel/core/crc32c.c`): the integrity checksum, host
  tested.

## Non-responsibilities

- Permissions beyond an owner uid/gid record and the uid-0 check on
  mount: the security phase brings credentials and capability checks.
- Symbolic links, hard links across directories (`nlink` is tracked,
  `link()` is not offered), special files other than the console,
  file locking, `mmap` of files, and asynchronous I/O.
- Data checksums, snapshots, clones, multi-device pools, redundancy,
  compression, quotas: designed for (superblock fields and the pool
  interface reserve the places) and listed as future work in the
  cosmofs design. Section 32 snapshots become possible because every
  committed root is immutable; exposing them is a later phase.
- Memory-pressure eviction of clean pages: the page cache drops a
  vnode's pages when the vnode is released, not on demand.
- A userland `mount` tool or `mkfs`: init exercises the system calls;
  tools arrive with the userland phases.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `vfs_init`, `vfs_register_fs`, `vfs_mount`, `vfs_umount`, `vfs_root` | `kernel/vfs.h` | `kernel_main`, self-tests, `sys_mount` |
| `vfs_lookup`, `vfs_open`, `vfs_mkdir`, `vfs_unlink`, `vfs_rmdir`, `vfs_rename`, `vfs_stat`, `vfs_sync` | `kernel/vfs.h` | system calls, self-tests |
| `file_read`, `file_write`, `file_seek`, `file_stat`, `file_readdir`, `file_put` | `kernel/vfs.h` | system calls, kernel users |
| `vnode_get/put`, `vnode_lookup_cached`, `struct vnode_ops`, `struct fs_type` | `kernel/vfs.h` | filesystems |
| `pagecache_*` | `kernel/pagecache.h` | VFS, filesystems |
| `pool_open/close/read/write/flush` | `kernel/storage.h` | cosmofs |
| `cosmofs_format`, `cosmofs_fs_type` | `kernel/cosmofs.h` | self-tests, `kernel_main` |
| `crc32c` | `kernel/crc32c.h` | cosmofs, host tests |
| `SYS_open` … `SYS_umount`, `struct cosmo_stat`, `struct cosmo_dirent`, `O_*` | `uapi/cosmo/syscall.h` | user programs |

Tests: self-tests `vfs-ramfs`, `pagecache`, `pool`, `cosmofs-format`,
`cosmofs-ops`, `cosmofs-crash` (a transaction abandoned before its root
commit leaves the previous state intact on remount); host tests for
CRC32C and the cosmofs on-disk layout helpers; init's user-mode
self-test exercises every new system call on ramfs and reads a file the
kernel test left on the scratch disk through `mount`.
