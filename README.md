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

The path to 1.0 is the [1.0 roadmap](docs/roadmap-1.0.md); what else
remains open, and why, is in [`docs/plan.md`](docs/plan.md).

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

On an ARM64 or x86-64 Debian/Ubuntu host:

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
| [`docs/roadmap-1.0.md`](docs/roadmap-1.0.md) | The 1.0 definition of done and its milestones |
| [`docs/plan.md`](docs/plan.md) | Remaining work, grouped by area |
| [`docs/audit/`](docs/audit/README.md) | Architecture audits, the deferred-work inventory and per-unit reports |

## Status

The constitution's roadmap (Phases 0-13) and the post-roadmap audit's
milestones are complete, followed by subsystem units chosen one at a
time from the [deferred-work inventory](docs/audit/2026-09-deferred-work-inventory.md).
The latest milestone is lock discipline and lockdep hardening
(PRs #302-#308). Work now proceeds milestone by milestone along the
[1.0 roadmap](docs/roadmap-1.0.md), design documents first.
