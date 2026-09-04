# CosmoOS

A new general-purpose, Unix-philosophy operating system built from scratch.

CosmoOS is a hybrid kernel with a small trusted core and modular privileged
services, a POSIX-oriented userland, capability-oriented security,
copy-on-write storage, an mbuf-based network stack, native virtualization,
and Linux ABI compatibility at the boundary. The project name is temporary.

## Governing document

Everything in this repository is governed by the master prompt in
[`prompts/`](prompts/). It defines the vision, the kernel architecture,
fifteen architectural invariants, coding rules, the development workflow,
and the phased roadmap. Read it before contributing.

## Priorities, in order

1. correctness
2. architectural cleanliness
3. observability
4. security
5. portability
6. performance
7. optimization

## Targets

- **x86-64** first, booted via UEFI under QEMU.
- **AArch64** designed for from day one, implemented in Phase 13.

## Development environment

The kernel is cross-compiled from an ARM64 Linux VM (Parallels on Apple
Silicon). QEMU is the deterministic kernel test platform. The kernel never
depends on Parallels-specific hardware and never runs natively on macOS.

## Repository layout

| Path | Owns |
|---|---|
| `boot/` | UEFI bootloader and boot protocol |
| `kernel/` | Trusted kernel core; `kernel/arch/` isolates all architecture-specific code |
| `drivers/` | PCI, VirtIO, NVMe, network, and storage drivers |
| `kernel-services/` | VFS, networking, storage, filesystems, virtualization |
| `libc/` | Native C library |
| `userland/` | init, shell, coreutils, system and network tools |
| `compat/linux/` | Linux process personality |
| `pkg/`, `ports/` | Package manager and declarative recipes (userland only) |
| `tools/`, `scripts/` | Host-side tooling and automation |
| `tests/` | Host, integration, QEMU, property, and fuzz tests |
| `docs/` | Subsystem documentation |
| `build/` | Build system definitions; output goes to git-ignored `out/` |

Each directory has a `README.md` stating its ownership boundary.

## Status

Phase 0 (build system and development environment) is in progress. See
section 70 of the master prompt for the first engineering task.
