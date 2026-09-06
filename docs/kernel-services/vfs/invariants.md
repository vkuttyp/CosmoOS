# VFS and storage: invariants

Rules that must not be broken without changing this document and the
code together. Each names how it is checked today and what is not yet
covered.

**V1. The VFS depends on no filesystem.** `kernel-services/vfs/vfs.c`
and `pagecache.c` include no filesystem header and call filesystems only
through `struct fs_type` and `struct vnode_ops`; ramfs is registered by
name like any other filesystem. Check: review of includes; `vfs-ramfs`
mounts a second ramfs and cosmofs through the same entry points. Gap:
no build-time enforcement (an include-path barrier like the arch one is
future work).

**V2. Every committed cosmofs root describes a completely valid
filesystem.** A commit writes every new metadata block and data block,
then writes the superblock into the slot the current root did not come
from with `BIO_PREFLUSH | BIO_FUA` (the block layer runs flush, write,
flush). Before the superblock write the previous
root is intact because every block the transaction touched was free in
it; after it the new root is complete. Check: `cosmofs-crash` (mutate,
discard the transaction, remount: the previous state and free count
are unchanged; corrupt the newer slot: mount falls back to the older
generation and still reads every file); `cosmofs-replay` (every prefix
of the write stream); `cosmofs-fallback` (a newer root whose tree does
not load is passed over for the older one, and the next commit replaces
it). Gap: the flush ordering relies on the device honouring `flush`; a
device that did not negotiate one is taken to have no volatile cache.

**V3. A block referenced by a committed root is never overwritten.**
Metadata is modified only after `cfs_buf_cow` moved it to a block that
was free in the committed root; data pages and directory blocks are
always written to freshly allocated blocks; the superblock alternates
slots. Check: review (`cfs_buf_cow`, `cfs_writepage`, `dir_write_block`,
`cfs_commit`); `cosmofs-crash`. Gap: no test compares the block image
before and after a discarded transaction.

**V4. Blocks freed in a transaction become allocatable only after that
transaction's root is durable.** They go on `pending_free`; the bitmap
bits are cleared after the superblock flush. The in-memory bitmap is
authoritative during a transaction; the on-disk copy is rewritten at
commit through the reserve-then-write fixpoint. Check: `cosmofs-ops`
(free count returns after a delete plus commit); review. Gap: the
fixpoint is exercised only with one bitmap chunk (8 MiB disk).

**V5. Every block read from the pool is verified before use.** Metadata:
magic, kind, own block number and CRC32C (`mhdr_check`); superblocks
also magic, version, block size, total blocks and CRC. Data and
directory blocks (since format version 2): CRC32C against the entry in
the owner inode's checksum tree, `-EIO` and a log line on mismatch.
Extents (range, order, no overlap), inode numbers, directory entry
lengths and block numbers are range-checked before they index anything.
Check: `cosmofs-format` (an unformatted disk is `-EIO`), `cosmofs-crash`
(a corrupted superblock is ignored), `cosmofs-csum` (a flipped byte in a
data block reads `-EIO` and a rewrite repairs it; in a directory block the
lookup is `-EIO`), `cosmofs-badmap` (an inode whose two direct runs were
swapped and re-sealed is `-EIO` on the map fast path, not read as a
hole), `fuzz_cosmofs` (`make fuzz`).

**V6. Inode numbers are never reused.** `next_ino` only grows; a freed
inode's slot is zeroed. Check: review. Gap: none needed while the
64-bit space is not exhaustible in practice.

**V7. Lock order.** `g_mounts_lock` → `mount->rename_lock` →
`vnode->lock` (parent before child, annotated `VNODE_NESTED_CHILD`; for
`rename`, the ancestor parent first and the second parent annotated
`VNODE_NESTED_PARENT2`, address order only for two unrelated directories)
→ `pagecache.lock` → `cfs->lock` → block layer. `mount->lock` is a spinlock
protecting only the vnode hash and is a leaf under any of them.
`file->lock` is taken alone before any vnode lock; `mount->sync_lock` is
taken alone before filesystem locks. `vnode_put` unhashes under
`mount->lock` before the last reference drops, so `vnode_release` takes no
mount lock and a hashed vnode always has a reference. (The earlier text
put `mount->lock`, then a mutex, above `vnode->lock`, while the code took
it below: the audit's finding; and the address order for rename's
parents was an ABBA against rmdir.) Check: the debug-build lock-order
checker on every boot (`docs/kernel/lockdep/`), test `vfs-concurrency`
(rename against rmdir/mkdir on two CPUs; open/close of one file on two
CPUs with the vnode count exact afterwards), and the self-tests under the
hang watchdog.

**V8. A vnode's page cache is written back and dropped only at release
or truncation.** Nothing evicts pages under memory pressure; a ramfs
file's only copy of its data is its page cache, which is why ramfs pins
its vnodes (V9). Check: `pagecache` (dirty counts, truncate tail
zeroing), `vfs-ramfs` (vnode count returns to baseline after unmount).
Gap: memory-pressure eviction is future work.

**V9. A linked ramfs node is pinned; an unlinked node lives while open.**
`VNODE_PINNED` marks the filesystem's reference; `unlink`/`rmdir` drop
it; an open `struct file` keeps the vnode and its data until the last
`file_put`. Check: `vfs-ramfs` (open, unlink, read 14 bytes, `nlink` 0
and `VNODE_DEAD`, then release). Gap: none.

**V10. No stacked mounts and no unmounting a busy mount.** `vfs_mount`
refuses a target that is already covered, is a mount root, or is `/`;
`vfs_umount` refuses while any vnode holds references beyond the
filesystem's own (pins, the mount's root reference). Check:
`vfs-ramfs` (`-EBUSY` on the second mount, `-EBUSY` while a file is
open, `-EBUSY` for `/`, `-EINVAL` for a non-root). Gap: none.

**V11. `vfs_umount` commits through `fs->sync` before dismantling
anything and keeps the mount if that fails; `fs->unmount` then runs
with the root alive and only drops state.** `VFS_UMOUNT_FORCE` is the
deliberate exception: it skips the commit so an abandoned transaction
can be dropped and the device released. A discarded (test hook) or
abandoned (`cfs_fail`, after a mutation that could not be completed
consistently) transaction is dropped there, leaving the last committed
root current. A filesystem's `evict` must tolerate
`mnt->fs_priv` already being NULL (cosmofs root eviction after
`cfs_destroy`). Check: `cosmofs-crash` relies on it (the discard hook
would be bypassed otherwise); `cosmofs-format` (unmount of an untouched
filesystem commits nothing). Once abandoned, every cosmofs operation
returns `-EIO`. Gap: the in-memory fields of vnodes that were already
open (`nlink`, `size`, times) still reflect the half-applied mutation,
so `fstat` on an existing handle can show it until the forced unmount;
nothing of it can become durable, and making `vnode_stat` consult the
filesystem would put filesystem state into the VFS (invariant 4), so
this is accepted.

**V12. Directory mutations are validated by the VFS before a filesystem
sees them.** Existence, kinds (`-EISDIR`/`-ENOTDIR`), emptiness for
`rmdir` and replacing renames, mountpoints (`-EBUSY`), cross-mount
renames (`-EXDEV`), a directory moved beneath itself (`-EINVAL`),
read-only mounts (`-EROFS`), `.`/`..` as targets (`-EINVAL`). Check:
`vfs-ramfs`, `cosmofs-ops`, init's `fs_selftest`. Gap: none.

**V13. Paths from user space are bounded copies.** `strncpy_from_user`
up to `VFS_PATH_MAX`, then the walk touches kernel memory only; `getdents`
fills a kernel buffer of at most 64 KiB and copies out; `stat` structures
are copied out. Check: init's `fs_selftest` (`-EFAULT` for a bad path
pointer). Gap: none.

**V14. Every namespace operation is checked against the caller's
credentials by `vfs_permission`; `mount`/`umount` need
`cred_privileged`.** Search on each directory entered, write and search
on the parent of a mutation, read/write on an opened file per its mode
(a just-created file excepted), execute for spawn; root passes all but
execute without any x bit; new vnodes take their creator's effective
ids. Check: `init --selftest` spawns `init --unpriv-test`, which drops
to uid 1000 and is refused `/dev/vmm`, a 0700 root directory, writing or
unlinking a 0644 root file, creating in a 0755 root directory,
executing a 0600 file, `mount`, `umount`, signalling root's process, and
the kernel log, while creating its own files in `/tmp` (owned 1000:1000)
and running `/bin/true` succeed, and root's entry in the sticky `/tmp`
cannot be unlinked or renamed by it. Gap: `chmod`/`chown`; no
group-permission test (no process holds a supplementary group yet).

**V14a. ramfs is bounded and the page cache is bounded.** The rules S6
and S7 of `docs/kernel/security/invariants.md`: a mount's cached pages
never exceed its budget (ramfs: 16 384), and the global limit reclaims
only clean pages of mounts with a backing store, never waiting on a
cache lock while holding another. Check: `cache-limits`.

**V15. Data written to a file reaches the filesystem only through
`writepage`, and a hole costs nothing.** Runs carry their logical
position (format version 2), so `cfs_writepage` writes exactly the block
asked for and `readpage` returns zeros for any logical block no run
covers. Check: `cosmofs-ops` (a 45-block file with a rewrite in the
middle reads back exactly); `cosmofs-holes` (a 200 MiB sparse file on a
4 MiB device costs five blocks, reads zeros in the holes, keeps its data
across a remount); `pagecache` (hole reads).

**V16. cosmofs writes an inode through to its (copy-on-write) inode
block on every mutation.** `struct cfs_vnode` caches the inode; the
buffer cache therefore always holds the complete pre-commit state and
eviction has nothing to flush but pages. Check: `cosmofs-ops` (remount
after `vfs_sync` shows every change). Gap: none.

**V17. Commit happens on `vfs_sync`, `fsync`, unmount, and on the
writeback thread's thresholds.** `file_sync` commits the open
transaction (a synced file is durable); the thread commits when dirty
buffers, pending frees or the mount's dirty pages pass their thresholds
or the oldest change is older than the interval, so neither the loss
window nor the transaction's memory is unbounded. Check: `cosmofs-fsync`
(a synced file survives a discarded transaction, an unsynced one does
not), `cosmofs-writeback` (a change commits on its own within the
interval and nothing commits when nothing is dirty), `cosmofs-format`
(unmount of an untouched mount does not advance the generation),
`cosmofs-ops` (each `vfs_sync` and `fsync` advances the generation by
one). The tests that count generations turn the thread off
(`cosmofs_test_set_writeback`).

**V19. A full disk can still delete and commit.** File data is allocated
from the data class, refused at or below the reserve (`max(32,
nblocks / 32)` blocks); metadata, directory blocks and the commit's
bitmap chunks come from the metadata class and may use it. Check:
`cosmofs-reserve` (a 256-block device filled to `-ENOSPC` keeps its 32
reserve blocks; two unlinks and a commit return the space and a new file
is written). Gap: the reserve is sized by a constant, not by the
metadata the filesystem actually holds.

**V20. A block-layer queue-full answer never reaches a filesystem.** A
driver's `-EAGAIN` parks the bio in the device's pending list, resubmitted
in order from completions; `blk_submit` returns 0 and `done` runs once.
Check: `blk-queue` (eight writes against two slots all complete, in
order; `requeued` counts six), review of `cfs_fail` callers (none can be
reached by `-EAGAIN`).

**V18. The pool is the only thing cosmofs addresses, and the pool
addresses one device.** `cosmofs_core.c`/`cosmofs.c` call `pool_*` only;
`pool.c` calls `blk_*` only. Check: `pool` self-test; review of
includes. Gap: none.

**V21. A snapshot's blocks are never handed out while the snapshot
exists, and are handed back the moment it stops needing them.** A commit
either clears a released block's bitmap bit or holds the block on the
newest snapshot's deadlist, decided by whether that snapshot's own
allocation bitmap still occupies it — which is exact, because a
snapshot's bitmap *is* the set of blocks its tree reaches, and a block
reaching the free list was in the live tree until now. Deleting a
snapshot asks the same question of every block it held against the
snapshots that remain: what none of them occupies goes back to the
allocator, the rest pass to the oldest remaining snapshot. Check:
`cosmofs-snapshot` reads history at depth while the live tree is
rewritten, and requires the free count to rise when a snapshot is
deleted whose blocks were born after an older snapshot that still
exists — the case a conservative scheme gets wrong;
`cosmofs-snapshot-remount` shows the list is on disk, not in memory.
The blocks a deletion releases and the entry that named
them are one transaction, so a failure once releasing has begun abandons
it rather than returning: the two must reach the disk together. An
unreadable snapshot list makes the commit hold the block, since a failed
read is not evidence that nothing needs it, and a deletion works from
the whole list however many blocks it spans. Gap: nothing reports how
much space a snapshot is keeping alive, and a deadlist that cannot be
extended (no memory) logs and holds the block to the end of the mount
rather than risk handing it out.

**V22. A snapshot is read-only, and history is reachable only by
name.** Every write path refuses a snapshot vnode with `-EROFS`, and
`.snapshots` is answered by `lookup` but never listed by `readdir`, so
nothing walking the tree descends into history by accident and `rm -r`
on the mount cannot reach a snapshot. Snapshot vnodes carry a tag above
the inode-number space so the VFS cache cannot confuse a snapshot's
inode with the live one — and that tag is the snapshot's `id`, never
reused while the filesystem lives, because a positional index would be
reassigned when a deletion compacts the list and a cached vnode would
then serve another snapshot's contents. `..` keeps the tag too — it is
resolved through the snapshot's own inode map, and at the snapshot's
root it is `.snapshots`, whose `..` is the live root — because an
untagged parent would step out of a read-only snapshot into a live,
writable vnode of the same inode number. Check: `cosmofs-snapshot`
(create, mkdir and unlink inside a snapshot all `-EROFS`; and take A and
B, delete A, take C, then require B and C each to read their own; and
`.snapshots/first/dir/../keep` reads the snapshot's contents and refuses
a write, while `.snapshots/..` is the live root), the
shell test (`SNAPTEST`), and the host test's tag arithmetic. Gap: `readdir` on `.snapshots` itself
lists the snapshots, but a snapshot's own `.snapshots` is not nested —
untested because nothing creates one.

**V23. Storage somebody is reading is not dismantled.** A snapshot's
blocks are freed by its deletion, but an open file inside it reads those
blocks through extents its vnode already holds, so a deletion under an
open handle would hand that reader whatever the allocator gave the
blocks next. `rmdir` inside `.snapshots` therefore refuses with `-EBUSY`
while any vnode of that snapshot is in use, the same answer a mount
gives while anything on it is open. The census is exact and needs no
counting of its own: a hashed vnode always holds a reference, so
`vnode_cache_any` over the mount's cache *is* the set in use, and the
only reference the deletion discounts is the one `rmdir` itself holds on
the snapshot's root. Nothing new can appear while it decides — every
path into a snapshot goes through `.snapshots`, whose lock `rmdir`
holds, and a walk already inside one holds a reference on the tagged
vnode it is standing on. Check: `cosmofs-snapshot` (hold a file open
inside a snapshot: `rmdir` gives `-EBUSY` and the file still reads; close
it and the same `rmdir` succeeds).
