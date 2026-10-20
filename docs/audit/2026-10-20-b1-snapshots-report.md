# Roadmap 1.2 B1: snapshots and rollback from the shell

Date: 2026-10-20. Base: `main` at `45a7d4de` (after #419). The unit is
roadmap 1.2's first milestone (`docs/roadmap-1.2.md`, direction B, chosen
by the owner on 2026-10-20). Its acceptance test is `make
test-snapshot`, as the roadmap writes it: snapshot the root, break the
system, roll back, reboot into the snapshot; a crash at the rollback's
points leaves one consistent tree; a writable clone stays apart from its
snapshot and the live tree, and deleting it frees exactly its blocks.

The design is `docs/kernel-services/filesystem/cosmofs/design.md`,
"Format version 12". This report says what each pull request built and
what it measured.

## Scope and baseline

At `45a7d4de` a cosmofs snapshot (format version 3) is a kept root
tuple read through `.snapshots/<name>` and deleted by `rmdir`. Nothing
reports its size, nothing rolls the live tree back to one, nothing
writes to a copy of one, and a deleted newest snapshot's id is handed out
again after a remount (inventory §3; backlog CFS-3). The superblock has
two unused words (`csum_root`, never read, and `reserved[1]`); the
96-byte snapshot entry is full.

## Decisions the owner took (2026-10-20)

Asked when the format had no room for the rollback record and the
snapshot uuids, and when the deadlist could not make a clone's deletion
exact:

- **Format version 12, raised lazily.** A pool stays at 11 until the
  first snapshot taken on it, which raises it in its own transaction;
  `cosmo-upgrade` refuses to switch to a set whose kernel reads less than
  the root's format. `reserved[1]` becomes `rollback_root`, naming a
  `CFS_KIND_ROLLBACK` block; snapshot entries are 128 bytes in a list kind
  of their own.
- **Clones are walk-based and exact.** A clone's frees are tested against
  its origin's tree, an origin cannot be deleted while cloned, a clone
  cannot be snapshotted, and a clone's deletion walks both trees.

## First pull request (#421): identity, the interface, sizes

As built:

- `cosmofs_format.h`: `CFS_VERSION` 12, `CFS_FORMAT_VERSION` 11; the
  128-byte `struct cfs_snapshot` (`uuid`, `origin_id`, `flags`) and the
  96-byte `struct cfs_snapshot_v11`; `CFS_KIND_SNAPLIST2` (31 entries and
  `max_id`), `CFS_KIND_ROLLBACK` and `struct cfs_rollback` (defined,
  written by the second pull request); `cfs_super.rollback_root`.
- `cosmofs_snap.c`: the list behind four accessors that read either
  layout; `snap_raise` (11 to 12 in the snapshot's transaction, every
  entry given a uuid, the old chain freed exempt); ids from the head's
  `max_id`, which every snapshot raises; `cfs_snapshot_fix_identity`, the
  checker's repair.
- `cosmofs_reach.c` (new): `cfs_tree_walk`, every block one tree reaches.
- `cosmofs.c`: `cosmofs_snapctl` (CREATE, LIST with sizes from a
  per-block count of trees, DELETE refused while in use).
- `cosmofs_check.c`: each snapshot walked with its own `next_ino`; class
  `snap_identity` (CF18) with its repair; classes `rollback_record` and
  `hold` reported as zero until the second pull request.
- `/dev/fsctl` version 3 (`uapi/cosmo/fsctl.h`, `kernel-services/vfs/
  fsctl.c`): the SNAP operations, the SNAP_LIST pool record, sixteen check
  classes; `fs_type.snapctl`.
- `cosmo-snap` (`/sbin`): `create`, `list`, `delete`, `diff`.
- `cosmo-upgrade`: refuses a set whose kernel's `cosmofs-format-max`
  marker (11 when absent) is below the root's format, for an upgrade and
  for `--rollback`.
- Tests: `cosmofs-snap-identity` (self-test); `test-snapshot` stage
  `identity` (`tests/boot/snapshot_test.py`, CI's install job).

Measured (2026-10-20, this branch):

- x86-64 debug `make test`: PASS, 482 self-tests, `cosmofs-snap-identity`
  ok (raised 11 to 12 with a legacy snapshot converted; ids not reused
  after a remount, 4 > 3; the snapshot holding the removed file held
  160 KiB alone).
- x86-64 release `make test-snapshot`: PASS in 32.9 s (the fresh root is
  format 11; `create base` raised it; `diff base -` named the four changes
  under `/snapdemo` and one outside it, `/root/.ash_history`, the shell's
  own; ids 1 < 2 < 3 across a delete and a remount).

### Review and CI of the first pull request (2026-10-20)

CI's `entropy boots` job failed on both architectures: the no-entropy boot
waited 5 s in `cfs_need_seeded` for every snapshot and 16 self-tests failed
(`cosmofs-replay`, `cosmofs-snapshot`, `cosmofs-snap-identity`, ...).
A snapshot's uuid now needs no seeded pool: `cfs_snapshot_new_uuid` mixes
the random bytes with the clock and a counter and draws again while the
list (or the batch) already has them, so CF18's uniqueness is checked
rather than hoped for. The raise, `create` and the identity repair use it.

Review (Qodo, Codex), each fixed:

- `cosmo-snap` refuses a name over 31 bytes (it reached the kernel cut
  short and could delete another snapshot) and a `create` name starting
  with `-`; `diff` fails on a file or directory it cannot read instead of
  reporting a change or an empty directory, and on a path longer than its
  1024-byte buffers instead of walking a truncated one (`fit`); the mount listing is read
  whole (grown on `-ERANGE`, never read past what came back), in
  `cosmo-snap` and `cosmo-upgrade`.
- `cosmo-upgrade`'s format gate fails closed: a root format it cannot
  read refuses the switch. A running kernel whose `/dev/fsctl` takes only
  the 16-byte command of versions 1 and 2 predates format 12 and mounted
  the root, so the root is not 12 and the gate passes: CI's `upgrade
  test` runs this build's `cosmo-upgrade` on an installed v1.0.3, and the
  first fail-closed push refused there (run 39413879230, both
  architectures). `make BUILD=release test-upgrade` x86-64 PASS 156.9 s
  with the fix.
- `SNAP_LIST` pages: `struct cosmo_fsctl` gains `cursor` (88 bytes),
  1024 records a page from the id after it; `cosmo-snap list` follows.
- Sizes count the allocation maps and the deadlist blocks a snapshot
  holds, each block once per root; the tally is allocated in 4096-block
  chunks.
- The checker: identity repair after the counter repair; a repeated id is
  a `snap_identity` finding, repaired with a fresh id; the reach walk
  reads every checksum leaf.
- The no-raise test hook is compiled only under `CONFIG_DEBUG`.

Measured after the fixes: x86-64 debug `make test` PASS 162.1 s; `make
test-entropy` PASS (no-entropy 162.0 s, cpurng 153.5 s, devrng 160.3 s) on
the second run -- the first failed `sched-restore-loop` one trip short,
the known signature, recorded in flakes.md as its fourth sighting;
release `make test-snapshot` PASS 32.5 s; `make analyze` clean; `make
host-test` PASS. AArch64 debug `make test` PASS 157.6 s; release `make
test-snapshot` PASS 43.0 s; `make analyze` clean.

## Second pull request (#422): rollback at the next mount

As built:

- The record (`cosmofs_format.h`): `struct cfs_rollback`, 120 bytes in a
  `CFS_KIND_ROLLBACK` block named by `cfs_super.rollback_root` -- magic
  `CFRB`, version 1, state (`PENDING` 1, `HELD` 2), the target's and the
  safety snapshot's uuids, their ids (for the log), the live root's
  generation, the request time, and a CRC32C over the struct with the crc
  field zero. `cfs_rollback_seal` and `cfs_rollback_parse` are the bytes
  alone; `cfs_rollback_read` is parse over the block.
- `cosmofs_rollback.c` (new). `cfs_rollback_request` draws the safety
  snapshot's uuid first (`cfs_snapshot_new_uuid`; a raise from 11 gives no
  converted entry the same one), puts the safety snapshot
  `pre-rollback-<seconds>` into the open transaction without committing
  it, writes the PENDING record beside it and commits once (CF19). One
  pending rollback at a time (`-EBUSY`); a clone is not a target.
  `cfs_rollback_cancel` removes a pending record (the safety snapshot
  stays); `cfs_rollback_boot_ok` removes a held one.
- The mount (`cfs_rollback_at_mount`, from `load_root`): a record that
  does not parse, or names a target or safety snapshot not on the list,
  is refused and logged and the current tree mounted, nothing cleared; a
  read-only mount leaves it pending. Otherwise, in the mount's first
  transaction: walk the target's tree into a set; free every block the
  old live tree reaches and the target does not (the commit's
  newest-snapshot rule puts each on the safety snapshot's deadlist); take
  the target's blocks off every deadlist (`cfs_snapshot_deadlists_filter`,
  they are live again); drop the old tree's orphan record; make the
  target's roots the live roots; write the record back as HELD; commit.
  A failed switch is abandoned and the mount reloads the current tree
  with the rollback off.
- The hold: while PENDING, the target and the safety snapshot cannot be
  deleted; while HELD, the safety snapshot cannot (`cfs_rollback_protects`,
  `-EBUSY`). `init` sends `COSMO_FSCTL_BOOT_OK` to every writable
  snapshot-capable mount once `/etc/rc` exits 0, which releases it.
- The checker: `rollback_record` (a record that does not parse, or a
  pending one naming a missing snapshot) and `hold` (a held record naming
  a missing safety snapshot); the repair for both removes the record
  (`cfs_rollback_drop`). Scrub reads the record block.
- `cosmo-snap rollback [--no-reboot] NAME`, `rollback --cancel`; a root
  rollback reboots (`kill(1, SIGTERM)` after `sync`).
- Tests: self-tests `cosmofs-rollback` (a pool that is not the root),
  `cosmofs-rollback-atomic` (the machine stopped after each commit the
  request crosses, `cosmofs_test_commit_budget`), and
  `cosmofs-rollback-record` (the parse and checksum on bytes: 960
  single-bit changes over the 120 bytes refused; a wrong magic, version
  or state refused under a matching crc). `test-snapshot` stages
  `rollback` (break `/bin` and `/etc/rc`, roll back, reboot into the
  snapshot, the hold released at boot complete, check clean) and `crash`
  (QEMU killed after the record's commit, inside the switch -- a
  `COSMOFS_STOP=1` kernel stops in the switch's commit with its blocks
  written and its root not -- and after the release; `fsctl check` clean
  after each). Probe `tools/b1-rollback-probe.py --old` commits the
  record alone before the safety snapshot.
- Docs: design.md "Rollback", CF19, testing.md, api.md, the guide's
  [Snapshots and rollback](../guide/snapshots.md), upgrading.md's format
  12 section.

Measured (2026-10-20):

- Probe `--old`: FAIL as designed on both architectures (x86-64 and
  AArch64): stops after commits 0, 1 and 2 of the split request each
  left a record whose safety snapshot did not exist, and
  `cosmofs-rollback-atomic` failed at `rep.rollback_record.count == 0 &&
  rep.hold.count == 0` (logs `out/b1-rollback-probe/<arch>-old`).
- x86-64: debug `make test` PASS 156.0 s (163.2 s with the record test);
  `make test-entropy` PASS (175.0, 161.7, 164.7 s); release `make
  test-snapshot` PASS 87.7 s (identity, rollback, crash); `make analyze`
  clean.
- AArch64: debug `make test` PASS 169.3 s; release `make test-snapshot`
  PASS 152.8 s; `make analyze` clean.

### Review of the second pull request (#422, 2026-10-20)

Qodo and Codex, fourteen comments; each fixed or answered on the PR:

- `init`'s BOOT_OK listing grows on `-ERANGE` and never reads past what
  came back; a failure says the hold stays.
- A rollback while a hold is in place is refused unless it is back to
  the held snapshot; that record carries `CFS_ROLLBACK_F_FROM_HELD` (the
  old `pad` word), and cancelling it restores the hold.
- `cfs_rollback_protects` fails closed on a record that does not read.
- The record is checked for meaning: a safety uuid, and for a pending
  record a target uuid that is not the safety's; known flags only.
- `rb_write` frees the old record block only when it reads as one.
- The switch refuses a target whose root inode is not a directory,
  before changing anything; it puts the target's unlinked inodes on the
  orphan record it commits, and the orphan replay now runs after a switch.
- `snap_walk` is bounded; `cfs_snapshot_lookup_uuid` tells a missing
  snapshot from a list that does not read, and the checker condemns a
  record only for the first.
- Snapshot creation and the rollback request write dirty pages first.
- Answered, not changed: the newest-snapshot verdict stays exact after a
  rollback, because a snapshot is asked with its bitmap
  (design.md, "Why the newest snapshot still answers"); now asserted by
  `cosmofs-rollback-shared`. Recorded: CFS-20, a nearly full pool can
  refuse the switch (abandoned, logged, still pending).

Measured after the fixes: x86-64 debug `make test` PASS 164.4 s
(`cosmofs-rollback-shared` ok: 74 blocks released, 74 deadlist entries
live again, 1 orphan of the target released); AArch64 debug PASS 168.8 s;
release `make test-snapshot` PASS x86-64 83.2 s, AArch64 120.6 s; `make
host-test` PASS; `make analyze` clean on both.
## Third pull request (#423): writable clones

As built:

- `cosmofs_clone.c` (new). A clone is a snapshot-list entry with
  `CFS_SNAP_F_CLONE` and `origin_id`; the open transaction's roots are
  in `struct cfs_clone` (`fs->clones`), published into the entries by
  each commit before it allocates (`cfs_clones_publish`). An operation on
  a clone's vnode enters the clone under the mount's lock, swapping its
  `imap_root`, `next_ino` and `inode_count` into the in-memory
  superblock, and leaves it before the lock goes (`cfs_lock_tree_w`,
  `cfs_unlock_tree` on create, symlink, unlink, rename, writepage(s),
  truncate, evict and setattr); a commit from inside a clone leaves it
  first. Vnodes in a clone carry its id as their tag; inode numbers are
  masked with `CFS_INO_OF` on the inode and directory paths.
- Exact ownership (CF20): a block a clone frees is kept when its
  origin's tree reaches it (the origin's tree, walked once into a bit
  set) and freed exempt otherwise (`cfs_clone_free`, from the deferred
  free path). Deleting a clone walks its tree and frees every block the
  origin's set lacks, in the transaction that removes the entry
  (`cfs_clone_delete`). An origin with a clone cannot be deleted
  (`-EBUSY`); a clone cannot be cloned or rolled back to (`-EINVAL`); a
  clone is never the newest snapshot a live free is offered to, nor a
  keeper of a deleted snapshot's deadlist.
- `.snapshots/CLONE` resolves to the clone and is writable; a rename
  between two trees is `-EXDEV`. Found on the way: `symlink` and `rename`
  inside a read-only snapshot had no `-EROFS` guard (every other write
  path had one); the tree guard every write path now takes covers both,
  and `cosmofs-clone` asserts it.
- `SNAP_CLONE` (`/dev/fsctl` op 9), `cosmo-snap clone NAME CLONE`;
  `list` flags a clone `clone-of:<origin id>` and sizes it from its current roots; the
  checker walks each clone's tree from its current roots
  (`cfs_tree_roots`).
- Tests: self-test `cosmofs-clone`; `test-snapshot` stage `clone` (a
  clone of a root snapshot written apart from the snapshot and the live
  tree, its origin refused deletion while it exists, `fsctl check` clean
  after its writes and after its deletion).
- Docs: design.md "Clones", CF20, testing.md, api.md, the guide's
  Clones section, limitations.md. Backlog: CFS-8 narrowed to boot
  environments (B4); CFS-21 (a file unlinked in a clone while open keeps
  its blocks until the clone goes; clones are not snapshotted).

Measured (2026-10-20):

- The AGENTS matrix on this branch before #422's review fixes, every step
  PASS: x86-64 host-test, fuzz, analyze, debug boots at `QEMU_SMP=1`, 2
  and 4 (152, 152, 154 s), `test-smp2`, `test-chaos`, `test-harness-retry`,
  release test; AArch64 host-test, analyze, debug boots (146.7, 166.8 s,
  and 172 s at 4), `test-smp2`, `test-chaos`, `test-harness-retry`,
  release test.
- Rebased on the review fixes: x86-64 debug `make test` PASS 155.0 s
  (`cosmofs-clone`: 33 blocks back when the clone was deleted, the
  checker clean throughout); AArch64 debug PASS 160.5 s; release `make
  test-snapshot` PASS on x86-64 (95.0 s) and AArch64 (139.2 s), every
  stage -- install, identity, rollback, crash, clone; `make analyze` clean;
  `make host-test` PASS.

### Review of the third pull request (#423, 2026-10-20)

Qodo and Codex (one of them Codex's security review):

- `fsync` on a file in a clone, or in a snapshot, wrote its inode
  through the live tree's inode map: `cfs_vnode_sync` took the plain
  lock, and this pull request's masking of tagged inode numbers turned
  the vnode's number into the live inode's. A reader of a snapshot could
  replace a live inode, the root directory's included. Fixed: every
  write path takes `cfs_lock_tree_w`, which enters the vnode's clone or
  refuses (`-EROFS`; `fsync` of a snapshot file writes nothing and
  returns 0), and `cfs_inode_write` refuses a tagged number whose tree
  is not the one entered. `cosmofs-clone` fsyncs a clone file and a
  snapshot file and checks the live file of the same number, also after
  a remount. With the old `cfs_vnode_sync` and without the guard (a
  throwaway worktree, x86-64 debug boot) it fails at that check:
  `read_matches(ENG "/a", "live2", 5)`.
- A write racing a clone's deletion could fall through to the live
  tree: the tree is now decided under the same hold that enters it.
- A clone deletion that cannot build its origin's set fails before the
  list is copied, leaving the mount writable.

Measured after the fixes: x86-64 debug `make test` PASS 154.4 s; AArch64
debug PASS 169.1 s; release `make test-snapshot` PASS x86-64 95.1 s,
AArch64 138.8 s; `make analyze` clean on both.
