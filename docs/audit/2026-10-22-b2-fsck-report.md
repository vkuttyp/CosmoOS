# Roadmap 1.2 B2: fsck and scrub as commands that repair

Work date 2026-10-22. Baseline: main at `4527d16d` (v1.0.9 published).
B2's acceptance (docs/roadmap-1.2.md): a host tool damages an installed
image in each of the checker's finding classes; `fsck.cosmofs` at boot
reports and repairs each repairable class and refuses the rest with the
class named; on a two-way mirror with one member's blocks flipped, `scrub
--repair` restores them and a second scrub is clean.

Planned in three pull requests: the cosmofs defects B2 absorbs (this
section), fsck and scrub with `make test-fsck` (ticks B2), and the
v1.0.10 release.

## First pull request: the defects

Each fix is one commit, and each has a self-test that fails without it:
`tools/b2-defects-probe.py --case NAME --old` reverts that commit
(three-way, the whole commit) in a throwaway worktree and runs a debug
`make test`.

| Case | Defect | Self-test | `--old` on x86-64 and AArch64 |
|---|---|---|---|
| `ro` | CFS-1: a read-only mount committed its free/orphan replay at unmount (and read repair and a rollback switch would write) | `cosmofs-ro-replay` | fails at `changed == 0` |
| `compress` | CFS-2: a new file was LZ4 on any format version | `cosmofs-version-compress` | fails at `v5 == 0` |
| `nomem` | CFS-6: `extents_store` and `inode_sync` abandoned the transaction on an allocation | `cosmofs-nomem` | fails at `retry == 0` |
| `scrub-counts` | CFS-11: the scrub's -EIO dropped its counts at `/dev/fsctl` | `cosmofs-scrub-counts` | fails at `direct == 0 && sc.unrecoverable == 1` |
| `root-type` | found: a root inode that was not a directory mounted and panicked at the VFS's assertion | `cosmofs-root-type` | the kernel stops at the assertion |
| `scrub-open` | found: the scrub checked device copies of metadata the open transaction held only in memory | `cosmofs-scrub-open` | fails at `sc.unrecoverable == 0`, and `cosmofs-compress` with it |

**How `scrub-open` was found.** With CFS-6's `inode_prepare` in place,
`cosmofs-compress` failed: its scrub, run after a truncate and before a
commit, reported two unrecoverable blocks (`dva 0x10` kind CSUMIDX and
`0x12` kind CSUM). Turning `inode_prepare` into a no-op made the test
pass, so CFS-6 was first suspected. But the scrub log at its start showed
`nr_dirty 5` both with and without `inode_prepare`: the scrub checks a
block's copies on the devices, and a block copied-on-write in the open
transaction is not there yet. Without `inode_prepare` the new checksum
blocks happened to land on stale blocks of the same kind and address and
verified by luck; with it they landed on others. `cosmofs-scrub-open`
makes the case deterministic (a young pool's never-written blocks are
zeros) and the scrub now skips what the transaction holds dirty.

**CFS-6's shape.** `extents_store` reads the old chain and sets aside every
block its copies and new blocks need (`cfs_prep_meta`: blocks allocated,
buffers made, nothing changed), then rewrites the inode's runs and the
chain, which cannot fail. Every operation ending in `inode_sync` first
makes each inode it stores writable (`cfs_inode_prepare`), so the final
write touches only dirty, cached buffers.

### Validation (2026-10-22)

The matrix of AGENTS.md on the tree the PR opens with, less two
commit-only moves made after it (below):

| Step | x86-64 | AArch64 |
|---|---|---|
| `make host-test`, `fuzz`, `analyze` | pass, pass, pass | pass, pass, pass |
| debug `make test`, `QEMU_SMP` 1 / 2 / 4 | pass / pass / pass | pass / pass / pass |
| `test-smp2`, `test-chaos`, `test-harness-retry` | pass, pass, pass | pass, pass, pass |
| release build and boot | pass | pass |

The first matrix run found two `core.NullDereference` reports in
`cfs_unlink_common` and `cfs_rename`: the first draft prepared `victim`
under `victim != NULL`, which told the analyzer it could be NULL; it
cannot, and the check is gone. The x86-64 `QEMU_SMP=1` boot of the second
run never started -- QEMU could not bind the UDP port the harness had
picked (`Could not set up host forwarding rule
'udp:127.0.0.1:55879-:7'`) -- and was run again. After the matrix two
moves were made so that every fix commit reverts cleanly for the probe
(the root's check placed before the orphan replay, away from CFS-1's
lines; `cfs_buf_is_dirty`'s declaration moved beside
`cfs_buf_mark_dirty`); neither changes behaviour, and the debug boots at
one CPU and `analyze` on both architectures were run again on the final
tree: pass.

### Review of the first pull request (#433, 2026-10-22)

Codex's six comments, each checked against the code:

- **Fixed (P1):** `cfs_truncate_blocks` queued the dropped runs' blocks
  free before `extents_store`; safe while a failed store abandoned the
  transaction, not once CFS-6 made it an ordinary error. The blocks are
  now released after the store. `cosmofs-nomem` gained the case (a
  truncate whose chain copy fails, then a commit and a clean check); with
  the frees made unconditional it fails at `tr.clean` with one
  free-in-use block.
- **Fixed (P1):** `cfs_truncate` trimmed the page cache before
  `inode_prepare`, so an `-ENOMEM` there left trimmed pages and the old
  size. The inode is prepared first, and again under the lock after the
  trim (a no-op unless a commit came between).
- **Fixed (P2):** a failed set-aside gave its blocks back deferred, held
  allocated until a commit; `alloc_undo` clears their bits at once.
- **Fixed (P2):** `buf_alloc` made its buffer before evicting a clean
  one; `buf_make` evicts first again.
- **Fixed in the second pull request (P2):** a scrub counted an
  `-ENOMEM` from a helper as an unrecoverable block. The second pull
  request rewrites the scrub; it stops and returns `-ENOMEM`.
- **Answered (P2):** "`--old` cannot find the subjects after the squash":
  this repository merges with merge commits, so each fix commit stays
  reachable by its subject; the probe reverts it three-way.

After the fixes: debug `make test` at QEMU_SMP 4 (x86-64) and 2
(AArch64), `analyze` on both architectures: pass; each fix commit still
reverts cleanly.

## Second pull request: fsck and scrub as commands

The design is `design.md`, "fsck and scrub as commands (roadmap 1.2
B2)"; the invariants CF23 and CF24. In brief:

- **One checker.** `fsck.cosmofs` sends `/dev/fsctl` `CHECK_DEV` (op 11)
  for an unmounted device; `cosmofs_check_dev` mounts it where no
  namespace sees it (`vfs_mount_detached`: `do_mount`, so the device is
  claimed, and nothing else), runs `cosmofs_check`, and releases it. No
  new mount mode was needed: `--check` is a read-only mount, which CF21
  makes write nothing, and `--repair` a writable one, which commits only
  when it repaired something. Two superblock slots naming different
  pools, neither slot verifying, or a mount failing at a named stage are
  answered in a device record (`struct cosmo_fsctl_dev`), as classes
  `superblock`, `root`, `free-record`, `orphan-record`, `snapshots`, ...
- **Offline, one more repair:** a reachable block the bitmap calls free
  (`COSMOFS_CHECK_OFFLINE`, `cfs_bitmap_claim`). No class turned out to
  need a format change to repair.
- **At boot**, before the root mounts, `fsck.cosmofs --boot` with the
  policy of `fsck=` (repair by default): bookkeeping repaired unattended,
  orphans and link counts repaired and logged by inode, anything else
  refused by name with init left on the live root; the kernel enforces
  the same set (`COSMOFS_CHECK_BOOT`). One line every boot.
- **The scrub** reports without repairing unless asked (`SCRUB_EX`, op
  12; the old SCRUB still repairs), reads every tree each block once, and
  paces itself at `fs.scrub_rate` blocks a second in 100 ms windows,
  letting go of the lock between them. `mkfs.cosmofs --mirror` makes the
  two-way mirror it repairs from.
- **CFS-20 surfaced:** the snapshot listing's pool record carries why the
  mount left a rollback pending (`rollback_error`, a reserved word
  before), and init's line ends with it.
- No fsctl version bump: two new ops with records of their own, a flag
  only they accept, a reserved word given a meaning.

### Found while building it

- **A refused repair lost the block it refused to touch.** The first
  `cosmofs_check_dev` committed every writable mount on release; in
  `cosmofs-check-dev` the boot-mode pass refused (a free block in use is
  not a boot's), then its commit's copies took the very block the bitmap
  called free, and the next pass saw no free-in-use block. A writable
  check now commits only after a repair. The same hazard remains for a
  repair's own mount, which replays the orphan record and may switch a
  rollback before the check runs (CFS-25).
- **`cosmofs_test_corrupt`'s FREE_IN_USE goes through a commit**, whose
  copies take the freed block at once; `cosmofs-check-dev` clears the
  bit on the device instead.
- **Four chain walkers had no bound** (`deadlist_settle`,
  `snapshot_holds`, `snaplist_insert`, the deadlist handover in
  `cfs_snapshot_delete`), now bounded (CF24; on write paths, by review).
- **No `/dev/urandom`** (USR-7): `head -c N /dev/urandom` gave 32 bytes,
  which first left `/fsck/sparse` and the mirror's file a block each; the
  test uses gzip output, which does not compress again.

### The checker's classes and what a boot does

| Class | Boot | `test-fsck` damage | After `fsck.cosmofs --repair` |
|---|---|---|---|
| leaked | unattended | `leaked` | (repaired at boot) |
| counter | unattended | `counter` | (repaired at boot) |
| snap-identity | unattended | `snap-identity` | (repaired at boot) |
| rollback-record | unattended | `rollback-record` | (repaired at boot) |
| hold | unattended | `hold` | (repaired at boot) |
| nlink | logged | `nlink` | (repaired at boot) |
| orphan | logged | `orphan` | (repaired at boot) |
| free-in-use | refuse | `free-in-use` | repaired, exit 1, then clean |
| cross-linked | refuse | `cross-linked` | its leaked block repaired; cross-linked left, exit 4 |
| dangling | refuse | `dangling` | refused; dangling, orphan left |
| bad-entry | refuse | `bad-entry`, `cycle-directory` | refused; bad-entry and orphan / nlink left |
| extent-order | refuse | `extent-order` | left |
| extent-overlap | refuse | `extent-overlap` | left |
| dup-name | refuse | `dup-name` | left |
| unreadable | refuse | `unreadable` | refused; unreadable, orphan left |
| cycle | refuse | `cycle-extent` (also cross-linked), `cycle-deadlist` | left |
| snapshots, free-record, orphan-record, root, superblock | refuse | `cycle-snaplist`, `cycle-free-record`, `cycle-orphan-record`, `root-type`, `superblock` | the mount (or the slot comparison) refuses again; left |

### `make test-fsck` (2026-10-22, release builds)

Every class above, the read-only stage and the mirror stage pass on
x86-64 (182.3 s) and AArch64 (283.9 s). The mirror: 16 of a 64-block
file's blocks flipped on one device, `fsctl scrub` 16 bad copies and none
repaired (exit 4), `--repair` 16 repaired (exit 1), the next scrub clean
(exit 0), the file's sha256 unchanged.

**The bound and a writer.** At `fs.scrub_rate=512`, the scrub read 1806
blocks in 3447 ms on x86-64 (the bound allows no less than 3400) and 1866
in 3635 ms on AArch64 (3500). A 4 MiB writer took 0.41 s alone and 0.41 s
during the scrub on both (0.31 s during it in one earlier x86-64 run):
at this bound the scrub leaves the writer's time where it was, within
QEMU's noise. The default bound is 4096 blocks a second.
