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
| `cosmofs-extent-fail` | CF16, CF17: one `set_extent` of a write-back fails with `-ENOMEM` and with `-EFBIG` (`FI_COSMOFS_EXTENT_NOMEM`/`FBIG`) over plain blocks (the fifth), over a compressed record (its own mapping), on an encrypted pool, and inside a record the plain fallback dropped (the third block); every block the open inode maps verifies (`cosmofs_test_verify_file`), the retry writes the pages left dirty, a remount reads the file back with no checksum failure and a clean scrub; the dropped record abandons the transaction and reads back whole (`tools/cosmofs-extent-fail-probe.py`) |
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
the next boot the ids rise across the delete and the remount. Rollback
and clone stages arrive with B1's later pull requests.

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

- `tools/cosmofs-metadata-csum-probe.py` demonstrated, on the tree
  before format version 11, that a metadata header recorded no checksum
  algorithm. It is anchored to that tree and is not a regression test;
  `cosmofs-metadata-csum-id` is.
- `tools/cleanup-path-probe.py --old` reproduced the RAM-device leak in
  the test fixture that `cosmofs-fixture-cleanup` now covers.

## Gaps

- Four of the five walkers that can report `chain_cycle` (the deadlist,
  imap and directory-depth guards) have no test; only the extent
  chain's does.
- No test mounts an image with a free or orphan record `MOUNT_RDONLY`;
  by reading the code such a mount commits the replay (`api.md`,
  `cosmofs_mount`).
- `cosmofs-v3` writes compressed records into a version-3 image and
  passes, because this kernel reads them; nothing checks that an older
  kernel could (`invariants.md`, CF9).
- `fsctl_selftest` scrubs only a clean filesystem. An unrecoverable
  scrub through `/dev/fsctl` returns `-EIO` with no result record, and
  no test covers that path.
- No test drives `cosmofs_format_encrypted` concurrently with another
  format (the key passes through an unlocked global).
- Files beyond `CFS_MAX_EXTENTS` runs, and files near the checksum
  index's limit (516128 blocks, or 64516 encrypted), are not exercised.
- Resilvering does not exist, so nothing tests a stale copy rejoining.
- No host `mkfs` and no offline checker: `cosmofs_format` and
  `cosmofs_check` run only in the kernel.
