# Roadmap 1.2 B4: boot environments with cosmo-upgrade

Date: 2026-10-20. Base: `main` at `50ef1f4d` (after #421-#423, B1) and,
for the second pull request, `3e28941d` (#424). The unit is roadmap 1.2's
B4, chosen by the owner after B1. Its acceptance, as `docs/roadmap-1.2.md`
writes it: `test-upgrade` gains an upgrade, `pkg upgrade` and a changed
`/etc` file, `cosmo-upgrade --rollback` and a reboot, after which
packages, `/etc` and the base system are byte for byte the pre-upgrade
snapshot's and the operator's files written after the upgrade are kept in
a named place, with a crash at each step booting one consistent
environment; and, for PKG-3 outside an upgrade, `pkg install a b` killed
between the two leaves both or neither at the next boot, `pkg upgrade`
likewise, and `pkg rollback NAME` returns one package to its previous
version and its files.

## Scope and baseline

At `50ef1f4d`: `cosmo-upgrade --rollback` returns the boot set, its
loader and the base system (bin, sbin, etc, usr, lib and the repository,
from `\cosmo\prev`'s `boot.tar`), and nothing else: the packages `pkg
upgrade` changed stay changed and `/etc` files the newer release added
stay (backlog PKG-4). `pkg` has no transactions: a multi-package request
interrupted part way leaves part of it, and nothing rolls one package back
(PKG-3). B1 gives the root snapshots, a rollback applied at the next mount
with a hold until the boot completes, clones and `cosmo-snap diff`.

## Decisions (the owner's, 2026-10-20) and how they were read

1. A boot environment is a root snapshot paired with a boot set, recorded
   in `\cosmo\be`, which the loader does not verify and `cosmo-upgrade`
   does. The loader reads only its four fixed files and never lists
   `\cosmo` (`boot/uefi/main.c`), so nothing about it changes and the stop
   condition "the loader must verify `\cosmo\be`" did not arise.
2. The operator's files under `/root`, `/home` and `/var/log` are kept
   across a rollback. Read as: copied, at the first boot after the
   rollback and before any service, from the tree the rollback replaced --
   which B1 keeps, held, as the safety snapshot -- since the target is a
   read-only snapshot until the switch and the switch replaces the whole
   live tree; nothing written into either before the switch survives it.
3. Package transactions are root snapshots undone by init after an
   interruption. `pkg`'s database needed no format change (the stop
   condition did not arise): the snapshot is the transaction.
4. Space: refused before anything changes, naming the room; CFS-20's
   refused rollback said by `--status` and at boot.
5. Retention: the newest two environments; `--prune`.

One consequence the decisions did not name, found by `test-upgrade`: a
transaction's snapshot raises a format-11 root to 12 (B1's lazy raise),
which a release reading 11 at most -- v1.0.7 and older -- cannot mount.
`pkg upgrade` on a system just upgraded from v1.0.3 made `cosmo-upgrade
--rollback` to v1.0.3 impossible. The rule built: `pkg` raises the root
only when every boot set kept on the ESP reads 12
(`/var/lib/cosmo-upgrade/oldest-format`, which `cosmo-install` and
`cosmo-upgrade` write); otherwise it takes no transaction and says so.

## First pull request: package transactions (PKG-3)

As built:

- `userland/lib/rootsnap.c` (new): a mount's snapshots through
  `/dev/fsctl` for the programs that take them on the operator's behalf
  -- list (with the pool record), create, delete, rollback, cancel,
  clone.
- `/dev/fsctl`: SNAP_LIST's pool record carries `free_bytes` (what data
  can still use; the metadata reserve excluded) and `total_bytes` in two
  of its reserved words; a held rollback's target is flagged `LIVE`.
- `pkg`: `install`, `remove`, `upgrade` and the new `rollback` each take
  `pkg-tx-<seconds>` of the root before their first change and delete it
  when they end (`tx_begin`, `tx_end`); refused before anything changes
  when the root lacks twice the packages' size and 512 KiB (`not enough
  space for this install: about N KiB needed ..., M KiB free; nothing
  changed`); no transaction on a root that keeps no snapshots, or when its
  snapshot would raise the root to format 12 while a kept boot set reads
  11 at most (`/var/lib/cosmo-upgrade/oldest-format`, written by
  `cosmo-install` and `cosmo-upgrade`); the installed version and the one
  before kept under `/var/cache/pkg/kept/NAME/` (`kept_note`); `pkg
  rollback NAME` installs the one before; `--stop-after=N`, the tests'
  exact interruption.
- `init`: before any service, a leftover `pkg-tx-*` is rolled back to at
  the next mount (B1's pending rollback, the tree it replaces kept as
  `pkg-undone-<seconds>`) with one reboot; the boot after deletes it and
  the lock it was taken under; a rollback the mount left pending is the
  loop guard (said, not rebooted again); `pkg-undone-*` is deleted once the
  boot completes.
- `cosmo-snap list` leaves `pkg-*` and `be-*` out unless `--all`, and
  prints the root's free space.
- Tests: `test-pkgtx` (`tests/boot/pkgtx_test.py`), run by `test-upgrade`:
  an install and an upgrade killed between two packages, `pkg rollback`,
  the space refusal; `test-upgrade`'s v1.0.3 stage asserts `pkg` takes no
  transaction while v1.0.3 is kept. Invariants PK11, PK12.
- Backlog: PKG-3 done; LOCK-9 (a `mutex PI: donation chain cycle`
  warning in release boots of the installed system, on main since at
  least B1) and MEM-9 (dirty page cache unbounded: a `dd` to `ENOSPC`
  exhausted memory) recorded, found by `test-pkgtx`'s first fill.

Measured (2026-10-20, branch `b4-pkg-tx` on `50ef1f4d`): the AGENTS
matrix, every step PASS on x86-64 (host-test, fuzz, analyze, debug boots
at `QEMU_SMP=1`, 2, 4, `test-smp2`, `test-chaos`, `test-harness-retry`,
release test) and AArch64 (the same without fuzz); release `make
test-snapshot` PASS (x86-64 97 s, AArch64 176 s); release `make
test-upgrade`, `test-pkgtx` in it, PASS (x86-64 190 s, AArch64 280 s):

```text
test-pkgtx: tx-install: PASS (killed after the first of two packages; pkg-tx-... rolled back at the next boot, one reboot; neither installed, check clean, no pkg-* left; installed again after)
test-pkgtx: tx-upgrade: PASS (killed after the first of two upgrades; pkg-tx-... undone; both at their old versions; a full upgrade then took both)
test-pkgtx: rollback: PASS (hello 1.1 back to 1.0, verified; the two last versions kept)
test-pkgtx: space: PASS (a full root: refused before any change, 806 KiB needed, 12 KiB free; installed once the room was back)
```

Two harness facts found on the way, both in `docs/testing/flakes.md`: a
recovery boot that stopped in the firmware's RNG call (first sighting),
and the OVMF reset stall inside one QEMU (second sighting), after which
`pkgtx_test.py` lets init's reboot end QEMU and starts the next boot in a
fresh one.

### Review of the first pull request (#425, 2026-10-20)

Qodo and Codex, seventeen comments; fixed:

- **Which leftover snapshot init undoes.** Every `pkg-tx-*` was treated as
  an interrupted transaction, so an operator's snapshot of that name, one
  a finished command failed to delete, or one left while another rollback
  was pending could later roll the root back over newer work. Now `pkg`
  records the transaction in `/var/db/pkg/tx-active` after taking the
  snapshot and removes it after deleting it, and init acts only on a
  snapshot that record names; `cosmo-snap create` and `clone` refuse the
  `pkg-` and `be-` prefixes; init deletes only the `pkg-undone-*` it made.
- **The format record.** Written after the set switch, a crash between
  left a stale `none`; now written before the commit sector (the least of
  the new and current sets), a failure refusing the switch, and recomputed
  by `finish()`; parsed strictly (`none` or 1-3 digits; else 0).
- **Room from the whole plan.** `install` and `upgrade` plan everything --
  dependencies included -- before the transaction and size it from the
  plan.
- **`pkg rollback`** refuses a version an installed dependant's constraint
  would not accept, or one whose own dependencies are not installed (`-f`
  overrides); a version installed before copies were kept comes from the
  repositories (`test-pkgtx` deletes the kept 1.0 and rolls back from the
  repository).
- **Failures.** A kept copy that cannot be written refuses the install
  before it changes anything; `ORDER` failing is a failed command;
  snapshots that exist but cannot be reached refuse the command; a removal
  on a too-full root runs without a transaction; a no-op upgrade takes
  none; init says when it cannot reach the snapshots; every boot removes
  the stale `pkg` lock.

Answered: `--stop-after` stays in release builds, as `cosmo-upgrade
--stop-after` does (shell completeness S7): the acceptance tests that use
it run on release images; it acts only when asked.

Found while validating: `test-upgrade`'s v1.0.3 refusal stage holds the
set, its variants and the tools in `/tmp`, a ramfs of at most 64 MiB on
v1.0.3 (`RAMFS_MAX_PAGES`), and a copy of the kernel there tipped it once
this build's `boot.tar` grew (`cp: write error: No space left on device`,
AArch64, twice); a link replaces the copy.

After the fixes: x86-64 release `make test-upgrade` (with `test-pkgtx`)
PASS, debug `make test` PASS 154.1 s, `make analyze` clean, `make
host-test` PASS; AArch64 release `make test-upgrade` PASS (after the link),
debug `make test` PASS 159.8 s, `make analyze` clean.
