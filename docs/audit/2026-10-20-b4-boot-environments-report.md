# Roadmap 1.2 B4: boot environments with cosmo-upgrade

Date: 2026-10-20 (started; validated and merged 2026-10-21). Base: `main` at `50ef1f4d` (after #421-#423, B1) and,
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

## Second pull request: boot environments (PKG-4)

As built:

- `userland/system/upgrade_be.{c,h}` (new): the pairing file `\cosmo\be`,
  text: `cosmo-be 1`, an `intent <set>` line and at most eight `env NAME
  UUID SET` lines, a set named by its manifest's SHA-512. Parsing refuses
  any other line, a name `cosmo-snap` would refuse, and a short or
  upper-case digest; `be_name` makes `be-<tag>` with a `-N` suffix when
  taken. `tests/host/test_upgrade_be.c` checks the grammar.
- `cosmo-upgrade.c`: an upgrade, before writing anything, deletes an
  earlier run's unpaired `be-*`, checks the room (the new `boot.tar`'s
  size and 1 MiB) and refuses naming it, writes
  `/var/lib/cosmo-upgrade/root-set` when missing, takes `be-<running
  build>`, and stages the new `\cosmo\be` (the environment paired with
  the current set, the intent the new set, the newest two kept) into the
  set's one-sector switch; environments let go are deleted after it. A
  rollback to a set an environment pairs with writes the intent, then the
  root's pending rollback (B1) keeping the replaced tree as
  `be-<running build>`, then the set switch; no base pass and no pending
  marker. `--list`, `--prune`, `--repair` (the root and the set made the
  intent's, by the same steps), `--after-rollback`, and `--status` naming
  the environments and a pending rollback the boot's mount did not apply.
  A set without an environment (installed by v1.0.7 or older) rolls back
  as before, and says so.
- `init.c`: after `pkg_recover`, when the root came from a `be-*`
  rollback, runs `cosmo-upgrade --after-rollback` before any service:
  `cosmo-snap diff` between the environment and the held replaced tree,
  and copies the files it added or changed under `/root`, `/home` and
  `/var/log` to `/var/lib/cosmo/rollback/<that tree>/`.
- `tests/boot/be_test.py` (in `test-upgrade`): builds a second set B from
  this build's set A (one added file, re-signed with the build key),
  installs A, upgrades to B, runs a `pkg upgrade` and changes `/etc`,
  rolls back, and checks the root byte for byte against the environment
  over the returned trees; then kills `cosmo-upgrade` after each step and
  boots the older kernel on the newer root.
- Docs: userland design ("Boot environments"), U13, `upgrading.md`
  ("Boot environments", "Rolling back"), `limitations.md`, testing.
- Not built: PKG-5 (the base pass could run the new set's own
  `cosmo-upgrade`), which only two releases whose tools differ can test;
  it stays in the backlog.

Validation (2026-10-21, local, both architectures): `make host-test`,
`make analyze`, debug `make test` at `QEMU_SMP=1`, `2` and `4` with the
network harness passing in each (x86-64 148.3 s, 151.1 s, 155.2 s; AArch64
145.5 s, 166.3 s, 158.6 s), `make test-smp2`, `make test-chaos`, `make
test-harness-retry`, release build and boot (x86-64 18.9 s, AArch64
21.0 s), all PASS; `make fuzz` PASS (a host build, run once). Release
`make test-snapshot` PASS (x86-64 157 s, AArch64 176 s); release `make
test-upgrade` PASS (x86-64 610 s, AArch64 597 s), its boot-environment
stages:

```text
test-be: upgrade: A -> B, environment be-... taken before anything was written
test-be: new: B ran hello 1.1 (a transaction) and changed /etc; --rollback recorded the intent, the root's rollback to be-... (keeping be-...-2) and the set
test-be: back: A on be-...'s root: hello 1.0, /etc and the base system byte for byte be-...'s; 8 operator file(s) kept in /var/lib/cosmo/rollback/be-...-2; --repair: nothing to do
test-be: crash-record: killed after the root's record: A's kernel booted B's root; --repair switched the set; then B, B
test-be: mixed-old: A's kernel booted B's root (the root's rollback withdrawn); --repair asked for it again; then A, A
test-be: crash-intent: killed after the intent: A, A booted; --repair rolled both; then B, B
test-be: crash-set: killed after the set switch: the next boot was A, A
test-be: space: a full root refused the upgrade (16134 KiB needed, 284 KiB free); the ESP byte for byte unchanged
test-be: PASS in 349.3s (AArch64 316.1s; 16540 KiB needed, 64 KiB free)
```

On Parallels Desktop (2026-10-21), main after the first pull request
(`cceff596`) installed from its live image on a new VM, booted the
installed disk, reboots and powers off; `pkg install hello fortune` left no
transaction behind, and `pkg install -f hello=1.0`, `pkg upgrade`, `pkg
rollback hello` went 1.0, 1.1, 1.0 with `pkg verify` clean. The message for
that downgrade says "upgraded" (PKG-9).

### Review of the second pull request (#426, 2026-10-21)

Qodo and Codex, twenty comments, thirteen distinct points; fixed:

- **Unpaired is not a reason to delete.** A missing, unreadable or
  malformed `\cosmo\be` read as empty, and the sweep deleted every `be-*`
  it did not name. Now the upgrade records the environment it is taking
  (`/var/lib/cosmo-upgrade/be-taking`) before the snapshot and removes the
  record once the switch pairs it; the sweep deletes only that one, when
  unpaired and not held. A `\cosmo\be` that does not parse refuses the
  upgrade, the rollback and `--repair`; `--prune` writes it again, empty,
  keeping every snapshot.
- **Refusals that were fallbacks.** A snapshot listing that fails other
  than `EOPNOTSUPP` refuses the rollback instead of rolling the set back
  alone. An upgrade refuses while a rollback of the root is pending, and
  when the root's record names another set than the current one.
- **Which sets get an environment.** None when the current set's own
  `cosmo-upgrade` does not read `\cosmo\be` (its binary lacks `cosmo-be
  1`): v1.0.7 and older refuse an ESP with the entry, so a rollback to such
  a set's environment would leave a system whose updater refuses its own
  ESP. None when the snapshot would raise a format-11 root to 12 that the
  new or current set's kernel cannot mount (B1's lazy raise, as `pkg`'s
  PK11 rule). So the upgrade to v1.0.8 takes none -- v1.0.7's kernel keeps
  no snapshots anyway -- and the first that can be fully undone is the
  one made from v1.0.8 (the guide and release notes say so).
- **The kept tree paired at once.** A crash between the root's record and
  the set switch left the tree the record kept unpaired after `--repair`.
  It is now paired in a one-sector switch right after the record.
- **Rollback reboots.** What is written between arming the root's
  rollback and the reboot is in neither tree; `--rollback` (and
  `--repair` when it arms one) asks init to restart, as `cosmo-snap
  rollback` does; `--no-reboot` leaves it to the operator.
- **`--repair`** finishes an upgrade whose set switch is done and base pass
  is not (the pending marker), and checks that `\cosmo\prev` is the
  intended, verified set before arming the root's rollback.
- **The operator's files.** `.done` is written only when every file is
  copied; a failed copy or diff exits non-zero, and init then does not
  report the boot complete, so B1's hold stays on the replaced tree and
  the next boot tries again. Paths are no longer cut at 900 bytes; writes
  loop over short writes.
- A compile-time bound for the dropped-environment table
  (`BE_KEEP < BE_MAX`); the roadmap no longer says B4 absorbs PKG-5; the
  dates say started 2026-10-20, validated 2026-10-21.

`be_test.py` gains a `back-held` stage (a file in the way of
`/var/lib/cosmo/rollback`: the copy refused, the hold kept, the files
kept at the boot after), `crash-record`'s kept tree paired, the
refusal with a rollback pending, and a `pending` stage (an upgrade
stopped after its switch, finished by `--repair`); `test_upgrade_be`
checks `be_aware`.
