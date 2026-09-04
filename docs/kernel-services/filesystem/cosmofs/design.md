# cosmofs: design

Source of truth for the layout is `cosmofs_format.h`; for the in-memory
state `cosmofs_internal.h`, `cosmofs_core.c` and `cosmofs.c`.

## On-disk layout

Block size 4096 bytes; all integers little-endian; block numbers are
pool blocks (`pool_*`). Every metadata block starts with a 32-byte
header:

```c
struct cfs_mhdr { uint32_t magic /* "CFSM" */; uint32_t kind; uint64_t generation; uint64_t blkno; uint32_t crc; uint32_t pad; };
```

`crc` is CRC32C over the whole block with the field taken as zero;
`blkno` must equal the block's own number, so a misdirected write is
detected as well as a corrupted one. `generation` is the transaction
that wrote the block; a block whose generation equals the open
transaction may be modified in place, any other is copied first.

| Kind | Payload (4064 bytes) |
|---|---|
| `IMAP1` (1) | 508 block numbers of `IMAP0` blocks |
| `IMAP0` (2) | 508 block numbers of `INODES` blocks |
| `INODES` (3) | 15 × 256-byte `struct cfs_inode` |
| `ALLOCIDX` (4) | 508 block numbers of `BITMAP` blocks |
| `BITMAP` (5) | 32512 bits, one per pool block, 1 = allocated |
| `EXTENTS` (6) | 254 × 16-byte `struct cfs_extent` |
| data | file contents or directory entries, no header |

Inode `i` lives in inode block `i / 15`, slot `i % 15`; that inode
block is entry `(i/15) % 508` of the `IMAP0` block found at entry
`(i/15) / 508` of the `IMAP1` root. Capacity: 3.87 M inodes, 16.5 M
blocks (63 GiB). Inode 1 is the root directory; inode 0 is never used.

```c
struct cfs_extent { uint64_t start; uint32_t count; uint32_t pad; };          /* a run of `count` pool blocks */
struct cfs_inode {                                                              /* 256 bytes */
    uint32_t mode;               /* CFS_TYPE_REG=1 or CFS_TYPE_DIR=2 in bits 12+, permissions below */
    uint32_t nlink, uid, gid;
    uint64_t size, mtime_ns, ctime_ns;
    uint64_t generation;         /* transaction that last wrote it */
    uint64_t ino;                /* self check; 0 in a free slot */
    struct cfs_extent direct[10];
    uint64_t indirect;           /* EXTENTS block or 0 */
    uint64_t parent;             /* directories: parent inode (root points at itself) */
    uint64_t reserved[3];
};
struct cfs_dirent { uint64_t ino /* 0 = free slot */; uint8_t type, namelen; uint8_t pad[6]; char name[48]; };  /* 64 bytes, names up to 47 */
```

A file's logical block `n` is found by walking its runs in order,
direct then indirect (`cfs_map_block`); a logical block beyond the runs
is a hole that reads as zeros. Runs cannot express a hole *inside* the
span, so `cfs_writepage` allocates zero blocks for any unwritten
logical block below the one being written. At most 264 runs per file;
sequential allocation merges adjacent runs (`extents_merge`), and a
rewrite in the middle of a run splits it into up to three.

Directories are ordinary file data: 64 entries per block, linear scan,
a removed entry is zeroed, a new entry takes the first free slot or
appends a block. `.` and `..` are synthesised by `readdir` from the
inode and its `parent` field.

### Superblock

Blocks 0 and 1 are slots A and B of:

```c
struct cfs_super {
    uint8_t magic[8] = "COSMOFS1"; uint32_t version = 1; uint32_t block_size = 4096;
    uint64_t total_blocks, generation, imap_root, alloc_root, next_ino, inode_count, free_blocks;
    uint64_t csum_root, snap_root;   /* reserved: data checksums, snapshot roots */
    uint64_t members = 1;            /* pool members */
    uint64_t reserved[8]; uint32_t crc; uint32_t pad;
};
```

Mount reads both, discards any slot whose magic, version, block size,
total blocks, generation (0 = never written) or CRC is wrong, or whose
roots lie past the end, and keeps the valid slot with the higher
generation. Commit writes the *other* slot. `free_blocks` is advisory:
mount recounts from the bitmap and warns on a mismatch.

### A freshly formatted disk (`cosmofs_format`, 2048-block example)

```text
block 0   superblock A: generation 1, imap_root 4, alloc_root 2, next_ino 2, inode_count 1, free 2041
block 1   superblock B: zeroed (invalid until the first commit)
block 2   ALLOCIDX:     [0] = 3
block 3   BITMAP:       bits 0..6 set (blocks 0–6 in use), bits past the end set, everything else clear
block 4   IMAP1:        [0] = 5
block 5   IMAP0:        [0] = 6
block 6   INODES:       slot 1 = root directory (mode dir|0755, nlink 2, parent 1, size 0)
block 7…  free
```

One bitmap block covers 32512 blocks; a larger device gets one per
chunk after block 3, and the imap blocks follow them. Formatting
refuses fewer than 64 blocks or more than 508 chunks.

## In-memory state (`struct cfs`)

Per mount: the pool, the in-memory superblock (its root fields advance
during the open transaction), which slot the committed root came from,
`gen` (= committed generation + 1, the open transaction), the bitmap
(one bit per block, authoritative during a transaction) with a dirty
flag per chunk and an allocation hint, `pending_free` (blocks released
in this transaction), the metadata buffer cache (`struct cfs_buf`:
block number, 4 KiB, dirty, refcount; MRU list, clean unreferenced
buffers evicted beyond 64), one mutex, and the `discard_on_unmount`
test flag. Per vnode: `struct cfs_vnode` holding the inode as last
written through.

## Operations

- **Read path.** `cfs_buf_get(blkno, kind)` returns a cached buffer or
  reads the block and verifies its header (`-EIO` on any mismatch).
  `cfs_inode_read` walks IMAP1 → IMAP0 → INODES; a slot whose `ino` or
  `nlink` is 0 is `-ENOENT`. `cfs_readpage` maps the logical block and
  reads it, or zero-fills a hole.
- **Copy on write.** `cfs_buf_cow(&buf, &parent_slot)` returns
  immediately if the buffer already belongs to the open generation;
  otherwise it allocates a block, copies, seals the copy with the new
  generation and number, marks it dirty, puts the old block on
  `pending_free`, and stores the new number into the parent's slot (an
  `IMAP1` entry, an `IMAP0` entry, the superblock's `imap_root` or
  `alloc_root`, an inode's `indirect`). `inode_block(ino, writable,
  create)` applies this along the whole inode-map path and creates
  missing `IMAP0`/`INODES` blocks.
- **Write-through inodes.** Every mutation (`create`, `mkdir`, `unlink`,
  `rmdir`, `rename`, `writepage`, `truncate`, directory block writes)
  ends with `inode_sync`, which copies the vnode's public fields into
  the cached inode and `cfs_inode_write`s it. The buffer cache thus
  always holds the complete pre-commit state and eviction of a clean
  vnode needs no metadata write.
- **Data and directory writes.** Always to a freshly allocated block:
  `cfs_alloc_block`, `pool_write`, `cfs_set_block` (rewrites the runs;
  returns the previous block, which goes on `pending_free`), then
  `inode_sync`. `cfs_truncate_blocks` cuts the runs at `keep` blocks and
  defers the rest.
- **Allocation.** First fit from the hint in the in-memory bitmap,
  never a block on `pending_free` (its bit is still set), marking the
  chunk dirty; `free_blocks` decremented. Frees are deferred.
- **Eviction.** `cfs_evict` on a vnode with `nlink` 0 truncates it to 0
  blocks, zeroes the inode slot and decrements `inode_count`; otherwise
  it only frees the cached inode. `mnt->fs_priv` may already be NULL
  (root eviction after unmount), in which case nothing is done.

## Commit (`cfs_commit`, under `cfs->lock`)

1. If nothing is dirty and nothing is pending, return (the generation
   does not advance; unmounting an untouched filesystem writes nothing).
2. **Bitmap.** CoW the allocation index. Reserve a destination block
   for every dirty chunk; reserving sets bits and may dirty more
   chunks, so iterate until no dirty chunk lacks a reservation. Then
   write each dirty chunk from the in-memory bitmap into its reserved
   block, point the index at it, defer the old bitmap block, clear the
   chunk's dirty flag. Re-seal the index.
3. Re-seal and write every dirty buffer; `pool_flush`.
4. Write the superblock (generation = `gen`, `free_blocks` = the
   in-memory count, which still excludes the pending frees) into the
   slot the current root did **not** come from; `pool_flush`. From here
   the new root is durable.
5. Mark buffers clean, clear the pending blocks' bits (marking their
   chunks dirty for the next commit), add them to `free_blocks`,
   `gen++`.

Commit is triggered by `vfs_sync` (`cosmofs_sync`: first
`cfs_sync_vnodes` writes back every dirty regular file **without**
`cfs->lock`, since `writepage` takes it, then the metadata commit) and
by unmount (`cosmofs_unmount`, unless `discard_on_unmount` is set, in
which case the buffers, bitmap and pending list are simply dropped).
There is no size or time threshold.

### Crash cases

- Before step 4's superblock write: the previous root is intact. Every
  block written by the transaction was free in it (V3/V4), including
  the new bitmap blocks and the new index.
- During the superblock write (torn): its CRC fails; mount takes the
  other slot, the previous root.
- After step 4: the new root is complete. `pending_free` blocks are
  still marked allocated in the new root's bitmap and are reclaimed by
  the next commit; a crash loses nothing but that reclamation, and the
  count is reconciled at mount from the bitmap.

## Locking

`cfs->lock` is one mutex per mount, taken beneath the VFS's vnode locks
and above the block layer. `cfs_sync_vnodes` runs before taking it.
`cfs_vnode_get` is called with it held and calls
`vnode_lookup_cached`/`vnode_hash_insert` (which take `mount->lock`);
the order `vnode->lock → cfs->lock → mount->lock` never reverses because
the VFS releases `mount->lock` before calling any filesystem code.

## Memory

Per mount: 64 buffers × 4 KiB, `nblocks/8` bytes of bitmap, one dirty
flag per chunk, the pending list (grows by doubling from 64 entries),
`struct cfs`. Per vnode: 256 bytes of cached inode. Directory and
extent operations use one or two 4 KiB `kmalloc` scratch blocks and
stack arrays of up to 266 extents (4.3 KiB) in `cfs_set_block`.

## Errors

`-EIO` for any header, checksum, range or superblock problem; `-ENOSPC`
when the bitmap is exhausted or an inode number would exceed the map;
`-EFBIG` for more than 264 runs; `-ENAMETOOLONG` above 47 bytes;
`-ENOTEMPTY`, `-EEXIST`, `-ENOENT` as the VFS expects. A failed
mutation may leave buffers dirty for the open transaction but never
touches the committed root; a later commit publishes whatever
consistent state the in-memory structures hold.

## Future extensibility

Snapshots (`snap_root` → a list of immutable roots whose blocks are
excluded from `pending_free`), data checksums (`csum_root`), hole-aware
extents, inode slot reuse, an auto-commit threshold, B-tree
directories, multiple pool members behind `pool_*`, a host `mkfs` and
`fsck` over `cosmofs_format.h`.
