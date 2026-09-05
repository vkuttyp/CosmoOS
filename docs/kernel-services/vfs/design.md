# VFS and storage: design

## Data structures

### VFS core (`kernel/include/kernel/vfs.h`)

```c
enum vnode_type { VNODE_REG, VNODE_DIR, VNODE_CHR };

struct vnode {
    struct kobject obj;             /* vnode_type; release() is the eviction path */
    struct mount *mnt;
    uint64_t ino;
    enum vnode_type type;
    uint32_t mode;                  /* permission bits, enforced by vfs_permission */
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t mtime_ns, ctime_ns;
    const struct vnode_ops *ops;
    void *fs_priv;
    struct pagecache pc;            /* regular files */
    struct mutex lock;              /* size, links, directory contents, fs_priv */
    struct mount *covered_by;       /* a mount whose root replaces this directory */
    struct list_node hash_link;     /* mount->vnodes[ino % VNODE_HASH] */
    unsigned flags;                 /* VNODE_PINNED (fs holds a reference), VNODE_DEAD */
};

struct vnode_ops {
    int  (*lookup)(struct vnode *dir, const char *name, size_t len, struct vnode **out);   /* referenced */
    int  (*create)(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out);
    int  (*mkdir)(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out);
    int  (*unlink)(struct vnode *dir, const char *name, size_t len, struct vnode *victim);
    int  (*rmdir)(struct vnode *dir, const char *name, size_t len, struct vnode *victim);
    int  (*rename)(struct vnode *odir, const char *oname, size_t olen, struct vnode *victim,
                   struct vnode *ndir, const char *nname, size_t nlen, struct vnode *replaced);
    int  (*readdir)(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg);
    int  (*readpage)(struct vnode *vn, uint64_t index, void *buf);     /* 4 KiB, zero-fills holes */
    int  (*writepage)(struct vnode *vn, uint64_t index, const void *buf);
    int  (*truncate)(struct vnode *vn, uint64_t size);
    int64_t (*read)(struct vnode *vn, uint64_t off, void *buf, size_t len);    /* CHR only */
    int64_t (*write)(struct vnode *vn, uint64_t off, const void *buf, size_t len);
    int  (*sync)(struct vnode *vn);
    void (*evict)(struct vnode *vn);          /* last reference gone; free fs_priv, frees blocks if nlink==0 */
};

struct fs_type {
    const char *name;
    int  (*mount)(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt);  /* sets mnt->root */
    int  (*unmount)(struct mount *mnt);       /* root still alive; commit or discard, drop pins */
    int  (*sync)(struct mount *mnt);          /* commit */
    struct list_node link;
};

struct mount {
    struct kobject obj;
    struct fs_type *fs;
    struct vnode *root;                       /* referenced */
    struct vnode *mountpoint;                 /* referenced directory in the parent, NULL for / */
    struct mount *parent;
    struct blkdev *bdev;                      /* referenced, or NULL */
    void *fs_priv;
    unsigned flags;                           /* MOUNT_RDONLY */
    struct list_node vnodes[VNODE_HASH];      /* 64 buckets; cached vnodes by ino */
    struct mutex lock;                        /* the hash and covered_by pointers */
    struct list_node link;                    /* g_mounts */
    unsigned nr_vnodes;
    uint64_t next_ino;                        /* for filesystems that number in memory */
};

struct file {
    struct kobject obj;                       /* file_type, a kobject_io_type */
    struct vnode *vn;                         /* referenced */
    uint64_t pos;
    unsigned flags;                           /* O_RDONLY/O_WRONLY/O_RDWR/O_APPEND/O_DIRECTORY */
    struct mutex lock;                        /* pos */
};
```

Path resolution (`vfs_walk`): start at the process's root (the global
root in this phase), take one component at a time under the parent's
`lock`, call `ops->lookup`, then follow `covered_by` to a mount's root
(and `..` at a mount root back to the mountpoint's parent). Every step
holds a reference to the current vnode and drops the previous one, so
an unmount cannot free a directory under a walker (unmount fails with
`-EBUSY` while any vnode of the mount is referenced beyond what the
filesystem itself holds: a pinned vnode's pin, the mount's reference on
its root). A path ending in a slash requires its last component to be a
directory (`-ENOTDIR` otherwise; `vfs_open` treats it as
`O_DIRECTORY`). Components
are limited to `VFS_NAME_MAX` (255) bytes and paths to `VFS_PATH_MAX`
(1024); more than 40 components or a walk through a dead vnode fails.
Symbolic links do not exist.

Mount and unmount: `vfs_mount` refuses a target that is already covered
by a mount, is itself a mount's root, or is `/` (`-EBUSY`): mounts do
not stack. `vfs_umount` resolves the path to a mount root (`-EINVAL`
otherwise, `-EBUSY` for the root filesystem), checks the busy rule
above, commits through `fs->sync` while the mount is still whole (a
failed commit returns the error and leaves the mount in place, so no
data is lost and the caller can retry), uncovers the mountpoint, and
then calls `fs->unmount` **with the root still alive** so the
filesystem can drop its state and release its own pins; only afterwards
does the VFS drop the root reference, so a filesystem's `evict` must
tolerate `mnt->fs_priv` already being NULL. cosmofs treats a mutation
that fails after its namespace change was published (a metadata
write-through hitting ENOSPC or I/O error) by abandoning the open
transaction (`cfs_fail`): commits refuse from then on, and unmount drops
the transaction, so the on-disk state stays at the last committed root.
The crash-test hook makes `fs->sync` a no-op so unmount drops the
transaction the same way, and `vfs_umount2(path, VFS_UMOUNT_FORCE)`
(user space: `umount` with `COSMO_UMOUNT_FORCE`, uid 0) skips the
commit on purpose so an abandoned transaction can be dropped and the
device released.

Vnode cache: `vnode_lookup_cached(mnt, ino)` returns a referenced vnode
if one is hashed; a filesystem's `lookup` calls it before instantiating.
The cache holds **no** reference, but a hashed vnode always has one:
`vnode_put` reads the count and, when it is 1, takes the mount's hash
spinlock, re-reads it (a lookup that raised it since must have held the
same lock, so the re-read is final) and unhashes before dropping; the
`release` path therefore takes no mount lock and only syncs dirty pages
(`pagecache_sync`), drops the cache, calls `evict`, and frees. This closes
the check-then-get of the audit (a second vnode for an inode whose first
was mid-release). ramfs
pins its vnodes (`VNODE_PINNED`: the fs holds a reference while
`nlink > 0`) because the page cache is its only copy of the data;
cosmofs vnodes are re-read from disk after eviction. Directory vnodes
that are mountpoints are pinned by the mount's reference.

Locking: `g_mounts_lock` (mutex) for the mount table and `fs_type`
registry; `mount->lock` (a spinlock, a leaf) for the vnode hash;
`mount->rename_lock` (mutex) serialises renames on a mount;
`mount->sync_lock` (mutex) orders `fs->sync` against `fs->unmount`;
`vnode->lock` for contents and size; `file->lock` for the position;
`pagecache.lock` inside `vnode->lock`. Directory operations lock the
parent, then a child (`mutex_lock_nested(..., VNODE_NESTED_CHILD)`); a
child is never locked while holding only the child. `rename` takes the
mount's `rename_lock`, then decides the parent order from the ancestry
(stable under that lock: only rename changes a parent): the ancestor
first, or address order for two unrelated directories, the second parent
annotated `VNODE_NESTED_PARENT2`; the "directory under itself" walk runs
under `rename_lock` with no parent locked. Order: `g_mounts_lock` →
`rename_lock` → `vnode->lock` (parent, second parent, child) →
`pagecache.lock` → filesystem private locks → block layer; `mount->lock`
is a spinlock leaf under any of them; `file->lock` is taken alone before
`vnode->lock`; `sync_lock` is taken alone before filesystem locks. The
debug-build lock-order checker (`docs/kernel/lockdep/`) verifies this on
every boot. `vfs_sync` no longer holds `g_mounts_lock` across a commit: it
takes each mount by position with a reference and syncs it under its
`sync_lock`.

### Page cache (`kernel/include/kernel/pagecache.h`)

```c
struct pc_entry { uint64_t index; struct page *page; bool dirty, on_lru; struct pc_entry *next;
                  struct vnode *vn; struct list_node lru; };
struct pagecache { struct pc_entry *buckets[PC_HASH]; /* 32 */ unsigned nr_pages, nr_dirty; struct mutex lock; };

int  pagecache_read(struct vnode *vn, uint64_t off, void *buf, size_t len);      /* bounded by vn->size */
int  pagecache_write(struct vnode *vn, uint64_t off, const void *buf, size_t len); /* grows vn->size */
int  pagecache_sync(struct vnode *vn);                                             /* writepage every dirty page */
void pagecache_truncate(struct vnode *vn, uint64_t size);                          /* drop pages past size, zero tail */
void pagecache_drop(struct vnode *vn);                                             /* free every page (clean) */
```

A miss checks the mount's page budget (`mount.cache_limit_pages`;
`-ENOSPC` at or above it, nothing allocated), allocates a frame
(`pmm_alloc_page`, NORMAL zone), calls `readpage` for pages inside the
file's block-aligned size (holes zero), inserts, counts the page on the
mount, and links it on the global LRU when the mount's pages can be
rebuilt (no `MOUNT_CACHE_IS_STORE`). Writes mark pages dirty and take
them off the LRU; nothing reaches the filesystem until `pagecache_sync`
(called by `vfs_sync`, `fs->sync`, and eviction), which writes the
dirty pages in ascending index order and puts them back on the LRU.
Before a read, write or page get/put locks a cache,
`reclaim_if_needed` evicts clean LRU-tail pages while the global count
is at or above `pagecache_limit()` (a quarter of RAM at boot;
`docs/kernel/security/design.md` §3). The frame is addressed through the
direct map. Memory: 48 bytes of entry per cached page plus the page.

### ramfs (`kernel-services/vfs/ramfs.c`)

`ramfs_node` = the vnode plus, for directories, a list of
`ramfs_dirent { name, len, struct vnode *child (referenced) }`. Regular
file data lives in the vnode's page cache (`readpage` zero-fills,
`writepage` is a no-op that leaves the page resident). Inode numbers
come from a per-mount counter. Every node is pinned while linked;
`unlink` drops the pin so an open file survives until its last handle
closes. `rename` moves the dirent, replacing an existing target of the
same type (a non-empty directory target is refused).

### Storage pool (`kernel/include/kernel/storage.h`)

```c
struct spool { struct blkdev *dev; uint32_t block_size; /* 4096 */ uint64_t nblocks; unsigned sectors_per_block; };
int  pool_open(struct blkdev *bd, struct spool **out);      /* references bd */
void pool_close(struct spool *p);
int  pool_read(struct spool *p, uint64_t blk, void *buf);   /* one pool block */
int  pool_write(struct spool *p, uint64_t blk, const void *buf);
int  pool_write_flags(struct spool *p, uint64_t blk, const void *buf, unsigned flags);   /* BIO_PREFLUSH, BIO_FUA: the root write */
int  pool_flush(struct spool *p);
```

The pool is the only thing cosmofs addresses. Adding a second member,
allocation groups, or redundancy changes this file and the superblock's
member table, not the filesystem.

### cosmofs on disk (`kernel-services/filesystem/cosmofs/cosmofs_format.h`)

Summary; the authoritative description, with a block-by-block picture of
a formatted disk and the commit sequence, is
`docs/kernel-services/filesystem/cosmofs/design.md`. Divergences from the
text below as first drafted, now reflected there: inodes are written
through on every mutation (`struct cfs_vnode` caches them), directory
blocks bypass the page cache, `writepage` fills unwritten logical blocks
below the written one with zero blocks, inode numbers are never reused,
and the inode carries a `parent` field for `..`.

Block size 4096. All integers little-endian. Every metadata block
starts with:

```c
struct cfs_mhdr { uint32_t magic; /* "CFSM" */ uint32_t kind; uint64_t generation; uint64_t blkno; uint32_t crc; uint32_t pad; };  /* 32 bytes */
```

`crc` is CRC32C over the block with the field zeroed, and `blkno` must
equal the block's own number (misdirected writes are detected).

| Block | Content |
|---|---|
| 0, 1 | Superblock slots A and B (`struct cfs_super`): magic `COSMOFS1`, version 1, block size, total blocks, `generation`, `imap_root`, `alloc_root`, `next_ino`, `inode_count`, `free_blocks`, `csum_root` (reserved for a data-checksum tree), `snap_root` (reserved for snapshot roots), member table (one entry), CRC32C. Mount reads both, keeps the valid one with the higher generation. Commit writes the slot the current root did **not** come from. |
| imap L1 | `kind IMAP1`: 508 block numbers of L0 blocks. |
| imap L0 | `kind IMAP0`: 508 block numbers of inode blocks. |
| inode block | `kind INODES`: 15 × 256-byte `struct cfs_inode`. Inode `i` lives in inode block `i / 15`, slot `i % 15`; L0 index `(i/15) % 508`, L1 index `(i/15) / 508`. |
| alloc index | `kind ALLOCIDX`: 508 block numbers of bitmap blocks. |
| bitmap | `kind BITMAP`: 32512 bits, one per pool block, 1 = allocated. |
| indirect | `kind EXTENTS`: 254 `struct cfs_extent`. |
| data | file contents and directory entries; no header. |

```c
struct cfs_extent { uint64_t start; uint32_t count; uint32_t pad; };      /* 16 bytes, count in blocks */
struct cfs_inode {                                                          /* 256 bytes */
    uint32_t mode;      /* CFS_TYPE_* << 12 | permissions */
    uint32_t nlink;
    uint32_t uid, gid;
    uint64_t size;
    uint64_t mtime_ns, ctime_ns;
    uint64_t generation;                 /* last transaction that changed it */
    uint64_t ino;
    struct cfs_extent direct[10];        /* 160 bytes */
    uint64_t indirect;                   /* EXTENTS block or 0 */
    uint64_t reserved[4];
};
struct cfs_dirent { uint64_t ino; uint8_t type; uint8_t namelen; uint8_t pad[6]; char name[48]; };   /* 64 bytes, ino 0 = free */
```

Inode 1 is the root directory. A file's logical block `n` is found by
walking the extents in order (direct then indirect); a block number of
0 inside an extent run is not possible (runs are contiguous allocated
blocks), and a logical block beyond the runs is a hole. Maximum extent
count is 264, which bounds file size by fragmentation; sequential
allocation merges runs.

### cosmofs in memory and transactions (`cosmofs.c`, `cosmofs_txn.c`, `cosmofs_alloc.c`)

```c
struct cfs { struct spool *pool; struct cfs_super sb; unsigned sb_slot; uint64_t gen; /* sb.generation + 1 = the open transaction */
             uint8_t *bitmap; /* in-memory copy, nblocks bits */ struct list_node pending_free; struct list_node bufs; /* dirty metadata */
             unsigned nr_dirty; struct mutex lock; bool dirty; struct mount *mnt; };
struct cfs_buf { struct list_node link; uint64_t blkno; uint8_t *data; /* 4 KiB */ bool dirty; };
```

One filesystem lock serialises every metadata operation (v1). The open
transaction is generation `gen`. Rules:

1. **Read**: metadata blocks are read through `cfs_buf` (a small LRU of
   64 buffers keyed by block number); the header's `crc`, `blkno` and
   `kind` are verified on every read from the pool (`-EIO` on mismatch).
2. **Copy on write**: to modify a metadata block whose header
   `generation < gen`, allocate a fresh block, copy, set
   `generation = gen`, `blkno = new`, mark dirty, put the old block on
   `pending_free`, and update the parent pointer (which recurses: the
   L0 block's parent L1, the L1's parent is the superblock root field;
   the bitmap's parent is the alloc index). A block with
   `generation == gen` is already new in this transaction and is
   modified in place.
3. **Data**: `writepage` for a logical block always allocates a new
   block, writes the page there, and rewrites the extent (splitting a
   run if necessary); the old block goes on `pending_free`. `readpage`
   maps and reads. So committed data is never overwritten either.
4. **Allocation**: first-fit in the in-memory bitmap, never returning a
   block on `pending_free` (they are still referenced by the committed
   root). The on-disk bitmap blocks touched by allocation are CoW'd like
   any metadata.
5. **Commit** (`cfs_commit`): write every dirty data page
   (`pagecache_sync` on dirty vnodes), write every dirty `cfs_buf`,
   `pool_flush`, then write the superblock into the other slot with
   `generation = gen`, `pool_flush`, then `gen++`, clear `pending_free`
   bits in the in-memory bitmap (they become allocatable in the next
   transaction, which will CoW the bitmap blocks and write them with the
   next commit), clear the buffers' dirty flags. Runs on `vfs_sync` and
   `unmount` only; there is no size or time threshold (V17). The full
   sequence, including the reserve-then-write bitmap fixpoint, is in
   `docs/kernel-services/filesystem/cosmofs/design.md`.
6. **Crash**: before the superblock write, the old root is intact (all
   new blocks were free in it); after, the new root is complete. The
   superblock CRC makes a torn slot write detectable, and the other slot
   still holds the previous root.
7. **Frees**: `unlink` of the last link with no open file, or eviction
   of an unlinked vnode, puts the file's blocks and its inode slot on
   `pending_free` and clears the inode.

Format (`cosmofs_format(pool)`): writes bitmap, alloc index, inode
block 0 with the root directory (inode 1, empty), imap L0 and L1, and
superblock slot A at generation 1 with slot B zeroed.

### System calls (`uapi/cosmo/syscall.h`)

```text
11 open(path, flags, mode) -> h      12 stat(path, struct cosmo_stat *)    13 fstat(h, stat *)
14 lseek(h, off, whence) -> pos       15 mkdir(path, mode)                 16 unlink(path)
17 rmdir(path)                        18 rename(old, new)                  19 getdents(h, buf, len) -> bytes
20 sync()                             21 mount(source, target, fstype, flags) 22 umount(target)
```

`O_RDONLY 0`, `O_WRONLY 1`, `O_RDWR 2`, `O_CREAT 0x40`, `O_EXCL 0x80`,
`O_TRUNC 0x200`, `O_APPEND 0x400`, `O_DIRECTORY 0x10000`.
`struct cosmo_stat { ino, type, mode, nlink, uid, gid, size, mtime_ns, ctime_ns }`;
`struct cosmo_dirent { ino, type, reclen, name[] }` records packed into
the `getdents` buffer (at least `sizeof(struct cosmo_dirent) + 2` bytes;
`-EINVAL` only when the very next entry does not fit). Paths are copied
with `strncpy_from_user` (1024
max); `open` installs the file with `HANDLE_RIGHT_READ`/`WRITE` from the
access mode, so `read`/`write`/`close` need no change. `mount` names a
block device (`vda`) and requires uid 0.

## Ownership and lifetime

Mounts hold their root and mountpoint; a mount is freed at `umount`
after every other vnode of it is gone. Vnodes are freed by the last
`vnode_put` (see above). Files hold their vnode; handle tables hold
files. Page frames belong to the page cache entry and are freed by
`pagecache_drop`. `cfs_buf`s belong to the filesystem's buffer LRU;
`struct cfs` is freed at unmount. The pool holds a reference on the
block device.

## Concurrency

Described under "Locking" above. Filesystem callbacks run with the
vnode locks the VFS took; cosmofs adds `cfs->lock` beneath them. Page
cache writeback runs under `vnode->lock` and calls `writepage`, which
takes `cfs->lock`: the order `vnode->lock → pagecache.lock → cfs->lock →
blk` holds everywhere. `bio_complete` runs in interrupt context and only
completes a `completion`.

## Memory

Vnode ~400 bytes plus page cache entries; ramfs stores every file page
resident; cosmofs keeps 64 metadata buffers (256 KiB) and a bitmap of
`nblocks/8` bytes (256 bytes for the 8 MiB test disk) per mount.

## Error handling

Every VFS entry validates before calling into a filesystem
(`-ENOENT`, `-ENOTDIR`, `-EISDIR`, `-EEXIST`, `-ENOTEMPTY`, `-EXDEV`,
`-EROFS`, `-ENAMETOOLONG`, `-EBADF`, `-EINVAL`). cosmofs reports `-EIO`
for any checksum, magic, kind or block-number mismatch and `-ENOSPC`
when the bitmap is exhausted; a failed operation inside a transaction
leaves the in-memory state consistent (allocation is the last
fallible step of every mutation, done before pointers are rewritten)
and the on-disk state untouched until commit.

## Performance

Not a goal in this phase: linear directories, one lock per filesystem,
synchronous commit, no read-ahead. The structures (extents, a
buffer LRU, a page cache with per-vnode locks) are the ones a later
phase optimises.

## Security

Paths from user space are length-checked copies; the kernel never
follows user pointers during a walk. Every field read from disk is
bounds-checked (block numbers below `nblocks`, extent counts, inode
numbers below `next_ino`, directory entry lengths) before use, and
metadata is checksummed; corrupt or hostile images yield `-EIO`, not a
wild pointer. `mount`/`umount` need `cred_privileged`. ramfs limits a
file to `RAMFS_MAX_FILE` (64 MiB) and a mount to `RAMFS_MAX_PAGES` (16 K
pages, enforced by the page cache's per-mount budget since audit
milestone 6; before it the constant was declared and unused) so a user
program cannot exhaust RAM through `/tmp`; the cache as a whole is
bounded by `pagecache_limit()` with reclaim of clean pages
(`docs/kernel/security/design.md` §3).

### Permissions

Discretionary access control is one function, `vfs_permission(vn, mask)`
with `VFS_MAY_READ/WRITE/EXEC`, judged with the caller's credentials
(`cred_current()`, `kernel/cred.h`: the process's, or the kernel's for
boot-time work): the owner bits when the effective uid matches `vn->uid`,
the group bits when the effective or a supplementary gid matches
`vn->gid`, else the other bits. A privileged caller (euid 0) passes every
check except executing a regular file that has no x bit at all. The
checks sit where POSIX puts them: `step` needs search (`VFS_MAY_EXEC`) on
every directory it enters, including `.` and `..`; `vfs_open` needs
search on the last directory, write on it to create, and then read
and/or write on the file per the access mode, except for the file it
just created; `parent_for_mutation` (mkdir, unlink, rmdir, both parents
of rename) needs write and search; `process_chdir` needs search on the
target; `spawn` needs execute on the program (and no read: the kernel
reads it on its own authority through `vfs_open_vnode`). New vnodes are
owned by their creator's effective ids (`ramfs_new`, `cfs_create_common`;
cosmofs persists them in the inode). `/tmp` is 01777: in a directory with
the sticky bit (01000) an entry is removed, renamed or replaced only by
the owner of the entry, the owner of the directory, or a privileged
caller (`sticky_denies`). Moving a directory to another parent also
needs write permission on the directory itself (its `..` changes).

## Testing strategy

Self-tests: `vfs-ramfs` (create, write, read, stat, mkdir, readdir,
rename, unlink, rmdir, open-unlinked-file survives, errors), `pagecache`
(hole reads zero, cross-page writes, truncate, dirty counts), `pool`
(read/write/flush a block on `vda`), `cosmofs-format` (format the
scratch disk, mount, superblock fields, empty root), `cosmofs-ops`
(files across extents and the indirect block, directories, rename,
unlink frees blocks, remount shows the same tree and data, free-block
accounting returns to baseline), `cosmofs-crash` (mutate, drop the
transaction without commit, remount, the previous state is intact; a
corrupted superblock slot is ignored in favour of the other). Host
tests: CRC32C vectors, inode/dirent layout sizes, extent mapping
arithmetic. Init: every new system call on ramfs, then `mount("vda",
"/mnt", "cosmofs")` and a read of the file the kernel test left.

## Future extensibility

- Snapshots: the superblock's `snap_root` points at a block listing
  immutable roots; taking one is recording the current root before the
  next commit and excluding its blocks from `pending_free`.
- Data checksums: `csum_root` → a tree keyed by block number.
- Multiple pool members and allocation groups behind `pool_*`.
- Symbolic links (a new vnode type), hard links, `chmod`/`chown`, the
  sticky bit, memory-pressure eviction, a dentry cache, `mmap` of
  files (the page cache already owns frames), a host `mkfs`.
