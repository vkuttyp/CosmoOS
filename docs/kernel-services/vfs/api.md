# VFS and storage: API

Every entry follows constitution section 52: purpose, inputs, outputs,
ownership, lifetime, concurrency, blocking, interrupt-context rules,
failure modes, ABI stability. Kernel-side interfaces are **internal**
(they may change with the code); the system-call numbers and structures
in `uapi/cosmo/syscall.h` are **stable**.

Common rules unless stated otherwise: every function sleeps (mutexes,
allocation, block I/O), so none may be called from interrupt context or
with a spinlock held; a `struct vnode *` or `struct file *` parameter
must be referenced by the caller; a `struct vnode **out` is returned
referenced and the caller owns that reference.

## VFS core (`kernel/include/kernel/vfs.h`)

Constants: `VFS_NAME_MAX` 255 (one component), `VFS_PATH_MAX` 1024
(the whole path, NUL included), `VFS_MAX_COMPONENTS` 40 (directories
walked before the last one), `VNODE_HASH` 64 (per-mount vnode buckets).

### Types

- `enum vnode_type { VNODE_REG, VNODE_DIR, VNODE_CHR }`: the values equal
  `COSMO_DT_REG/DIR/CHR` so `stat` and `getdents` report them directly.
- `struct vnode_ops`: the filesystem's per-vnode callbacks (`lookup`,
  `create`, `mkdir`, `unlink`, `rmdir`, `rename`, `readdir`, `readpage`,
  `writepage`, `truncate`, `read`/`write` for `VNODE_CHR`, `sync`,
  `evict`). All are called with the vnode locks the VFS holds
  (parent locked for directory operations, the vnode locked for data
  operations). A NULL callback yields `-ENOTSUP` from the VFS entry.
- `struct vnode`: kobject, `mnt`, `ino`, `type`, `mode`, `uid`, `gid`,
  `nlink`, `size`, `mtime_ns`, `ctime_ns`, `ops`, `fs_priv`, page cache
  `pc`, `lock`, `covered_by` (a mount whose root replaces this
  directory), `hash_link`, `flags` (`VNODE_PINNED`: the filesystem holds
  a reference while the node is linked; `VNODE_DEAD`: unlinked, no new
  lookups).
- `struct fs_type`: `name`, `mount(fs, bdev, flags, mnt)` (must set
  `mnt->root` to a referenced directory vnode), `unmount(mnt)` (called
  with the root still alive; the filesystem commits or discards and
  releases its own references), `sync(mnt)` (commit; may be NULL).
- `struct mount`: kobject, `fs`, `root` (referenced), `mountpoint`
  (referenced directory in the parent, NULL for `/`), `parent`, `bdev`
  (referenced or NULL), `fs_priv`, `flags` (`MOUNT_RDONLY`), the vnode
  hash and its `lock`, `next_ino` for filesystems that number in memory.
- `struct file`: kobject of a `kobject_io_type`, `vn` (referenced),
  `pos`, `flags` (the `COSMO_O_*` it was opened with), `lock` (for
  `pos`).

### Lifecycle

**`void vfs_init(void)`** Registers ramfs and mounts it as `/`. Once,
after `kmalloc_init` and `timer_init` (times), before `cosmofs_init`.
Panics on failure.

**`int vfs_register_fs(struct fs_type *fs)`** Adds an immortal
`fs_type`. `-EINVAL` (missing name/mount/unmount), `-EEXIST`.

**`struct fs_type *vfs_find_fs(const char *name)`** Borrowed pointer or
NULL.

**`struct vnode *vfs_root(void)`** The global root, referenced.

**`int vfs_mount(const char *path, const char *fsname, struct blkdev *bdev, unsigned flags)`**
Instantiates `fsname` (with the optional device; the mount takes its own
`blkdev_get`) and covers the directory at `path`. `-ENODEV` unknown
filesystem, `-ENOENT`/`-ENOTDIR` for the target, `-EBUSY` if the target
is already covered or is a mount root (no stacking) or is `/`, or the
filesystem's own error (`-EIO` for an unformatted device, `-EINVAL` for
a ramfs given a device or a cosmofs given none). Logs `vfs: mounted`.

**`int vfs_umount(const char *path)`** `path` must resolve to a mount's
root (`-EINVAL` otherwise, `-EBUSY` for `/`). `-EBUSY` while any vnode of
the mount is referenced beyond what the filesystem itself holds (a
pinned vnode's pin, the mount's root reference), for example an open
file. Otherwise: call `fs->sync` while the mount is still whole (on
failure the mount stays and the error is returned, so nothing is lost
and the caller can retry), uncover the mountpoint, call `fs->unmount`
(which only drops state: a deliberately discarded or abandoned
transaction is dropped here), drop the root, release the device.
`vfs_umount2(path, VFS_UMOUNT_FORCE)` skips the commit and drops the
open transaction: the recovery path when a commit keeps failing (for
example after cosmofs abandoned a transaction). User space reaches it
through `umount(target, COSMO_UMOUNT_FORCE)`, uid 0 only.

**`int vfs_sync(void)`** `fs->sync` on every mount; the first error is
returned after all mounts were tried.

### Vnodes (for filesystems)

**`struct vnode *vnode_alloc(struct mount *mnt, uint64_t ino)`** A zeroed
vnode with one reference, `nlink` 1, times set, page cache initialised,
not yet hashed. NULL on `-ENOMEM`.

**`void vnode_hash_insert(struct vnode *vn)`** Publishes it in the
mount's hash (takes `mnt->lock`).

**`struct vnode *vnode_lookup_cached(struct mount *mnt, uint64_t ino)`**
Referenced live vnode or NULL. The hash holds no reference of its own.

**`bool vnode_cache_any(struct mount *mnt, bool (*pred)(const struct
vnode *vn, void *arg), void *arg)`** True if any vnode of the mount
satisfies `pred`. A hashed vnode always holds a reference, so the cache
is exactly the set in use and this is how a filesystem asks whether
something of its own is still open before taking its storage apart
(V23). Holds `mnt->lock` across the walk: `pred` must not sleep, and the
vnode it is shown is not referenced for it.

**`vnode_get` / `vnode_put`** Reference counting. The last put runs the
release path: unhash, sync dirty pages (regular files), drop the page
cache, `ops->evict`, free. Release may therefore do block I/O and take
`mnt->lock`; never drop the last reference with that lock held.

**`uint64_t vfs_now_ns(void)`** The monotonic clock, used for times.

### Namespace operations

All take `struct vnode *start` (NULL or an absolute path means the
global root). Paths are walked one component at a time under the
parent's lock; `.` and `..` are handled by the VFS (`..` at a mount root
crosses back to the mountpoint's parent, `..` at `/` stays); a mount
covering a directory is followed. A trailing slash makes the final
component require a directory.

**`int vfs_lookup(struct vnode *start, const char *path, struct vnode **out)`**
`-ENAMETOOLONG` (path ≥ 1024 or a component > 255), `-ELOOP` (more than
40 components), `-ENOTDIR` (a non-directory in the middle, or a trailing
slash on a file), `-ENOENT`.

**`int vfs_open(struct vnode *start, const char *path, unsigned flags, uint32_t mode, struct file **out)`**
`flags` are `COSMO_O_*`. Access mode `COSMO_O_ACCMODE` alone is
`-EINVAL`. `O_CREAT` creates a regular file with `mode & 07777` when the
last component is missing (`-EROFS` on a read-only mount, `-ENOTSUP`
without `create`); `O_CREAT|O_EXCL` on an existing name is `-EEXIST`.
`O_DIRECTORY` (or a trailing slash) requires a directory (`-ENOTDIR`).
Opening a directory for writing is `-EISDIR`; writing on a read-only
mount `-EROFS`. `O_TRUNC` with write access truncates to 0 before the
file object exists. Returns a referenced `struct file`.

**`int vfs_mkdir(struct vnode *start, const char *path, uint32_t mode)`**
`-EEXIST` (including `path` = `/`), `-EINVAL` for `.`/`..`, `-ENOENT`
for a missing parent, `-ENOTDIR`, `-EROFS`, `-ENOTSUP`.

**`int vfs_unlink(struct vnode *start, const char *path)`** Removes a
non-directory (`-EISDIR` for a directory, `-EBUSY` for a mountpoint or a
mount root). The vnode is marked `VNODE_DEAD`; open files keep it alive
and readable until the last `file_put`.

**`int vfs_rmdir(struct vnode *start, const char *path)`** `-ENOTDIR`,
`-ENOTEMPTY`, `-EBUSY` (mountpoint), `-EINVAL` for `.`/`..`.

**`int vfs_rename(struct vnode *start, const char *oldpath, const char *newpath)`**
Both parents on the same mount (`-EXDEV` otherwise). Parents are locked
in address order. An existing target of the same kind is replaced (an
empty directory only; `-EISDIR`/`-ENOTDIR` for a kind mismatch,
`-ENOTEMPTY`). Moving a directory beneath itself is `-EINVAL`; mount
roots and mountpoints are `-EBUSY`. Renaming an entry onto itself is a
no-op success.

**`int vfs_stat(struct vnode *start, const char *path, struct cosmo_stat *st)`**
and **`void vnode_stat(struct vnode *vn, struct cosmo_stat *st)`** Fill
`ino`, `type`, `mode`, `nlink`, `uid`, `gid`, `size`, `mtime_ns`,
`ctime_ns`.

### Files

**`int64_t file_read(f, buf, len)` / `file_write(f, buf, len)`** At and
advancing `f->pos` (under `f->lock`; `O_APPEND` writes at the current
size). `-EBADF` when the access mode forbids the direction, `-EISDIR`
for directories, `-EROFS`. Regular files go through the page cache;
`VNODE_CHR` through `ops->read/write` and never move `pos`. A short
count is possible only on `-ENOMEM` mid-way.

**`file_pread` / `file_pwrite`** The same at an explicit offset without
touching `pos`.

**`int64_t file_seek(struct file *f, int64_t off, int whence)`**
`COSMO_SEEK_SET/CUR/END`; returns the new position. `-EINVAL` for a
negative result or overflow, `-ESPIPE` for a character device.

**`int file_stat(struct file *f, struct cosmo_stat *st)`**

**`int64_t file_readdir(struct file *f, void *buf, size_t len)`** Packs
`struct cosmo_dirent` records (8-byte aligned, `reclen` each, name NUL
terminated) starting at the file position, which afterwards names the
next unread entry. Returns the bytes packed, 0 at the end, `-ENOTDIR`,
`-EINVAL` if `len` is below `sizeof(struct cosmo_dirent) + 2` or the
very next entry does not fit in `len`.

**`int file_sync(struct file *f)`** Writes back the file's dirty pages
and calls `ops->sync`.

**`file_get` / `file_put`** The last put syncs dirty pages of a regular
file and drops the vnode reference.

**`struct file *file_from_kobject(struct kobject *obj)`** The file behind
a handle's kobject, or NULL for another object kind (the console).

**`void ramfs_populate_boot(void)`** Creates `/boot`, `/boot/modules`,
`/boot/tests`, `/tmp`, `/mnt`, `/dev`, `/bin`, `/sbin`, `/etc` and copies
every boot archive entry into the namespace: entries named `bin/...` and
`sbin/...` become `/bin/...` and `/sbin/...` with mode 0755, `etc/...`
becomes `/etc/...` with mode 0644, everything else goes to
`/boot/<name>` with mode 0644 (the bootstrap namespace policy of the
archive, `docs/userland/`). Entry names may be nested
(`etc/pkg/keys/dev.pub`, `repo/hello-1.1.cpk`): the intermediate
directories are created with mode 0755 (`ensure_parents`), an existing
one is fine, any other failure is logged and the entry skipped. Once,
after `vfs_init` and `bootarchive_init`.

**`int ramfs_mkchr(const char *path, uint32_t mode, const struct chrdev_ops *ops, void *priv, struct vnode **out)`**
(Phase 12) Creates a character device node in the ramfs at `path` (the
parent must exist and be a ramfs directory): `struct chrdev_ops { int64_t
(*read)(struct vnode *, uint64_t off, void *, size_t); int64_t
(*write)(struct vnode *, uint64_t off, const void *, size_t); }` receives
the node's reads and writes with the vnode lock held (a NULL operation is
`-ENOTSUP`); `priv` is returned by **`void *ramfs_chr_priv(const struct
vnode *)`**. `out` may be NULL, else it receives a reference. Errors:
`-EINVAL` (no name, name too long), `-ENAMETOOLONG`, path errors,
`-ENOTDIR` (parent not a ramfs directory), `-EEXIST`, `-ENOMEM`. The
first user is `/dev/vmm` (`docs/kernel-services/virtualization/`); the
console is still a kobject handed to processes at spawn, not a node.

**`vfs_mount_count`, `vfs_vnode_count`, `vfs_dump`** Diagnostics.

## Page cache (`kernel/include/kernel/pagecache.h`)

`struct pagecache`: `PC_HASH` (32) buckets of `struct pc_entry { index,
page, dirty, next }`, `nr_pages`, `nr_dirty`, a mutex. Callers hold the
vnode lock; the functions take `pc.lock` beneath it.

- **`void pagecache_init(struct pagecache *pc)`**
- **`int64_t pagecache_read(vn, off, buf, len)`** Bounded by `vn->size`;
  misses allocate a frame (`pmm_alloc_page`, zeroed) and call
  `ops->readpage` for pages inside the file. Returns bytes or, if
  nothing was read, the error.
- **`int64_t pagecache_write(vn, off, buf, len)`** Dirties pages and
  grows `vn->size`; `-EFBIG` on offset overflow.
- **`int pagecache_sync(vn)`** `ops->writepage` for every dirty page, in
  bucket order; stops at the first error.
- **`void pagecache_truncate(vn, size)`** Drops pages entirely past
  `size`, zeroes the tail of the last page. Does not change `vn->size`.
- **`void pagecache_drop(vn)`** Frees every page; dirty data is lost.
- **`int pagecache_get_page(vn, index, buf)` / `pagecache_put_page(vn, index, buf)`**
  Whole-page access for filesystems that keep structures in file data.
- **`void pagecache_get_stats(struct pagecache_stats *out)`** `hits`,
  `misses`, `writebacks`, `pages` (global).

## Storage pool (`kernel/include/kernel/storage.h`)

`POOL_BLOCK` = 4096. `struct spool { m[POOL_MAX_MEMBERS], nmembers,
block_size, nblocks (member 0's), reads, writes, flushes }`, each member
`{ dev, sectors_per_block, nblocks }`.

A block is addressed by a **DVA**: `POOL_DVA(vdev, blk)` packs the member
into bits 63:56 and the block within it into 55:0, with
`POOL_DVA_VDEV`/`POOL_DVA_BLK` to take them apart. Member 0 at block `b`
is the DVA `b`, which is the sector a single-device pool always
addressed — so a filesystem written before members existed reads back
unchanged.

- **`int pool_open(struct blkdev *bd, struct spool **out)`** A
  one-member pool; takes a reference on `bd`. `-EINVAL` if the sector
  size does not divide the pool block, `-ENOMEM`.
- **`int pool_add_member(p, bd, unsigned *vdev)`** Appends a member and
  reports its vdev number; takes a reference. `-EINVAL` for a bad sector
  size, `-ENOSPC` past `POOL_MAX_MEMBERS` (255).
- **`void pool_close(struct spool *p)`** Drops every device reference.
- **`int pool_read(p, dva, buf)` / `pool_write(p, dva, buf)`** One pool
  block through `blk_read`/`blk_write` (synchronous, sleeps). `buf` must
  be DMA-able (kmalloc or dma_alloc memory). `-EINVAL` for a member the
  pool does not have or past that member's end, or the block layer's
  error.
- **`int pool_write_flags(p, dva, buf, flags)`** The same write with
  `BIO_PREFLUSH` and/or `BIO_FUA`: the block layer flushes before and/or
  after it. cosmofs writes its root this way (one call for the two-flush
  commit).
- **`int pool_flush(p)`** `blk_flush` on every member, first error wins:
  a commit durable on one device and in another's cache is not durable.

## cosmofs (`kernel/include/kernel/cosmofs.h`)

- **`extern struct fs_type cosmofs_fs_type`**, **`void cosmofs_init(void)`**
  Registers it (once, after `vfs_init`; panics on failure).
- **`int cosmofs_format(struct blkdev *bd)`** Writes a generation-1
  filesystem with an empty root over the whole device: superblock A,
  a zeroed slot B, the member table, the allocation index, one bitmap
  block per 32512 blocks, imap L1, imap L0, inode block 0. `-EINVAL` for
  fewer than 64 blocks or more than 508 bitmap chunks, `-ENOMEM`, or the
  pool's error. Existing contents are not preserved.
- **`int cosmofs_format_pool(struct blkdev **bd, unsigned n)`** The same
  over `n` devices as one pool. Member 0 carries the superblocks and the
  member table; every other member carries a label at its block 0 and
  its own allocation index and bitmaps, so a mount handed member 0 finds
  the rest by matching labels against the registered block devices.
  `-EINVAL` past 255 members.
- **`int cosmofs_stats(struct mount *mnt, struct cosmofs_stats *out)`**
  `generation` (last committed), `free_blocks` (in-memory count, which
  includes blocks freed since the last commit once that commit is
  done), `total_blocks`, `inode_count`, `dirty_buffers`, `pending_frees`,
  `reserve_blocks`, `commits`, `wb_commits` (by the writeback thread),
  `csum_failures`, `members`. `-EINVAL` for a mount that is not a
  cosmofs.
- **`void cosmofs_test_discard_on_unmount(struct mount *mnt, bool discard)`**
  Test hook: the next unmount drops the open transaction instead of
  committing it, as a crash before the root write would.
- **`void cosmofs_test_set_writeback(struct mount *mnt, bool on)`**,
  **`void cosmofs_test_set_writeback_interval(struct mount *mnt, unsigned ms)`**
  Test hooks for the writeback thread (`docs/kernel-services/filesystem/cosmofs/design.md`).
- **`file_sync`** on a cosmofs file writes its pages back and then commits
  the open transaction: the file is durable when the call returns.

### Snapshots (format version 3)

A snapshot is the tree a commit published, kept. There is no new system
call: the interface is the two calls that already mean "make a
directory" and "remove one", under a directory the mount root answers by
name.

- **`mkdir <mount>/.snapshots/<name>`** takes a snapshot. It commits
  first, so a snapshot always names a durable tree. `-EEXIST` for a name
  in use, `-ENAMETOOLONG` above 31 characters, `-EROFS` on a read-only
  mount, and the commit's errors otherwise.
- **`rmdir <mount>/.snapshots/<name>`** deletes one, returning every
  block no remaining snapshot still occupies. `-ENOENT` when there is no
  such snapshot.
- **`<mount>/.snapshots/<name>/…`** reads it: an ordinary tree, because
  every tree in a copy-on-write filesystem is. Every write is `-EROFS`.
- **`readdir`** on `.snapshots` lists the snapshots; `readdir` on the
  mount root does **not** list `.snapshots` itself, which is found by
  name only.

A version-2 filesystem mounts unchanged and has no snapshots
(`CFS_VERSION_MIN`); the fields version 3 uses were reserved by version
2. Kernel-internal: `cfs_snapshot_create/delete/list/find` and
`cfs_snapshot_references` in `cosmofs_snap.c`.

The on-disk format is `kernel-services/filesystem/cosmofs/cosmofs_format.h`;
see `docs/kernel-services/filesystem/cosmofs/`.

## CRC32C (`kernel/include/kernel/crc32c.h`)

**`uint32_t crc32c(const void *data, size_t len)`** and
**`uint32_t crc32c_update(uint32_t crc, const void *data, size_t len)`**
Standard CRC-32C (Castagnoli), `crc32c("123456789") == 0xE3069283`;
`crc32c_update(crc32c(a), b) == crc32c(a||b)`. Table driven, built on
first use, any context. Integrity detection only, not authenticity.

## System calls (`kernel/include/uapi/cosmo/syscall.h`) — stable

Numbers 0–10 are unchanged (Phase 4). New:

| # | Name | Arguments | Returns | Errors |
|---|---|---|---|---|
| 11 | `open` | `const char *path, int flags, uint32_t mode` | handle ≥ 0 | `EFAULT`, `ENAMETOOLONG`, `ENOENT`, `ENOTDIR`, `EISDIR`, `EEXIST`, `EINVAL`, `EROFS`, `EMFILE`, `ENOMEM` |
| 12 | `stat` | `const char *path, struct cosmo_stat *st` | 0 | path errors, `EFAULT` |
| 13 | `fstat` | `int h, struct cosmo_stat *st` | 0 | `EBADF`, `EFAULT` |
| 14 | `lseek` | `int h, int64_t off, int whence` | new position | `EBADF`, `EINVAL`, `ESPIPE` |
| 15 | `mkdir` | `const char *path, uint32_t mode` | 0 | path errors, `EEXIST`, `EROFS` |
| 16 | `unlink` | `const char *path` | 0 | path errors, `EISDIR`, `EBUSY` |
| 17 | `rmdir` | `const char *path` | 0 | path errors, `ENOTDIR`, `ENOTEMPTY`, `EBUSY` |
| 18 | `rename` | `const char *old, const char *new` | 0 | path errors, `EXDEV`, `EISDIR`, `ENOTDIR`, `ENOTEMPTY`, `EBUSY`, `EINVAL` |
| 19 | `getdents` | `int h, void *buf, size_t len` | bytes, 0 at end | `EBADF` (also a handle without READ), `EFAULT`, `ENOTDIR`, `EINVAL` |
| 20 | `sync` | — | 0 | a filesystem's error |
| 21 | `mount` | `const char *source, const char *target, const char *fstype, unsigned flags` | 0 | `EPERM` (uid ≠ 0), `ENODEV` (unknown device or filesystem), `EBUSY`, `EIO`, path errors |
| 22 | `umount` | `const char *target` | 0 | `EPERM`, `EINVAL`, `EBUSY`, path errors |

`SYS_COUNT` is 23. Paths are copied with `strncpy_from_user` up to
`VFS_PATH_MAX`; an empty path is `-ENOENT`; an unreadable pointer is
`-EFAULT`. `open` installs the file with `HANDLE_RIGHT_READ` for
`O_RDONLY`/`O_RDWR` and `HANDLE_RIGHT_WRITE` for `O_WRONLY`/`O_RDWR`, so
the Phase 4 `read`, `write` and `close` calls work on files unchanged and
enforce the access mode (`-EBADF`). `getdents` copies at most 64 KiB per
call through a kernel buffer. `mount` takes a block device name (`vda`)
or `none`/empty for a memory filesystem, and honours
`COSMO_MOUNT_RDONLY`.

Flags and constants: `COSMO_O_RDONLY 0`, `O_WRONLY 1`, `O_RDWR 2`,
`O_ACCMODE 3`, `O_CREAT 0x40`, `O_EXCL 0x80`, `O_TRUNC 0x200`,
`O_APPEND 0x400`, `O_DIRECTORY 0x10000`; `COSMO_SEEK_SET/CUR/END` 0/1/2;
`COSMO_DT_UNKNOWN/REG/DIR/CHR` 0/1/2/3; `COSMO_MOUNT_RDONLY 1`.

```c
struct cosmo_stat { uint64_t ino; uint32_t type, mode, nlink, uid, gid, pad; uint64_t size, mtime_ns, ctime_ns; };
struct cosmo_dirent { uint64_t ino; uint16_t reclen; uint8_t type, namelen; char name[]; };  /* NUL terminated, reclen 8-aligned */
```

New errno values exported to user space: `COSMO_EXDEV 18`, `ENODEV 19`,
`ENOTDIR 20`, `EISDIR 21`, `EFBIG 27`, `EROFS 30`, `ENAMETOOLONG 36`,
`ENOTEMPTY 39`.

### User-side wrappers (`libc/include/cosmo/syscall.h`)

`cosmo_open`, `cosmo_close`, `cosmo_stat`, `cosmo_fstat`, `cosmo_lseek`,
`cosmo_mkdir`, `cosmo_unlink`, `cosmo_rmdir`, `cosmo_rename`,
`cosmo_getdents`, `cosmo_sync`, `cosmo_mount`, `cosmo_umount`: thin
inline wrappers over `cosmo_syscallN`, returning the kernel's value
(negative errno on failure). `userland/init/init.c` (`fs_selftest`) is
the reference user.
