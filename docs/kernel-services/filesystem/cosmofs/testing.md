# cosmofs: testing

Every test that exercises cosmofs, what it asserts, and how to run it.
Several of the older tests are described step by step in
`docs/kernel-services/vfs/testing.md`; this file lists all of them in
one place and points there rather than repeating it. The rules each
test holds are in `invariants.md`.

## What runs where

| Level | What | Command |
|---|---|---|
| Host (ASan/UBSan) | `test_cosmofs`: the on-disk layout compiled natively | `make host-test` |
| Host fuzz (ASan/UBSan) | `fuzz_cosmofs`: the real `cosmofs_core.c`, `cosmofs.c`, `cosmofs_snap.c`, `cosmofs_member.c`, `cosmofs_scrub.c`, `cosmofs_check.c` and `cosmofs_crypt.c` mount, walk and read mutated images | `make fuzz` |
| Target self-tests (debug builds) | 71 tests: 67 `cosmofs-*`, `pool`, and three `fsctl-*`, in `cosmofstest.c`, `cosmofscrash.c` and `vfstest.c`, listed below | `make test` |
| Other target self-tests on cosmofs | tests of other subsystems that use cosmofs as the filesystem under them | `make test` |
| User mode (debug builds) | `init --selftest` mounts the kernel tests' cosmofs, commits a compressible file, and drives `/sbin/fsctl` | `make test` |
| Shell (debug builds) | `/etc/rc.test` takes, reads and deletes a snapshot (`SNAPTEST`) | `make test` |
| Installed system | `cosmo-install` formats a cosmofs root and the machine boots from it | `make test-install` |

Release builds compile no self-test (`SELFTEST=0`; the cosmofs tests have
release stubs that return true so the tables link), so a release
`make test` runs no cosmofs test; the host, fuzz and install rows do not
depend on the build's self-tests.

## Host test (`tests/host/test_cosmofs.c`)

Compiles only `cosmofs_format.h`; 83 `EXPECT`s in four cases.

| Case | What it checks |
|---|---|
| `cosmofs-layout` | `cfs_mhdr` is 32 bytes with `csum_algo` right after `crc`; `cfs_inode` 256, `cfs_extent` 16, `cfs_dirent` 64; the superblock fits a block and `free_root` and `orphan_root` sit in what were reserved words; snapshot entries per block and the snapshot inode tag (`CFS_SNAP_INO`, `CFS_SNAP_TAG`, `CFS_INO_OF`); DVA packing (`CFS_DVA(0, n) == n`, `CFS_DVA_NONE` is vdev 255); `cfs_member` 64 bytes with `copies` at offset 52; `cfs_csum_aead` 32 bytes and its index arithmetic |
| `cosmofs-inode-index` | inode block, slot and the two imap indices |
| `cosmofs-extents` | `cfs_map_block` over sorted runs and holes, and `-1` for a compressed record |
| `cosmofs-csum-index` | checksum index and slot; `CFS_CSUM_MAX_BLOCKS == 516128` |

## Fuzz target (`tests/fuzz/fuzz_cosmofs.c`)

The input is a 64-block (`CFS_MIN_BLOCKS`) image over the host pool shim
(`tests/fuzz/shim_fs.c`). Mount; on success the root must be a
directory, a walk of up to 256 entries six levels deep reads the first
four pages of every regular file, `cosmofs_stats` must report no more
than 64 blocks, and after unmount no vnode may remain. A refused mount
must answer `-EIO`, `-EINVAL` or `-ENOMEM` and leave `fs_priv` and the
root NULL. The seed is `cosmofs_format` of the empty image. Any ASan or
UBSan report fails the run; a crashing input is saved under
`out/<arch>-<build>/fuzz/` (`docs/verification/testing.md`).

## Target self-tests

All are registered in `kernel/core/selftest.c` and run in debug builds.
`cosmofs-fixture-cleanup` also needs `CONFIG_FAULTINJECT` (on by default
in debug builds). The first four use the scratch disk `vda` (`make test`
creates a fresh one each run) and skip with a log line when there is
none; every other test formats its own RAM device (`ramblk_create`),
mounted under `/mnt/eng` or a path of its own, usually with the
writeback thread off so that generation counts are exact.

### Format, operations and the transaction engine

| Test | Asserts |
|---|---|
| `pool` | `pool_open` on `vda`; write, flush, read back the last block; `-EINVAL` past the end |
| `cosmofs-format` | an unformatted disk is `-EIO`; after `cosmofs_format` the mount is generation 1 with one inode; an untouched remount stays at generation 1 |
| `cosmofs-ops` | create, mkdir, a 45-block file rewritten in the middle, rename (replacing, to the parent, onto a removed directory's name), rmdir, unlink, `..`; each sync advances the generation by one; everything survives a remount; deleting the big file returns its blocks; `-ENAMETOOLONG`, `-ENOTEMPTY`, `-EXDEV`. Leaves `hello.txt` and `dir/nested.txt` for user mode |
| `cosmofs-crash` | a discarded transaction leaves generation, free count and contents unchanged; a corrupted newer superblock slot falls back to the older one and the next commit rewrites it |
| `cosmofs-fixture-cleanup` | the test fixture `engine_mount` releases its RAM device when format, directory creation or mount fails (fault injection at each stage) |
| `cosmofs-holes` | a write 200 MiB into a file on a 4 MiB device costs only its own blocks; holes read zeros; truncation frees the checksum tree |
| `cosmofs-csum` | a flipped byte in a data block reads `-EIO` and counts a checksum failure; a rewrite repairs it; a flipped directory block makes the lookup `-EIO` |
| `cosmofs-metadata-csum-id` | `cfs_mhdr_fault_of` on one sealed block: `MHDR_OK` with `csum_algo` CRC32C, a zero algorithm still `MHDR_OK`, an unknown algorithm with a valid CRC `MHDR_ALGO`, a flipped byte `MHDR_CRC`, the algorithm word covered by the CRC |
| `cosmofs-fsync` | a `file_sync`ed file survives a discarded transaction, an unsynced one does not |
| `cosmofs-reserve` | a 256-block device filled to `-ENOSPC` keeps its 32-block reserve; unlink and commit still work |
| `cosmofs-fallback` | a newer root whose imap root is corrupt is passed over for the older one, with a warning; a commit repairs the pair |
| `cosmofs-writeback` | with a 50 ms interval the thread commits on its own (generation and `wb_commits` advance together) and commits nothing while nothing is dirty |
| `cosmofs-mount-no-early-writeback` | the mount's replay makes dirty marks (must be non-zero) and none finds a writeback thread running (must be zero) |
| `cosmofs-badmap` | an inode whose two direct runs were swapped and re-sealed reads `-EIO`, not a hole |
| `cosmofs-symlink` | a link and its target survive a remount with a cold cache; one block per link, returned when it goes |
| `cosmofs-symlink-version` | a version-7 filesystem mounts and works, and `symlink` is `-EOPNOTSUPP` |
| `cosmofs-ro-replay` | CF21 (CFS-1): a pool left with a free record and an orphan record pending (a held file unlinked, the eviction dropped by a forced unmount) is mounted `MOUNT_RDONLY`: the check is clean (the replay ran in memory), a repairing check is `-EROFS`, and after the unmount every block's CRC32C is what it was; a writable mount then writes the replay and leaves no orphan record |
| `cosmofs-version-compress` | CF22 (CFS-2): one compressible record's worth of data on a version-5 pool maps a plain block 0; the same file at the current format is a record (`cosmofs_test_block_of` finds no plain run, the file reads back) |
| `cosmofs-nomem` | CFS-6: `FI_COSMOFS_CHAIN_NOMEM` fails the chain block a file's eleventh run needs and the write-back is `-ENOMEM`, the retry commits; `FI_COSMOFS_INODE_NOMEM` fails a chown's map path and the chown is `-ENOMEM`, the retry and a sync succeed; a truncate whose chain copy fails is `-ENOMEM` and, after a commit, the check is clean (the dropped run's block was not freed under the old map); the check is clean and the eleven pages and the owner read back after a remount |
| `cosmofs-root-type` | a root inode whose mode says regular file (`COSMOFS_CORRUPT_ROOT_TYPE`, in both slots' trees) is refused by the mount with `-ENOTDIR` rather than met by the VFS's assertion |
| `cosmofs-scrub-counts` | CFS-11: a plain data block rotted on a single-device pool: `cosmofs_scrub` returns 0 with one unrecoverable block, and `/dev/fsctl`'s SCRUB succeeds with a record saying so |
| `cosmofs-check-dev` | CF23: `cosmofs_check_dev` on a pool with a leak and a free block in use: `-EBUSY` while mounted; read-only it names both and every block's CRC is unchanged; a boot repair refuses (`boot_refused`) and repairs nothing; the offline repair repairs both, commits, and the next check is clean; a root not a directory is `MOUNT_FAILED` at `STAGE_ROOT` with `-ENOTDIR`; a second slot naming another pool is `SUPER_DISAGREE`, not checked |
| `pool-copy-states` | CF26 at the pool: on a two-way mirror of RAM devices, a copy answering `-EIO` is written around and counted, `FAULTED` on its fourth error with `fs.fault_errors` 3 -- before that `ONLINE` with the three blocks it failed as holes, not read from -- and its record is exactly the four blocks written since the last root; it is then neither read nor written; the last `ONLINE` copy is refused `OFFLINE` and, failing, is not faulted (writes, reads and the flush `-EIO`, and no hole set); a copy answering `-ENODEV` is `MISSING` at once with the block written before it in its record; a copy taken `OFFLINE` with nothing written comes back `ONLINE`, one that missed a write does not (`-EAGAIN`) |
| `cosmofs-degraded` | CF26: a mirror with copy 1 pulled (`-ENODEV`) writes and syncs, reports one device and one copy out, is raised to format 13, and `/proc/mounts` says `rw,degraded`; with both pulled, a read is `-EIO` and the commit fails; a copy failing with `-EIO` at `fs.fault_errors` 4 is `FAULTED`, and after a remount with its device answering again it stays out |
| `cosmofs-copy-bitmap` | CF25: a mirror at one block a region, copy 1 pulled after a sync; every block copy 0 is written after it, through the unmount, is in copy 1's record, and nothing else but the superblock slots; the remount reads the record back `MISSING` since the generation the pulled device still carries; `tools/b3-copystate-probe.py --old` fails it |
| `cosmofs-copy-geometry` | a mirror of a 512-block and an 8192-block device: the member is 512 blocks, a region of one; with the small device pulled and the pool written, it mounts through the big one at the table's size and reads back |
| `cosmofs-copy-import` | CF27: the mirror mounted through the copy that was away mounts the newest root from the other device and leaves the lagging one out; with the newer device mounted elsewhere, it is refused `-EBUSY` |
| `cosmofs-scrub-modes` | CF23: on a two-way mirror with one copy of a file's four blocks rotted, a scrub without repair counts 4 bad copies and a second counts them again; a repairing scrub rewrites 4; the next finds none and the file reads back; the scrub reads a snapshot's own blocks; at `g_cosmofs_scrub_rate` 50 (five blocks a window, so the pool takes several) its time is at least its block count allows |
| `cosmofs-scrub-open` | a scrub while a transaction holds a rewritten file's checksum tree in memory (in blocks the young pool never wrote) finds nothing unrecoverable; after the commit neither does the next |

### The free record (format version 9)

| Test | Asserts |
|---|---|
| `cosmofs-unmount-leak` | write, delete, unmount: the deleted file's blocks are free on the next mount (fails on the tree before version 9) |
| `cosmofs-freelog-format` | a version-8 image works and checks clean; with `0x5a5a5a5a` poisoned into the word `free_root` occupies, it still mounts and reports `free_root == 0`; a fresh version-11 filesystem has no record |
| `cosmofs-freelog-accounted` | the record's own blocks are allocated in the bitmap the root publishes (structural check clean) |
| `cosmofs-freelog-supersede` | each commit releases the previous record; repeated commits do not leak |
| `cosmofs-freelog-not-held` | a superseded record block is freed even when a snapshot's bitmap marks it allocated |
| `cosmofs-freelog-snapshot` | a record never names a block a snapshot holds; checked across a remount |
| `cosmofs-freelog-idempotent` | a mount that replays and leaves without committing leaves the image as it found it; a second mount replays the same record |
| `cosmofs-freelog-reuse` | a replayed block is allocatable at once, and the commit that reuses it supersedes the record naming it |
| `cosmofs-freelog-chain` | more frees than one record block holds (506) make a chain, written and replayed whole; over-reserved blocks are listed as free |
| `cosmofs-freelog-rollback` | a fill that fails (`cosmofs_test_fail_freelog`) gives back the reserved blocks and publishes no root |
| `cosmofs-freelog-malformed` | a record whose count exceeds a block refuses that root rather than being read as empty |

### The orphan record (format version 10)

| Test | Asserts |
|---|---|
| `cosmofs-orphan-crash` | open, unlink, commit, forced unmount with the handle open: the remount reclaims the inode and its blocks without an operator |
| `cosmofs-orphan-cancels` | an ordinary unlink writes no record; a held one does |
| `cosmofs-orphan-dir` | an empty directory removed while it is a working directory; the parent's link count is right at every step |
| `cosmofs-orphan-rename` | a file replaced by `rename` while open is recorded and reclaimed |
| `cosmofs-orphan-idempotent` | two mounts replay one record and agree on the inode count |
| `cosmofs-orphan-reserved` | the record's blocks are in the published bitmap; a record longer than one block chains |
| `cosmofs-orphan-supersede` | a hundred commits with a handle held leave one chain and no residue |
| `cosmofs-orphan-rollback` | a fill that fails gives back its reservation and publishes no root |
| `cosmofs-orphan-suspect` | a record naming a linked inode (`cosmofs_test_orphan_add`) is refused and the file survives |

### Snapshots (format version 3)

| Test | Asserts |
|---|---|
| `cosmofs-snapshot` | history read at depth while the live tree changes; writes inside a snapshot `-EROFS`; the free count rises exactly when a deleted snapshot's blocks are needed by no other; `rmdir` `-EBUSY` while a file inside is open; `..` stays inside history and `.snapshots/..` is the live root |
| `cosmofs-snapshot-remount` | a snapshot survives an unmount |
| `cosmofs-snap-cow` | one commit moves the snapshot list to a new block (copy, not edit in place) |
| `cosmofs-snap-unmount` | a file a snapshot holds, unlinked in the live tree just before an unmount: after the remount the filesystem checks clean, the deadlist has entries and no duplicate, and the snapshot still reads the file |
| `cosmofs-snap-nogrow` | many commits with a snapshot present: the list moves only when it changes, deadlist entries are distinct, no superseded list block is held |
| `cosmofs-snap-reserved` | more frees than one deadlist block holds: every block the list gained is in the published bitmap |
| `cosmofs-snap-rollback` | a deadlist fill that fails partway abandons the transaction and returns the reservation |
| `cosmofs-snap-onewalk` | one snapshot verdict per freed block (walks minus verdicts) |
| `cosmofs-rollback` | B1's rollback on a pool that is not the root (CF19): the request takes a `pre-rollback-` safety snapshot; while pending the pool and both snapshots carry their flags, both are undeletable and a second request is `-EBUSY`; `--cancel` withdraws it; a second request names its safety snapshot `keep-me`, a file written after it is in neither snapshot; at the remount the live tree is the target's, the old tree is `keep-me`'s, the pool is held and `keep-me` undeletable until BOOT_OK (which reports one release, then none); deleting every snapshot after the switch leaves the checker clean and the live files intact after a remount; a record whose own CRC is wrong (`COSMOFS_CORRUPT_ROLLBACK_CRC`) is refused at the mount, named by `rollback_record` and removed by the repair |
| `cosmofs-rollback-atomic` | CF19's same-transaction rule: the machine stopped at each commit the request crosses (`cosmofs_test_commit_budget`, budgets 0-3) and the pool mounted again leaves either neither the record nor the safety snapshot, or both (then switched and held), with no `rollback_record` or `hold` finding; both outcomes seen. `tools/b1-rollback-probe.py --old` commits the record before the snapshot and fails it |
| `cosmofs-rollback-record` | the rollback record on bytes alone (`cfs_rollback_seal`, `cfs_rollback_parse`): a sealed PENDING or HELD record parses back field for field; every single-bit change over its 120 bytes is refused; a wrong magic, version or state, a pending record whose safety uuid is its target's, and one with no safety uuid are refused under a crc that matches them |
| `cosmofs-rollback-shared` | after a rollback to `t`: a file the live tree deleted after `t` is live again, and rewriting and deleting it and writing 8 files of churn leave `t`'s copy intact (the bitmap answer, design.md "Why the newest snapshot still answers"); an inode unlinked and open when `t` was taken is released at the switch (no orphan, no leak); while held, a rollback to another snapshot is `-EBUSY` and one back to the safety snapshot is taken, and cancelling it restores the hold; clean after every snapshot is deleted and a remount |
| `cosmofs-clone` | CF20: a clone of snapshot `o` starts as `o`; a file rewritten, a 16-block incompressible file added, a directory made and a file removed in it are seen neither in `o` nor the live tree, and a live write not in the clone; `o` cannot be deleted (`-EBUSY`), the clone is no rollback target and cannot be cloned (`-EINVAL`), a rename out of it is `-EXDEV`, a symlink or rename inside `o` is `-EROFS` and a symlink in the clone works; after a remount its writes are there; deletion is `-EBUSY` while a file in it is open; after it at least 16 blocks are back and the checker is clean, and again after `o` goes and a remount |
| `cosmofs-snap-identity` | format version 12 (CF18): a pool formatted at 11 stays 11 until a snapshot raises it, converting a version-11 entry (`cosmofs_test_snap_no_raise` builds one) that stays readable; every snapshot gets a distinct uuid through `cosmofs_snapctl`; an id is not reused after a delete and a remount; `list`'s sizes: a snapshot holding a 32-block incompressible file the live tree removed has at least that much unique, one taken before the file has less, and the list is oldest first; a zeroed uuid (`COSMOFS_CORRUPT_SNAP_UUID`) is named by `snap_identity` and repaired; a version-10 pool refuses `create` with `-EOPNOTSUPP` and keeps its version |

### Pools, mirrors, compression, encryption (versions 4 to 7)

| Test | Asserts |
|---|---|
| `cosmofs-pool2` | two members: both carry blocks, free counts add up, the second is found by its label on remount, a snapshot spans both |
| `cosmofs-v3` | a version-3 image (`cosmofs_test_format_version`) mounts, is written, remounts and takes a snapshot |
| `cosmofs-badmembers` | a member table claiming too many chunks, a member larger than its device, or a first usable block inside the label is refused at mount; restored, it mounts |
| `cosmofs-mirror` | one rotted copy of a metadata block and of a data block is read from the other copy and repaired; scrub finds rot no read touched; both copies rotted is `-EIO` for that file only; a device one generation old is left out |
| `cosmofs-mirror-stale` | the stale device is the one labelled copy 0: the mount passes over it and comes up degraded with correct reads |
| `cosmofs-compress` | 32 compressible blocks stored in fewer, 32 incompressible stored as they are; a page rewritten inside a record; a partial page; eight incompressible pages over a record, which then goes whole before plain blocks replace it (S7; `tools/cosmofs-record-fallback-probe.py`); truncation into a record and re-extension reading zeros; a clean scrub |
| `cosmofs-extent-fail` | CF16, CF17: one `set_extent` of a write-back fails with `-ENOMEM` and with `-EFBIG` (`FI_COSMOFS_EXTENT_NOMEM`/`FBIG`) over plain blocks (the fifth), over a compressed record (its own mapping), on an encrypted pool, and inside a record the plain fallback dropped (the third block); every block the open inode maps verifies (`cosmofs_test_verify_file`), the retry writes the pages left dirty, a remount reads the file back with no checksum failure and a clean scrub; the dropped record abandons the transaction and reads back whole (`tools/cosmofs-extent-fail-probe.py`); `evict`: the plain rewrite released with every `set_extent` failing, so its pages are dropped, then a commit: `cosmofs_check` finds no leaked and no free-in-use block and the first four pages read back new after a remount (`tools/cosmofs-partial-write-probe.py`) |
| `cosmofs-crypt` | plaintext absent from the disk; wrong key `-EKEYREJECTED`; a bent block refused; a keyless scrub reads and repairs; rotation (`cosmofs_rekey`) keeps files readable; a keyless remount refuses lookups (`-ENOKEY`); eight writes of one block give eight ciphertexts; a block moved to another offset is refused. On an unseeded boot it instead asserts that the encrypted format is `-EAGAIN` and the device untouched |

### The structural check and `/dev/fsctl`

| Test | Asserts |
|---|---|
| `cosmofs-check-clean` | a populated filesystem checks clean and the totals are plausible |
| `cosmofs-check-leak` | one leaked block is found by number and given back by repair |
| `cosmofs-check-faults` | seven manufactured faults (`seen_not_alloc`, `dup`, `nlink_wrong`, `orphan`, `dangling_entry`, `dir_bad`, `counter_wrong`), each found by the block or inode the corruption touched; `nlink_wrong`, `orphan` and `counter_wrong` repaired; `seen_not_alloc` and `dup` never repaired; with a dangling or malformed entry present, `repair_refused` is set and nothing is repaired |
| `cosmofs-check-extent-order` | two runs swapped so `lblk` descends: `extent_order`, not `extent_overlap` |
| `cosmofs-check-extent-overlap` | two runs over one `lblk` at different blocks |
| `cosmofs-check-bad-ptr` | a block pointer past the end of the pool is `dir_bad` |
| `cosmofs-check-namelen` | a `namelen` longer than its slot is refused by length before the name is read |
| `cosmofs-check-two-parents` | a directory named from two directories is reported |
| `cosmofs-check-chain-cycle` | an extent chain whose last block names an earlier one is `chain_cycle` |
| `cosmofs-check-snap-members` | a snapshot member count larger than its block is reported, and what the block holds is still walked (no false leaks) |
| `cosmofs-check-dup-name` | one name twice in a directory; two different names whose hashes collide (asserted with `cosmofs_test_name_hash`) are not reported |
| `cosmofs-check-snapshot` | blocks a snapshot shares with the live tree are neither leaks nor cross-links |
| `cosmofs-check-orphan-crash` | an orphan the record did not name is found and repaired by the check |
| `cosmofs-check-partial` | an unreadable directory block: report partial, repair refused, blocks named only inside it not freed |
| `cosmofs-check-many-orphans` | twelve orphans, more than the eight names a class carries, all repaired in one pass |
| `cosmofs-check-slot-identity` | a slot whose `ino` disagrees with its position is named, partial, repair refused |
| `fsctl-list` (`vfstest.c`) | every mount of the namespace once, the root at `/`, `count == total` |
| `fsctl-check` | a leak found and repaired through `/dev/fsctl` with the same numbers as a direct call; `-EOPNOTSUPP`, `-ENOENT`, `-EINVAL` for a wrong version or size |
| `fsctl-result-per-open` (`vfstest.c`) | two open files keep their own results; a file with no command reads zero bytes; a short buffer is refused |

### Crash consistency: `cosmofs-replay` (`cosmofscrash.c`)

The harness `docs/verification/design.md` section 4 describes, with
what it has gained since. A 512-block RAM device is formatted and its
image saved; the workload records every write and flush
(`ramblk_record_start`) across its sync points: creates, rewrites that
grow and shrink, renames, unlinks, a rename that replaces, two snapshots
taken and one deleted, and a file unlinked while a handle is held. For
every sampled prefix of the log (all of them when the log has at most
256 entries, otherwise every entry around a sync point plus a stride),
intact and with the last write torn, the image is restored, the prefix
replayed (`ramblk_replay`) and the result mounted. Each prefix must:

- mount, and show every file committed by the last sync whose commit
  write is in the prefix with its exact contents;
- walk and read every directory and file, and every snapshot it still
  has;
- check **clean** under `cosmofs_check` (the stranded-block total over
  all prefixes must be zero);
- have no block on two deadlists (`cosmofs_test_deadlist_dups`), with
  the number of entries examined required to be non-zero over the run.

Its boot budget is 40 s (`kernel/core/selftest.c`, the `SELFTEST:
budgets` line), not the 8 s default; it logs how many prefix images it
checked.

### Other self-tests that run on cosmofs

| Test | File | Uses cosmofs for |
|---|---|---|
| `fault-blk` | `kernel/core/faulttest.c` | device errors injected under a workload; a forced unmount and remount show a committed root |
| `nvme` | `kernel/device/devtest.c` | formats and mounts the NVMe disk, and leaves the cosmofs `SNAPTEST` uses |
| `cache-limits`, `read-bounce`, `wb-error-fsync`, `wb-error-once`, `wb-error-close`, `wb-error-lost`, `read-bench`, `write-bench`, `pagecache-pinned` | `kernel-services/vfs/vfstest.c` | page cache and write-back behaviour (`docs/kernel-services/vfs/testing.md`) |
| `vfs-mount-id`, `vfs-mount-pin` | `kernel-services/vfs/vfstest.c` | mount ids and the pass hold behind `/dev/fsctl` |

## User mode and shell

- `init --selftest` (run by the `process-user` self-test): mounts
  `vda` at `/mnt`, reads the files `cosmofs-ops` left, writes and commits
  a compressible file (`usertest: cosmofs compressed a file committed
  from user mode`, required by the boot harness in every self-test
  boot), and runs `fsctl_selftest`: `fsctl list` works, `check` is
  refused on a filesystem with no pass and for ids `0` and `999999999`,
  and `check`, `scrub` and `check --repair` succeed on the cosmofs.
  `unpriv_test` (`init --unpriv-test`, as uid 1000) requires an open of
  `/dev/fsctl` to fail with `EACCES`.
- `/etc/rc.test` (`userland/etc/rc.test`): mounts `nvme0n1`, writes a
  file, `mkdir .snapshots/shell`, rewrites the file, reads the old
  contents through the snapshot and the new through the live tree,
  requires a write into the snapshot to fail, lists and `rmdir`s it.
  `tests/boot/run_boot_test.py` requires `SNAPTEST: PASS` and forbids
  `SNAPTEST: skipped`.

## Snapshots on the installed system (`make test-snapshot`)

Roadmap 1.2 B1's acceptance test, `tests/boot/snapshot_test.py`, in CI's
install job on both architectures (release builds). It installs as
`test-install` does and boots the installed disk alone. Stage
`identity`: the fresh root is format 11 (`cosmo-snap list`); `cosmo-snap
create base` raises it to 12 (the kernel's line is required) and prints
the uuid; a second `base` is refused; after the live tree changes,
`cosmo-snap diff base -` names exactly the four changes under the test's
directory; the snapshot still reads the old contents; `list` shows base
with its uuid and non-zero sizes; `fsctl check` of the root is clean and
counts one snapshot; then a delete, a new snapshot, a power-off, and on
the next boot the ids rise across the delete and the remount.

Stage `rollback`: the root is snapshotted as `good`, then broken --
`/bin` removed, `/etc/rc` made to `exit 7`, a file rewritten -- and
`cosmo-snap rollback good` reboots the same QEMU. The next boot's log
must hold the switch's line, `/etc/rc` exiting 0 and init releasing the
hold; `/bin/sh` is back, the file has its old contents and the safety
snapshot the broken one; `fsctl check` is clean; both snapshots are then
deleted, clean again.

Stage `crash`, CF19's three points, each a killed QEMU and a clean
`fsctl check` on the next boot: after the record's commit (`rollback
--no-reboot`, then the kill: the next boot switches); inside the switch
(the installed disk's `kernel.elf` and `boot.tar` replaced by a build with
`COSMOFS_STOP=1`, the set signed again with the build's key, the boot
killed at the kernel's `test stop` line after the switch's blocks and
before its root; then the ordinary set again, which redoes the switch);
and after the hold's release (the kill once the boot completed: the next
boot applies nothing). `make test-snapshot` builds the `COSMOFS_STOP=1`
image in `$(OUT)-stop`, as `test-crash` builds its own.

Stage `clone`: a clone of a snapshot of the installed root is written
(a file changed, a directory made, a directory removed) and the live tree
is changed; each tree reads only its own; `cosmo-snap list` marks the
clone `clone-of:`; the origin's deletion is refused (exit 1); `fsctl
check` is clean after the writes, after the clone's deletion and after
the origin's.

## fsck and scrub from the shell (`make test-fsck`)

Roadmap 1.2 B2's acceptance test, `tests/boot/fsck_test.py`, in CI's
`fsck` job on both architectures (release builds). It installs as
`test-install` does, then a preparation boot gives the root a tree under
`/fsck` (files `a` to `f`, `dir/x`, and `sparse`: eight blocks of gzip
output and one twelve blocks on, at least two plain runs; `extent-overlap`
makes its own two-block run from two new blocks, so the damage does not
depend on how the allocator laid the file out), takes a snapshot `fsckbase` (format 12)
and removes a file after it, so the snapshot has a deadlist. That disk
is the base; the boot's check of it must say `cosmofs: root checked, 0
findings, 0 repaired`.

Stage `classes`: `tools/cosmofs-damage.py` writes each of its damages
into a copy of the base (`--list` names them and what the checker is
expected to report; every structure touched is re-sealed and both
superblock slots name the damaged tree), and the copy is booted. A
damage a boot repairs -- `leaked`, `counter`, `nlink`, `orphan`,
`snap-identity`, `rollback-record`, `hold` -- must give the line
`cosmofs: root checked, N findings, N repaired`, its class with every
finding repaired (`nlink` and `orphan` also the logged inode), the disk
root mounted and `fsctl check` of it clean. Any other must give `cosmofs:
root refused: ...` naming its class, leave init on the live root, and
then from the live shell `fsck.cosmofs --repair` must exit 1 (all
repaired: `free-in-use`) or 4, and `fsck.cosmofs --check` report exactly
the classes left: `cross-linked` (its leaked block repaired), `dangling`
and `orphan`, `bad-entry` and `orphan`, `extent-order`, `extent-overlap`,
`dup-name`, `unreadable` and `orphan` (the unreadable directory's file
has no name the walk reached), `cycle` (a deadlist), `cycle` and
`cross-linked` (an extent chain naming itself: each revisit up to the
walker's bound is a second claim), `snapshots` (the snapshot list naming
itself: the mount refuses it while it loads the snapshots), `bad-entry`
and `nlink` (a directory naming its parent, which gains a link the
refused repair leaves: `cycle-directory`), `free-record` and `orphan-record` (a cycle in each,
which the mount refuses), `root` (not a directory) and `superblock` (two
slots, two pools).

Stage `readonly` (CF21 on the installed system): a blank disk beside the
base is formatted and a file written to it and removed, so its root has a
free record pending (the orphan record's case is `cosmofs-ro-replay`'s);
on the next boot `fsck.cosmofs --check` reports the replay in memory and
a read-only mount reads it, and on the host every block of that disk is
what it was before the boot.

Stage `mirror`: two blank disks beside the base become one pool
(`mkfs.cosmofs --mirror`), a file of 64 random blocks is written and its
sha256 taken; on the host `cosmofs-damage.py --flip-mirror` flips 16 of
its blocks on one device; booted again, `fsctl scrub` reports 16 bad
copies and repairs none (exit 4), `fsctl scrub --repair` repairs 16
(exit 1), a third scrub is clean (exit 0) and the file's sha256 is
unchanged. Then the bound: a 4 MiB writer is timed alone and during a
scrub at `fs.scrub_rate=512` (both reported, neither asserted: QEMU's
timing is the host's), and the scrub's own time must be at least what
its block count allows at that bound.

## Installed system (`make test-install`)

`tests/boot/install_test.py` runs `cosmo-install`, which formats GPT
partition 2 as cosmofs through `/dev/blkctl` and copies the system onto
it; it then mounts that partition with the `cosmofs` type and lists it,
boots the installed disk, and requires `vfs: root switched to cosmofs`.
A `root=` naming a disk with no cosmofs must fail with a clear line and
the live shell (`docs/boot/testing.md`).

## Running

```sh
make host-test                  # test_cosmofs among the host tests
make fuzz                       # fuzz_cosmofs among the targets (FUZZ_RUNS=20000)
make fuzz FUZZ_RUNS=200000      # longer
make test                       # every self-test, 4 CPUs (x86-64)
QEMU_SMP=1 make test            # one CPU
make test-smp2                  # two CPUs, as CI runs
make ARCH=aarch64 test          # AArch64
make BUILD=release test         # release: no self-tests
make test-install               # the installed cosmofs root
```

Use `gmake` on macOS. There is no switch to run one self-test alone;
read the run's own `SELFTEST:` lines for the `cosmofs-*` and `fsctl-*`
entries, and the harness verdict for the `SNAPTEST` and `usertest`
markers. A host binary can be run directly as
`out/<arch>-<build>/host/test_cosmofs`, and the fuzz target as
`out/<arch>-<build>/fuzz/fuzz_cosmofs` (with an input file to replay
one case).

## Probes

- `tools/cosmofs-damage.py IMAGE CLASS [--out OUT]`, `--list`,
  `--describe`, `--flip-mirror COPY0 COPY1 --path P --count N`: the host
  side of `test-fsck` (above); not a probe, a damage of each class.
- `tools/b2-defects-probe.py --case ro|compress|nomem|scrub-counts|root-type|scrub-open [--old] [--arch ...]`:
  `--old` reverse-applies that fix's whole commit in a throwaway
  worktree; the case's self-test then fails at its stated check
  (`root-type`: the kernel stops at the VFS assertion before the test
  reports; `scrub-open`: `cosmofs-compress` fails with it).
- `tools/b1-rollback-probe.py [--old] [--arch ...]`: `--old` commits a
  rollback's record on its own before its safety snapshot, in a
  throwaway worktree; `cosmofs-rollback-atomic` then fails at its
  `rollback_record` check (a stop between the two left a record naming a
  safety snapshot that does not exist).

- `tools/cosmofs-metadata-csum-probe.py` demonstrated, on the tree
  before format version 11, that a metadata header recorded no checksum
  algorithm. It is anchored to that tree and is not a regression test;
  `cosmofs-metadata-csum-id` is.
- `tools/cleanup-path-probe.py --old` reproduced the RAM-device leak in
  the test fixture that `cosmofs-fixture-cleanup` now covers.

## Gaps

- The directory walker's depth bound (64) is not reached by a test: a
  directory cycle is caught first, as a directory with two parents
  (`cycle-directory` reports `bad-entry`). The other walkers have a
  `test-fsck` class each.
- Nothing checks that an older kernel reads what this one writes into
  an older-format pool; CF22 is checked against this kernel's own map
  (`cosmofs-version-compress`).
- No test drives `cosmofs_format_encrypted` concurrently with another
  format (the key passes through an unlocked global).
- Files beyond `CFS_MAX_EXTENTS` runs, and files near the checksum
  index's limit (516128 blocks, or 64516 encrypted), are not exercised.
- Resilvering does not exist yet (roadmap 1.2 B3's next part), so nothing
  tests a stale copy rejoining; a lagging copy is only recognised and
  left out (`cosmofs-copy-bitmap`, `cosmofs-copy-import`).
- No host `mkfs`: `cosmofs_format` runs only in the kernel, so
  `cosmofs-damage.py` cannot make an older-format image to damage; CFS-2's
  proof is `cosmofs-version-compress`. The offline checker is the
  kernel's (`fsck.cosmofs`, `CHECK_DEV`), by design.
