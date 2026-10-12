# cosmofs: API

Every interface cosmofs offers, in four groups: what the VFS calls
(`struct fs_type`, `struct vnode_ops`), the kernel entry points in
`kernel/include/kernel/cosmofs.h`, the operator channel `/dev/fsctl`
(`kernel/include/uapi/cosmo/fsctl.h`), and the on-disk format
(`kernel-services/filesystem/cosmofs/cosmofs_format.h`). Each entry
follows constitution section 52. None of these functions is exported to
modules; cosmofs is built into the kernel.

Common to every kernel entry point below:

- **Context.** Thread context only. Every one that touches a mount,
  except the test hooks noted below, takes `cfs->lock` (a `struct mutex`,
  one per mount) and may perform pool I/O, so each may sleep and must not
  be called from interrupt context, with a spinlock held, or with
  `cfs->lock` already held. The test hooks that only set or read a field
  of a live mount (`cosmofs_test_fail_freelog`, `cosmofs_test_fail_snapfill`,
  `cosmofs_test_fail_orphan`, `cosmofs_test_discard_on_unmount`, the
  snapshot counters) take no lock: the self-test that calls them is the
  mount's only user while it does, and that is the caller's to ensure.
- **Allocation.** Every one may call `kmalloc` (4 KiB scratch blocks,
  the bitmap, the pending lists); `-ENOMEM` is a possible result
  wherever an error is returned.
- **"Not a cosmofs".** `cfs_of(mnt)` is `mnt->fs_priv`
  (`cosmofs_internal.h`). The entry points answer `-EINVAL` (or 0, or
  do nothing, for the `void` and count-returning hooks) when it is NULL
  -- an unmounted mount. They do **not** check `mnt->fs ==
  &cosmofs_fs_type`; passing another filesystem's mount reinterprets its
  private data. Only `/dev/fsctl` reaches `check` and `scrub` through
  `mnt->fs`, which guarantees the type.

## VFS registration (`cosmofs_core.c`)

### `struct fs_type cosmofs_fs_type`

| Field | Function | Notes |
|---|---|---|
| `name` | `"cosmofs"` | the type name `vfs_mount` and `mount(2)` take |
| `mount` | `cosmofs_mount` | below |
| `unmount` | `cosmofs_unmount` | below |
| `sync` | `cosmofs_sync` | commit the open transaction |
| `check` | `cosmofs_check` | the structural check, reached from `/dev/fsctl` |
| `scrub` | `cosmofs_scrub` | the scrub, reached from `/dev/fsctl` |
| `format` | `cosmofs_format` | one device, unencrypted; reached from `/dev/blkctl` `COSMO_BLKCTL_FORMAT` through `vfs_format` |

### `void cosmofs_init(void)`
Registers `cosmofs_fs_type` with `vfs_register_fs`. Called once from
`bootfs_init` (`kernel/core/bootfs.c`). Panics if registration fails.

### `cosmofs_mount(fst, bdev, flags, mnt)` (static; via `vfs_mount`)
- Purpose: assemble the pool from `bdev` (members past the first are
  found by their labels among registered block devices), read both
  superblock slots, load the newer valid root, fall back to the older
  slot when the newer root's member table, bitmap, free-record replay,
  orphan replay or root inode does not load, and publish the root vnode
  in `mnt->root`.
- Inputs: `bdev` must be non-NULL (`-EINVAL`). **`flags` is ignored**:
  cosmofs parses no mount options. `MOUNT_RDONLY` is enforced by the VFS
  for ordinary writes and by `cfs_mkdir`/`cfs_rmdir` for snapshots
  (`-EROFS`), but the mount itself still replays the free and orphan
  records into the open transaction, and `cosmofs_sync` does not test
  the flag, so a read-only mount whose root carries either record
  commits it (at unmount, or from the writeback thread). No test covers
  a read-only mount of such an image.
- Encryption: when the root has `key_root` (version 7 or later) the mount
  reads the key from the firmware configuration string `fskey`
  (`opt/cosmo/fskey`) and unwraps the master key. A wrong key logs and
  the mount continues locked; an unseeded entropy pool leaves it locked
  (`-EAGAIN` from `cfs_need_seeded`, section S18 of
  `docs/kernel/security/invariants.md`). A locked mount succeeds and
  answers `-ENOKEY` to every data and directory read.
- Results: 0; `-EIO` when neither slot is valid or neither root loads
  (a slot whose version is outside `CFS_VERSION_MIN..CFS_VERSION` is
  logged by number and refused); `-ENOMEM`; errors from `pool_open`.
  On failure `mnt->fs_priv` is NULL and nothing is left allocated.
- Side effects: the writeback thread is started only after the mount is
  complete (`fs->mount_done`), if the replay left the transaction dirty.

### `cosmofs_unmount(mnt)` (static; via `vfs_umount2`)
The VFS has already called `cosmofs_sync` unless `VFS_UMOUNT_FORCE` was
given. Stops and joins the writeback thread, then frees everything
(`cfs_destroy`); a transaction that is still open -- forced unmount,
`cosmofs_test_discard_on_unmount`, or abandoned by `cfs_fail` -- is
dropped and the device keeps its last committed root. Always returns 0.

### `cosmofs_sync(mnt)` (static; via `vfs_sync`, unmount, the writeback thread)
Writes back every dirty regular file without `cfs->lock`
(`cfs_sync_vnodes`, because `writepage` takes it), then takes the lock
and commits (`cfs_commit_ex`; steps in `design.md`, "Commit"). Returns 0
without writing when nothing is dirty, so an untouched mount never
advances the generation; `fs->failed` (an abandoned transaction) when
set; the first pool or allocation error otherwise. Callers hold
`mount.sync_lock` (the writeback thread takes it with `mutex_trylock`).

## Vnode operations (`cosmofs.c`)

Called by the VFS with the vnode locks it documents
(`docs/kernel-services/vfs/api.md`); each takes `cfs->lock` beneath
them. Common results: `-EIO` for an unmounted filesystem, an abandoned
transaction, or any block that fails its header or checksum; `-ENOKEY`
for a data or directory read on a locked encrypted mount; `-EROFS` for
any write inside a snapshot or in `.snapshots`.

| Table | Operations |
|---|---|
| `cfs_dir_ops` | `lookup`, `create`, `symlink`, `mkdir`, `unlink`, `rmdir`, `rename`, `readdir`, `sync`, `evict`, `setattr` |
| `cfs_file_ops` | `readpage`, `writepage`, `writepages`, `truncate`, `sync`, `evict`, `setattr` |
| `cfs_lnk_ops` | `readlink`, `evict` |

- Names: at most `CFS_NAME_MAX` (47) bytes, else `-ENAMETOOLONG`.
- `create`/`mkdir`: a new inode number from `next_ino` (never reused);
  owner is the caller's effective uid and gid; regular files are
  created with `compress_algo = CFS_COMPRESS_LZ4`, directories with
  none; `csum_algo` is `CFS_CSUM_POLY1305` on an encrypted filesystem,
  `CFS_CSUM_CRC32C` otherwise. `-ENOSPC` when the bitmap or the inode
  map is exhausted.
- `symlink`: `-EOPNOTSUPP` on a filesystem whose superblock version is
  below 8. The target is written in the same transaction as the entry.
- `mkdir`/`rmdir` **inside `.snapshots`** take and delete snapshots
  (`cfs_snapshot_create`, `cfs_snapshot_delete` in `cosmofs_snap.c`).
  Creation commits the open transaction first, then writes the list.
  Results: `-ENAMETOOLONG` above `CFS_SNAP_NAME_MAX` (31); `-EINVAL` for
  an empty name or one containing `/`; `-EEXIST`; `-ENOSPC` once ids
  reach `CFS_SNAP_ID_MAX` (0xFFFE); `-EROFS` on a `MOUNT_RDONLY` mount;
  `-EBUSY` from `rmdir` while any vnode of that snapshot is in use;
  `-ENOENT` for an unknown name.
- `.snapshots` is found by `lookup` on the mount root only and is not
  returned by the root's `readdir`; its own `readdir` lists the
  snapshots.
- `readpage`/`writepage`/`writepages`: one 4 KiB page, or several
  consecutive dirty pages offered together (a whole aligned record of
  `CFS_RECORD_BLOCKS` is written compressed when that saves at least
  one block; anything else is written one page at a time and `*done`
  says how many were taken). A logical block no run covers reads as
  zeros. `-EFBIG` past the format's limits (below).
- `sync` (`cfs_vnode_sync`): writes the inode through and commits the
  whole filesystem's open transaction; this is what `fsync` means here.
- `evict` (`cfs_evict`): for a vnode with `nlink == 0`, releases its
  blocks and clears its slot; a failure abandons the transaction
  (`cfs_fail`) rather than publish blocks freed under a slot that still
  names them. Returns nothing.

## Kernel entry points (`kernel/include/kernel/cosmofs.h`)

### Formatting

#### `int cosmofs_format(struct blkdev *bd)`
- Purpose: write an empty filesystem at `CFS_VERSION` (11): superblock A
  at generation 1, slot B zeroed, one member, the root directory as
  inode 1, `next_ino` 2.
- Results: 0; `-EINVAL` for a device below `CFS_MIN_BLOCKS` (64) blocks,
  above `CFS_MAX_BLOCKS`, or needing more than `CFS_PTRS_PER_BLOCK`
  (508) bitmap chunks; `-ENOMEM`; pool I/O errors.
- Concurrency: none of its own. The caller must ensure the device is not
  mounted; `vfs_format` refuses a mounted device with `-EBUSY` (V36 in
  `docs/kernel-services/vfs/invariants.md`). The superblock is written
  last with `BIO_PREFLUSH | BIO_FUA`.

#### `int cosmofs_format_pool(struct blkdev **bd, unsigned n)` / `int cosmofs_format_mirror(struct blkdev **bd, unsigned n, unsigned copies)`
`n` members (1..`CFS_MAX_MEMBERS`, 255), each of `copies` devices
(1..`CFS_MAX_COPIES`, 4), taken from `bd` member by member. Member 0
carries the superblocks and the member table; every other device gets a
label (`struct cfs_label`). Same results as `cosmofs_format`, plus
`-EINVAL` for `n == 0` or `copies` out of range. Callers today: the
self-tests only.

#### `int cosmofs_format_encrypted(struct blkdev *bd, const void *key, size_t len)`
Formats one device with a random master key wrapped by `key`
(`CFS_KIND_KEYS` block named by `key_root`). `-EINVAL` for an empty key;
`-EAGAIN` (or `-EINTR`) when the entropy pool is not seeded within
`RANDOM_KEYGEN_WAIT_NS`, in which case the device is not written.
Concurrency: the key is passed to `format_at` through the file-scope
`g_format_key`/`g_format_key_len` with no lock, so a concurrent
`cosmofs_format` on another device could pick it up. The only caller is
the `cosmofs-crypt` self-test.

#### `int cosmofs_rekey(struct mount *mnt, const void *key, size_t len)`
Rewraps the master key under a new user key with a new salt
(`cfs_keys_rotate`). The rewritten key block is in the open transaction
and becomes durable at the next commit; this call does not commit.
`-ENOTSUP` for an unencrypted filesystem, `-ENOKEY` for a locked mount.
No operator interface reaches it; the only caller is `cosmofs-crypt`.

### Statistics

#### `int cosmofs_stats(struct mount *mnt, struct cosmofs_stats *out)`
Fills `out` under `cfs->lock`, so the generation and the commit counts
are one snapshot: last committed `generation`, in-memory `free_blocks`,
`total_blocks`, `inode_count`, `dirty_buffers`, `pending_frees`
(pending plus exempt), `reserve_blocks`, `commits` and `wb_commits`
since mount, `csum_failures`, `members`, `devices`, `repairs`,
`degraded`, the on-disk `version`, `free_root` (0 below version 9),
`orphan_root` (0 below version 10) and `pending_orphans`. `-EINVAL` for
an unmounted mount. The struct is kernel-internal and not an ABI.

### Maintenance passes

Both are compiled in every build and reach an operator through
`/dev/fsctl` (below). Both hold `cfs->lock` for the whole walk, so every
mutation and commit on that mount waits until the pass returns. Both
return `-EIO` without walking when the open transaction is abandoned.

#### `int cosmofs_scrub(struct mount *mnt, struct cosmofs_scrub_stats *out)`
- Purpose: read every block the filesystem reaches -- both superblock
  slots, every member's allocation index and bitmap, the snapshot list
  and deadlists, every inode's metadata and data -- on **every copy** of
  a mirrored member (`cfs_verify_all`), and write a good copy over each
  bad one.
- Output: `blocks_read`, `inodes`, `repaired`, `unrecoverable`. `out` may
  be NULL.
- Results: 0; `-EIO` when anything was unrecoverable (`*out` is still
  filled); `-ENOMEM`; `-EINVAL` for an unmounted mount. Needs no key:
  it verifies the keyless CRC32C kept beside each authentication tag.

#### `int cosmofs_check(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags)`
- Purpose: the structural check (`cosmofs_check.c`; `design.md`, "The
  structural check"). Walks the live tree, every snapshot, the allocation
  maps and the inode map, and compares what is reached with what the
  bitmaps and superblock say.
- Inputs: `flags` is 0 or `COSMOFS_CHECK_REPAIR`. Other bits are ignored.
- Output: thirteen `struct cosmofs_check_class` (count, repaired, up to
  `CFS_CHECK_NAMES` (8) offending block or inode numbers): `alloc_not_seen`,
  `seen_not_alloc`, `dup`, `nlink_wrong`, `orphan`, `dangling_entry`,
  `dir_bad`, `extent_order`, `extent_overlap`, `dir_dup_name`,
  `counter_wrong`, `chain_cycle`, `unreadable`; walk totals; `partial`
  (something was unreadable or the duplicate-name confirmations hit
  their cap of 256 per directory); `clean` (every class empty **and**
  not `partial`); `repair_refused`; `elapsed_ns`.
- Repair: with `COSMOFS_CHECK_REPAIR`, only `alloc_not_seen`, `orphan`,
  `nlink_wrong` and `counter_wrong` are repaired, working from the full
  maps rather than the eight names. If the walk skipped anything
  (unreadable block, malformed entry, dangling entry, cycle) no repair
  is made and `repair_refused` is set. The repairs go into the open
  transaction; this function does not commit them.
- Results: 0 whenever the walk ran, whatever it found; `-ENOMEM` before
  the walk starts (all maps are allocated up front); `-EIO` for an
  abandoned transaction; `-EINVAL` for an unmounted mount.

### Test hooks

Declared in `cosmofs.h` and compiled in every build (only
`cosmofs_test_name_hash` is under `CONFIG_DEBUG`); called only by the
self-tests in `cosmofstest.c`, `cosmofscrash.c`, `vfstest.c`,
`faulttest.c` and `devtest.c`. Not an operator interface, no stability
promise.

| Hook | What it does |
|---|---|
| `cosmofs_test_discard_on_unmount(mnt, on)` | the next unmount drops the open transaction, as a crash before the root write would |
| `cosmofs_test_set_writeback(mnt, on)`, `cosmofs_test_set_writeback_interval(mnt, ms)` | turn the writeback thread's commits off/on; shorten its age trigger |
| `cosmofs_test_mount_writeback(mnt, &notes, &early)` | dirty marks the mount's replay made, and how many found a writeback thread already running |
| `cosmofs_test_format_version(bd, v)` | format at an older version (`CFS_VERSION_MIN`..`CFS_VERSION`) |
| `cosmofs_test_poison_free_root(bd, v)`, `cosmofs_test_poison_freelog_count(bd, n)` | write a value into slot A's `free_root` word; write a too-large count into the live root's free record (re-sealed) |
| `cosmofs_test_fail_freelog`, `cosmofs_test_fail_snapfill`, `cosmofs_test_fail_orphan` | make the record fill, deadlist fill or orphan fill return `-EIO` inside the next commit |
| `cosmofs_test_orphan_add(mnt, ino)` | put a still-linked inode into the orphan set |
| `cosmofs_test_corrupt(mnt, kind, ino, &what)` | seventeen named corruptions (`enum cosmofs_corruption`); `ino` 0 is accepted only by the whole-filesystem kinds |
| `cosmofs_test_block_of(mnt, ino, lblk, &dva)` | where a logical block lives; `-ENOENT` for a hole |
| `cosmofs_test_member_free(mnt, vdev)` | free blocks on one member, `UINT64_MAX` past the last |
| `cosmofs_test_deadlist_len`, `cosmofs_test_deadlist_dups`, `cosmofs_test_snap_root`, `cosmofs_test_snap_walks`, `cosmofs_test_snap_verdicts` | deadlist entries (all, or naming one block); entries whose block another entry names; the snapshot list's head block; list walks and verdicts since mount |
| `cosmofs_test_unlock(mnt, key, len)` | supply the key to a locked mount; `-ENOTSUP` if unencrypted, `-EKEYREJECTED` for a wrong key |
| `cosmofs_test_name_hash(name, len)` | the checker's duplicate-name hash (debug builds) |

## The operator channel: `/dev/fsctl`

Kernel side `kernel-services/vfs/fsctl.c`; ABI
`kernel/include/uapi/cosmo/fsctl.h`; user tool `userland/system/fsctl.c`
(`/sbin/fsctl`). The design is in `docs/audit/next-subsystem-fsctl.md`.
The node is created by `fsctl_dev_init` with mode 0600 and logs
`fsctl: /dev/fsctl ready (0600): mount listing, check and scrub` on every
boot, release included.

### Protocol
- `open`: `-EPERM` without `PRIV_SYS_ADMIN`. Each open file owns its own
  result buffer (`struct fsctl_open`, its own mutex).
- `write`: exactly `sizeof(struct cosmo_fsctl)` bytes (16), else
  `-EINVAL`. `PRIV_SYS_ADMIN` is checked again (`-EPERM`). `version` must
  equal `COSMO_FSCTL_VERSION` (2), else `-EINVAL`. `flags` may contain
  only `COSMO_FSCTL_F_REPAIR`, and only for `CHECK`; any other bit is
  `-EINVAL`. An unknown `op` is `-EINVAL`. A command that fails leaves no
  result. Returns the length written on success.
- `read`: the whole result or nothing: 0 bytes when no command has
  succeeded on this file, `-ERANGE` when the buffer is smaller than the
  result. A result may be read more than once.
- Blocking: `CHECK` and `SCRUB` run the pass synchronously in the
  writer's thread, holding the mount by `vfs_mount_acquire` (an unmount
  waits for the pass, V30); the call may sleep for the whole walk.

### Operations
| `op` | Result after `struct cosmo_fsctl_result` | Failures |
|---|---|---|
| `COSMO_FSCTL_LIST` (1) | `count` records of `struct cosmo_fsctl_mount` (id, namespace id, flags, `caps`, fstype, path): the root mount first at `/`, then the caller's mount namespace; `total` larger than `count` means the list changed while it was built | `-ENOMEM` |
| `COSMO_FSCTL_CHECK` (2) | one `struct cosmo_fsctl_check`: `nclasses` (13), `COSMO_FSCTL_R_PARTIAL`/`_CLEAN`/`_REPAIR_REFUSED`, the walk totals, and `class[13]` in the fixed index order of `fsctl.h` | `-EINVAL` for id 0; `-ENOENT` for an id this namespace does not hold; `-EBUSY` for a mount being unmounted; `-EOPNOTSUPP` for a filesystem without `check`; any error of `cosmofs_check` |
| `COSMO_FSCTL_SCRUB` (3) | one `struct cosmo_fsctl_scrub` (`blocks_read`, `inodes`, `repaired`, `unrecoverable`) | as CHECK, and any error of `cosmofs_scrub` |

Because `cosmofs_scrub` returns `-EIO` whenever `unrecoverable` is
non-zero, and a failed command leaves no result, an operator never
receives a SCRUB record with `unrecoverable > 0`: the write fails with
`-EIO` and the counts are only in the kernel log.

### `fsctl(8)`
`fsctl list`, `fsctl check <id> [--repair]`, `fsctl scrub <id>`. Ids are
decimal digits only and non-zero. Exit status: 0 when the command
worked (a check with findings is 0), 1 when the kernel refused, the pass
failed, or repair was refused, 2 for a usage error.

### ABI stability
`COSMO_FSCTL_VERSION` 1 carried ten check classes; version 2 appended
`extent_order`, `extent_overlap` and `dir_dup_name` at indices 10-12 and
moved nothing. The check record is fixed-size and written whole, so a
size change is a version bump: a version-1 client is refused at the
version gate. `cosmo_fsctl_result.bytes` gives the record size so a
reader can skip records it does not know. `struct cosmofs_check_report`
is not the ABI; `fsctl_run` copies it field by field.

## Other ways in

- `mount(2)` (`cosmo_mount(dev, path, "cosmofs", flags)`) and the root
  switch (`root=` on the command line, `docs/kernel-services/vfs/`) reach
  `cosmofs_mount` through the VFS.
- `/dev/blkctl` `COSMO_BLKCTL_FORMAT` with `fstype` `"cosmofs"` reaches
  `cosmofs_format` through `vfs_format`; `cosmo-install`
  (`userland/system/cosmo-install.c`) uses it to make partition 2.
- Snapshots: `mkdir` and `rmdir` under `<mount>/.snapshots`, reading
  under `<mount>/.snapshots/<name>/`.
- There is no system call for keys, compression policy, rekeying or
  member management.

## On-disk format and its stability

`cosmofs_format.h` is the normative layout; `design.md` explains it by
version. All integers are little-endian, blocks are 4096 bytes, and the
superblock magic is `"COSMOFS1"`.

- **Versions.** This kernel writes `CFS_VERSION` 11 and mounts
  `CFS_VERSION_MIN` 2 through 11. A version-1 image, or one newer than
  11, is refused at mount with a message naming the version. Mounting an
  older image never rewrites its superblock version.
- **What each version added**: 2 extents with `lblk`, extent chains,
  per-inode checksum trees; 3 snapshots (`snap_root`); 4 DVAs and the
  member table (`members`, `uuid`); 5 mirror copies (`cfs_member.copies`,
  label generation); 6 compressed records (bit 31 of
  `cfs_extent.count`); 7 encryption (`key_root`, `CFS_CSUM_POLY1305`);
  8 symbolic links (`CFS_TYPE_LNK`); 9 the free record (`free_root`,
  `CFS_KIND_FREELOG`); 10 the orphan record (`orphan_root`,
  `CFS_KIND_ORPHAN`); 11 `cfs_mhdr.csum_algo`.
- **Gates.** Fields added by a version are read only when the
  superblock's version is at least that version (`free_root` from 9,
  `orphan_root` from 10, `key_root` from 7, the member table from 4) and
  written only then. Symlink creation is refused below 8. **Compression
  is not gated**: `cfs_create_common` gives every new regular file
  `CFS_COMPRESS_LZ4` whatever the superblock version, so a version-2..5
  image written by this kernel can hold compressed records its own
  version does not define. The snapshot list is likewise written into a
  version-2 image on its first snapshot, by design (`design.md`,
  "Format version 3").
- **Limits.** `CFS_NAME_MAX` 47; `CFS_SNAP_NAME_MAX` 31; `CFS_MAX_INODES`
  508 x 508 x 15; `CFS_MAX_BLOCKS` 508 x 32512 per member;
  `CFS_MAX_MEMBERS` 255; `CFS_MAX_COPIES` 4; `CFS_MAX_EXTENTS` 4096 runs
  loaded per inode (`-EFBIG` above; an implementation bound). A file's
  logical blocks are bounded by the 32-bit `lblk` (`set_extent`:
  `-EFBIG`), but the checksum index is one level, so `csum_block_at`
  refuses (`-EFBIG`) a block at or past `CFS_CSUM_MAX_BLOCKS` (516128
  blocks, about 1.97 GiB) on a CRC32C file, and past 508 x
  `CFS_AEAD_PER_BLOCK` (127) = 64516 blocks (252 MiB) on an encrypted
  file.
- **Host-testable.** `cosmofs_format.h` has no kernel dependency and is
  compiled by `tests/host/test_cosmofs.c`, which fixes the structure
  sizes and field offsets the format depends on.
