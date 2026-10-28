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
| `format` | `cosmofs_format_label` | one device, unencrypted, named or not; reached from `/dev/blkctl` `COSMO_BLKCTL_FORMAT` through `vfs_format` |

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

- Names: at most `CFS_NAME_MAX` (47) bytes below format version 15, and
  `CFS_NAME_MAX_LONG` (255) from it; the first longer name raises the pool
  to 15 (design.md, "Format version 15"), and a pool that cannot be raised
  answers `-ENAMETOOLONG`.
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

#### `int cosmofs_format_label(struct blkdev *bd, const char *label)`
- Purpose: `cosmofs_format`, with the superblock's `label` set to `label`
  (shell completeness S6): up to `CFS_LABEL_MAX` (16) bytes, NUL-padded;
  NULL or `""` leaves it zero. The VFS's `format` for cosmofs.
- Results: as `cosmofs_format`, and `-EINVAL` for a longer label. What
  characters a label may hold is `/dev/blkctl`'s rule, not this one's.

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

#### `int cosmofs_format_keys(struct blkdev **bd, unsigned n, const char *label, const uint8_t uuid[16], const uint8_t master[32], const struct cosmo_keyslot *slots)`
Roadmap 1.2 B5 (`design.md`, "Format version 14"): an encrypted pool on
`n` devices (1, or 2 as one mirrored member) at version 14, named
`label`, with `uuid` as its uuid, `master` as its master key and
`slots` (eight, `uapi/cosmo/keyslot.h`) as user space sealed them. The
uuid is the caller's because a slot's associated data names it: the
slots are sealed before the pool exists. Every block it writes
is MAC'd; on success the key is loaded in the key table, so the pool
mounts at once. `-EINVAL` for a slot of the wrong shape (a key-file
slot past 0, a passphrase slot with no passes or too little memory) or
no usable slot, an all-zero uuid, or a longer label; `-EAGAIN`/`-EINTR` when the entropy
pool is not seeded (the uuid and the format's own draws need it);
otherwise as `cosmofs_format`. The VFS's `format_keyed`, reached from
`/dev/blkctl` `FORMAT_KEYED`. `cosmofs_test_format_keyed_members`
(`CONFIG_DEBUG`) makes one of several members, for the label test.

#### `int cosmofs_keyctl(struct blkdev *bd, struct mount *mnt, struct cosmofs_key_req *req)`
The key of a version-14 pool, by device (`bd`) or by mount (`mnt`),
never both (`-EINVAL`). The VFS's `keyctl`, reached from `/dev/fsctl`'s
`KEY_*` operations (below).
- `COSMOFS_KEY_INFO`: fills `req->info` (`struct cosmo_fsctl_keyinfo`):
  encrypted, MAC'd, unlocked; the format, uuid, label, generation; and
  for a version-14 pool the eight slots as stored. By device the
  superblock and the key block are read **unverified** -- without the
  key nothing else is possible, and a slot authenticates itself when it
  is opened -- and `-EXDEV` when the key block is not on member 0.
- `COSMOFS_KEY_LOAD` (device): installs `req->key` in the key table if a
  superblock slot and the key block verify under it (CF31);
  `-EKEYREJECTED` when no slot does, `-EIO` when the key block does not,
  `-EINVAL` for a pool that is not version 14 with a key block,
  `-ENOSPC` when eight pools are already unlocked.
- `COSMOFS_KEY_UNLOAD` (device): drops an entry no mount holds;
  `-EBUSY` when one does, `-ENOKEY` when there is none.
- `COSMOFS_KEY_SLOT` (mount): writes `req->rec` to slot `req->slot` and
  commits. `req->key` must be the mount's master key (`-EKEYREJECTED`);
  emptying the last slot is `-EBUSY`; `-EROFS` read-only; `-EINVAL` for a
  slot of the wrong shape or a pool that is not version 14.
Concurrency: the key table has its own spinlock (`cosmofs-keys`); the
slot write runs under `fs->lock`.

#### `int cosmofs_rekey(struct mount *mnt, const void *key, size_t len)`
Rewraps the master key under a new user key with a new salt
(`cfs_keys_rotate`). The rewritten key block is in the open transaction
and becomes durable at the next commit; this call does not commit.
`-ENOTSUP` for an unencrypted filesystem, `-ENOKEY` for a locked mount,
`-EOPNOTSUPP` for a version-14 pool (its slots change through
`COSMOFS_KEY_SLOT`).
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

#### `int cosmofs_scrub(struct mount *mnt, struct cosmofs_scrub_stats *out, unsigned flags)`
- Purpose: read every block the filesystem reaches -- both superblock
  slots, every member's allocation index and bitmap, the snapshot list
  and deadlists, the rollback record, every inode's metadata and data in
  the live tree and in each snapshot's and clone's (roadmap 1.2 B2) --
  each block once, on **every copy** of a mirrored member
  (`cfs_verify_all`), and with `COSMOFS_SCRUB_REPAIR` write a good copy
  over each bad one. Without it nothing is written.
- Pacing: at most `g_cosmofs_scrub_rate` blocks a second (sysctl
  `fs.scrub_rate`, default `COSMOFS_SCRUB_RATE_DEFAULT` 4096, 0 for no
  bound), spent in 100 ms windows; between windows the scrub lets go of
  `cfs->lock` (design.md, "The scrub, as a command").
- Output: `blocks_read` (distinct blocks), `inodes`, `bad_copies`,
  `repaired`, `unrecoverable`, `skipped` (dirty in the open transaction),
  `elapsed_ns`, `rate`. `out` may be NULL.
- Results: 0 whenever the walk ran -- `unrecoverable` counts what no
  copy could satisfy (CFS-11; it was `-EIO`) -- `-EIO` for an abandoned
  transaction, `-ENOMEM`, `-EINVAL` for an unmounted mount. A metadata
  block the open transaction rewrote is skipped: it is not on the
  devices yet. Needs no key: it verifies the keyless CRC32C kept beside
  each authentication tag. On a read-only mount nothing is repaired.

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
- Read-only mount: `COSMOFS_CHECK_REPAIR` is `-EROFS`.
- `COSMOFS_CHECK_OFFLINE` (with REPAIR; `cosmofs_check_dev` adds it):
  also repair `seen_not_alloc`, by setting the block's bit.
  `COSMOFS_CHECK_BOOT` (with REPAIR): repair only the boot's classes
  (`alloc_not_seen`, `counter_wrong`, `snap_identity`, `rollback_record`,
  `hold`, `orphan`, `nlink_wrong`), and nothing when any other class was
  found or the answer is partial (`boot_refused`, `repair_refused`).
- Repair: with `COSMOFS_CHECK_REPAIR`, only `alloc_not_seen`, `orphan`,
  `nlink_wrong` and `counter_wrong` are repaired, working from the full
  maps rather than the eight names. If the walk skipped anything
  (unreadable block, malformed entry, dangling entry, cycle) no repair
  is made and `repair_refused` is set. The repairs go into the open
  transaction; this function does not commit them.
- Results: 0 whenever the walk ran, whatever it found; `-ENOMEM` before
  the walk starts (all maps are allocated up front); `-EIO` for an
  abandoned transaction; `-EINVAL` for an unmounted mount.

#### `int cosmofs_check_dev(struct blkdev *bd, unsigned flags, struct cosmofs_check_report *rep, struct cosmofs_dev_report *dev)`
- Purpose: the check on a device nothing has mounted (roadmap 1.2 B2):
  both superblock slots compared, the pool mounted where no namespace
  sees it (`vfs_mount_detached`; `MOUNT_RDONLY` unless
  `COSMOFS_CHECK_REPAIR`), `cosmofs_check` run with `flags` (plus
  `COSMOFS_CHECK_OFFLINE` when repairing), unmounted
  (`vfs_mount_detached_release`, which commits the writable mount when a
  repair was made, and otherwise drops what its mount changed).
- Output: `rep` as `cosmofs_check`'s; `dev` with `uapi/cosmo/fsctl.h`'s
  `COSMO_FSCTL_DEV_F_*` flags (`CHECKED`, `SUPER_NONE`,
  `SUPER_DISAGREE`, `MOUNT_FAILED` with `mount_error` and `mount_stage`,
  `FELL_BACK`, `ROLLBACK_PENDING`/`HELD`/`APPLIED`/`REFUSED` with
  `rollback_error` and `rollback_target`, `COMMITTED`, `BOOT_REFUSED`),
  the format, the generation checked, each slot's generation, and what
  the mount's replays released.
- Results: 0 whenever it answered, including "not checked, and why";
  `-EBUSY` for a device a mount holds; `-ENOMEM`.

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
| `cosmofs_test_corrupt(mnt, kind, ino, &what)` | twenty named corruptions (`enum cosmofs_corruption`); `ino` 0 is accepted only by the whole-filesystem kinds |
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
`fsctl: /dev/fsctl ready (0600): mount listing, check (mounted or not), scrub, snapshots, pools and keys` on every
boot, release included.

### Protocol
- `open`: `-EPERM` without `PRIV_SYS_ADMIN`. Each open file owns its own
  result buffer (`struct fsctl_open`, its own mutex).
- `write`: exactly `sizeof(struct cosmo_fsctl)` bytes (88) -- or, for
  the `KEY_*` operations and only for them, `sizeof(struct
  cosmo_fsctl_key)` (224) -- else `-EINVAL`. `PRIV_SYS_ADMIN` is checked again (`-EPERM`). `version` must
  equal `COSMO_FSCTL_VERSION` (3), else `-EINVAL`. `flags` may contain
  only `COSMO_FSCTL_F_REPAIR` for `CHECK` and `SCRUB_EX`, `F_REPAIR` and
  `F_BOOT` (only with `F_REPAIR`) for `CHECK_DEV`, `COSMO_FSCTL_F_NO_SIZES` for
  `SNAP_LIST` and `COSMO_FSCTL_F_SAFETY_NAME` for `SNAP_ROLLBACK`; any
  other bit is `-EINVAL`. `name` and `name2` must each hold a NUL within
  their 32 bytes. An unknown `op` is `-EINVAL`. A command that fails
  leaves no result. Returns the length written on success.
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
| `COSMO_FSCTL_CHECK` (2) | one `struct cosmo_fsctl_check`: `nclasses` (16), `COSMO_FSCTL_R_PARTIAL`/`_CLEAN`/`_REPAIR_REFUSED`, the walk totals, and `class[16]` in the fixed index order of `fsctl.h` | `-EINVAL` for id 0; `-ENOENT` for an id this namespace does not hold; `-EBUSY` for a mount being unmounted; `-EOPNOTSUPP` for a filesystem without `check`; any error of `cosmofs_check` |
| `COSMO_FSCTL_SCRUB` (3) | one `struct cosmo_fsctl_scrub` (`blocks_read`, `inodes`, `repaired`, `unrecoverable`) | as CHECK, and any error of `cosmofs_scrub` |
| `COSMO_FSCTL_SNAP_CREATE` (4) | one `struct cosmo_fsctl_snap`: the snapshot `name` taken (id, uuid, generation); raises a format-11 pool to 12 | as CHECK; `-EOPNOTSUPP` below format 11 or for a filesystem without `snapctl`; `-EEXIST`; `-EINVAL`/`-ENAMETOOLONG` for a bad name; `-EROFS` (a snapshot's uuid does not wait for a seeded entropy pool: `cfs_snapshot_new_uuid` makes it unique by checking the list) |
| `COSMO_FSCTL_SNAP_LIST` (5) | one `struct cosmo_fsctl_snap_pool` (the format; a pending or held rollback; `free_bytes`, what data can still use, and `total_bytes`), then `count` `struct cosmo_fsctl_snap` oldest first -- at most `COSMO_FSCTL_SNAP_PAGE` (1024), only those with an id above the command's `cursor`, so a full page is followed by one more from its last id -- with `referenced` and `unique` bytes (0 with `F_NO_SIZES`) and flags `CLONE`, `TARGET` and `SAFETY` (a pending rollback's), `HELD` (a held rollback's safety snapshot) and `LIVE` (a held rollback's target: the live tree was rolled back to it) | as CHECK; `-EIO` when a tree does not walk |
| `COSMO_FSCTL_SNAP_DELETE` (6) | none (`count` 0) | as CREATE; `-ENOENT`; `-EBUSY` while anything inside it is in use |
| `COSMO_FSCTL_SNAP_ROLLBACK` (7) | one `struct cosmo_fsctl_snap`: the safety snapshot taken (`pre-rollback-<seconds>`, or `name2` with `F_SAFETY_NAME`), flag `SAFETY`; a pending record names it and `name` (`design.md`, "Rollback") | as CREATE; `-ENOENT` for no such target; `-EINVAL` for a clone; `-EBUSY` while another is pending, or while a hold is in place unless `name` is the held snapshot |
| `COSMO_FSCTL_SNAP_CANCEL` (8) | none | `-ENOENT` when nothing is pending |
| `COSMO_FSCTL_BOOT_OK` (10) | none; `total` is 1 when a hold was released, 0 when there was none | `-EROFS` on a read-only mount |
| `COSMO_FSCTL_CHECK_DEV` (11) | one `struct cosmo_fsctl_devcheck`: the device record (`struct cosmo_fsctl_dev`) and, when its `F_CHECKED` is set, the check record. `name` is the block device, `name2` the filesystem (`""` = cosmofs); flags `F_REPAIR`, and `F_BOOT` with it (roadmap 1.2 B2) | `-ENOENT` for no such device; `-EBUSY` when it is mounted; `-EOPNOTSUPP` for a filesystem without `check_dev`; `-ENODEV` for no such filesystem; `-EINVAL` for `F_BOOT` without `F_REPAIR` |
| `COSMO_FSCTL_SCRUB_EX` (12) | one `struct cosmo_fsctl_scrub_ex`: `blocks_read`, `inodes`, `bad_copies`, `repaired`, `unrecoverable`, `skipped`, `elapsed_ns`, `rate`; repairs only with `F_REPAIR` | as SCRUB |
| `COSMO_FSCTL_KEY_INFO` (21) | one `struct cosmo_fsctl_keyinfo` (`cosmofs_keyctl`, INFO); the pool named by the device in `name` (`name2` the filesystem, `""` = cosmofs) or, when `name` is empty, by `mount_id` | `-ENOENT` no such device; `-EINVAL` not a cosmofs; `-EXDEV`; as CHECK for a mount |
| `COSMO_FSCTL_KEY_LOAD` (22) | none: the 32-byte master key in `key` is checked and kept (CF31); the command is wiped once handed over | `-EKEYREJECTED`; `-EIO`; `-EINVAL`; `-ENOSPC` |
| `COSMO_FSCTL_KEY_UNLOAD` (23) | none | `-EBUSY` while a mount holds it; `-ENOKEY` |
| `COSMO_FSCTL_KEY_SLOT` (24) | none: slot `slot` of the mount `mount_id` becomes `rec` (`COSMO_KEYSLOT_EMPTY` clears it), committed before the answer | `-EKEYREJECTED` when `key` is not its master key; `-EBUSY` for the last slot; `-EROFS`; `-EINVAL` |
| `COSMO_FSCTL_SNAP_CLONE` (9) | one `struct cosmo_fsctl_snap`: clone `name2` of snapshot `name`, flag `CLONE`, `origin_id` (`design.md`, "Clones") | as CREATE; `-ENOENT` for no such snapshot; `-EINVAL` to clone a clone or for no `name2`; `-EOPNOTSUPP` below format 12 |

A SCRUB that met blocks no copy could satisfy succeeds and its record
says how many (`unrecoverable`); before roadmap 1.2 B2 the write failed
with `-EIO` and the counts were only in the kernel log (CFS-11).

### `cosmo-snap(8)`
`cosmo-snap [-m MOUNTPOINT] create NAME | list | delete NAME | diff A B |
rollback [--no-reboot] NAME | rollback --cancel | clone NAME CLONE`
(`userland/system/cosmo-snap.c`): the SNAP operations against the mount
at MOUNTPOINT (`/` by default), found in `fsctl`'s listing. `list` prints
the pool's format and one row per snapshot (name, id, creation time in
UTC, referenced and unique KiB, uuid, flags); `diff` walks the two trees
under `.snapshots` (`-` is the live tree) and prints `+`, `-` or `M` and a
path per difference, then a count. `rollback` prints the safety
snapshot's name and, for `/`, reboots through init (`kill(1, SIGTERM)`)
unless `--no-reboot`; for another mount it says the rollback applies at
that mount's next mount. `clone` prints where the clone is
(`MOUNTPOINT/.snapshots/CLONE`); `delete CLONE` deletes one. Exit status
0 done, 1 refused or failed, 2 usage. Init sends `BOOT_OK` to every snapshot-capable writable mount
after `/etc/rc` exits 0 (`userland/init/init.c`, `boot_complete`).

### `fsctl(8)`
`fsctl list`, `fsctl check <id> [--repair]`, `fsctl scrub <id>
[--repair]`. Ids are decimal digits only and non-zero. A check prints its
summary and then fsreport's `class` and `finding` lines (below); a scrub
one line, `mount N: B blocks read, I inodes, X bad copies, R repaired, U
unrecoverable, S skipped (T ms, bound RATE blocks/s)`, and `clean` when
there was nothing to say. `scrub` without `--repair` writes nothing
(`SCRUB_EX`); against a kernel without `SCRUB_EX` (`-EINVAL`), `--repair`
falls back to the old `SCRUB` and a report-only scrub is refused. Exit
status since roadmap 1.2 B2, fsck's convention: 0 nothing found, 1
everything found repaired, 4 findings left (a refused repair among them),
8 the pass could not be run, 2 a usage error.

### `fsck.cosmofs(8)`
`fsck.cosmofs [--check | --repair] DEVICE` and, for init, `fsck.cosmofs
--boot check|repair DEVICE` (`userland/system/fsck.cosmofs.c`, roadmap
1.2 B2): `/dev/fsctl` `CHECK_DEV` on DEVICE (`NAME` or `/dev/NAME`), which
must not be mounted. `--check` (the default) writes nothing; `--repair`
repairs what has one right answer, `free-in-use` included (offline only),
and as any writable mount applies a pending rollback -- committed only
when it repaired something. Output, stable and
parsed by `test-fsck` (`userland/system/fsreport.h`):

```text
fsck.cosmofs: vda2: format 12, generation 61, 3311 blocks seen, 44716 free, 412 inodes, 61 directories, 1 snapshots (2810 us)
class leaked count 1 repaired 1 unattended
finding leaked block 0:48210 repaired
fsck.cosmofs: vda2: 1 finding(s), 1 repaired, all repaired
```

`class NAME count N repaired M POLICY` per class with findings and
`finding NAME KIND ID STATE` per object the kernel names (at most eight a
class, then `+N more`). The classes are the check's sixteen under their
fsreport names -- `leaked`, `free-in-use`, `cross-linked`, `nlink`,
`orphan`, `dangling`, `bad-entry`, `counter`, `cycle`, `unreadable`,
`extent-order`, `extent-overlap`, `dup-name`, `snap-identity`,
`rollback-record`, `hold` -- and the device's (`superblock`, `root`,
`free-record`, `orphan-record`, `snapshots`, `members`, `bitmap`,
`mount`). KIND is `block` (printed `member:block`), `inode`, `snapshot`,
`count` or `object`; STATE `repaired`, `left` or `partly`; POLICY the
boot's (design.md, "fsck at boot"). `--boot` prints, after those, the one
line a boot gets (`cosmofs: root checked, N findings, M repaired`, or
`cosmofs: root refused: ...`) and the inode of each logged repair. Exit
status: 0 nothing found, 1 all repaired, 4 findings left (`--boot`: the
pool is refused) -- a device class (the superblocks, a mount that stops)
and `incomplete` (a partial walk with nothing else found) are findings --
8 the check could not be asked for (`-EBUSY` for a mounted device or a
mirror one of whose other devices is mounted, `-EINVAL` from a kernel
without `CHECK_DEV`), 2 usage.

### `mkfs.cosmofs --mirror` and `fs.scrub_rate`
`mkfs.cosmofs --mirror A B` makes one pool kept as two copies
(`/dev/blkctl` `COSMO_BLKCTL_FORMAT_MIRROR`: the second device's name in
the `COSMO_BLKCTL_NAME` bytes after the command; both unmounted, neither
a disk with partitions). `sysctl fs.scrub_rate` reads and, with
`PRIV_SYS_ADMIN`, sets the scrub's bound in blocks a second (decimal, at
most 2^24; 0 for none); a scrub reads it when it starts.

### ABI stability
`COSMO_FSCTL_VERSION` 1 carried ten check classes; version 2 appended
`extent_order`, `extent_overlap` and `dir_dup_name` at indices 10-12 and
moved nothing; version 3 appended `snap_identity`, `rollback_record` and
`hold` at 13-15, and grew the command by two name fields. Roadmap 1.2 B2
added two operations, `CHECK_DEV` and `SCRUB_EX`, each with a record of
its own, and the flag `F_BOOT`, without a version bump: nothing an
existing client sends or reads changed size, and a kernel without them
answers `-EINVAL`, which is how a client tells it. The SNAP_LIST pool
record's last word, reserved and written 0 before, is `rollback_error`
(CFS-20). The check record is fixed-size and written whole, so a
size change is a version bump: a version-1 client is refused at the
version gate. `cosmo_fsctl_result.bytes` gives the record size so a
reader can skip records it does not know. `struct cosmofs_check_report`
is not the ABI; `fsctl_run` copies it field by field.

## Other ways in

- `mount(2)` (`cosmo_mount(dev, path, "cosmofs", flags)`) and the root
  switch (`root=` on the command line, `docs/kernel-services/vfs/`) reach
  `cosmofs_mount` through the VFS.
- `/dev/blkctl` `COSMO_BLKCTL_FORMAT` with `fstype` `"cosmofs"` reaches
  `cosmofs_format_label` through `vfs_format`; `cosmo-install`
  (`userland/system/cosmo-install.c`) uses it to make partition 2, named
  `cosmo-root`, and `mkfs.cosmofs` (shell completeness S6) any device.
- The label is read by no call: `blkid` reads it from the raw superblock
  (`cfs_super.label`, offset 136).
- Snapshots: `mkdir` and `rmdir` under `<mount>/.snapshots`, reading
  under `<mount>/.snapshots/<name>/`.
- `/dev/blkctl` `COSMO_BLKCTL_FORMAT_KEYED` (roadmap 1.2 B5) reaches
  `cosmofs_format_keys` through `vfs_format_keyed`; `/dev/fsctl`'s
  `KEY_*` operations reach `cosmofs_keyctl`. `CHECK_DEV` on a pool that
  is locked answers `COSMO_FSCTL_DEV_F_LOCKED` and checks nothing.
- There is no system call for compression policy, rekeying a version-7
  pool or member management.

## On-disk format and its stability

`cosmofs_format.h` is the normative layout; `design.md` explains it by
version. All integers are little-endian, blocks are 4096 bytes, and the
superblock magic is `"COSMOFS1"`.

- **Versions.** This kernel writes `CFS_FORMAT_VERSION` 11 and mounts
  `CFS_VERSION_MIN` 2 through `CFS_VERSION` 14. A version-1 image, or
  one newer than 14, is refused at mount with a message naming the
  version. Mounting an older image never rewrites its superblock
  version; an operation that needs a newer one raises it (12 for a
  snapshot, 13 for a degraded mirror's copy state, 14 for a two-level
  checksum index), and an encrypted pool is made at 14.
- **What each version added**: 2 extents with `lblk`, extent chains,
  per-inode checksum trees; 3 snapshots (`snap_root`); 4 DVAs and the
  member table (`members`, `uuid`); 5 mirror copies (`cfs_member.copies`,
  label generation); 6 compressed records (bit 31 of
  `cfs_extent.count`); 7 encryption (`key_root`, `CFS_CSUM_POLY1305`);
  8 symbolic links (`CFS_TYPE_LNK`); 9 the free record (`free_root`,
  `CFS_KIND_FREELOG`); 10 the orphan record (`orphan_root`,
  `CFS_KIND_ORPHAN`); 11 `cfs_mhdr.csum_algo`; 12 snapshot identity,
  rollback and clones; 13 copy state; 14 the metadata MAC on a pool with
  a key block (`cfs_super_is_mac`), the version-2 key block of eight
  slots, and `CFS_KIND_CSUMIDX2`.
- **Gates.** Fields added by a version are read only when the
  superblock's version is at least that version (`free_root` from 9,
  `orphan_root` from 10, `key_root` from 7, the member table from 4) and
  written only then. Symlink creation is refused below 8. Compression is gated at 6: a new
  regular file gets `CFS_COMPRESS_LZ4` only from version 6, and
  `record_write` forms no record below it (CF22; before roadmap 1.2 B2 a
  version-2..5 image written here could hold records its version does
  not define, CFS-2). The snapshot list is likewise written into a
  version-2 image on its first snapshot, by design (`design.md`,
  "Format version 3").
- **Limits.** `CFS_NAME_MAX` 47 (`CFS_NAME_MAX_LONG` 255 from version 15); `CFS_SNAP_NAME_MAX` 31; `CFS_MAX_INODES`
  508 x 508 x 15; `CFS_MAX_BLOCKS` 508 x 32512 per member;
  `CFS_MAX_MEMBERS` 255; `CFS_MAX_COPIES` 4; `CFS_MAX_EXTENTS` 4096 runs
  loaded per inode (`-EFBIG` above; an implementation bound). A file's
  logical blocks are bounded by the 32-bit `lblk` (`set_extent`:
  `-EFBIG`). The checksum index is one level up to 516128 blocks (about
  1.97 GiB) on a CRC32C file and 64516 blocks (252 MiB) on an encrypted
  one; past that `csum_block_at` makes it two levels (version 14,
  CFS-10), to 508 times those (1000 GiB and 125 GiB). A pool below 11, or
  with a version-1 key block, keeps the one-level limit (`-EFBIG`).
- **Host-testable.** `cosmofs_format.h` has no kernel dependency and is
  compiled by `tests/host/test_cosmofs.c`, which fixes the structure
  sizes and field offsets the format depends on.
