# CosmoOS 1.3 roadmap: a release people can install (proposed)

Written 2026-11-11 on main at `5a36f9c1`, at the owner's request to
re-evaluate the [2.0 roadmap](roadmap-2.0.md). **Proposed, not adopted.**
If adopted it replaces 2.0's *order*; 2.0's milestone texts stay as the
record of the longer work, and [`AGENTS.md`](../AGENTS.md#scope-rules) is
rewritten to match in the adopting PR.

## Why a new order

Facts on 2026-11-11:

- The project began 2026-09-04. v1.0.0 was released 2026-10-12 and
  v1.0.14 on 2026-10-26. Since v1.0.14 main has 505 commits, nearly all
  of them the 2.0 SA track (the kernel's conversion to CeeSafe), and no
  release.
- `scripts/cee-share.py` on main: ring 0 is 49.5% CeeSafe (124,440 lines;
  125,103 lines of C left). That took 13 days.
- 2.0 is sized at 83.5x: ten to thirteen months in three streams. Only
  the conversion stream runs. Its first release, 2.0-alpha, is after
  thirteen units of work, and no SA milestone changes what a user of the
  system gets.
- A release today is a raw disk image per architecture, proven on QEMU
  and one Parallels VM. It has BusyBox, dropbear and tcc. It has never
  run on another hypervisor, a cloud or a physical machine, and there is
  no page that tells a stranger how to install it.
- CeeSafe released 0.8.0 to 0.31.0 in ten days. CosmoOS pins 0.24.2.

So the kernel is getting safer and nobody can use it. This roadmap puts
the release first, bounds the conversion, and asks of CeeSafe only what
makes CosmoOS's work faster.

## The goal

> **CosmoOS 1.3** is a server operating system a person outside the
> project can download, install in a virtual machine or on a KVM cloud,
> log into over SSH, install software on, and keep upgraded, with its
> ring-0 code in CeeSafe down to a named floor.

"Distributable" is these bars, each checked by a CI job or a dated
report:

| Area | Today | Bar for 1.3 | Checked by |
|---|---|---|---|
| Images | raw `.img`, both architectures | also qcow2, VMDK and an installer ISO | R1's CI job |
| Where it runs | QEMU, Parallels | also VMware, VirtualBox, UTM, one KVM cloud | R1's and R4's dated reports |
| First boot | a console login | SSH key, hostname and network from a seed; root grows to the disk | `make test-cloud` (R2) |
| Software | BusyBox, dropbear, tcc | also OpenSSH, curl, git, nano as packages | `make test-ports` (R3) |
| Upgrades | `cosmo-upgrade`, `pkg` | unchanged, proven from v1.0.14 to every 1.x release | `test-upgrade` |
| Kernel | 49.5% of ring 0 in CeeSafe | ring 0 in CeeSafe except a named floor; no conversion after the freeze | `cee-share.py`, K3's soak |
| Documentation | for contributors | an install guide per platform, a download page, known limits | R5 |

**Not in 1.3** (each stays where 2.0 describes it): physical machines,
benchmarks against Linux and FreeBSD, glibc, containers, the messaging
track, building ports on the target (1.2's A2, A3 and A6), and converting
userland (SA12, SA13). The first of these after 1.3 is one physical
x86-64 machine (1.2's C4).

## How the work runs

- **Two streams and a side.** The release stream (R) and the kernel
  stream (K) run at once and never change the same subsystem in the same
  week. CeeSafe's work (C) is in CeeSafe's repository; the test work (T)
  is small and goes first.
- **Pull requests again.** Every change reaches main through a PR with CI
  green, including conversions. A conversion PR may hold several ports;
  it runs the validation matrix once.
- **A release at least every two weeks,** from main, by the rc and VM
  rule in `AGENTS.md`. A red main stops both streams.
- **A conversion changes no behaviour.** A defect found while porting is
  a backlog line, not a change in the port.
- **New kernel code is CeeSafe.** 2.0's CeeSafe rule stands for what R
  needs written. Nothing else new is written in the kernel before 1.3.

## Order

```text
week 0     T1, T2 ─► R0 (v1.1.0)          K1 (finish SA8)        C1 starts
weeks 1-2  R1 (images) ─► R2 (first boot)  K2 (ring 0, boxed)     C1 ─► C2 ─► C3
weeks 3-4  R3 (packages) ─► R4 (cloud)     K2 ends ─► K3 (soak)   C4
weeks 5-6  R5 (documentation) ─► R6 (v1.3.0)
```

Six to eight weeks is an estimate, not a measurement. R3 carries the most
uncertainty: OpenSSH and git have never been built for CosmoOS.

---

## T. Faster checks (first, because every milestone pays for them)

One conversion section costs about 40 minutes of boot tests on the
owner's Mac, and main's CI takes 60 to 120 minutes.

### T1 Order and load

`make analyze` on both architectures raises the Mac's load average to
about 100, and boot tests started straight after it fail their timing
bounds (seen 2026-11-11: `process-user` at 22.9 s against 15 s). The
matrix runs its boots first, or waits for the load to fall.

- [ ] **Acceptance:** `docs/development.md` states the order; a
  `make matrix` target runs it that way and prints one verdict per step.

### T2 A faster place to boot

Two candidates, each measured before it is chosen: the Debian host under
KVM for x86-64, and QEMU's `hvf` accelerator on the Mac for AArch64.

- [ ] **Acceptance:** a dated report with the debug boot's time under TCG,
  KVM and HVF, which self-tests fail under each and why, and the choice.

Size: about 0.2x for both.

---

## R. The release stream

### R0 v1.1.0 from today's main

The half-converted kernel has run only in test boots. Release it.

- [ ] **Acceptance:** `test-upgrade` takes an installed v1.0.14 to the rc;
  the rc passes the VM rule; v1.1.0 is tagged on that commit.

### R1 Images for the hypervisors people have

`make dist` also writes qcow2 and VMDK images and a UEFI installer ISO
that runs `cosmo-install` onto the machine's disk.

- [ ] **Acceptance:** a CI job boots the qcow2 image, and boots the ISO,
  installs from it and boots the result, on both architectures. A dated
  report records the release booting on VMware, VirtualBox and UTM, with
  each device that platform presents and whether a driver claimed it.

### R2 First boot without a console

A seed disk (cloud-init's NoCloud layout: `meta-data` and `user-data`,
the subset named in the design note) gives the SSH key, the host name
and the network. The root partition and file system grow to the disk.

- [ ] **Acceptance:** `make test-cloud` (both architectures): the image is
  copied to a disk four times its size and booted with a seed; the
  harness logs in over SSH with the seed's key and sees the host name and
  a root file system of the disk's size. Without a seed the boot is as
  today.

### R3 The software a server needs, as packages

OpenSSH, curl, git and nano, cross-built on the host as BusyBox and
dropbear are, published in the package repository. This is not 1.2's A
track: nothing here is built on the target.

- [ ] **Acceptance:** `make test-ports` installs each with `pkg install`
  on an installed system and runs it: `sshd` accepts a key login and
  `scp` copies a file both ways, `curl` fetches over HTTPS, `git` clones
  over HTTPS and commits, `nano` edits a file under the harness's
  terminal. Each Linux call a port needed and CosmoOS lacked is listed
  with its PR.

### R4 One KVM cloud

The image runs on one provider that takes a custom image and presents
virtio devices. The owner chooses the provider and holds the account.

- [ ] **Acceptance:** a dated report: the image uploaded, booted with the
  provider's seed, logged into over SSH from the internet, upgraded with
  `cosmo-upgrade`. The release rule in `AGENTS.md` gains this machine
  beside the Parallels VM.

### R5 Documentation for a stranger

An install guide per platform (R1's and R4's), a download page with the
checksums and how to verify the signature, a first-hour guide (log in,
add a user, install a package, upgrade, roll back), and the known limits.

- [ ] **Acceptance:** the owner installs from the guide alone on a
  machine that has never held the tree; every command in the guides is
  run by a CI job against the release image.

### R6 v1.3.0

- [ ] **Acceptance:** every box above ticked; K3's soak passed on the
  release candidate; v1.3.0 tagged by the release rule, with R4's cloud
  machine in it.

Sizes: R0 0.1x, R1 0.6x, R2 0.6x, R3 1.5x, R4 0.3x, R5 0.4x.

---

## K. The kernel stream: finish, bound, prove

### K1 Finish SA8

`kernel/syscall/` is done on 2026-11-11 except the two tables, which only
C can write today (C4 below). `compat/linux/syscalls.c` has about 4,200
lines left, in sections.

- [ ] **Acceptance:** 2.0's SA8 acceptance, its box ticked.

### K2 Ring 0, in a box of three weeks

2.0's SA6 remainder, SA9 (drivers), SA10 (the core) and SA11
(architecture code and the hypervisor), in that order, for three weeks
from adoption. When the box closes, what is still C is written into the
floor list with "not yet" beside it, and conversion stops until 1.3 is
released.

- [ ] **Acceptance:** `cee-share.py`'s report in the closing PR; the floor
  list names every C file left in ring 0 and why.

### K3 Prove what was converted

By 1.3 most of ring 0 will have been rewritten in two months. The
self-tests cover what they covered before; this adds time.

- [ ] **Acceptance:** `make test-soak`: an installed system on each
  architecture runs BusyBox's test suite, the network harness and a
  package install and removal in a loop for 72 hours with lockdep on, and
  ends with no panic, no lockdep report and its object census unchanged.

Sizes: K1 0.3x, K2 boxed at 2.0x, K3 0.4x.

---

## C. CeeSafe: only what makes CosmoOS faster

Each is a change in CeeSafe's repository, ordered by what it saves. The
evidence is the SA8 ports of 2026-11-11.

| | CosmoOS pays today | Wanted | Proof here |
|---|---|---|---|
| C1 | Each port copies its C structs, constants and prototypes by hand: about 150 of `sockets.cee`'s 640 lines | A generator of the `extern "C"` block from the kernel's headers | `sockets.cee`'s block is generated and the hand-written one deleted |
| C2 | A local of an imported C struct is refused (N0294), so each is a word array and a layout check | Locals of imported structs, their size taken from the C compiler | `sockets.cee`'s `AnyAddr` holds a `netaddr` and a `unix_addr` |
| C3 | Each kernel `.cee` compiles alone, so helpers repeat: the socket address code is in `sockets.cee` and `lxsock.cee` | CeeSafe-typed calls between kernel files, as the driver build already has | one shared file for the system-call helpers |
| C4 | A table of C functions cannot be written, so `native.c` and the Linux table stay C | Static arrays of C function pointers | both tables in CeeSafe; `native.c` deleted |
| C5 | Moving the pin costs a matrix and the analysis baselines; CeeSafe makes several releases a day | CeeSafe's CI builds CosmoOS's tree at head; CosmoOS moves its pin at most weekly | a pin bump whose `out/cee` difference is version lines only |
| C6 | Unknown: `c2cee`'s drafts were not used for SA8 | Measure it on SA9's first driver: lines of the draft kept unchanged | a dated report |

K2 does not wait for these. C1 comes first because every port still to
write pays for it.

---

## Risks

- **The estimate.** Six to eight weeks assumes R3's ports need few new
  Linux calls. Each missing call is found only by building the port.
- **Hypervisors nobody has booted.** VMware, VirtualBox and UTM may
  present devices no driver claims. R1's report is written before R1's
  code, so the drivers needed are known first.
- **A rewritten kernel in users' hands.** K3's soak and a release every
  two weeks are the guard; a defect in converted code is fixed in the
  CeeSafe, never by restoring the C.
- **Stopping the conversion half way.** A kernel partly C is what 1.0
  was. The floor list says what is left, and 2.0's SA track resumes after
  1.3 where K2 stopped.
- **One person's hardware.** Every boot runs on the owner's Mac or in CI.
  T2 and R4 each add a machine and its cost.

## Decisions for the owner

1. **The definition.** This roadmap takes "distributable" to mean virtual
   machines and one KVM cloud, with a physical machine first after 1.3.
2. **The cloud** for R4.
3. **K2's box:** three weeks, or stop the conversion at SA8.
4. **Python** (1.2's A6): left out of 1.3 here.
