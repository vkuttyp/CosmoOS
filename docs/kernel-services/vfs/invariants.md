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
flushes, then writes the superblock into the slot the current root did
not come from, then flushes. Before the superblock write the previous
root is intact because every block the transaction touched was free in
it; after it the new root is complete. Check: `cosmofs-crash` (mutate,
discard the transaction, remount: the previous state and free count
are unchanged; corrupt the newer slot: mount falls back to the older
generation and still reads every file). Gap: no power-cut injection at
arbitrary points of the commit sequence; the two-flush ordering relies
on the device honouring `flush`.

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

**V5. Every metadata block read from the pool is verified before use.**
Magic, kind, own block number and CRC32C (`mhdr_check`); superblocks
also magic, version, block size, total blocks and CRC. Extents,
inode numbers, directory entry lengths and block numbers are
range-checked before they index anything. Check: `cosmofs-format` (an
unformatted disk is `-EIO`), `cosmofs-crash` (a corrupted superblock is
ignored). Gap: no fuzzing of on-disk images.

**V6. Inode numbers are never reused.** `next_ino` only grows; a freed
inode's slot is zeroed. Check: review. Gap: none needed while the
64-bit space is not exhaustible in practice.

**V7. Lock order.** `g_mounts_lock` → `mount->lock` → `vnode->lock`
(parent before child; for `rename`, both parents in address order) →
`pagecache.lock` → `cfs->lock` → block layer. `file->lock` is taken
alone before any vnode lock. A vnode's last reference is never dropped
while `mount->lock` is held (release takes it). Check: review; the
self-tests run every operation under the scheduler's hang watchdog.
Gap: no lock-order checker.

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

**V15. Data written to a file reaches the filesystem only through
`writepage`, and cosmofs never leaves a hole inside a file's mapped
span.** Runs cannot express holes, so `cfs_writepage` first allocates
zero blocks for unwritten logical blocks below the one being written;
`readpage` returns zeros beyond the mapped span. Check: `cosmofs-ops`
(a 45-block file with a rewrite in the middle reads back exactly);
`pagecache` (hole reads). Gap: sparse files consume disk for their
holes; a hole-capable extent format is future work.

**V16. cosmofs writes an inode through to its (copy-on-write) inode
block on every mutation.** `struct cfs_vnode` caches the inode; the
buffer cache therefore always holds the complete pre-commit state and
eviction has nothing to flush but pages. Check: `cosmofs-ops` (remount
after `vfs_sync` shows every change). Gap: none.

**V17. Commit happens only on `vfs_sync` and unmount.** There is no
auto-commit threshold; dirty metadata buffers and pending frees grow
until then. Check: `cosmofs-format` (unmount of an untouched mount does
not advance the generation), `cosmofs-ops` (generation advances by one
per `vfs_sync`). Gap: an unbounded transaction can exhaust memory or
free space; a threshold is future work.

**V18. The pool is the only thing cosmofs addresses, and the pool
addresses one device.** `cosmofs_core.c`/`cosmofs.c` call `pool_*` only;
`pool.c` calls `blk_*` only. Check: `pool` self-test; review of
includes. Gap: none.
