# VFS and storage: design

## Data structures

### VFS core (`kernel/include/kernel/vfs.h`)

```c
enum vnode_type { VNODE_REG, VNODE_DIR, VNODE_CHR, VNODE_LNK, VNODE_SOCK, VNODE_FIFO };

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
    struct list_node covers;        /* mounts whose root replaces this directory */
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
    struct list_node cover_link;              /* mountpoint->covers */
    struct mutex lock;                        /* the hash */
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
`lock`, call `ops->lookup`, then follow `covers` to a mount's root
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

A mountpoint carries the mounts covering it on a list, `vnode.covers`:
a directory can be a mountpoint in one mount namespace and an ordinary
directory in another, and one pointer cannot say that.

Mount namespaces (`kernel-services/vfs/mountns.c`,
`docs/kernel/security/design.md` §1d). A `struct mount_ns` is a set of
mounts a process can see; `process->mntns` names it, children inherit
it, and `COSMO_SPAWN_NEWMOUNTNS` starts a new one that begins as a copy
of the parent's view. The copy is of the *view*: a `struct mount_ns_ref`
per (mount, namespace) pair, one on the mount's `ns_refs` and one on the
namespace's `mounts`. The filesystem instance is never duplicated --
one mount, one vnode cache, one transaction, however many namespaces
show it.

`covering_mount_ns(dir, ns)` walks `covers` and returns the first mount
that `ns` can see, which is what `follow_mount` and `vfs_mount`'s
no-stacking check use. `covering_mount(dir)` answers the weaker "in any
namespace", which is what `remove_entry` and `rename` use: a mount is
attached to the vnode rather than the path, so a namespace that cannot
see one has no basis to remove the directory under it.

A `ns_refs` list is protected by the mount's own mountpoint lock -- the
same lock as `cover_link`, since where a mount is attached and who can
see it are one question -- so `follow_mount`, already holding that lock
as `dir->lock`, reads both without reaching for `g_mounts_lock` and
inverting the order. The root mount is outside the machinery entirely:
every namespace has it, none may unmount it.

`vfs_umount` in a namespace that is not the last one to see a mount only
removes that namespace's ref: nothing is committed and no busy check
runs, because the filesystem stays. The last one out does the unmount
that exists today. A namespace whose last reference goes drops its refs
in reverse order, newest first, so a nested mount is gone before the one
holding its mountpoint, and unmounts whatever that leaves unreachable.

The lock discipline is deliberately unchanged, since it is the part that
is easy to get wrong: the list is protected by the mountpoint vnode's
own lock, exactly as the pointer was. `follow_mount` reads it under that
lock and `vfs_umount` removes from it under the same lock, so a walker
either already holds the mounted root or is turned away by the
`unmounting` flag. Which accessor a caller uses is decided by what it
already holds: `covering_mount` when it holds the vnode's own lock
(`remove_entry`, on the victim); `is_mountpoint_child` when it holds
only the parent, which takes the lock as a child in the V7 order; and
`entry_is_mountpoint(vn, held1, held2)` in `rename`, which holds both
parents and can look up an entry that *is* one of them -- renaming a
directory onto its own parent, `/a/b` -> `/a`, returns the locked `/a`
as the replaced entry -- so it compares against the two vnodes it holds
rather than asking the mutex who owns it.

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
`vnode_put` drops through `kobject_put_and_lock`, which decrements
without the lock while the count is above one and takes the mount's hash
spinlock only for the drop that reaches zero; the unhash happens in that
same hold, and only then does the release run -- so it takes no mount
lock and only syncs dirty pages (`pagecache_sync`), drops the cache,
calls `evict`, and frees. A lookup under the hash lock may therefore
take a plain reference: the count reaches zero only under that lock, and
an object at zero has already left the hash.

The first version of this read the count, decided it was the last
holder, and then dropped -- three steps, and two holders dropping from 2
each read 2, neither unhashed, and the second drop reached zero with the
vnode still hashed. `vfs-concurrency` caught it on aarch64 as the
release assertion, once in many runs; `vfs-put-race` reproduces it on
demand by dropping the last references of one vnode from every CPU at
once. It is the audit's `pmm_page_put` finding again, in the cache that
was written to close a neighbouring one. ramfs
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

**Mapped into user space (the file-regions unit,
`docs/kernel/memory/design.md` §7).** A cache frame carries
`PG_PAGECACHE` and may be installed in user address spaces by a
`VM_REGION_FILE` region; each installation takes one reference to it
under `pc->lock` (`pagecache_fault_page`), so `refcount == 1 + the PTEs
mapping it`, and the mapping puts the reference after its PTE is gone.
The cache keeps `mappings`, the list of `struct vm_file_map` records
(one per `mmap`, linked under `pc->lock`), and walks it in two places:
`pagecache_truncate` unmaps every record's pages from the dropped index
on *before* it frees them, and `pagecache_sync` lowers every present
writable PTE of a run *before* it reads the run for the disk, so a
write landing after that faults and dirties the page again. Reclaim
leaves a frame whose count is not one alone (`pinned_skips`). The
cache also keeps `trim_bound`: both filesystems trim the cache before
they lower `vn->size`, and a fault installs only below
`min(vn->size, trim_bound)` so that window cannot install a page past
the new end; a `pagecache_write` that grows the file lifts it. The
install itself happens under `pc->lock`, which is what serialises it
against a truncate or a write-back on the same file (invariant V33).

### Symbolic links and the walk

A fourth vnode type, `VNODE_LNK` (`COSMO_DT_LNK`, 6). The walk expands a
link the moment it meets one, because a link rewrites *the rest of the
path*: `walk_parent` replaces the path being walked with the target
followed by the remainder, and goes round its own loop.

- **The budget.** `VFS_MAX_SYMLINKS` (8) expansions per resolution, and
  the component count that already bounded the walk. Both answer
  `ELOOP`, which is what Linux answers for both. `resolve()` -- the
  whole-path form behind `vfs_lookup` -- carries the same budget to the
  last component, so a chain ending in a link cannot outrun it.
- **The buffers.** Two `VFS_PATH_MAX` halves of one allocation, taken on
  the first expansion and never by a walk that meets no link. They
  ping-pong: the target is read into the half that is not the current
  path and the remainder appended there, so the copy never overlaps the
  string it reads. An expansion that would exceed `VFS_PATH_MAX` is
  `ENAMETOOLONG`; nothing after the link is ever dropped.
- **Where a target resolves.** A relative target resolves against the
  directory the link was found in -- `step()` consumes the caller's
  reference to that directory, so the walk holds a second one across the
  step and releases it as soon as the child turns out not to be a link.
  An absolute target restarts at `vfs_current_root()`, the *calling
  process's* root: a link is bounded by the root a leading slash is
  bounded by, and a process confined to a subtree cannot be handed one
  out of it.
- **`..` after an expansion** names the target's parent, because the
  expansion is textual and happens before the component is stepped.

**The last component is the caller's decision.** `vfs_open` is a loop:
when the last component is a link it re-enters with the target path, so
`O_CREAT` creates the target of a dangling link, `O_TRUNC` truncates the
target and the permission checks are the target's. `O_NOFOLLOW` makes a
link named last `ELOOP` and says nothing about links in between;
`O_CREAT|O_EXCL` on an existing link is `EEXIST`, since that branch runs
before the expansion. `lstat` and `readlink` never follow; `unlink`,
`rmdir` and `rename` name an entry in a parent and never did.

### Unmounting a filesystem somebody is walking

A maintenance pass (`docs/audit/next-subsystem-fsctl.md`) walks a whole
filesystem while holding its lock. Nothing pinned a mount before:
`vfs_umount` infers busyness by scanning the vnode hash for references
beyond the ones the filesystem itself holds, and a walk holding a
kobject reference does not appear there. Tearing the buffers down under
a walk is a crash rather than a wrong answer, so the mount carries
`passes_running` and unmount **drains** it.

The handshake, and it needs no claim about who takes which lock first:

- `vfs_umount_at` (and `vfs_umount2`, its NULL-start form) refuses
  `-EBUSY` if `mnt->unmounting` is already set -- one unmount at a time,
  because the drain below drops `g_mounts_lock` and a second caller in
  that gap would remove the namespace links the first is removing. Only
  a path from inside the mount reaches it then (`follow_mount` refuses an
  unmounting mount to anyone crossing its mountpoint); `vfs-umount-once`
  fires it that way.
- `vfs_umount2` sets `mnt->unmounting` before anything else it decides,
  to turn new walkers away. `vfs_mount_acquire` is one of the things it
  turns away, so from that moment the count can only fall.
- The drain then waits for zero, **dropping `g_mounts_lock` across the
  wait** -- the thread that will decrement needs it, and waiting with it
  held is waiting for a thread that cannot run.
- No wakeup is lost: `wait_event` queues the waiter and marks it BLOCKED
  *before* it evaluates the condition, so a release landing in the gap
  wakes one that is already there. The release wakes on the falling edge
  only.

**The wait is bounded**, which is why it is a wait rather than `-EBUSY`:
a pass is a call the kernel makes and returns from, never a userland
round trip, so the longest wait is one pass over one filesystem. A
forced unmount waits on the same drain, because `VFS_UMOUNT_FORCE` is
about references held by files, not a kernel call in flight.

**Every decision taken before the drain is a decision about a
filesystem that may have changed.** `vfs_umount2` works out whether it
is the last namespace that can see the mount, and only then drains; a
namespace created during the wait copies its parent's view and takes a
reference, so that answer stops being true. The unmount counts again
after the drain and steps down to "this namespace forgets it" if anyone
else can now see the mount. Refusing to copy a dying mount would be the
wrong fix: an unmount can fail and be restored, and the child namespace
would then be permanently short a mount its parent has.

Dropping that lock mid-unmount opens a window the code never had, and
the first thing through it would be a second unmount: `g_mounts_lock`
used to be held unbroken from the namespace scan to the teardown, so two
unmounts of one mount serialised on it and nothing tested `unmounting`.
One does now. In practice `follow_mount` already refuses to walk to a
mount that is unmounting, so a second unmount by path fails before it
gets there; the guard is for the relative path resolved from inside the
mount, which does not traverse the mountpoint.

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

**Links in ramfs.** The target is a bounded string in the node, freed at
evict, and `ramfs_lnk_ops` has no `readpage` or `writepage`: a link is
never a page, which matters here because for ramfs the page cache *is*
the store.

### System calls (`uapi/cosmo/syscall.h`)

```text
11 open(path, flags, mode) -> h      12 stat(path, struct cosmo_stat *)    13 fstat(h, stat *)
14 lseek(h, off, whence) -> pos       15 mkdir(path, mode)                 16 unlink(path)
17 rmdir(path)                        18 rename(old, new)                  19 getdents(h, buf, len) -> bytes
20 sync()                             21 mount(source, target, fstype, flags) 22 umount(target)
```

`O_RDONLY 0`, `O_WRONLY 1`, `O_RDWR 2`, `O_CREAT 0x40`, `O_EXCL 0x80`,
`O_TRUNC 0x200`, `O_APPEND 0x400`, `O_NONBLOCK 0x800` (the named-pipes
unit: a FIFO's open rules and the open's mode; a regular file ignores
it), `O_DIRECTORY 0x10000`. `mknod(path, mode, type)` is 100 (the
named-pipes unit; `type` is `COSMO_DT_FIFO`).
`struct cosmo_stat { ino, type, mode, nlink, uid, gid, size, mtime_ns, ctime_ns }`;
`struct cosmo_dirent { ino, type, reclen, name[] }` records packed into
the `getdents` buffer (at least `sizeof(struct cosmo_dirent) + 2` bytes;
`-EINVAL` only when the very next entry does not fit). Paths are copied
with `strncpy_from_user` (1024
max); `open` installs the file with `HANDLE_RIGHT_READ`/`WRITE` from the
access mode, so `read`/`write`/`close` need no change. `mount` names a
block device (`vda`) and requires uid 0.

## Per-open character devices

A character device may keep state per open, not just per node: `chrdev_ops`
(and the underlying `vnode_ops`) carry optional `open`, `release`,
`read_file` and `write_file`. `open` runs on each open of the node and may
refuse (`-EBUSY`, `-ENOSPC`); it sets `struct file`'s `priv` to the device's
per-open instance. `read_file`/`write_file`, preferred over `read`/`write` by
`file_pread`/`pwrite`, carry the `struct file` so the device reaches `priv`.
`release` runs exactly once, when the last reference to that `struct file`
drops — the file is a `kobject`, so "last close" is a defined moment. A
refused `open` drops the file without running `release` (the `dev_open` flag
gates it), since `open` never succeeded. The invariant: a device's per-open
state lives and dies with its `struct file`, never with the vnode. Devices
that set none of the hooks (console, tty, `/dev/vmm`) behave exactly as
before; `/dev/net/tap` uses them to give each opener its own tap.

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
blk` holds everywhere. Since the file-regions unit `pagecache.lock →
vm_space.lock` as well: a FILE fault installs its page under the cache
mutex with the space's spinlock nested inside, and truncate and
write-back take each mapping's space lock the same way, while the space
lock is never held when the mutex is taken (a region's record is
unlinked from the cache after the lock is released). `bio_complete`
runs in interrupt context and only completes a `completion`.

## Memory

Vnode ~400 bytes plus page cache entries; ramfs stores every file page
resident; cosmofs keeps 64 metadata buffers (256 KiB) and a bitmap of
`nblocks/8` bytes (256 bytes for the 8 MiB test disk) per mount.

**A freed vnode is poisoned in debug builds** (`vnode_release` fills it
with `0x5a` before `kfree`), as the pmm already poisons freed frames. The
slab poisons nothing, so without this a vnode freed under a walk kept
looking like a directory until its memory was reused, and the cwd-ref
unit's use-after-free test passed on a broken kernel every time
(`docs/audit/next-subsystem-cwd-hold.md`, "Measured"). With it the same
test caught the bug in one boot of five; the held-walk seam below is
what makes it every boot.

## Socket and FIFO nodes

A unix socket's name in the filesystem is a `VNODE_SOCK` (the
unix-sockets unit, `docs/kernel/ipc/design.md`): a node with no contents
and no operations of its own, made by `bind` through the optional
`mknod` vnode operation (`vfs_mknod`, the same parent rules as
`create`), owned by the caller, mode 0755. The node is a name and an
access control -- `connect` needs write permission on it -- and not the
socket: the socket keeps a reference to the node from bind to release
and a registry in `kernel/ipc/unix.c` maps the node to it; `unlink`
removes the name and leaves the socket and its connections alone;
`open` of the node is `-ENXIO`. ramfs implements `mknod`; cosmofs has no
on-disk type for a socket and leaves it NULL, so `bind` on a cosmofs
path is `-EOPNOTSUPP` -- socket names live under the ramfs root, which
is where Unix keeps them too.

A named pipe is a `VNODE_FIFO` (the named-pipes unit,
`docs/kernel/ipc/design.md`, "Named pipes"), made by `SYS_mknod`
through the same `mknod` operation, owned by the caller, mode as given.
Unlike a socket's name it is opened: `vfs_open` applies the file's
permission checks and the node's per-open hooks (`open` waits for the
other side by POSIX's rules or refuses `-ENXIO`, `release` takes the
open's count back), `file_pread`/`file_pwrite` route to `read_file`/
`write_file` as for a character device (no vnode lock across the ring,
no position, `lseek` `-ESPIPE`), and the file's readiness comes from
the vnode's optional `ready`/`poll_wq`/`set_nonblock` -- three
operations `struct vnode_ops` and `chrdev_ops` gained in that unit,
which the file kobject type delegates to when present (a file without
them is always ready, never changes and cannot be made non-blocking,
as before). `open` keeps `COSMO_O_NONBLOCK` in `file->flags`. ramfs
allocates the node's `struct fifo` at `mknod` and frees it at evict;
cosmofs and procfs refuse the type as they refuse a socket's.

## Error handling

Every VFS entry validates before calling into a filesystem
(`-ENOENT`, `-ENOTDIR`, `-EISDIR`, `-EEXIST`, `-ENOTEMPTY`, `-EXDEV`,
`-EROFS`, `-ENAMETOOLONG`, `-EBADF`, `-EINVAL`). cosmofs reports `-EIO`
for any checksum, magic, kind or block-number mismatch and `-ENOSPC`
when the bitmap is exhausted; a failed operation inside a transaction
leaves the in-memory state consistent (allocation is the last
fallible step of every mutation, done before pointers are rewritten)
and the on-disk state untouched until commit.

## Write-back errors

The unit `docs/audit/next-subsystem-file-path.md`. Dirty pages are
written back at `fsync`, at `sync`, at a file's `close`, at a file's
last reference and at a vnode's last reference. The rule for a failure,
in three parts:

1. **The page cache remembers.** `pagecache_sync` stops at the first
   `writepage`/`writepages` error, leaves the failed pages dirty for the
   next attempt, and records the error where it is seen: `pc->wb_err`
   and `pc->wb_seq` (incremented per recording) under `pc->lock`, the one
   lock every write-back passes through. No caller records, and no
   caller's locking matters: `vnode_release` holds nothing, cosmofs's
   `sync` (which `vfs_sync` reaches through `mnt->fs->sync`) holds
   `vn->lock` per vnode, `file_sync` and `file_flush` hold `f->lock`
   then `vn->lock`. (`file_pread` and `file_pwrite` hold `vn->lock` for
   the regular-file path only: the character-device path runs with no
   filesystem lock, invariant V32.) `pagecache_error_since(pc, seen, &err, &now)` reads
   the pair under the same lock.
2. **Each open file hears once.** `file.wb_seq_seen` is set at open to
   the current sequence (a failure before the open is not this file's).
   `file_sync` (`fsync`) and `file_flush` (`close`) run the write-back
   and then consult the record: a failure recorded after `wb_seq_seen`
   -- this attempt's own or a neighbour's since -- is returned once, and
   `wb_seq_seen` advances to the current sequence as it is reported, so
   the next successful call returns 0. Linux's `errseq_t` contract
   without the wrapping arithmetic.
3. **`close` asks before it lets go.** `struct kobject_io_type` has an
   optional `flush`; `handle_close` calls it on the object it is about to
   put, before the put and outside the table lock, and returns its result
   -- the handle is closed regardless (POSIX allows `close` to fail with
   `EIO`; the descriptor is gone). The file's `flush` is `file_flush`.
   An exiting process's `handle_table_destroy` and a `dup2` over an open
   slot discard the result: nobody is there to read it. `file_release`
   keeps its last-reference write-back for a file that reached zero some
   other way, and `pagecache_sync` records its failure like any other.

**What the vnode's release does.** A named file's (`nlink > 0`) dirty
pages get one last attempt; what is then dropped is data lost with no
file left to tell, counted in `pagecache_stats.dropped_dirty` and said
once per event (`vfs: N dirty page(s) of inode I on FS lost: write-back
failed (E)`). An unlinked file's pages have no reader left: they are
neither written back (the audit's 8.2 LOW, which the release used to
do) nor counted. The `cosmofs-reserve` self-test is the case the rule
was written against: it fills a disk on purpose, and its last file's
sixteen pages -- accepted into the page cache before the bitmap ran out
-- fail at the release with `-ENOSPC`. That file is named, so the run
says so (`16 dirty page(s) of inode 16 on cosmofs lost: write-back
failed (-28)`): the one loss the suite makes, silent before this unit.
The test itself learns the `-ENOSPC` from its `fsync`.

Nothing in cosmofs's transaction model moves: a data-page refusal is a
returned error, not a poisoned mount (`cfs_fail` is reached only from
rename's recovery and the commit's post-superblock failure).

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

**The held-walk seam** (`docs/audit/next-subsystem-cwd-hold.md`;
`CONFIG_DEBUG` only, every entry point a no-op otherwise). The fix for
the cwd use-after-free -- a reference taken under the process lock for
the length of the walk (P29) -- has a window a few instructions wide
against a whole path walk, and a racer lands in it in some boots and not
others. The seam makes the interleaving certain. Armed by
`vfs_test_cwd_hold_arm(name)` for a process *name*, it holds the next
relative walk made by a process of that name at the one line every
relative walk from every caller shares -- `walk_parent`'s relative
branch, before `vnode_get(cur)`, with the pointer read and nothing yet
taken -- until that process's `chdir` has published its new directory
and put the old one. The order is enforced here, not arranged by the
test: `process_chdir` registers as the swapper before its own lookup
(so the seam never holds the thread that releases), waits for the hold
after the lookup and **before** it publishes (a swapper that reaches
`chdir` first must not install the new directory under a walk that has
yet to capture the old one), records the old directory's count before
the put, and releases after it. The walk on resume checks the directory
is live -- `VNODE_DIR`, count nonzero, count not the poison word;
`VNODE_DEAD` is recorded, not judged, because an unlinked directory a
walk still references is exactly what the proof produces. That the put
preceded the release is read by the **releasing** side at the instant it
releases -- the count one below what it read before its put -- because
a walk woken before the put loses the race to it every time, and a count
read on resume cannot tell the two orders apart (the release-before-put
mutation survived that derivation). Both waits are killable and bounded
(five and two seconds); a timeout is recorded and fails the test by name.
One arm serves one hold. A `chdir` in a single-threaded process registers
no swapper: there is no other thread whose walk it could pull from under.
`debug.cwd_hold` reads the state (0 idle, 1 armed, 2 held, 3 released,
4 armed with the swapper waiting inside `chdir`) -- the last is what lets
a racer start its walker only once the swapper is provably ahead of it.

## Future extensibility

- Snapshots: the superblock's `snap_root` points at a block listing
  immutable roots; taking one is recording the current root before the
  next commit and excluding its blocks from `pending_free`.
- Data checksums: `csum_root` → a tree keyed by block number.
- Multiple pool members and allocation groups behind `pool_*`.
- Hard links, `chmod`/`chown`, the sticky bit, memory-pressure
  eviction of *mapped* pages, a dentry cache, a host `mkfs`. Symbolic
  links are built (this document, "Symbolic links and the walk");
  `mmap` of files is built, over the frames the cache already owned
  (the file-regions unit, "Page cache" above).
