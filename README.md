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

## Quick start

On an ARM64 or x86-64 Debian/Ubuntu host (the primary environment is an
ARM64 Ubuntu VM under Parallels):

```sh
scripts/setup-dev-linux.sh   # clang, lld, llvm, make, mtools, qemu, ovmf
make check-tools             # verify the cross toolchain
make                         # kernel ELF + UEFI loader (x86-64, debug)
make image                   # FAT boot image
make test                    # boot under QEMU, PASS/FAIL from serial + exit code
make run                     # interactive boot on the terminal
```

Other targets: `make BUILD=release`, `make analyze`, `make reproducible`,
`make test-crash`, `make host-test`, `make compile-commands`, `make help`.
See [docs/development.md](docs/development.md).

## Status

- **Phase 0/1 (done):** LLVM cross build, our own UEFI loader and boot
  protocol, x86-64 kernel entry with GDT/IDT, serial console, diagnostics,
  QEMU test harness, CI.
- **Phase 2 (done):** physical memory manager (zones, buddy allocator,
  page descriptors), kernel-owned page tables mapping all RAM with large
  pages, kernel virtual-address arena with guard pages and demand-zero
  faults, MMIO mapping, slab caches and `kmalloc`. Host unit tests under
  ASan/UBSan cover the buddy and slab algorithms.
- **Phase 3, part 1 (done):** ACPI table parsing, local APIC and I/O APIC
  with a generic IRQ API, TSC clock and LAPIC tick, one-shot timers,
  kernel threads with guarded stacks, a preemptive priority scheduler with
  a pluggable policy, wait queues, mutex, semaphore, completion, timed
  sleep, per-CPU data.
- **Phase 3, part 2 (done):** SMP bring-up through a real-mode
  trampoline, per-CPU tables and ticks, IPIs for reschedule, function
  call, TLB shootdown and halt, cross-CPU scheduling, a scheduler hang
  watchdog. Self-tests run on four CPUs under QEMU.
- **Phase 4 (done):** kernel objects and handle tables, processes with
  their own address spaces, an in-kernel static ELF loader, user mode via
  SYSCALL/SYSRET with SWAPGS, a personality-based syscall dispatcher with
  eleven native calls, validated user-memory access, fatal-fault
  termination, and an `init` program delivered by the loader. The user
  program's own self-test exercises every syscall.
- **Phase 5 (done):** kernel modules. A ustar boot archive (protocol v3)
  carries `init` and the modules; modules are signed `ET_REL` objects
  with a versioned metadata section, verified with an in-kernel SHA-512
  and Ed25519 implementation against a compiled-in key ring before any
  byte is parsed, laid out as three W^X regions in a near-kernel arena,
  relocated against the kernel's export table (`EXPORT_SYMBOL`, module
  ABI v1) and declared dependencies, reference counted for unload.
  Signature enforcement is on by default; RFC 8032 vectors and crafted
  ELF images run on the host, six self-tests load, call, and unload
  fixture modules on the target.
- **Phase 6 (done):** device infrastructure. A bus/device/driver model
  with resources and probing, a DMA API (no IOMMU yet, but no driver
  assumes virtual equals physical), PCI enumeration over ECAM with BAR
  sizing, capabilities and MSI/MSI-X through the interrupt layer, a
  block layer (`blkdev`/`bio` with synchronous helpers), an entropy pool
  and console sinks. VirtIO is the first real driver stack and lives
  entirely in boot modules: `virtio` (bus, split virtqueues, virtio-pci
  modern transport) plus `virtio_blk` (`vda`), `virtio_rng` and
  `virtio_console`. QEMU attaches a scratch disk, an RNG and a console
  whose output the boot test reads back; six self-tests cover the model,
  PCI, DMA, entropy, block I/O and the console sink (44 in total).
- **Phase 7 (done):** VFS and storage. A VFS with mounts, path
  resolution, vnodes and `struct file` kobjects (so the existing
  `read`/`write`/`close` work on files), a per-vnode page cache, ramfs
  as the root with `/boot` populated from the boot archive, a
  single-member storage pool, and cosmofs: a copy-on-write filesystem
  with two superblock slots, a two-level inode map, extent-mapped files,
  a bitmap allocator, CRC32C on every metadata block and a transaction
  model in which a committed root is never overwritten. Twelve new
  system calls (`open` … `umount`, numbers 11–22) and seven self-tests
  (51 in total) including crash consistency and a torn-superblock
  fallback; `init` mounts the disk from user mode.
- **Phase 8 (done):** networking. Reference-counted mbufs with
  clusters and explicit ownership, `struct netif` with one receive
  queue drained by the `netrx` worker thread (all protocol input on one
  thread, output on the caller), Ethernet and ARP, IPv4 with ICMP, IPv6
  with ICMPv6 and neighbour discovery, UDP, TCP (RFC 793 states, RTO
  estimation, fast retransmit, slow start, delayed ACK, TIME_WAIT,
  listen backlog), and sockets as kobjects behind nine new system calls
  (`socket` … `getsockname`, numbers 23–31) plus `read`/`write`/`close`.
  `virtio_net` is a boot module driving `eth0`; the loopback interface
  makes the protocol tests deterministic. Seven self-tests (58 in
  total) include a 1 MiB TCP transfer under injected loss, and the boot
  test drives the guest's echo services from the host through QEMU
  user-mode networking while the guest connects back; `init` exercises
  the socket calls from user mode.
- **Phase 9 (done):** userland. A native C library (`libc/`: errno,
  strings, an allocator over `mmap`, buffered stdio and `printf`,
  files and directories, `spawn`/`waitpid`/`kill`, sockets) and the
  first programs on it: `init` (runs `/etc/rc`, then the console
  shell), `sh` (quotes, `$VAR`, pipelines, redirections, `;` `&&` `||`,
  builtins), the coreutils (`echo cat ls cp mv rm mkdir rmdir pwd true
  false sleep`) and the system tools (`mount umount ps kill dmesg
  sysctl`), delivered in the boot archive as `/bin`, `/sbin`, `/etc`.
  The kernel gained what a shell needs: a console tty (line editing,
  echo, the UART receive interrupt), anonymous pipes as kobjects,
  `spawn` from an executable with an explicit handle map (no `fork`),
  zombies and `wait`, `kill` delivered at the system-call and
  return-to-user boundaries and in killable waits, a per-process working
  directory, `dup`, `fstat` on any I/O object, a kernel log ring and
  `sysctl` values (system calls 32–42). Three new self-tests (61 in
  total), a libc host test, the shell's own test script and an
  interactive harness that types at the `cosmo$ ` prompt through QEMU's
  serial port.
- **Phase 10 (done):** the package system, entirely in userland.
  Declarative recipes under `ports/` (`name`, `version`, `depends`,
  `program`, `file`), a host builder (`tools/pkgbuild.py`, `make ports`)
  that cross-compiles them into signed, checksummed, reproducible
  `.cpk` packages (a ustar archive with a `+MANIFEST`, then the module
  signature trailer) and a signed repository `INDEX`, and `/sbin/pkg`
  (`update install remove upgrade list info search verify`) with
  dependency resolution and version constraints, Ed25519 signature and
  SHA-512 checksum verification against `/etc/pkg/keys`, atomic
  per-file installation with rollback, and a text database under
  `/var/db/pkg`. The repository ships in the boot archive as
  `/boot/repo`; the shell test script installs, upgrades, refuses
  tampered fixtures and removes packages, and a host test covers the
  parsers.
- **Phase 11 (done):** Linux compatibility, stage 1 (constitution
  sections 38 to 40, invariant 7). A static x86-64 ELF without the
  CosmoOS ABI note (a `PT_NOTE` every native program now carries from
  `crt0.S`) runs under a Linux personality: its own 512-entry
  system-call table in `compat/linux/` translating 87 Linux calls onto
  the native services (files and directories with Linux `struct stat`
  and `linux_dirent64`, `brk` and anonymous `mmap`/`mprotect`, the
  thread pointer through `arch_prctl` and `MSR_FS_BASE`, a futex
  primitive in `kernel/ipc/`, signal tables that are stored but not
  delivered, `wait4`/`kill`, monotonic clocks, `uname` reporting
  `Linux`, IPv4/IPv6 sockets), a Linux initial stack with the auxiliary
  vector (`AT_PHDR`, `AT_RANDOM`, ...), and `-ENOSYS` with a per-process
  count for everything else; the native ABI is untouched. Tested by a
  freestanding raw-ABI program (`LINUXTEST: PASS`), a real statically
  linked musl program, a host test of the conversions and a kernel
  self-test (62 in total).
- **Phase 12 (done):** virtualization, stage 1 (constitution sections
  41 to 43, invariants 9 and 10). A VM manager in
  `kernel-services/virtualization/` behind the backend interface
  `arch/hv.h`, implemented on AMD-V with nested paging (`svm.c`,
  `svm_npt.c`, `svm_run.S`; VT-x is the next backend, chosen because
  the QEMU/TCG harness emulates SVM and not VMX). VMs and vCPUs are
  kobjects handed out by seven system calls (43 to 49) gated by
  `/dev/vmm`, the first device node; guest memory is zeroed host pages
  behind the nested table; the run loop emulates CPUID and MSRs,
  delivers injected vectors through the hardware, routes port I/O to
  device backends (a debug console on port 0xE9) or the owner, and
  reports halts, MMIO, hypercalls and shutdowns as `struct
  cosmo_vm_exit`. Real-mode and protected-mode guests run under TCG;
  `vmctl` drives one from the shell (`HVTEST: PASS`), eight kernel
  self-tests run six guest images (70 self-tests in total), and a host
  test covers the nested page tables.
- **Phase 13 (done):** AArch64, stage 1 (constitution sections 3, 4 and
  17, invariants 1 and 10). The same kernel, boot archive and boot-test
  harness on QEMU's `virt` machine with EDK2 firmware: `make
  ARCH=aarch64 test`. A second loader architecture directory
  (`boot/uefi/arch/aarch64/`: EL1, TTBR0/TTBR1 bootstrap tables with
  RAM and device attributes from the EFI map, `BOOTAA64.EFI`), boot
  protocol v4 (a second table root), and `kernel/arch/aarch64/`: the
  exception vector table, GICv2 with GICv2m MSI behind `arch/irqc.h`
  (GSI = INTID, 1312 vectors), the generic timer with absolute compares,
  a 4-level stage-1 MMU with the same virtual layout as x86-64 and
  hardware TLB broadcast, PSCI secondary bring-up, PL011, MMIO fw_cfg,
  `R_AARCH64` module relocations in a near arena within `CALL26` reach,
  semihosting exit. Two interface additions (`arch_dma_barrier`,
  `arch_mmu_near_arena`), the ELF machine and the MADT GIC entries made
  generic; the Linux table and the virtualization backend are documented
  stubs (`LINUXTEST: skipped`, `HVTEST: skipped`). CI runs both
  architectures. Documented in `docs/kernel/arch/aarch64/`.
- **Post-roadmap audit and critical-fix pass (done):**
  `docs/audit/2026-09-post-roadmap-audit.md` audited the whole tree and
  the fix pass closed its seven CRITICAL findings: TCP no longer takes
  the interface registry mutex under its spinlock (and every sleeping
  primitive now panics when entered under a spinlock); virtqueue chains
  are built and reclaimed from driver-private records, never from the
  device-writable descriptor table (host test `test_virtq` with a
  hostile peer); floating-point/SIMD state has an explicit owner per
  thread with eager switching and per-guest areas (`arch/fpu.h`,
  XSETBV intercepted); the LAPIC ICR pair is written with interrupts
  masked; NMI, `#MC`, `#DB` and `#DF` run on their own IST stacks with a
  paranoid entry that recovers the per-CPU block from the GS base MSR; a
  POSIX credential model with one privilege predicate gates every
  privileged call and the VFS enforces permissions (system calls 50-55,
  `init --unpriv-test`); and no private signing key is in the repository
  (per-machine developer keys, `scripts/check-secrets.sh`, the leaked key
  revoked). 75 self-tests, 12 host tests.
- **Kernel object lifetime and quiescence (done):** the second half of
  Prompt #3. An epoch-based grace-period mechanism
  (`kernel/core/quiesce.c`, `docs/kernel/quiesce/`): a read-side section
  is a preemption-disabled region, a CPU is quiescent only at interrupt
  return to a preemptible context, in `schedule()`, in the idle loop and
  at CPU bring-up; `synchronize_quiesce` waits with documented
  release/acquire/seq_cst ordering, `call_quiesce` defers. On it:
  `synchronize_irq`/`interrupt_unregister_sync` (handlers are read-side
  sections; the IRQ layer's release paths wait), `timer_cancel_sync`
  (spins on the running callback, defeats re-arming), a mandatory
  release for every kernel object with owner-module tracking, referenced
  lookups for devices, block devices and interfaces, a six-step
  `netif_unregister`, `blk_submit` refusing a removed device, the TCP
  accept race and the socket-wake reference closed, and a module unload
  protocol (GOING, shutdown, grace period, live objects, zombie). Module
  ABI v2. Nine new self-tests and a host model under ASan
  (`docs/kernel/quiesce/testing.md`).
- **Lock discipline and lockdep (done):** milestone 3 of the audit's
  plan. Debug builds run a lock-order checker on every spinlock and mutex
  acquisition (`kernel/core/lockdep.c`, `docs/kernel/lockdep/`): locks are
  classified by their initialisation name, held-lock stacks are kept per
  CPU (spinlocks) and per thread (mutexes), a dependency graph catches
  order inversions and un-annotated same-class nesting, interrupt-context
  locks may not be held with interrupts enabled, and `might_sleep()` in
  every sleeping primitive and user copy catches blocking under a
  spinlock. The panic report prints the held locks. The fixes it and the
  audit drove: the VFS rename lock order (a per-mount rename lock and
  ancestor-first parents), the vnode cache's check-then-get (unhash before
  the last drop, the mount hash lock a spinlock leaf), `vfs_sync` without
  the mount list held across a commit, the futex user copy outside the
  bucket lock (a wake sequence keeps the compare-and-enqueue atomic), and
  the AArch64 IPI send lock-free under the run-queue lock. Module ABI v3.
  Six new self-tests (five checker tests and a two-CPU VFS concurrency
  test) and a host test of the graph core.
- **Next:** the roadmap's numbered phases are complete. What follows are
  the milestones the constitution defers in section 68 (among them the
  USB stack, AHCI and the full NVMe feature set, containers, eBPF,
  graphics and a desktop, fuller Linux compatibility, NUMA, live
  migration, nested virtualization), plus the AArch64 follow-ups noted
  in `docs/kernel/arch/aarch64/design.md` (the Linux AArch64 table, an
  EL2 virtualization backend, GICv3, ASIDs, FP/SIMD in userland); design
  documents first, one subsystem at a time.
