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

## First pull request: identity, the interface, sizes

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
