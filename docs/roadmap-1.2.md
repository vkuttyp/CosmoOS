# CosmoOS 1.2 roadmap

Written 2026-10-20 at v1.0.7, after the [1.1 roadmap](roadmap-1.1.md)
completed (13 units in ten days, 2026-10-13 to 2026-10-20), as three
candidate directions. **Adopted 2026-10-20: the owner chose
[B, storage depth](#b-storage-depth-chosen).** Its milestones are the 1.2
roadmap; A and C stay below as deferred directions, not scheduled.
[`AGENTS.md`](../AGENTS.md#scope-rules)'s scope rules apply to B's
milestones as they did to 1.0's: one at a time, the owner choosing the
next (B1 first; B2 or B4 after it), each done when its acceptance test is
green on main, its box ticked in the same PR, at most five PRs a
milestone.

Backlog references (`CFS-8`, `LX-7`, ...) are lines of
[`backlog.md`](backlog.md). Sizes are relative to 1.1 (1.0x = about ten
days of the same kind of work).

## Always, whichever direction

Small correctness and security lines from the backlog that any direction
should take first, as a first unit before its first milestone: each is a
size-S fix with its own failing test (or a settings change), several are
flakes that have already reached CI, and none waits on a feature.

| Line | What |
|---|---|
| TEST-1 | `wait-multi-prepare`'s guard misses `schedule()` re-picking the thread (AArch64 CI sighting) |
| TEST-2 | `module-unload-busy` compares two different clocks (flakes.md) |
| TEST-3 | `net-neigh-down-race`'s park hooks take another interface's caller (flakes.md) |
| TEST-5 | CI keeps no serial log for a failing `test-display` |
| LX-1 | `waitpid(WCONTINUED)` hides a continue when the child exits at once (two sightings) |
| CFS-1 | a read-only cosmofs mount writes its replay to the device |
| CFS-3 | a deleted newest snapshot's id is reused after a remount (V22) |
| PKG-1 | `pkg`'s database checks skip installed records past 128 silently |
| NET-6 | the SYN-cookie secret is drawn before the pool can seed |
| SEC-4 | the Linux signal trampoline is executable at a fixed address |
| BUILD-1 | the kernel builds without the stack protector |
| BUILD-2 | the release key is readable by any pushed workflow (repository settings, the owner's) |

About two to three days of work, or one unit of at most five PRs.

## A. Self-hosting software (deferred)

Not chosen for 1.2 (2026-10-20); kept as written for a later roadmap.

**Definition of done.** CosmoOS rebuilds BusyBox from source on itself,
on both architectures, with a C compiler and `make` that run on the
target, and the rebuilt BusyBox passes M3's scripted test and BusyBox's
testsuite; a ports tree builds packages on CosmoOS; and five real ports
(OpenSSH, Python, git, nano, curl) are built there and pass their own
test suites (the documented subset where a suite needs what CosmoOS
does not have, each exclusion named with its reason), all checked by CI.

### A1 A C compiler on the target

TinyCC (tcc 0.9.27 plus the `mob` fixes for AArch64) as a port, built by
the cross toolchain against musl, with musl's headers, `crt*.o`, `libc.a`
and `libc.so` installed under `/usr`; `cc` names it. tcc assembles and
links itself, so no binutils is needed. Clang is evaluated in the same
milestone and recorded: a static musl `clang`+`lld` cross-built on the
host runs under the Linux personality without new kernel work if its
memory and `mmap` needs fit, and is adopted only if the evaluation says
so.

- [ ] **Acceptance:** `make test-selfhost` (both architectures) compiles
  and runs, on the installed system, a static and a dynamic program and a
  `dlopen` one; tcc's own `tests/tests2` suite passes (exclusions
  listed); tcc rebuilds itself on the target and the rebuilt compiler
  builds the same `hello` byte for byte.

### A2 `make` and the build tools

GNU make 4.4, built on the target by its own `build.sh` with A1's
compiler; `patch`, `diff`, `cmp`, `awk`, `sed`, `tar`, `xz` (a
compressor: USR-5) as BusyBox applets or ports; `/usr/bin/env`,
`install`, `ar` (tcc's). Hard links (VFS-4) and a native `chmod`
(VFS-6), which build systems use, land here.

- [ ] **Acceptance:** on the installed system GNU make builds itself
  from its release tarball and the rebuilt `make` builds it again; then
  `make` and `cc` build BusyBox 1.37.0 from source with the checked-in
  `busybox.config`, and that BusyBox passes M3's scripted test and
  BusyBox's testsuite (`make test-selfhost`, both architectures). **This
  is the first half of the definition of done.**

### A3 A ports tree that builds on CosmoOS

`pkg build NAME` on the target: a port's recipe names its source tarball
by URL and SHA-256 (fetched with `cosmo-fetch`, checked before it is
unpacked), its patches, and its build and test commands; it builds in a
scratch directory as an unprivileged user and makes a `.cpk` signed with
a local key the operator trusts. Absorbs PKG-6 (versions and
provides/requires in the INDEX, so a port's library dependencies are
known), part of PKG-7 (the recipe pins its sources; an unprivileged build
user as the first sandbox) and USR-3 (`useradd`, for the build user).

- [ ] **Acceptance:** `hello`, `fortune` and a library port build and
  install on the target from their recipes; a tarball whose hash differs
  is refused before it is unpacked; a port whose dependency is missing is
  refused before its build; the build runs as a non-root user. For
  PKG-6: the INDEX carries each package's provided and required libraries
  with versions; `pkg install` of a program needing a library picks
  the package that provides it (of two versions, the one the requirement
  allows) and fetches nothing when no package provides it; an install
  whose requirement two packages provide picks by the stated rule.

### A4 nano and curl

ncurses and nano; curl over the mbedTLS already in the image. Their
suites: nano has none of its own (a scripted edit through a pty is the
acceptance); curl's `runtests.pl` needs Perl, so Perl 5 is ported here
first, with its own `make test` (the exclusions named). Absorbs LX-8
(`setsockopt`: curl and Perl's socket tests set `SO_REUSEADDR`,
`TCP_NODELAY` and timeouts) and LX-12 (the termios flags nano and
ncurses set).

- [ ] **Acceptance:** built on the target from their ports; Perl's `make
  test`, and curl's test suite against its own test servers, pass with
  the named exclusions; a scripted `nano` session edits and saves a file.

### A5 OpenSSH and git

Portable OpenSSH (with LibreSSL or `--without-openssl`, decided and
recorded), replacing nothing in 1.2 (dropbear stays the image's server);
git with `NO_PERL` off (Perl from A4). Absorbs LX-13 (nice values) and
LX-14 (`ITIMER_PROF`), which both suites touch.

- [ ] **Acceptance:** OpenSSH's `make tests` (the `t-exec` subset that
  needs no `sudo` and no second host, listed) and git's `t/` suite (the
  subset needing no network services, listed) pass on the target;
  `git clone` of a repository served by the harness over HTTPS works.

### A6 Python

CPython 3.13, built on the target. Absorbs LX-9 (`mremap` relocation,
which pymalloc's arena growth and `mmap` tests use), LX-10 (memfd
sealing, `test_os`), LX-11 (enough of `/proc` and `/sys` for the suite's
probes) and LX-23 (the vDSO, if the suite's timing tests need it).

- [ ] **Acceptance:** `python3 -m test` with a checked-in list of
  skipped tests (each with its reason) passes on the target, on both
  architectures. **With A2-A5 this completes the definition of done.**

**Backlog it would absorb.** PKG-6, PKG-7 (part), USR-3, USR-5, VFS-4,
VFS-6, LX-8, LX-9, LX-10, LX-11 (part), LX-12, LX-13, LX-14, LX-23, and
whatever correctness lines the five suites turn up.

**Risks.**
- The test suites are the size of the work, not the builds: Perl, curl,
  git and Python each run thousands of tests that exercise the Linux
  personality far more widely than BusyBox did. The skip lists are the
  control; each entry must say why, or the definition of done is hollow.
- tcc's AArch64 back end is less mature than its x86-64 one; BusyBox
  1.37 assumes GCC extensions in places. Patches may be needed in
  BusyBox, or the compiler may have to be clang (A1's evaluation).
- Build times under TCG: building BusyBox, Perl and Python on the
  target in CI may take tens of minutes a job on AArch64, with the
  timeouts that implies; a CI job may need to build once and test
  separately.
- Memory: the defaults (2 GiB) may not be enough for Python's build and
  suite.

**Size.** About 2-2.5x 1.1: six milestones, three of them (A4-A6)
porting-and-test-suite work whose length is set by how many Linux
personality gaps the suites find.

## B. Storage depth (chosen)

**Definition of done.** On the installed system, from the shell:
snapshots are taken, listed and rolled back to; a broken system is
rolled back and boots; `fsck` and `scrub` find and repair damage; a
mirror disk is removed, replaced and resilvered while the system runs;
`cosmo-upgrade` keeps a boot environment it can roll back to, packages
included; and the root can be encrypted. "Snapshot, break the system,
roll back, boot" and "pull a mirror disk, replace it, resilver" pass in
CI on both architectures.

### B1 Snapshots and rollback from the shell

cosmofs takes snapshots today through `mkdir .snapshots/NAME`; nothing
can roll back to one. `cosmo-snap create|list|delete|rollback` over
`/dev/fsctl` (version 3): a rollback names a snapshot that becomes the
live tree at the next mount (a pending-rollback record in the
superblock, so it is atomic and survives a crash), plus a writable clone
for inspection. Absorbs CFS-8 (rollback, clones), CFS-3 (snapshot ids
never reused, which rollback depends on).

- [x] **Acceptance:** `make test-snapshot`: on the installed system,
  snapshot the root, remove `/bin` and corrupt `/etc/rc`, roll back,
  reboot; the system boots and its files are the snapshot's; a crash
  between the rollback and the reboot (the harness kills QEMU) leaves
  either the old or the new tree, never a mixture, checked by
  `cosmofs_check`. A writable clone of a snapshot is mounted, changed,
  and its changes are seen neither in the snapshot nor in the live tree,
  nor the live tree's in it; deleting the clone frees its blocks (the
  checker's counts return to before). Done 2026-10-20, PRs #421 (format
  12, identity, `cosmo-snap`), #422 (rollback) and #423 (clones); report
  `docs/audit/2026-10-20-b1-snapshots-report.md`.

### B2 fsck and scrub as commands that repair

`fsck.cosmofs` (offline, on an unmounted pool or at boot before the root
mounts) and `fsctl scrub --repair` (online) run the checker's existing
four repairs and add the ones a mirror makes possible: a block whose copy
is bad on one member rewritten from the other. Absorbs CFS-1 (read-only
mounts write nothing), CFS-6 (failures that abandon a transaction),
CFS-11 (scrub's counts on `-EIO`), CFS-2 and part of TEST-16 (CF17's
paths, cosmofs chain cycles).

- [x] **Acceptance:** a host tool damages an installed image in each of
  the checker's finding classes; `fsck.cosmofs` at boot reports and
  repairs each repairable class and refuses the rest with the class
  named; on a two-way mirror with one member's blocks flipped, `scrub
  --repair` restores them and a second scrub is clean. Done 2026-10-22,
  `make test-fsck` (`tools/cosmofs-damage.py`, 23 damages; CI job
  `fsck`), PRs #433 (the defects CFS-1, CFS-2, CFS-6, CFS-11) and #434
  (fsck and scrub); report `docs/audit/2026-10-22-b2-fsck-report.md`.
  CFS-2's older-format image is not made by the tool (there is no host
  formatter); `cosmofs-version-compress` proves it.

### B3 Mirror replacement and resilvering

`cosmo-pool status|offline|replace|add-spare`; a member that missed
commits is resilvered from the others rather than left out; a spare
takes over a failed member. The test removes a disk while the system
runs, which needs PCI hot-removal and a partition scan on a disk that
appears later (DRV-6 part, BLK-5), and the blkctl check that sees every
member (CFS-5). Absorbs CFS-9, CFS-5, BLK-5, DRV-6 (PCI hot-removal and
rescan for virtio-blk).

- [ ] **Acceptance:** `make test-mirror`: install onto a two-disk
  mirror; while a writer runs, the harness removes one disk (QEMU
  `device_del`); the system keeps running degraded and says so; a new
  disk is added (`device_add`) and `cosmo-pool replace` resilvers it; a
  scrub is clean; the system reboots from the new pair. Then the same
  with a spare taking over by itself.

### B4 Boot environments with `cosmo-upgrade`

An upgrade snapshots the root first; `--rollback` returns the boot set
and that snapshot together, so the packages `pkg upgrade` changed and the
`/etc` files the new release added are returned too. Absorbs PKG-4 and
PKG-3 (a package transaction is a snapshot); it named PKG-5 too, which it
did not take (below).

- [x] **Acceptance:** `test-upgrade` gains: upgrade, `pkg upgrade` and a
  changed `/etc` file, `cosmo-upgrade --rollback`, reboot; packages,
  `/etc` and the base system are byte for byte the pre-upgrade
  snapshot's, the operator's files written after the upgrade kept in a
  named place; a crash at each step boots one consistent environment.
  For PKG-3, outside an upgrade: `pkg install a b` killed between the two
  packages leaves, at the next boot, both or neither installed; `pkg
  upgrade` interrupted the same way likewise; `pkg rollback NAME` returns
  one package to its previous version and its files. Done 2026-10-21, PRs
  #425 (package transactions) and #426 (boot environments), with #428
  (a cosmofs defect its acceptance test found) and #427 (main red on
  #425's merge); report
  `docs/audit/2026-10-20-b4-boot-environments-report.md`. PKG-5 is not
  absorbed: it needs two releases whose `cosmo-upgrade` differ to test,
  and stays in the backlog.

### B5 Encrypted root

cosmofs encryption exists; the root can use it: `cosmo-install
--encrypt`, the key unwrapped from a passphrase at boot (the console or a
serial line asks), the 252 MiB encrypted-file cap lifted (CFS-10) and
metadata authenticated with a keyed checksum (CFS-7), since an encrypted
pool with forgeable metadata is half a promise.

- [ ] **Acceptance:** `make test-encrypt`: install encrypted; the harness
  types the passphrase and the system boots; a wrong passphrase is
  refused and nothing mounts; the disk image contains none of a marker
  file's plaintext; for every metadata checksum domain (the block header
  `cfs_mhdr`, the superblock and its roots, the member labels) a block
  altered with its CRC32C recomputed is refused; a 1 GiB file is written
  and read back.

**Backlog it would absorb.** CFS-1, CFS-2, CFS-3, CFS-5, CFS-6, CFS-7,
CFS-8, CFS-9, CFS-10, CFS-11, PKG-3, PKG-4, PKG-5 (not taken by B4), BLK-5, DRV-6 (part),
TEST-16 (part). Left for later: CFS-12 (send/receive), CFS-13 (RAID-Z),
CFS-14 (deduplication).

**Risks.**
- Rollback of a mounted root is the hard case; B1 avoids it by rolling
  back at the next mount, which the definition of done allows. A live
  rollback would be its own unit.
- Hot removal of a virtio-blk under I/O reuses the device-lifecycle work
  (`virtio-remove-inflight` already removes one by test hook), but a real
  `device_del` arrives through PCIe hotplug signalling this kernel does
  not handle yet; that is new driver work inside B3.
- Encrypted root changes the boot path (a prompt before the root
  mounts) and the verified-boot story (the passphrase prompt is not
  measured); B5 must say what it does not protect.

**Size.** About 1-1.3x 1.1: five milestones, all on code that exists
(snapshots, mirrors, checker, encryption, the upgrade's switch), all
testable under QEMU in CI.

## C. Platform depth (deferred)

Not chosen for 1.2 (2026-10-20); kept as written for a later roadmap.

**Definition of done.** The VMX backend runs guests in CI (a nested-virt
or hardware-in-the-loop job); the kernel loads at a random address;
CPUs go offline and online at run time; and CosmoOS boots, installs and
uses its NIC and NVMe disk on one named physical machine per
architecture, both listed on the [Tested platforms](guide/platforms.md)
page with what works there.

### C1 VMX executed

A CI job on a runner with `/dev/kvm` boots CosmoOS x86-64 under KVM with
`-cpu host,+vmx` and runs the hypervisor's guest tests on the VMX
backend (today every boot runs them on the SVM backend, which QEMU's
TCG emulates; nothing has ever entered a guest through VMX). Fixes what
the first execution finds, then VIRT-2's gaps. Absorbs VIRT-1, VIRT-2,
VIRT-3 (two guest vCPUs) and MEM-7 (PCID, testable under KVM).

- [ ] **Acceptance:** a CI job `vmx` (x86-64) runs the hypervisor's guest
  self-tests on the VMX backend; the MOV-DR, RDPMC, WBINVD and RDTSCP
  exits, string I/O, the guest's TSC and the CPUID leaves it sees are each
  checked by a guest test; the virtio device models are driven by a guest
  with two vCPUs at once (VIRT-3); and with PCID enabled under KVM the
  TLB tests pass and a shootdown across address spaces is checked
  (MEM-7).

### C2 KASLR

A relocatable kernel (PIE with its relocations kept), the loader
choosing a slide from `EFI_RNG_PROTOCOL` or the CPU's instruction, the
slide in the boot protocol, `ksymtab`, the panic symboliser and the
harness adjusted. Absorbs BOOT-1, and SEC-4 and SEC-5 (the user-space
fixed addresses), which make the layout claim whole.

- [ ] **Acceptance:** `test-security` boots three times and requires
  three kernel bases (from a privileged read), a panic test still names
  its function, and the release images are still reproducible (the slide
  is chosen at boot, not build). For SEC-4 and SEC-5: every native
  program and the static BusyBox in the image are `ET_DYN` (a check over
  the image's files), two runs of each of `init`'s children and of
  BusyBox show different text addresses in `/proc/self/maps`, and so does
  a Linux process's signal trampoline.

### C3 CPU hotplug

`cpu_offline`/`cpu_online` through sysctl: migrate everything off,
quiesce, take the CPU out of every mask, park it (PSCI `CPU_OFF`, INIT on
x86-64), and bring it back. Absorbs SMP-3 and needs the quiescence and
balancer contracts to hold across it.

- [ ] **Acceptance:** `make test-hotplug` (both architectures, four
  CPUs): with `test-chaos`'s load running, CPUs 1-3 go offline and online
  a hundred times in random order; the suite passes; lockdep and the
  quiescence checks report nothing; a CPU offline holds no thread.

### C4 A physical x86-64 machine

One named machine the owner can buy and keep: a small Intel desktop
with an NVMe disk and a PCIe slot holding an Intel 82574L card, the only
NIC `e1000e.c` binds (`8086:10d3`, `docs/drivers/e1000e/api.md`), so no
new driver is needed to start. (A board's own I219-V would need that
driver extended first: its own unit.) The work is what QEMU never shows:
real firmware tables, interrupt routing, timer calibration, the CPU
feature and errata set (DRV-11), non-coherent assumptions (DRV-1 is
AArch64's, but its audit is shared).

- [ ] **Acceptance:** the machine boots the release image from USB,
  installs to its NVMe disk, boots that, gets an address by DHCP and
  accepts an ssh login; a new hardware harness (`make test-hw`: the
  machine's serial console and power controlled from the host, the image
  written to a USB stick or served over PXE, the same verdict rules as
  `run_boot_test.py`) boots the debug suite there and it passes, the
  QEMU-only tests skipped with a reason (TEST-15). `make test` cannot do
  this: it boots QEMU. The Tested platforms page names the model,
  firmware version and what works.

### C5 A physical AArch64 machine

One ACPI (SBSA-style) AArch64 machine with PCIe NVMe and a NIC this
kernel drives (an 82574L PCIe card if the board's own NIC is not): an Ampere Altra developer platform is the likeliest fit; a
Raspberry Pi 5 is cheaper but needs device-tree boot (DRV-5) and new
drivers, which would make this two milestones. Absorbs DRV-1
(non-coherent DMA syncs, if the machine is not coherent), DRV-9
(pseudo-NMI) and DRV-11.

- [ ] **Acceptance:** as C4, on that machine.

### C6 Hardware in the loop

A self-hosted runner attached to the two machines (power control and a
serial console, a network boot or a USB image writer) and a CI job that
boots main's images on each through C4's `make test-hw` and runs the
acceptance of C4 and C5. Absorbs TEST-12.

- [ ] **Acceptance:** CI's `hil` job passes on main for both machines
  and the Tested platforms page links its latest run. **With C1 this
  completes the definition of done.**

**Backlog it would absorb.** VIRT-1, VIRT-2, VIRT-3, MEM-7, BOOT-1,
SEC-4, SEC-5, SMP-3, DRV-1, DRV-9, DRV-11, TEST-12, TEST-15; DRV-5 if
the AArch64 machine needs device tree.

**Risks.**
- **Hardware and infrastructure are outside the repository.** Buying
  the machines, hosting a self-hosted runner and keeping it secure (a
  runner that executes pull requests' code) are the owner's decisions and
  costs; C4-C6 cannot start without them.
- **Nested VMX on CI may not exist.** GitHub's hosted runners expose
  `/dev/kvm`, but whether they let a KVM guest use VMX (nested
  virtualisation inside an already-virtualised runner) must be measured
  before C1 is planned; if not, C1 moves to the self-hosted runner of C6.
- Real hardware finds defects QEMU hides; the size of that list is
  unknown until the first boot, so C4 and C5 are the least predictable
  milestones of the three directions.
- AArch64 hardware choice: an affordable board likely needs device-tree
  boot and drivers this tree does not have.

**Size.** About 1.5-2x 1.1 for the work in the tree, plus an unbounded
wait on hardware and runner setup.

## Recommendation (as proposed; B was chosen)

B, storage depth. A and B are both testable under QEMU in CI, but B's
tests are boots of a few minutes where A's are builds and external test
suites that may take tens of minutes a job under TCG; C needs hardware.
B is built on code that already exists (snapshots, mirrors, the checker,
encryption, the upgrade's atomic switch), and it absorbs the largest
share of the backlog's open correctness lines (five of cosmofs's six)
while closing
the gaps users meet first in the
[limitations](guide/limitations.md#storage): no rollback from the running
system, an upgrade rollback that keeps changed packages, and a mirror
that never takes a member back. A is the more ambitious and the more
valuable to a user who wants to build software on CosmoOS, but its size
is set by how many Linux-personality gaps five external test suites find,
which nobody can estimate yet; it would go well after B, starting with
A1-A2 (a compiler and rebuilding BusyBox), which are small and
self-contained. C depends on hardware purchases and a self-hosted runner
that are outside the repository; its C1 (VMX under KVM) and C2 (KASLR)
could be taken as single units later without the rest.
