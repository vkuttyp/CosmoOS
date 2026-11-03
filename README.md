# CosmoOS

A new general-purpose, Unix-philosophy operating system built from scratch.

CosmoOS is a hybrid kernel with a small trusted core and modular privileged
services, a POSIX-oriented userland, capability-oriented security,
copy-on-write storage, an mbuf-based network stack, native virtualization,
and Linux ABI compatibility at the boundary. The project name is temporary.

## Governing document

Everything in this repository is governed by the master prompt in
[`prompts/`](prompts/), extended by the later prompts kept beside it. It
defines the vision, the kernel architecture, fifteen architectural
invariants, coding rules, the development workflow, and the phased
roadmap. Read it before contributing.

Priorities, in order: correctness, architectural cleanliness,
observability, security, portability, performance, optimization.

## What exists

- **A usable system (the 1.0 roadmap, M1-M5):** an installer
  (`cosmo-install`) that puts the system on a GPT disk with a cosmofs
  root, which init mounts and switches to, packages included; Linux
  `fork`, `vfork` and `execve` with copy-on-write; static musl BusyBox
  1.37 `ash` as the shell with job control on the console and on
  pseudo-terminals (`/dev/ptmx`, `/dev/pts`); a DHCP client in the
  kernel and DNS for userland (BusyBox `wget` and `nslookup`, HTTP
  only); ssh login by key with static dropbear; a verified boot chain
  (the UEFI loader checks an Ed25519-signed manifest over the kernel,
  boot archive and command line); user-space ASLR; operation-specific
  privileges instead of an all-powerful uid 0; and sound randomness
  (`getrandom` that blocks until the pool is seeded from RDSEED/RDRAND,
  RNDR or virtio-rng).
- **Kernel core:** SMP scheduling with per-CPU run queues, migration, a
  pull load balancer and priority inheritance; buddy and slab memory,
  demand paging and file-backed mappings; reference-counted kernel objects
  with per-handle rights; epoch-based quiescence for object lifetime;
  lockdep in debug builds; signed, unloadable kernel modules.
- **Storage:** a VFS with mount namespaces and a page cache, and cosmofs, a
  copy-on-write filesystem with snapshots, pools, mirrors, compression,
  encryption, crash replay, `fsck` and an operator interface.
- **Devices:** PCI with MSI/MSI-X, VirtIO (block, network, console, RNG),
  NVMe, AHCI, an Intel e1000e NIC, USB (xHCI, mass storage, hubs, HID
  keyboard), DMA remapping (Intel VT-d, ARM SMMUv3) and a framebuffer console.
- **Networking:** IPv4 and link-local IPv6, TCP and UDP with per-connection
  locking, per-CPU receive, a firewall, masquerade NAT and port-forward
  DNAT, DHCP and DNS proxying, and tap interfaces for guests.
- **Userland:** a native libc, a shell, coreutils, system tools, a service
  manager, `/proc`, and a package system built from declarative ports.
- **Linux personality:** x86-64 and AArch64 system-call tables running
  unmodified static and PIE binaries: threads, signals, `poll`/`epoll`,
  `eventfd`/`timerfd`/`signalfd`, `memfd`, System V shared memory and
  Unix-domain sockets.
- **Virtualization:** `/dev/vmm` with an AMD SVM backend, an Intel VMX
  backend that has not yet run on hardware, and an AArch64 EL2 hypervisor
  that boots Linux guests with VirtIO block and network devices.
- **Verification:** over four hundred in-kernel self-tests on both
  architectures, host unit tests under ASan/UBSan (plus TSan models for
  lockdep and interrupt dispatch), fuzzers, fault injection and filesystem
  crash-consistency replay. CI boots both architectures and runs the host
  tests and fuzzers.

1.0 is the [1.0 roadmap](docs/roadmap-1.0.md) completed; the
[1.1 roadmap](docs/roadmap-1.1.md) is complete too. The work now is the
[2.0 roadmap](docs/roadmap-2.0.md): a memory-safe server operating system
with its kernel converted to a memory-safe language; open defects and gaps are in
[`docs/backlog.md`](docs/backlog.md).

## Targets

- **x86-64**, booted via UEFI under QEMU's `q35` machine.
- **AArch64**, booted via UEFI under QEMU's `virt` machine.

QEMU is the deterministic test platform. Development happens on an ARM64
Linux VM (Parallels on Apple Silicon) or any Debian/Ubuntu host; the kernel
never depends on the host's hardware.

## Repository layout

| Path | Owns |
|---|---|
| `boot/` | UEFI bootloader and boot protocol |
| `kernel/` | Trusted kernel core; `kernel/arch/` isolates all architecture-specific code |
| `drivers/` | PCI, VirtIO, NVMe, network, storage, USB and IOMMU drivers |
| `kernel-services/` | VFS, networking, storage, filesystems, virtualization |
| `modules/` | Loadable kernel modules packed into the boot archive |
| `compat/linux/` | Linux process personality |
| `libc/` | Native C library |
| `userland/` | init, shell, coreutils, system and network tools |
| `pkg/`, `ports/` | Package manager and declarative recipes (userland only) |
| `tools/`, `scripts/` | Host-side tooling and automation |
| `tests/` | Host, integration, QEMU, property, and fuzz tests |
| `build/` | Build system definitions; output goes to git-ignored `out/` |
| `docs/` | Subsystem documentation, audits, history and the plan |
| `prompts/` | The governing prompts |

Each source directory has a `README.md` stating its ownership boundary.

## Quick start

To run the release images, follow the [user guide](docs/guide/README.md).
To build from source, on an ARM64 or x86-64 Debian/Ubuntu host:

```sh
scripts/setup-dev-linux.sh   # clang/lld/llvm, make, mtools, QEMU + UEFI firmware for both arches
make check-tools             # verify the cross toolchain
make                         # build everything (x86-64, debug)
make test                    # boot under QEMU, PASS/FAIL from serial + exit code
make run                     # interactive boot on the terminal
```

Pass `ARCH=aarch64` for AArch64 and `BUILD=release` for a release build;
`make help` lists every target. See
[docs/development.md](docs/development.md).

## Documentation

| Where | What |
|---|---|
| [`docs/README.md`](docs/README.md) | Index of subsystem documentation and the per-subsystem convention |
| [`docs/development.md`](docs/development.md) | Host setup, building, running, testing, CI |
| [`docs/history/`](docs/history/README.md) | What was built, phase by phase and unit by unit |
| [`docs/guide/`](docs/guide/README.md) | The user guide: installing, the shell, networking, ssh, packages, privileges, known limitations |
| [`docs/roadmap-1.0.md`](docs/roadmap-1.0.md) | The 1.0 definition of done and its milestones |
| [`docs/roadmap-1.1.md`](docs/roadmap-1.1.md) | What merged after 1.0 and the 1.1 units (complete) |
| [`docs/roadmap-1.2.md`](docs/roadmap-1.2.md) | Storage depth (B, complete) and self-hosting (A, folded into 2.0's SW0); superseded |
| [`docs/roadmap-2.0.md`](docs/roadmap-2.0.md) | The current roadmap: a memory-safe server OS, its tracks, milestones and acceptance tests |
| [`docs/backlog.md`](docs/backlog.md) | Open defects and gaps, by subsystem |
| [`docs/plan.md`](docs/plan.md) | Work after 1.0, grouped by area (historical record) |
| [`docs/audit/`](docs/audit/README.md) | Architecture audits, the deferred-work inventory and per-unit reports |

## Status

**CosmoOS 1.0** is the [1.0 roadmap](docs/roadmap-1.0.md) completed: it
installs to a disk, reboots into it, is used through BusyBox `ash`, keeps
files across reboots, reaches the network by name, accepts an ssh login,
and has a verified boot chain and sound randomness, on x86-64 and
AArch64 under QEMU, all checked by CI. The release images are signed and
rebuild bit for bit from their tag (`make release`); each release on
the [releases page](https://github.com/vkuttyp/CosmoOS/releases) carries
them with their `SHA256SUMS` and the release key (latest: v1.0.14, notes
in [`docs/releases/`](docs/releases/)). Since 1.0 the 1.1 roadmap and
the 1.2 roadmap's storage direction (snapshots and rollback, `fsck` and
scrub that repair, mirror resilvering, boot environments, encrypted
root) have shipped; 1.2's self-hosting direction is under way, with a C
compiler on the image (`cc`). Start with the
[user guide](docs/guide/README.md); what was built, milestone by
milestone, is in [`docs/history/`](docs/history/README.md) and the
milestone reports in [`docs/audit/`](docs/audit/README.md).

## Not in 1.0

QEMU is the tested platform (since v1.0.1 the AArch64 image also boots
to the live shell on Parallels Desktop on Apple Silicon, and since
v1.0.3 installs and networks there: [Tested platforms](docs/guide/platforms.md));
there is no physical hardware matrix,
no GPU or display beyond a text console (on the firmware's
framebuffer or a 2D virtio-gpu), no CPU or memory hotplug (PCIe disks
can be added and removed since v1.0.11), no NUMA, no
huge pages, no live migration, no RAID-Z, no eBPF, no more than 64 CPUs,
and the Intel VMX backend has not run on hardware. There is no KASLR. The guide's
[Known limitations](docs/guide/limitations.md) page lists what a user
will meet; [`docs/plan.md`](docs/plan.md) is the work that remains after
1.0, by area.

## License

Copyright (C) 2026 Veeran Puthumkara.

CosmoOS is free software, licensed under the
[GNU General Public License, version 2 only](COPYING) (`GPL-2.0-only`).

Third-party software that CosmoOS builds from `ports/` keeps its own
license; each port's recipe fetches the upstream source. When an image
includes mbedTLS (licensed `Apache-2.0 OR GPL-2.0-or-later`), it is used
under its GPL option, since Apache-2.0 alone is not compatible with
GPLv2.
