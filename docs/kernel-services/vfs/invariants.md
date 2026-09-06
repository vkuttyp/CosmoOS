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

**V24. A block is named by the member that holds it, and a run lies on
one member.** Every pointer on disk is a DVA: the member in the top 8
bits, the block within it in the low 56. Nothing else in the filesystem
changed shape, because a version-2 or -3 pointer *is* a version-4 DVA
with vdev 0 — so those disks mount and are written unchanged, with no
conversion and no second decoder. Each member carries its own allocation
index and bitmap, so losing a member cannot take another member's bitmap
with it; the allocator works on a linear index that concatenates them,
laid out so no bitmap chunk straddles two members, and the padding at a
member's end is marked allocated at mount so it can never be handed out.
`cfs_alloc_run` never returns a run that crosses a member, which is what
keeps an extent's `count` meaningful, and `extent_valid` checks both ends
against the same member. A commit copies the member table and every
member's allocation index before reserving destinations for its dirty
bitmap chunks, because a copy takes a block that the same commit's
bitmaps must show; a dirty chunk with no reserved destination fails the
transaction rather than writing a bitmap over member 0's superblock. A
member table is disk data and is checked against what the format can
express — block count, chunk count, first usable block, and the size of
the device actually carrying it — before a mount believes any of it. A metadata block's self-check is its DVA, so a
read served from the wrong member fails exactly as a misdirected read
within one does. Check: `cosmofs-pool2` (two members: both carry blocks,
their free counts sum to the pool's, a remount finds the second by its
label alone, and a snapshot spans both), `cosmofs-v3` (a version-3 disk
mounts, writes, remounts and takes a snapshot under this kernel),
`cosmofs-badmembers` (a member table claiming more chunks than an index
can hold, a member larger than its device, or a first usable block
inside the label: each refused on a freshly formatted pool, where there
is no older root to fall back to, and the mount works again once the
table is put back), `pool`
(a DVA naming a member the pool does not have addresses nothing), and
the host test's DVA arithmetic and member-table sizes.

**V25. A mirror is read with a checker, and a copy that missed a commit
is not mirrored.** A member may be a group of up to four devices holding
the same blocks, so the DVA is unchanged by adding a copy. Reading one
copy of two and trusting it doubles the chance of returning something
wrong, so every read is verified by the filesystem — metadata by its own
header (kind, its own DVA, CRC), data and directory blocks by the
per-block CRC32C in their inode's checksum tree — and the first copy
that verifies is written back over the copies that did not. A block no
copy can satisfy is `-EIO` for that block, not a poisoned mount. Writes
go to every copy, attempt all of them even after a failure, and return
the first error before any root is published. The commit flushes every
device of every member before the root write, because that write's own
preflush reaches only the devices holding the superblock: a root naming
blocks still in another member's cache could outlive them.

Checksums cannot tell a *stale* copy from a current one: a device
detached while the pool went on being written carries older blocks whose
every checksum is valid. So each device past member 0 carries its own
label stamped with the commit it last took part in, written after that
commit's blocks are stable and before the root that publishes them, and
member 0's devices carry the superblock, which is the same evidence. A
copy whose generation is *older* than the one being mounted is left out
and the pool comes up degraded — older rather than different, because a
commit interrupted after the labels and before the root leaves labels
one ahead of the durable root, and those devices hold everything that
root names. Nor does the label's copy number decide which device serves:
any current copy may be a member's first, since preferring the one
labelled 0 would serve a stale disk ahead of a good one. `cosmofs_scrub` reads **every** copy of
every block, because a read stops at the first copy that verifies and
rot behind it would stay invisible until that copy was the one
answering. Check: `cosmofs-mirror` (rot one copy of a data block and the
read still answers; scrub finds and repairs rot on a copy no read would
have touched; a second scrub finds nothing; both copies rotted is `-EIO`
for that file while the rest of the tree reads; a device aged by one
generation, with valid checksums throughout, is refused and the mount
reports one degraded copy).

**V26. A compressed record is one object, and its checksums are of what
is on the disk.** Compression works on records — up to
`CFS_RECORD_BLOCKS` consecutive logical blocks written as one — because
a single block that compresses to a quarter of itself still occupies a
block. A record's physical size lives in the top bits of the extent's
`count`, so no structure on disk changed width and every earlier
filesystem's runs decode as uncompressed. A record is compressed only if
it comes out strictly smaller in whole blocks; otherwise it is stored as
it was, which is always allowed and never wrong.

Nothing may cut a record. `set_extent` displaces one whole and frees its
blocks; `drop_range` removes one before its surviving blocks are written
back plain; both refuse a record that hangs out of the range they were
given rather than freeing half of it. Overwriting one page inside a
record reads the record, replaces the page and writes it again — the
cost of compression, paid by the write that caused it. Truncating into
one does the same and then keeps only the blocks below the new end, so
what is past it cannot reappear when the file grows.

The per-block checksums an inode keeps are checksums of what is *on the
disk*, so for a record they cover its physical blocks: entry `i` of the
record's logical range is the checksum of its `i`th physical block.
Repair on read (V25) and scrub therefore work on a record without
decompressing anything, and the scrub reads a record once rather than
once per logical block it holds. Check: `cosmofs-compress` (a
compressible file in a fraction of its blocks, an incompressible one
stored as it is, a page rewritten inside a record, a partial page that
has to read the record first, truncation into the middle of a record,
re-extension reading zeros, and a clean scrub), `test_lz4` and
`fuzz_lz4` for the codec that stands behind all of it.

**V27. What is encrypted is authenticated, and what is not encrypted is
said out loud.** An encrypted filesystem seals a block with its file's
key — derived from a master key that the user's key only wraps — and a
nonce of ninety-six random bits, drawn at write time and stored beside
the tag. Deriving the nonce from the block number and the generation
would tie the cipher's one hard requirement to a promise the filesystem
cannot keep: the older-root fallback reuses a generation while the
abandoned attempt's ciphertext is still on the disk, and a stream cipher
repeating a (key, nonce) pair hands out the xor of two plaintexts.
Compression runs before encryption, because the other order leaves
nothing to compress.

Each entry in the checksum tree holds a Poly1305 tag over the
ciphertext, the generation, and a plain CRC32C of the same bytes. The
two answer different questions: the CRC needs no key and is what a
mirror repairs against and a scrub verifies, so both keep working on a
machine that cannot decrypt the filesystem; the tag needs the file's key
and is what says a block is the one that was written, which a CRC cannot
say because whoever changed it could recompute it. The tag is checked
before the block is decrypted, so a forged block never becomes
plaintext. A wrong user key is refused by the tag over the wrapped
master key rather than producing rubbish, and rotation rewraps that one
block without rewriting a file.

What is *not* encrypted is the shape of the filesystem: allocation
bitmaps, the inode map, inode records, extent chains, the member table
and the superblock. An attacker with the disk learns how many files
exist, how large they are, when they were written, their owners and
modes — and not one file name or byte of content, because a directory's
entries are the data blocks of its inode. That last point cuts both
ways: a mount with no key cannot walk a path at all, and answers
`-ENOKEY` to a lookup while a scrub still reads every block. Check:
`cosmofs-crypt` (the plaintext is absent from the block the file
occupies; a wrong key is `-EKEYREJECTED`; a bent block is refused, not
returned; a keyless scrub reads and repairs; rotation keeps every file
readable; a keyless remount refuses lookups; and the same block written
eight times gives eight different ciphertexts, which is what a repeated
nonce would break), and `test_chacha20` against RFC 8439.
