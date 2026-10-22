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
