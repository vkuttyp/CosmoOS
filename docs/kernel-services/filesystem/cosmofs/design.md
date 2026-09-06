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
| `EXTENTS` (6) | `uint64_t next` (the chain) then 253 × 16-byte `struct cfs_extent` |
| `CSUMIDX` (7) | 508 block numbers of `CSUM` blocks (an inode's checksum index) |
| `CSUM` (8) | 1016 CRC32C values, one per logical block |
| data | file contents or directory entries, no header; checksummed in the owner's `CSUM` tree |

Inode `i` lives in inode block `i / 15`, slot `i % 15`; that inode
block is entry `(i/15) % 508` of the `IMAP0` block found at entry
`(i/15) / 508` of the `IMAP1` root. Capacity: 3.87 M inodes, 16.5 M
blocks (63 GiB). Inode 1 is the root directory; inode 0 is never used.

```c
struct cfs_extent { uint64_t start; uint32_t count; uint32_t lblk; };   /* logical [lblk, lblk+count) -> pool [start, start+count) */
struct cfs_extent_block { uint64_t next; struct cfs_extent ext[253]; }; /* the payload of an EXTENTS block */
struct cfs_inode {                                                        /* 256 bytes */
    uint32_t mode;               /* CFS_TYPE_REG=1 or CFS_TYPE_DIR=2 in bits 12+, permissions below */
    uint32_t nlink, uid, gid;
    uint64_t size, mtime_ns, ctime_ns;
    uint64_t generation;         /* transaction that last wrote it */
    uint64_t ino;                /* self check; 0 in a free slot */
    struct cfs_extent direct[10];
    uint64_t indirect;           /* head of the EXTENTS chain or 0 */
    uint64_t parent;             /* directories: parent inode (root points at itself) */
    uint32_t csum_algo;          /* CFS_CSUM_CRC32C (1) for every inode version 2 writes; 0 = none */
    uint32_t csum_pad;
    uint64_t csum_root;          /* CSUMIDX block or 0 */
    uint64_t reserved;
};
struct cfs_dirent { uint64_t ino /* 0 = free slot */; uint8_t type, namelen; uint8_t pad[6]; char name[48]; };  /* 64 bytes, names up to 47 */
```

A file's logical block `n` is found by walking its runs, sorted by
`lblk`, direct then the chain (`cfs_map_block`); a logical block no run
covers is a hole that reads as zeros, wherever it lies. Sequential
allocation merges runs that are adjacent both logically and physically
(`extents_merge`); a rewrite in the middle of a run splits it into up to
three. The implementation loads at most `CFS_MAX_EXTENTS` (4096) runs
per inode, a fragmentation bound rather than a format limit (version 2,
below).

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
excluded from `pending_free`), a pool-wide checksum tree (the
superblock's `csum_root`), inode slot reuse, B-tree directories,
multiple pool members behind `pool_*`, a host `mkfs` and `fsck` over
`cosmofs_format.h`, transaction groups in flight while the next one is
open. Hole-aware extents, per-inode data checksums, the writeback
thread and `fsync` durability arrived with audit milestone 7 (below).

## Format version 2 and the transaction engine (audit milestone 7)

Audit milestone 7 (`docs/audit/2026-09-post-roadmap-audit.md` §19;
findings #22, #23, #25, #26). Everything above describes version 1
except where this section says otherwise; the code implements version 2
only, and a version-1 image is refused at mount with a clear message
(the scratch disk is reformatted on every boot test, and no version-1
image is expected to exist anywhere else).

### What changes on disk

`CFS_VERSION` is 2. The superblock, the metadata header, the inode map
and the allocator are unchanged. Three things change:

**Extents carry their logical position.** `struct cfs_extent { uint64_t
start; uint32_t count; uint32_t lblk; }` (the former `pad` is `lblk`):
a run maps logical blocks `[lblk, lblk + count)` to pool blocks
`[start, start + count)`. Runs are sorted by `lblk` and never overlap;
a logical block that no run covers is a **hole** and reads as zeros.
`writepage` no longer fills the span below a written block with zero
blocks (V15's gap and the sparse-write exhaustion of #23 are gone).
Files are bounded at 2^32 blocks (16 TiB) by the 32-bit `lblk`.

**The extent block is a chain.** The payload of a `CFS_KIND_EXTENTS`
block is `uint64_t next` (0 or the next extent block) followed by 253
extents. The inode's `indirect` heads the chain; an inode therefore
holds 10 direct runs plus any number of chained blocks, and the 264-run
`-EFBIG` cap of #23 is gone (a badly fragmented file costs one chained
block per 253 runs and a longer linear walk).

**Every data and directory block has a checksum stored in the inode's
metadata.** The inode's `reserved[3]` becomes `uint32_t csum_algo;
uint32_t pad; uint64_t csum_root; uint64_t reserved;`. `csum_algo` is
`CFS_CSUM_CRC32C` (1) for every inode version 2 writes (0 would mean
"no checksums", kept for a future option). `csum_root` points at a
`CFS_KIND_CSUMIDX` block (kind 7): 508 pointers to `CFS_KIND_CSUM`
blocks (kind 8), each holding 1016 CRC32C values; logical block `l`'s
checksum is in index slot `l / 1016`, entry `l % 1016`. The checksum
tree is metadata: copy-on-write, sealed, freed with the inode. The
audit suggested the extent entry as the home of the checksum; a run of
`count` blocks has no room for `count` checksums without giving up
contiguity, so the per-inode tree is the "parent pointer" here, with
the algorithm id in the inode and the superblock's `csum_root` still
reserved for a pool-wide tree.

A `writepage` or directory-block write computes the block's CRC32C and
stores it before the inode is written through; `readpage` and the
directory reader verify the block against its entry and return `-EIO`
(and log the block number) on mismatch (finding #26). A hole has no
entry and is not checked. Truncation leaves entries past the new end
stale but unreachable; a later write at that position overwrites them.

### Allocation: contiguity and the metadata reserve

`cfs_alloc_run(fs, class, hint, want, &start, &got)` finds the first
free run at or after `hint` and returns up to `want` consecutive blocks
(at least one). `writepage` asks for the block after the file's
previous logical block when that one is mapped (`hint = pblk(l-1) + 1`),
so a sequentially written file lays out in one run; milestone 6's
in-order writeback made sequential the common case.

Two allocation classes exist. `CFS_ALLOC_DATA` (file data blocks) is
refused with `-ENOSPC` once free blocks are at or below the **reserve**
(`max(32, nblocks / 32)`); `CFS_ALLOC_META` (inode map, inode blocks,
extent and checksum blocks, directory blocks, bitmap chunks at commit)
may use the reserve. Directory blocks count as metadata because a
deletion rewrites one. Deleting a file needs a copy-on-write directory block,
inode block and bitmap chunks, which come from the reserve, and its
blocks become allocatable after the commit that publishes the deletion:
the full-disk deadlock of #23 is closed. The reserve is a policy of the
mounted filesystem, not an on-disk field; mount recomputes it.

### `fsync` commits

`cfs_vnode_sync` (the VFS `sync` operation, reached from `file_sync`
after the page cache wrote the file's dirty pages) writes the inode
through and then **commits the open transaction**. A transaction is
whole-filesystem, so `fsync` of one file publishes every mutation made
so far; the durability the caller asked for is exactly the commit
protocol above (finding #22). `vfs_sync` is unchanged.

### The writeback thread and dirty thresholds

Every mount starts one kernel thread (`cfs-wb/<dev>`) that wakes every
`CFS_WB_POLL_MS` (100 ms) and commits when any of these holds and the
open transaction is non-empty:

| Trigger | Threshold |
|---|---|
| dirty metadata buffers | `CFS_WB_DIRTY_BUFS` (64) |
| pending frees | `CFS_WB_PENDING` (512) |
| dirty pages of the mount (`mount.cache_dirty`, kept by the page cache) | `CFS_WB_DIRTY_PAGES` (256, 1 MiB) |
| age of the oldest uncommitted change | `CFS_WB_INTERVAL_MS` (5000) |

It commits through `cosmofs_sync` under `mount.sync_lock`, taken with
`mutex_trylock` so it never waits on an unmount or a `vfs_sync` in
progress (it retries on the next poll). Unmount sets `fs->wb_stop` and
joins the thread before destroying the filesystem. The loss window after
a crash is bounded by the interval; the transaction's memory by the
buffer and page thresholds. Two test hooks exist:
`cosmofs_test_set_writeback(mnt, on)` turns the thread's commits off
(the replay harness needs every superblock write to be one it asked
for) and `cosmofs_test_set_writeback_interval(mnt, ms)` shortens the
age trigger.

### Older-slot fallback at mount

Mount reads both superblock slots as before and prefers the newer valid
one. It then loads the allocator and the root inode; if either fails
(a bad header, a wrong checksum, a read error), it logs a warning and
retries with the other slot when that one is valid. The next commit
writes into the slot the chosen root did not come from, which is the
unusable newer one, so a broken root is replaced by the first commit
after recovery. Both roots unusable is `-EIO`, as before.

### The block layer: flags and queueing

`struct bio` gains `flags`: `BIO_PREFLUSH` (the device's volatile cache
is flushed before this write) and `BIO_FUA` (this write is on stable
media when it completes). The block layer implements both without
driver support as a sequence of bios (flush, write, flush) chained by
their completions, so a driver sees only plain reads, writes and
flushes. `pool_write_flags(p, blk, buf, flags)` exposes them; the commit
uses `BIO_PREFLUSH | BIO_FUA` for the superblock, which is the two-flush
protocol written as one call.

A driver that returns `-EAGAIN` from `submit` (virtio-blk with every
slot in flight) no longer fails the caller: the block layer queues the
bio on the device and resubmits it from `bio_complete` as slots free
up, in order. `blk_submit` returns 0 for a queued bio and `done` runs
when it eventually completes; `blkdev.requeued` counts them. A burst
larger than the queue can therefore never poison a mount through
`cfs_fail` (finding #25). virtio-blk completes a `BIO_FLUSH` at once
when the device did not negotiate `VIRTIO_BLK_F_FLUSH` (no volatile
cache: nothing to flush), instead of sending an unsupported request.

The RAM block device gains a deferred mode for tests
(`ramblk_set_deferred(bd, limit)`): completions run on a worker thread
and the driver returns `-EAGAIN` above `limit` in-flight requests, which
is how the queueing is exercised (`blk-queue`).

### What this is not

Multiple transaction groups in flight (open, quiescing, syncing): one
open transaction per mount is still committed under `cfs->lock`, so
mutations wait during a commit. Snapshots, a pool-wide checksum tree,
multi-device pools, a scrub, `fsck` and a host `mkfs` remain future
work; the format keeps `snap_root`, `csum_root` and `members` reserved
for them.


## Format version 3: snapshots

The audit's storage line names four features — snapshots, redundancy,
compression, encryption (`docs/audit/2026-09-post-roadmap-audit.md` §19,
§8.4). They are four units, not one: redundancy needs the `struct dva`
addressing change §8.4 describes, compression needs a per-extent
algorithm and length, encryption needs key management that reaches into
`kernel/security`. **This unit is snapshots**, taken first because the
filesystem is already copy-on-write — a snapshot is the feature the
design was built for, and it is the only one of the four that needs no
new on-disk addressing.

### What a snapshot is here

A commit publishes a superblock naming `imap_root`, `alloc_root`,
`next_ino`, `inode_count` and a generation. Every one of those trees is
copy-on-write: a block that a newer tree does not reference is exactly a
block the commit put on `pending_free`. So a snapshot is that tuple,
kept, plus a promise not to free what it still names.

```c
struct cfs_snapshot {          /* 96 bytes, in a CFS_KIND_SNAPLIST block */
    uint64_t generation;       /* the commit this snapshot pins */
    uint64_t imap_root;
    uint64_t alloc_root;
    uint64_t next_ino;
    uint64_t inode_count;
    uint64_t deadlist;         /* head of a CFS_KIND_DEADLIST chain, or 0 */
    uint64_t created_ns;       /* wall clock, for `ls` */
    uint64_t id;               /* never reused: the vnode tag (see below) */
    char name[32];             /* NUL terminated, unique in the mount */
};
```

`cfs_super.snap_root` — reserved since version 2 for exactly this —
points at a `CFS_KIND_SNAPLIST` block of 42 such entries with a `next`
pointer, so the list chains like the extent list does. The version goes
to 3; a version-2 image mounts unchanged and gains an empty list on its
first commit, because every field it needs was already reserved.

### Not freeing what a snapshot names

The commit's release loop is where a snapshot bites:

```c
for each blk in pending_free:
    if no snapshot names blk:  clear the bitmap bit       /* as today */
    else:                      append blk to the newest snapshot's deadlist
```

"Does a snapshot name this block" needs no new bookkeeping, because the
question is already answered on disk: **a snapshot's allocation bitmap
is the set of blocks its tree occupies.** The allocator's bit is set for
a block exactly while something reaches it, and a commit publishes the
bitmap and the tree together, so `cfs_snapshot_references` is one lookup
in the `alloc_root` the snapshot already recorded — no birth times, no
reference counts, no per-block table.

At commit time the *newest* snapshot alone has to be asked: a block
reaching the free list was in the live tree until now, so if the newest
snapshot does not occupy it, it was born after that snapshot and no
older one can name it either. It is freed immediately. Otherwise its
bitmap bit stays set — the allocator never hands it out — and it is
remembered on that snapshot's deadlist.

Deleting a snapshot asks the same question of every block on its
deadlist, against the snapshots that remain: a block none of them
occupies goes straight back to the allocator, and the rest are handed to
the oldest remaining snapshot, which is asked again when it goes. The
deadlist's own blocks are always freed, since nothing else names them.

That is exact, and the difference is visible: a file written after
snapshot `first`, snapshotted by `second` and then deleted, has its
blocks returned the moment `second` is deleted, even though `first` is
still there. The first version of this code handed the whole deadlist to
the previous snapshot — safe, but those blocks stayed held until that
snapshot was deleted too. The `cosmofs-snapshot` self-test asserts the
free count rises at exactly that point.

### Reading a snapshot

Snapshots appear as `.snapshots/<name>/…` at the mount root: a synthetic
directory the root's `lookup` answers, whose children are the snapshots'
root directories. A `struct cfs_vnode` gains a snapshot pointer; every
read path that resolves an inode uses `vn->snap ? snap->imap_root :
fs->sb.imap_root`, and every write path refuses with `-EROFS` when it is
set. The tag is the snapshot's `id`, which is **never reused**: a
positional index would be reassigned when a deletion compacts the list,
and a cached vnode would then be handed to a different snapshot
entirely. Ids run to `CFS_SNAP_ID_MAX` (0xFFFE, since 0xFFFF is
`.snapshots` itself and 0 is the live tree); creation past that is
`-ENOSPC`. Nothing else in the read path changes, because a snapshot's trees
are ordinary trees — that is the whole point of copy-on-write.

`.snapshots` is not returned by `readdir` on the root: it is found by
name only, so nothing walking the tree descends into history by
accident, and `rm -r /mnt` cannot delete a snapshot.

### The interface

Kernel: `cfs_snapshot_create(fs, name)`, `cfs_snapshot_delete(fs, name)`,
`cfs_snapshot_list(fs, out, max)`. Creation commits first, so a snapshot
always names a durable tree, then writes the list and commits again.

Userland reaches them through the existing filesystem calls rather than
a new system call: creating `/mnt/.snapshots/<name>` as a directory
takes a snapshot, and `rmdir` on it deletes one. That keeps the surface
to what `mkdir` and `rmdir` already are, and makes the shell test the
same shape as every other filesystem test.

### When something goes wrong

Two rules, both the same one the storage layers above keep: never let go
of a block whose fate is unknown, and never publish a change that was
reported as failed.

- A snapshot list that cannot be read makes the commit **hold** the
  block rather than free it: an unreadable list is not evidence that
  nothing needs it.
- Deletion takes the whole list, however many blocks it spans — a
  snapshot left out of that set would have its blocks freed while it
  still named them.
- A commit that fails after the list has changed marks the mount failed,
  as every other unpublishable change here does, so a later commit
  cannot publish a snapshot that `mkdir` reported as failed or drop one
  `rmdir` did.

### What this unit does not do

- Redundancy, compression, encryption: three separate units, in that
  order, each with its own format extension.
- Rollback (making a snapshot the live tree), sending or receiving a
  snapshot, quotas, and per-snapshot space accounting beyond the
  deadlists.
- Space accounting *per snapshot* (how much a snapshot is keeping
  alive): the deadlists know, but nothing reports it.
- Snapshots of a mount that is read-only, and nested `.snapshots`
  inside a snapshot: both refused.
