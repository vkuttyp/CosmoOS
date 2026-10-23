# Services after a reboot, every mirror disk's ESP, the rc's final notes (USR-8, PKG-11, BUILD-7)

Date: 2026-10-23. Base: `main` at `78afb0b8` (#445's merge). PR #446.

The three lines the owner made blockers for v1.0.12, before roadmap 1.2
B5. They sit outside B5's PR count.

## Scope and baseline

- **USR-8** (Commands, correctness, S). On the installed system
  `/run/svc` was on the root disk, nothing cleared it at boot, and
  `svc`'s `running()` trusted any pid `kill(p, 0)` could signal. On the
  Parallels VM `dropbear.pid` held 13 from 2026-10-22; pid 13 was
  `syslogd` at every boot, so `svc: dropbear is already running` and ssh
  was refused through several boots and upgrades.
- **PKG-11** (Packages and upgrade, usability, S). On a mirrored root
  `cosmo-upgrade` wrote the new release to the booted disk's ESP only;
  the other disk's ESP kept the release it was installed with.
- **BUILD-7** (Build and tooling, correctness, S). An rc's release run
  checked the notes built from `docs/releases/vX.Y.Z-rcN.md`;
  `vX.Y.Z.md` was first read by the final tag's run. v1.0.11-rc1 passed
  (run 39938572061) on notes saying "CosmoOS 1.0.10", and the v1.0.11
  tag's run on the same commit failed on them (run 39942569981).

## What changed

### USR-8: a pid file is this boot's and names its own process

- init mounts a fresh ramfs on `/run` after switching to the disk root
  and creates `/run/svc` in it (`fresh_run()` in `userland/init/init.c`),
  so nothing the old root held under `/run` is seen.
- `svc` writes `pid start-time` (`/proc/<pid>/stat` field 22) to each
  pid file. `running()` takes a pid as the service's only when the
  process is alive, has the recorded start time, and its
  `/proc/<pid>/cmdline` is `svc --supervise <name>`. A one-field file
  from before is checked by the cmdline alone.
- `svc stop` kills a recorded child only when it is the live
  supervisor's child (its parent pid) or has the child file's recorded
  start time; a reused pid is never signalled.
- Invariant U14 (`docs/userland/invariants.md`); the Services section of
  `docs/userland/design.md`.
- Tests: init's self-test writes a stale pid file and child file naming
  the test process itself, in both forms, and requires the service to
  start, report, stop, and the test to survive. test-install's reboot
  stage writes `/run/svc/*.pid` and `*.child` holding pid 1 into the old
  root's `/run` (under the ramfs), and the persist stage requires
  `syslogd` and `dropbear` started, `svc status dropbear` running and
  `/run` a ramfs in `/proc/mounts`.

### PKG-11: an upgrade reaches every disk's ESP

- `cosmo-upgrade` finds the pool's other copies (`COSMO_FSCTL_POOL_STATUS`,
  spares skipped), the ESP on each copy's disk (partition 1 with the
  booted ESP's type GUID), and after an upgrade, `--fetch`, `--rollback`
  or `--repair` copies the booted ESP to each in 256-sector chunks,
  writing only the chunks that differ and reading back what it wrote.
  `the ESP on vda1 is ahci0p0p1's (44032 sectors written)`. A marker in
  `/var/lib/cosmo-upgrade/esp-sync` is held while a copy is written.
- A copy that is absent is named, with the command that repairs it once
  it is back. The reboot after an upgrade or rollback does not happen
  when a present peer could not be written.
- `cosmo-upgrade --esp-check` (read-only) compares each peer's set, loader
  and boot-environment files with the booted ESP's; init runs it at every
  boot from a disk root, which prints `the ESP on X is not Y's (its set …,
  the booted …): run cosmo-upgrade --repair to bring it up to date`.
- Invariant U15; `docs/userland/design.md` "Every disk's ESP";
  `docs/guide/mirrors.md` "Upgrades reach both disks".
- Test: test-mirror's new `esp` stage, on two fresh disks e and f
  installed `--mirror` from set A, with signed sets B and C served over
  HTTP: upgrade A→B, then f alone boots B; B→C with f absent names the
  missing copy; f back, the boot says its ESP lags, `--repair` brings it
  up, f alone boots C; `--rollback` C→B on both, then f alone boots B.
  Which set booted is read from the loader's `archive: N bytes read`
  line against each set's `boot.tar`.

PKG-11 is a usability line, proven by that acceptance stage; it has no
`--old` probe.

### BUILD-7: an rc checks the final tag's notes

The release workflow's publish step, for a tag `vX.Y.Z-rcN`, also
requires `docs/releases/vX.Y.Z.md`, builds the final tag's notes from it
with `release-notes.py`, and runs `check-release-version.py --tag
vX.Y.Z` on them (`.github/workflows/release.yml`;
`docs/development.md`).

## Probes

- `tools/usr8-probe.py --check selftest|install [--arch A] [--old]`:
  `--old` reverse-applies the fix commit's `svc.c` (and `init.c` for
  install) in a throwaway worktree.

  | check | arch | fixed | `--old` |
  |---|---|---|---|
  | selftest | x86_64 | PASS: the stale pid files passed over, the service started | PASS: `svc: stale is already running` for a pid file naming the test (process-user FAIL) |
  | selftest | aarch64 | same | same |
  | install | x86_64 | PASS: every service started past stale pid files | PASS: the boot after stale pid files did not start a service (persist FAIL) |
  | install | aarch64 | same | same |

- `tools/build7-probe.py [--old]` runs the publish step's checks for
  v1.0.11-rc1 on v1.0.11's notes at `569d0905` (the rc1 commit) and
  `af381311` (the released one). Fixed: the first fails ("CosmoOS
  1.0.10"), the second passes. `--old`: both pass. Both modes PASS.

## Validation

The full matrix on head `7a8a770d`, one step after another, every step
exit 0 with its own PASS line:

| step | x86_64 | aarch64 |
|---|---|---|
| analyze | clean (7 baselined, 0 unexpected) | clean |
| debug `make test`, `QEMU_SMP=1` | PASS 161.4 s | PASS 151.5 s |
| `QEMU_SMP=2` | PASS 170.0 s | PASS 164.8 s |
| `QEMU_SMP=4` | PASS 170.7 s | PASS 169.3 s |
| `test-smp2` | PASS 168.2 s | PASS 162.9 s |
| `test-chaos` | PASS 173.6 s | PASS 167.2 s |
| `test-harness-retry` | PASS 175.2 s (recovered on attempt 2, the injected break) | PASS 167.8 s (same) |
| `test-crash` | PASS 144.9 s | PASS 150.0 s |
| `BUILD=release test` | PASS 19.1 s | PASS 22.0 s |
| release `test-install` | PASS 31.5 s | PASS 48.0 s |
| release `test-mirror` (all stages) | PASS 179.9 s | PASS 231.6 s |
| release `test-upgrade` (with `test-pkgtx`, `test-be`) | PASS (be 467.7 s) | PASS (be 544.8 s) |

The network harness passed in every debug boot. `make host-test`,
`host-test-fetch` and `fuzz` passed; `make reproducible`: yes.

ESP sectors written in test-mirror's `esp` stage: the upgrade 44032
(x86_64) and 45056 (aarch64); the repair 43776 and 44800.

## What is not done

- The VM proof (ssh after a reboot with no manual cleanup) is v1.0.12's
  rc step.
- `--esp-check` compares the files cosmo-upgrade writes, not every
  sector; a peer ESP damaged elsewhere is not reported.
