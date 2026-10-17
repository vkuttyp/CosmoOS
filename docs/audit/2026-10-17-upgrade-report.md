# Updating an installed system: S7 — 2026-10-17

The 1.1 roadmap's S7, the last unit of the shell-completeness track.
Base: main `81b317dc` (S6 merged). Goal: an installed system moves to a
newer release without reinstalling and back, refuses a set it cannot
verify, and stays bootable through an interruption.

## Before

| What | Before |
|---|---|
| Moving an installed system to a newer release | reinstall: `cosmo-install` from the new live image, which erases the disk |
| The ESP on an installed system | raw sectors through `/dev/blkctl`; nothing reads or writes its FAT |
| The loader | read by the firmware from `\EFI\BOOT`; not covered by the manifest, and no copy anywhere a signature covers |
| `\cosmo\prev`, fallback | none: the loader reads the fixed `\cosmo` paths and stops on any failure |
| `/var/lib/pkg/repo` | the image's repository as the installer copied it; never replaced, so `pkg upgrade` had nothing newer |
| Release assets | image, loader, kernel, archive, command line, manifest, `SHA256SUMS` |

## Decisions

Four were the owner's, asked before any code (2026-10-17):

**S7 before P6, from a local release.** The roadmap ordered S7 after P6
(the network repository). The release is read from a directory the
operator fills, by `wget`, `scp` or a mounted disk; P6 adds fetching.
The roadmap entry now says so.

**A one-sector switch, no loader change.** The loader has no fallback,
and v1.0.3's loader cannot be changed by an upgrade that has not
happened yet. So `cosmo-upgrade` writes everything the new set needs into
free clusters, flushes, and switches by rewriting directory entries,
one sector per switch: `\EFI\BOOT`'s loader entry first, then `\cosmo`'s.
The loader goes first because the new loader boots the old set (the boot
code is unchanged since v1.0.3 and every build's ring includes
`tools/keys/*.pub`), while v1.0.3's loader cannot verify a set signed by
the build's key. The states an interruption can leave are the old set,
the new loader with the old set, and the new set.

**Rollback returns the boot set and the base system.** An installed root
holds a copy of the archive's `bin/`, `sbin/`, `etc/` and `usr/`, so a
new kernel alone would run v1.0.3's programs, and a rolled-back kernel
the new ones. `cosmo-upgrade` brings the root from the old archive to the
new one; `--rollback` does the reverse. The previous base system is not
copied aside: it is the previous `boot.tar`, which `\cosmo\prev` keeps and
its manifest verifies. That is the same promise the owner chose ("keeps
every file it replaces"), kept by the archive rather than by a second
copy on the root. Packages `pkg upgrade` changed are not returned
(docs/pkg/invariants.md).

**Trust is the running system's key ring.** The manifest is checked
against `/etc/pkg/keys`, `pkg`'s trust root. Trusting another key means
putting its `.pub` file there, which the test does as a visible step. The
loader travels in `boot.tar` (`efi/<loader>`), so the signature covers
the loader `cosmo-upgrade` installs.

**The baseline is the published v1.0.3.** `tools/fetch-release.py`
downloads it (`gh`, or the API with the run's token) and checks it
against the release's `SHA256SUMS`.

Two more came from the tests:

**Each set keeps its own loader.** The first version kept a loader only
in `\EFI\BOOT`, and `\cosmo\prev`'s directory referenced the old loader's
clusters. An upgrade stopped after its loader switch and run again: the
rerun's sweep freed v1.0.3's loader, which no reachable entry named, and
`\cosmo\prev` ended up with the new loader (`test-upgrade`: "\cosmo\prev's
loader is not v1.0.3's"). Now `\cosmo\<loader>` and `\cosmo\prev\<loader>`
hold the loader each set came with, moved by the same sector write as
the set, and `\EFI\BOOT` holds a separate copy of the active one. A pending
marker on the root names the set being switched to, so a rerun after the
set switch finishes the root pass rather than starting another run. Without
it, a rerun of an interrupted `--rollback` would step forward again.

**v1.0.3 fetches into `/tmp`.** See the defect below.

## The cosmofs defect

The third `test-upgrade` run on x86 failed while v1.0.3 downloaded the
tampered 12 MB archive: `cosmofs: inode 186 block 480: no copy matches
its checksum`, then `2442 dirty page(s) ... lost: write-back failed
(-5)`. A repro, `wget` of `boot.tar` four times per boot on an installed
disk, failed once in 16 downloads on v1.0.3 and once in 24 on this
build's kernel. So it was a live defect, and it would have made the
acceptance test flaky.

A temporary event ring (data writes, checksum stores, allocations,
deferred frees, releases, dumped on a checksum failure; not committed)
named it. The record at logical block 552 was written compressed in
generation 90, five physical blocks with matching checksums. In generation
91 the checksum for block 552 was stored again, `780dd4da`, with no write
to the record's blocks and no change to its extent. A rebuilt record that
no longer compresses (a page that was zeros when the record was written,
filled by wget's next write) goes to `record_write`'s plain fallback. Its
first `data_write_block` stores the new block's checksum and then calls
`set_extent` for one block, which refuses a range that would cut a record
(-EIO). The fallback's comment assumed the first block displaced the
record whole. The record stayed mapped under the new checksum, and every
later read of it failed.

The fix: the fallback drops a record covering its range (`drop_range`)
before writing plain blocks. `cosmofs-compress` gains the case, eight
incompressible pages over a compressed record and then `file_sync`.
`tools/cosmofs-record-fallback-probe.py --old` reverse-applies the fix's
`cosmofs.c` in a worktree, and the sync fails on both architectures.
`data_write_block`'s order (the checksum before the mapping, so any other
mapping failure does the same damage) is inventory section 17. v1.0.3
keeps the defect, so the test, and the guide, fetch the release into
`/tmp` (ramfs) on v1.0.3.

## Changes

| Where | What |
|---|---|
| `userland/system/cosmo-upgrade.c` (new) | the tool: key ring, manifest check, archive index, ESP layout, the switches, the pending marker, the root pass, `--rollback`, `--status`, `--check`, `--stop-after` |
| `userland/system/upgrade_fat.c`, `.h` (new) | FAT32 over `/dev/blkctl`: boot sector checks, the FAT in memory, both copies and FSInfo on commit, lookups, file reads, chain writes, the sweep, entry stamps |
| `userland/system/upgrade_ed25519.c` (new) | the kernel's Ed25519 for user mode |
| `userland/system/blkdevs.c`, `.h` | `blkdevs_write`, `blkdevs_flush`; the command buffer holds a WRITE's data |
| `Makefile` | `efi/$(LOADER_EFI_NAME)` in the boot archive; `cosmo-upgrade` in the release set; `test-upgrade` |
| `scripts/mkrelease.py`, `release.yml` | `cosmoos-ARCH-cosmo-upgrade` |
| `tools/fetch-release.py` (new) | a release's set, checked against its `SHA256SUMS`, cached |
| `tests/boot/upgrade_test.py` (new), `ci.yml` | the acceptance test; job `upgrade` |
| `kernel-services/filesystem/cosmofs/cosmofs.c`, `cosmofstest.c` | the record-fallback fix and its self-test case |
| `tools/cosmofs-record-fallback-probe.py` (new) | the fix's probe |
| docs | userland design "Updating", api, testing, invariant U12; boot design; cosmofs design, testing, V26; pkg invariants; the guide's new Upgrading page, installing, packages, limitations, README; development "Releases"; inventory section 17; roadmap; history |

## Tests

`make test-upgrade` (job `upgrade`, both architectures; release builds):

| Stage | Checks |
|---|---|
| install | v1.0.3's live image boots verified with `36ae7bb4947896ab` and installs itself |
| refuse | v1.0.3 boots with the release key; `unknown signing key <build key>` before `trust.pub` is in `/etc/pkg/keys`; then `bad signature`, `unknown signing key <throwaway>`, `boot.tar does not match the manifest (SHA-512)`, each exit 3 with `nothing written`; `--check`; the ESP after power-off equals, sector for sector, the ESP read once the first prompt was up |
| written | `--stop-after=written` (exit 4), QEMU killed; `\cosmo`'s four files and the loader still v1.0.3's (mtools) |
| loader | v1.0.3 boots verified; `--stop-after=loader`, killed; the loader new, `\cosmo` v1.0.3's |
| upgrade | the new loader boots v1.0.3's set with the release key; the full upgrade; `\cosmo` the new set and loader, `\cosmo\prev` v1.0.3's set and loader, `\EFI\BOOT` the new loader |
| new | the build boots with its own key; `/sbin/mkfs.cosmofs`, the new `/etc/rc`, `/root/keep`, the installer's `/etc/passwd` and `repos.conf`; `--status`; `pkg update && pkg upgrade`; `--rollback` |
| back | the ESP is v1.0.3's again, `\cosmo\prev` the build's; v1.0.3 boots with the release key, its base system back, `/root/keep` kept |

The first refuse run compared the ESP from before the boot: 13 sectors
differed, all from `\NvVars`, which x86 firmware with no variable store
writes into the ESP at every boot. The snapshot is now taken at the first
prompt, after the firmware is done.

`cosmofs-compress` (every debug boot) has the record case.

### Mutations

Each in a throwaway worktree of `744213c0`, release build, `test-upgrade`
on x86-64:

| Mutation | Caught by |
|---|---|
| M1: the new set checked without the ring (`set_verify(&nw, false, ...)`) | refuse: the untrusted set upgraded, exit 0, want 3 |
| M2: the set switched before the loader | loader: after `--stop-after=loader`, `\cosmo\kernel.elf` is 5097040 bytes, not v1.0.3's 4888464 |
| M3: the root pass left out of the upgrade | upgrade: no `cosmo-upgrade: root:` line |

### The probe

`tools/cosmofs-record-fallback-probe.py`, debug `make test`:

| Arch | fixed | `--old` |
|---|---|---|
| x86-64 | PASS (cosmofs-compress ok) | PASS (FAIL: `file_sync(f) == 0` at line 1483) |
| AArch64 | PASS | PASS |

The first x86 `--old` run built nothing. Its worktree rebuilt BusyBox in
the shared `out/busybox-x86_64` while a `test-upgrade` in the main tree
used it (make error in `busybox.mk:103`). Run again alone, it passed.

## Validation

The AGENTS.md matrix on `384ef5e4`, every step passing on its first run:

| Step | x86-64 | AArch64 |
|---|---|---|
| `host-test` | PASS (34 checks) | PASS (34 checks) |
| `fuzz` | PASS | PASS |
| `analyze` | clean (7 baselined, 0 unexpected) | clean (7 baselined, 0 unexpected) |
| debug `test`, `QEMU_SMP=1/2/4` | 149.0 / 151.6 / 161.6 s | 139.2 / 163.2 / 155.3 s |
| `test-smp2` | 158.8 s | 159.7 s |
| `test-chaos` | 152.4 s | 166.4 s |
| `test-harness-retry` | 162.6 s | 155.0 s |
| release `test` | 18.8 s | 21.5 s |
| `test-busybox` | 52.5 s | 58.9 s |
| `test-net-login` | 85.9 s | 91.0 s |
| `test-install` | 27.4 s | 42.3 s |
| `test-upgrade` | 88.9 s | 80.5 s |

Branch CI: recorded in the PR.

## Review (PR #399)

The first round (Codex, Qodo; 18 findings) on `1681b22d`, whose branch CI
passed. Fixed in `b575b9e9`:

| Finding | Fix |
|---|---|
| The new loader may not trust the current set's key (a power cut after the loader switch leaves nothing that boots) | `loader_trusts`: the loader image must hold the set's 8-byte key id followed by a public key that verifies its manifest (the ring is `{ id, pub, name }`, revoked keys refused at build time), for the current set and for the new set's own; refused otherwise, exit 3 |
| A second rollback promised by the guide (Qodo), and with it the order | after a rollback to v1.0.3, `\EFI\BOOT` is v1.0.3's loader, which lacks the build's key: `--rollback` now switches the loader first when the running loader does not trust `\cosmo\prev` but `\cosmo\prev`'s loader trusts the current set, set first in the other case, refused when neither; `/sbin/cosmo-upgrade` is gone after that rollback, so the guide runs the newer binary from `/tmp` |
| Two runs at once share free clusters | an abstract unix name bound for the run's life; a second run is refused, exit 3 |
| `finish` cleared the marker with `\cosmo\prev` unreadable | it fails and keeps the marker when a root pass is pending |
| `keyid` uninitialised on an early `set_verify` failure | `(none)` first |
| sizes above 4 GiB narrowed; a `..` substring refused | refused; only a `..` component refused |
| FAT runs coalesced past the last cluster; reserved entries swept | `valid()` per added cluster; `0x0FFFFFF0`-`0x0FFFFFF7` left alone |
| No package in the rollback test | hello 1.0 on v1.0.3, `1 package upgraded` to 1.1, still 1.1 after the rollback |

Answered without a change: the `MAX_KEYS`/`MAX_MEMBERS` bounds (the table
and the loop use one constant); a failed plain write after the record is
dropped (inventory 17's ordering problem; the range was wrong before the
fix too); `TARGET.cosmo-upgrade` and `FILE.new` are the tool's names (now
stated in the design); the release cache is named by its tag. The
downloaded updater on v1.0.3 runs before anything verifies it: the guide
says so, and the inventory records the fix.

`test-upgrade` gains a refused set whose loader trusts a throwaway key in
place of the release key, the package, a second rollback with the
build's binary (loader first) and a ninth boot, `forward`: PASS 70.9 s on
x86-64, 101.3 s on AArch64. Mutations on `b575b9e9` (x86-64, release):

| Mutation | Caught by |
|---|---|
| M4: the current-set loader check deleted | refuse: the untrusting set upgraded, exit 0, want 3 |
| M5: `--rollback` always switches the set first | back: no `loader switched: ... is \cosmo\prev's` line |

`analyze` (7 baselined, 0 unexpected) and `host-test` pass on both
architectures; the full matrix was not repeated (the change is
`cosmo-upgrade`, its FAT code and its test).
