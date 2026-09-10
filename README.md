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
- **Verification infrastructure (done):** milestone 4 of the audit's
  plan, closing its finding that the tree had no fuzzing, fault injection
  or crash-consistency tests (`docs/verification/`). On the host, `make
  fuzz` runs six libFuzzer-compatible targets over the real parsers under
  ASan and UBSan (module ELF, user ELF, package manifest/index/tar, Linux
  ABI conversions, the split virtqueue against a hostile device, cosmofs
  images through mount and walk) with a portable seeded driver, or
  libFuzzer with `FUZZ_ENGINE=libfuzzer`. In debug kernels, fault
  injection fails `kmalloc` and block submissions or completions per
  thread on a schedule and budget (`kernel/core/faultinject.c`, a boot
  parameter, read through `debug.faultinject`); a RAM block device
  records its write stream (`kernel/block/ramblk.c`) and the
  `cosmofs-replay` self-test mounts and checks every prefix of it, intact
  and torn; `init --syscall-fuzz` makes 20 000 random system calls from an
  unprivileged process; every self-test reports its duration and the boot
  harness fails one over budget. Four new self-tests. The first fuzz run
  found an unaligned section-table read in the module loader; fault
  injection found a NULL dereference on a failed vnode allocation; both
  fixed with regression tests.
- **User-access fixups and user VMM regions (done):** milestone 5 of
  the audit's plan (`docs/kernel/memory/design.md` §6). Kernel access to
  user memory goes through one copy primitive per architecture whose
  faulting instructions are listed in an exception table
  (`kernel/arch/*/uaccess.S`, `kernel/core/extable.c`): a kernel-mode
  fault at a user address, including a demand-zero fault that finds no
  memory, resumes at the fixup and the system call returns `-EFAULT`; the
  region walk before every copy, and with it the check-then-copy window,
  is gone. `PROT_NONE` pages keep their frame and trap every access.
  `munmap` and `mprotect` take any page range: regions split at the ends
  and merge with equal neighbours, so the Linux heap shrinks and regrows,
  `MAP_FIXED` replaces, and a partial `mprotect` works. Every user space
  tracks the CPUs running it and shoots down only those; process exit
  no longer interrupts every CPU. `vmm_init` pre-populates the arena's
  top-level tables and debug builds assert that no kernel-half entry is
  created after the first user space. Five new self-tests, 100 in total;
  the Linux and native user tests cover the new semantics.
- **Access control and resource limits (done):** milestone 6 of the
  audit's plan (`docs/kernel/security/design.md`). The decision the audit
  asked for is recorded: privilege flows down. There are no setuid
  executables; a process is privileged only by inheritance, and `spawn`
  with `COSMO_SPAWN_SETCRED` is the transition primitive (any identity
  from root, only held identities from anyone else, no supplementary
  groups). Per-process resource limits (`getrlimit`/`setrlimit`, Linux
  `prlimit64`): address space, resident memory, handles, processes per
  user, guest memory per VM, inherited at spawn, lowered freely, raised
  only with privilege, enforced in the VMM, the handle table, process
  creation and the hypervisor. The ramfs page budget is real (the page
  cache counts pages per mount and refuses with `ENOSPC`) and the page
  cache as a whole has a limit with LRU reclaim of clean pages, so a
  large read no longer pins RAM; writeback runs in page order.
  `procinfo` shows an unprivileged user its own processes and `log` is
  rate limited. Five new self-tests, 105 in total.
- **Filesystem transaction engine (done):** milestone 7 of the audit's
  plan (`docs/kernel-services/filesystem/cosmofs/design.md`, "Format
  version 2 and the transaction engine"). cosmofs format version 2:
  extents carry their logical position, so holes cost nothing and a
  sparse write no longer fills the disk; extent blocks chain, so the
  264-run cap is gone; every data and directory block has a CRC32C in a
  per-inode checksum tree, verified on read. The allocator is
  contiguity-aware and keeps a metadata reserve, so a full disk can still
  delete and commit. `fsync` commits the transaction; a writeback thread
  commits on dirty and age thresholds, bounding the loss window; mount
  falls back to the older superblock slot when the newer root's tree
  does not load. The block layer gained `BIO_PREFLUSH`/`BIO_FUA` and a
  pending queue, so a driver's queue-full answer never reaches a
  filesystem; virtio-blk no longer sends unsupported flushes. Eight new
  self-tests, 113 in total.
- **Network stack hardening and per-connection locking (done):**
  milestone 8 of the audit's plan
  (`docs/kernel-services/network/design.md`, "Hardening and
  per-connection locking"). TCP pcbs are reference counted with one
  spinlock each and a hashed table, replacing the single TCP lock; a
  listener answers SYNs from a SYN cache or with SYN cookies and
  allocates a connection only for the completing ACK, so a SYN flood
  costs it nothing and locks nobody out; blind resets, SYNs and
  out-of-range ACKs earn RFC 5961 challenge ACKs; keepalive probes end
  dead connections and an orphaned FIN_WAIT_2 times out; out-of-order
  segments are reassembled instead of dropped; ICMP replies are rate
  limited and quote exactly the received header; "fragmentation
  needed" messages drive path MTU discovery. Every I/O object gained a
  readiness operation and a non-blocking mode (`ioready` 58,
  `setnonblock` 59, `COSMO_SOCK_NONBLOCK`; Linux `SOCK_NONBLOCK`,
  `accept4`, `pipe2(O_NONBLOCK)`, `fcntl(O_NONBLOCK)`), the piece
  `poll` and asynchronous I/O will build on. Six new self-tests, 119
  in total.
- **Async I/O and the block layer for NVMe (done):** milestone 9 of the
  audit's plan (`docs/kernel/io/design.md`, `docs/drivers/nvme/design.md`,
  `docs/kernel/device/design.md` "The block layer for NVMe"). An
  asynchronous I/O ring (`aio_create`/`aio_submit`/`aio_wait`, calls
  60–62): entries execute in the submitting process when their object is
  ready and park otherwise, driven by the readiness operation and a new
  `poll_wq` operation on every I/O object; completions are collected in
  batches with `min` and a timeout. The block layer gained multi-segment
  bios, an in-flight list with request timeouts and a driver `timeout`
  operation, exact-name registration and completion-locality counters;
  every DMA mapping now has its unmap and 64-bit devices declare their
  mask (finding #27). A new `nvme` driver module brings up the
  controller, creates one I/O queue per CPU with its MSI-X vector routed
  to that CPU, builds PRP lists from segments, aborts and resets on
  timeout, and registers each namespace as `nvme0n1`; QEMU attaches one
  on both machines and cosmofs mounts on it. Three new self-tests, 122
  in total.
- **Linux personality stage 2: signals, threads, PIE, poll, the wall
  clock (done):** milestone 10 of the audit's plan
  (`docs/kernel/process/design.md` §11, `docs/compat/linux/design.md`
  "Stage 2", `docs/kernel/io/design.md` "Polling"). A signal core the
  two personalities share (`kernel/process/signal.c`): per-thread
  pending and blocked sets, per-process actions, delivery at the two
  returns to user mode, `SA_RESTART`, faults as signals with `siginfo`;
  the Linux `rt_sigframe` on both architectures, `rt_sigreturn`, the
  alternate stack, `kill`/`tgkill`/`tkill`/`rt_sigsuspend`/`pause`. The
  SYSRET canonical guard (audit §14.2): a full-restore `iretq` exit for
  any frame `SYSRET` could fault on, sanitised register sets, and every
  user-mode CPU exception a signal rather than a panic. Several threads
  per process: `clone` with the pthread flag set, `CHILD_CLEARTID`
  joins, `exit` versus `exit_group`, futex requeue and bitset waits with
  `CLOCK_REALTIME`. `ET_DYN` executables at `USER_PIE_BASE` and
  `PT_INTERP` interpreters started first with the full auxiliary vector;
  private file-backed `mmap`. `io_poll` behind `poll`/`ppoll`. A wall
  clock seeded from the RTC on both machines (`clock_ns(REALTIME)`,
  Linux `CLOCK_REALTIME`, `gettimeofday`, `time`). The Linux system-call
  numbers split into `nr_x86_64.h`/`nr_aarch64.h` and the personality,
  its test programs and the boot harness now run on AArch64 too. New
  Linux fixtures `lxsig` (seven expected deaths, including the guard)
  and the PIE pair `lxinterp`/`lxdyn`; two new self-tests, 124 in total.
- **Network receive scaling and checksum offload (done):** the unit the
  audit names after its ten milestones
  (`docs/kernel-services/network/design.md`, "Receive scaling and
  offloads"). One receive queue and one pinned worker per CPU; a
  received packet is steered to a queue by its flow hash, so one flow
  is processed in order by one thread and different flows in parallel;
  timers hand work to the calling CPU's worker; unregister barriers
  every worker. The mbuf gains the headroom every transmit chain needs
  (128), a flow hash, and defined checksum-offload fields; TCP leaves
  its checksum in the partial form and `netif_transmit` finishes it in
  software for interfaces without the capability; the loopback offloads
  both ways; virtio-net negotiates `CSUM`/`GUEST_CSUM` where offered and
  virtqueues can route their vectors per CPU. Measured on loopback with
  4 CPUs: two concurrent TCP flows 30–40 % faster, one flow faster too;
  `net-bench` reports the numbers on every boot. Three new self-tests,
  127 in total. Not done, by the specification's rule that complexity
  must earn its place: device multi-queue (QEMU's user-mode backend has
  one queue), TSO/LRO, jumbo frames, zero-copy socket buffers.
- **DMA remapping (done):** the IOMMU unit the audit names next
  (`docs/kernel/iommu/`). Every bus-mastering device gets its own
  address space at `pci_enable_device`, and the buffers its driver
  mapped are the only memory it can reach: the DMA API is unchanged,
  but the bus address it returns is now an I/O virtual address in that
  device's domain, mapped read-only for `DMA_TO_DEVICE`, unmapped and
  invalidated when the driver is done. One core (domains, a bitmap IOVA
  allocator over `[1 MiB, 4 GiB)`, a shared four-level page-table
  walker, fault accounting) behind two drivers: Intel VT-d on q35
  (DMAR, root and context tables, register invalidation, fault records
  by MSI) and ARM SMMUv3, found through the ACPI IORT (a linear stream
  table, stage-2 translation, polled command queue, event queue on the
  interrupt the table names).
  Both machines boot with translation on before the first driver loads
  — 5 and 6 devices in domains, virtio, NVMe, cosmofs and the network
  stack all translating, no fault in a full run — and the new `iommu`
  self-test provokes one on purpose through an NVMe DMA to an unmapped
  address. 128 self-tests. Not done: interrupt remapping (`intremap=off`
  in the test machines), AMD-Vi, huge pages, an IOVA cache, PASID/ATS,
  stream ids above 255, requester-id aliasing behind bridges.
- **A vendor-neutral hypervisor seam, and the Intel VMX backend (done):**
  the unit the audit names after the IOMMU (§11.4). Segment attributes
  now cross `arch/hv.h` in the architectural descriptor layout instead
  of the VMCB's packing, which is what the UAPI always claimed and what
  made the long-mode `L && DB` check real; `hv_caps` grew the questions
  a VMM has to ask (can the reset state run, are mapping permissions
  honoured, are there large pages); `arch_hv_vm_map` takes permissions
  and uses 2 MiB leaves. Both x86-64 backends are now compiled in behind
  a small dispatcher: AMD-V as before, and a new Intel VT-x backend
  (`vmx.c`, `vmx_ept.c`, `vmx_run.S`) with capability-MSR control
  fixing, EPT, unrestricted guest, external-interrupt exiting and the
  exit map. **The VMX backend has never been executed**: QEMU's TCG
  emulates AMD-V only (`vmx: false`) and the development host is not
  Intel, so its evidence is a host test of the pure logic (control
  fixing, the I/O qualification, the EPT builder) plus every SVM test
  still passing — the gap is recorded in the invariants and the testing
  doc rather than papered over. 129 self-tests.
- **AArch64 exception level 2 (done):** the prerequisite for the EL2
  hypervisor backend, and the first half of the audit's AArch64
  virtualization unit. The test machine now runs with
  `virt,virtualization=on`, so firmware hands the loader control at EL2;
  the loader reserves a page, installs a small stub that owns EL2 and
  answers `HVC` (report version, take a new `VBAR_EL2`, give it back),
  turns the EL2 MMU off so nothing depends on firmware page tables the
  kernel reclaims, and `eret`s to the kernel at EL1 — which is where a
  higher-half kernel has to run, since the CPU model the tests use has
  no VHE. Secondary CPUs come up at EL2 too and do the same. The boot
  protocol carries the stub's address (version 5), the kernel reports
  `EL2 available`, and a new `el2` self-test hands the vectors over and
  back; `QEMU_EL2=0` still boots at EL1 with everything skipping
  cleanly. 130 self-tests. The world switch and stage-2 translation
  came in the next entry; the GIC list registers and the timer offsets
  became units of their own (the vGIC and the virtual timer, below).
- **The AArch64 EL2 hypervisor backend (done):** guests now run
  on the EL2 the previous unit kept. A vendor-neutral seam came first —
  `struct cosmo_vcpu_regs` is per architecture (x86's registers on
  x86-64, `x0`–`x30` with the EL1 system state on AArch64, both 448
  bytes) and two exits joined the set (`WFI`, `SYSREG`). Then stage-2
  translation (a third page-table builder beside NPT and EPT, with
  `VTCR_EL2` derived from `PARange`), and the world switch itself: EL2
  vectors installed through the loader's stub, host and guest EL1 state
  exchanged around every entry, exits decoded from `ESR_EL2`. Five
  AArch64 guests, one per exit the switch decodes, are run by five new
  self-tests — 135 in total — and `vmctl` runs one from userland.
- **Filesystem snapshots (done):** the first of the audit's four storage
  features (`docs/kernel-services/filesystem/cosmofs/design.md`, "Format
  version 3"); redundancy, compression and encryption are separate units
  after it. cosmofs was already copy-on-write, so a snapshot is the
  tuple a commit publishes — kept, with nothing copied. The commit's
  release loop either frees a block as before or holds it for a
  snapshot, decided exactly: a snapshot's allocation bitmap *is* the set
  of blocks its tree reaches, so the question needs no birth times and
  no reference counts, only the `alloc_root` the snapshot already
  records. Deleting a snapshot asks the same question of every block it
  held, so space comes back the moment nothing needs it rather than when
  the oldest snapshot goes. History reads at `<mount>/.snapshots/<name>`
  and is taken and deleted with `mkdir` and `rmdir` — no new system
  call. Version-2 filesystems mount unchanged. Two self-tests (137 in
  total) and a shell test that snapshots a real disk.
- **Storage pools: many members (done):** the addressing change the
  remaining storage features need
  (`docs/kernel-services/filesystem/cosmofs/design.md`, "Format version
  4"). Every pointer on disk is now a DVA — the member that holds the
  block in the top 8 bits, the block within it in the low 56 — and the
  superblock carries a member table, each member its own allocation
  index and bitmap, and every member past the first a label by which a
  mount finds it. Packing the address into the eight bytes a pointer
  already occupied is what makes this an extension rather than a
  rewrite: no structure changes width, no capacity is re-derived, and a
  version-2 or -3 pointer *is* a version-4 DVA on member 0, so those
  disks still mount and are written with no conversion. The allocator
  prefers whichever member has the most room and never lets a run cross
  a member, so an extent's `count` still means what it did. Two new
  self-tests (139 in total): a pool of two members, and a version-3 disk
  exercised under this kernel. Redundancy — mirroring, read repair and
  scrub — is the next unit and changes only the pool and the read
  path.
- **Storage redundancy (done):** a member may now be a **mirror group**
  of up to four devices holding the same blocks
  (`docs/kernel-services/filesystem/cosmofs/design.md`, "Format version
  5"), so adding a copy changes no address anywhere. The point of the
  unit is what surrounds the mirror rather than the mirror itself:
  reading one copy of two and trusting it doubles the chance of
  returning something wrong, so every read is checked — metadata by its
  own header, data by the per-block checksum its inode already kept —
  and the first copy that verifies is written back over the copies that
  did not. `cosmofs_scrub` reads *every* copy of every block the
  filesystem reaches, because a read stops at the first good one and rot
  behind it would wait until that copy was the one answering. Checksums
  cannot tell a stale copy from a current one, so each device records
  the commit it last took part in and a device that missed one is left
  out of the mirror instead of quietly serving old blocks. One new
  self-test (141 in total) covers rot on either copy, rot on both, a
  scrub that repairs and a second that finds nothing, and a device aged
  by one generation whose checksums are all valid.
- **Filesystem compression (done):** the third of the audit's four
  storage features (`docs/kernel-services/filesystem/cosmofs/design.md`,
  "Format version 6"). A block that compresses to a quarter of itself
  still occupies a block, so compression works on **records** — eight
  consecutive logical blocks written as one — and the page cache gained
  `writepages` to offer a filesystem several dirty pages at once. The
  physical size went into the top bits of the extent's `count`, so no
  structure on disk changed width and every earlier filesystem's runs
  decode as uncompressed. A record is compressed only if it comes out
  strictly smaller in whole blocks; its checksums cover its physical
  blocks, so a mirror repairs and a scrub verifies it without
  decompressing anything. The codec is LZ4 (`kernel/core/lz4.c`) with a
  host test and a fuzz target, because it is the one thing in the tree
  that parses attacker-controlled bytes off a disk. Nothing may cut a
  record: overwriting a page inside one rewrites it, and truncating into
  one reads it, drops it and writes back what survives — which is why
  `vfs_truncate` exists now at all. One new self-test (143 in total).
- **Filesystem encryption (done):** the fourth of the audit's storage
  features (`docs/kernel-services/filesystem/cosmofs/design.md`, "Format
  version 7"), answering the seven questions the constitution's section
  52 asks rather than copying somebody else's semantics. One master key
  per pool, wrapped by the user's key and never leaving the kernel in
  the clear; a key per file derived from it; a nonce per block of the
  block's number and the generation that wrote it, which copy-on-write
  makes unique. Rotation rewraps one block and rewrites no file. Data
  and directory blocks are encrypted — so **names are ciphertext** — and
  the metadata that describes the filesystem's shape is not, which is
  stated with what it leaks rather than left to be discovered. Each
  checksum entry carries a Poly1305 tag over the ciphertext *and* a
  keyless CRC of it: the CRC is what lets a mirror repair and a scrub
  read the whole filesystem on a machine with no key, and the tag is
  what says a block is the one that was written. ChaCha20 rather than
  AES, because there is no AES instruction here and a software AES is
  either slow or a key-dependent table lookup; both it and Poly1305 are
  checked against RFC 8439's vectors. One new self-test (144 in total).
- **Handle rights (done):** the first of the container primitives the
  constitution defers in section 53, and the beginning of section 54's
  aim that privileged operations stop depending on being uid 0. A handle
  is a capability: what a process may do with an object is what its
  handle says. The vocabulary is READ, WRITE, DUP, TRANSFER and MANAGE,
  with bits 16–31 reserved for each object type, and **rights only ever
  shrink** — `dup` and `spawn`'s handle map may hand over a subset of
  what the caller holds, and nothing anywhere adds a right to a handle
  that exists. Holding something is not permission to pass it on, and
  administering an object is separate from using it. A handle table also
  stops answering before it is torn down, so a thread still inside a
  syscall cannot be handed a reference the exit is releasing. `read` and
  `write` still answer `EBADF` where POSIX says they should; `EPERM` is
  for the operations POSIX has no opinion about.
- **Confinement: roots, domains and mount namespaces (done):** three
  more of the container primitives, each answering a different question
  about what a process can reach. A **root** says which subtree it may
  name: absolute paths start there, `..` stops there, and a spawn names
  the child's root in its own namespace, so confinement only ever
  tightens. A rooted child starts *at* its root, because inheriting the
  parent's working directory would leave it standing outside the thing
  meant to contain it. A **domain** says which processes it can see and
  signal: outside domain 0 a process sees only its own, and the domain
  the system boots in still sees all of them, which is how a host
  manages what it started; a signal across the boundary is `ESRCH`
  rather than `EPERM`, since "not permitted" would confirm the pid
  exists. A **mount namespace** says what is attached inside the root: a
  copy of the parent's view that then diverges, so what a confined
  process mounts stays its own and what is mounted outside does not
  appear beneath it. A **uts namespace** says what it believes it is
  running on — which first meant giving the machine a name at all, since
  `uname` had been answering with a constant; `gethostname`,
  `sysctl kernel.hostname` and `uname` now all answer from the caller's
  namespace, because three ways to ask must not give a contained process
  three answers. A namespace copies the *view* and never a
  filesystem — one mount is one vnode cache, one open transaction and
  one device — and the last namespace that can see a mount is the one
  that unmounts it. All three are privileged to start and are entered
  only at spawn: privilege flows down and there is no way back up.
- **The syscall filter (done):** a process may narrow the set of system
  calls it is allowed to make — a bitmap by call number, intersected
  with what is in force, inherited by children. It is the one primitive
  here that is **unprivileged**, and that is its shape: the others
  decide what a subtree of processes may see or reach, while this one
  only ever takes authority away from the caller. A denied call kills
  the process with `SIGSYS` rather than returning an error, because a
  filter states what the program will ever need, so a call outside it is
  a bug or an exploit and neither should continue into a state the
  author never tested. `exit` — and, for Linux binaries, `exit_group`
  and `rt_sigreturn` — stay allowed whatever the mask says, or a clean
  shutdown or a signal handler's return would itself be fatal. It reads
  the call number and nothing else: arguments live in user memory, so a
  filter that read one would be checking a value the process can change
  between the check and the call, and a filter that can only say things
  which stay true is worth more than one that can say more.
- **Per-type handle rights (done):** the upper sixteen bits of a handle,
  reserved when rights arrived, now say something. The generic five
  describe an object's *contents*; the type's own bits name the
  operations that are neither reading nor writing — for a socket `BIND`,
  `ACCEPT`, `CONNECT` and `SHUTDOWN`, for a VM `MAP` and `VCPU`, for a
  vCPU `RUN`, `REGS` and `IRQ`. Sockets are where this was missing
  rather than merely coarse: `bind`, `listen`, `connect` and `shutdown`
  required **no right at all**, so a socket lent to another process as
  read-only could still be pointed at a different peer or shut down by
  the borrower. The vCPU objects had the opposite fault — one WRITE
  covered running a guest, rewriting its registers and injecting
  interrupts. Each type right is required on its own rather than on top
  of a generic one, since the bit already names the operation. The same
  bit means different things on different types, which is safe because
  the object's kind is established *before* its bits are read: a handle
  to something else answers `EBADF`, not `EPERM`, because the bit means
  nothing there.
- **The service manager (done):** constitution section 55 asks for
  start, stop, restart, dependencies, logging, a restart policy,
  resource limits and supervision, and says not to reproduce systemd.
  What it describes is daemontools' shape, so `svc` has **no daemon**:
  one supervisor process per service, and the state in the filesystem
  (`/run/svc/<name>.pid`, `/var/log/svc/<name>`). A central manager
  would need a control channel, and the two Unix answers — a named pipe
  and a unix socket — are both things this kernel does not have;
  building an IPC mechanism in order to build a service manager is
  backwards. A definition is `key value` lines, and an **unknown key is
  an error**, because a typo in `root` or `user` would otherwise leave a
  service running with more authority than its author wrote down. Those
  keys are where the container primitives earn their place: a service is
  confined by naming it in a file. The restart policy is bounded in both
  directions — a backoff that doubles and a retry limit — since a
  service that dies instantly must not spin the machine, and a
  supervisor that gives up must say so where somebody will find it.
- **`/proc` (done):** the last item on the audit's list. Constitution
  section 56 asks for pseudo-filesystems with clear ownership and says
  not to make them a dumping ground for kernel internals, so the
  ownership question is answered first and the answer is in the name:
  **`/proc` holds facts about processes, and nothing else.** System-wide
  values stay in `sysctl`, whose names are a curated list; the log stays
  in `dmesg`; device nodes stay in `/dev`. There is no `/proc/meminfo`,
  because `sysctl vm.pages_free` already answers that and a second
  spelling of a fact is how a namespace rots. What it adds is
  addressability — `/proc/self/status` and `/proc/<pid>/limits` are
  readable by anything that reads files, where `procinfo` returns a
  struct to a caller that knows the ABI. **A process sees exactly what
  `procinfo` would show it, in the listing as well as the files**: a
  directory that names what it will not open is an information leak with
  extra steps, so the visibility rule lives in one function that
  `procinfo`, the lookup and the listing all call. `/sys` is not added:
  there is nothing to put in it that `sysctl` does not already hold.
- **An Intel gigabit NIC (done):** the first network device here that
  is not virtio, and so the first test of the stack's claim not to
  depend on one — which held: the driver adds nothing to the kernel's
  interface. `e1000e` drives the 82574L that QEMU models, with legacy
  descriptor rings, one MSI-X vector whose handler services both rings
  whatever the cause bits say (the legacy and queue-mapped schemes
  differ and a model raises one or the other), and a watchdog that
  resets a transmitter whose head has not moved in five seconds and says
  so. It claims no checksum offload, and now with a number rather than a
  deferral: `net-nicbench` sends traffic that leaves the machine over
  every interface — ARP round trips through the driver's rings, UDP
  through the whole stack — and shows the software checksum is one to
  two percent of a send on both drivers and both architectures, so the
  offload machinery would buy two percent and is not written. The same
  benchmark found the driver double-counting statistics the stack
  already keeps, which looked plausible alone and was obvious beside
  virtio-net in the same boot. A default boot has two interfaces, and a
  self-test brings the first down and requires the second to take over
  and resolve its gateway through its own rings.
- **USB host stack (done):** `docs/drivers/usb/`. The `xhci` module
  (the USB core and an xHCI 1.2 controller driver: rings, contexts,
  commands, one interrupter, root-hub ports, a worker per controller) and
  the `usb_storage` module (bulk-only mass storage as `sda`). The first
  bus whose devices arrive after boot, have a parent that is a device,
  and leave with I/O in flight; the model held, with two rules recorded:
  DMA goes through the controller (a USB device has no requester id),
  and a bus removes its children before itself. Tests: enumeration, the
  disk through the block layer, a device that stops answering, an
  unplug with a bio in flight (the driver's own detach path, since the
  harness has no monitor), IOMMU fault attribution to the controller,
  and `blk-bench`, which put the USB disk within noise of NVMe and
  virtio-blk on this device model — so no further scatter-gather work.
- **AHCI (done):** `docs/drivers/ahci/`. The `ahci` module drives SATA
  disks through an AHCI host bus adapter — on `q35` the ICH9 the machine
  always had, which turned out to carry the boot image too — as one
  blkdev per port (`ahci0p1`), non-queued commands in the HBA's 32
  slots, task-file recovery, hotplug through a worker. It answered the
  question the USB unit left: a SATA port has no identity of its own,
  so its disk's DMA device is the controller and there is no `struct
  device` per port; the model gained no DMA-parent pointer. NCQ was
  measured (four streams reach 87 % of NVMe's aggregate without it) and
  not written.
- **The machine's own console (done):** `docs/kernel/diagnostics/`
  (the display) and `docs/drivers/usb/` (the keyboard and the hub).
  Until this unit CosmoOS spoke to exactly one kind of console device, a
  UART, and the only two callers of `tty_input` in the tree were the two
  UART drivers -- so on the machines section 61 asks for next, none of
  which has a serial port, it would have booted blind and deaf. Four
  pieces: the UEFI Graphics Output Protocol's *already-set* mode carried
  through boot protocol v6 (no mode is ever set: that is the display
  driver section 60 defers); a framebuffer console sink with an in-tree
  8x8 face, panic-safe by the serial sinks' rules, which replays the
  newest screenful of the log ring when it registers; a HID
  boot-protocol keyboard whose keys go to the console tty through the
  same `tty_input` the UARTs call, so the shell cannot tell them apart;
  and a hub, which is the first device on that bus with devices behind
  it. The hub answered the unit's architectural question: topology lives
  in the USB core as a parent, a depth and a route on `struct
  usb_device`, and in two fields of the controller's slot context --
  nothing else in the kernel learned that hubs exist. Tests turn on
  reading the pixels back (a display is testable with no screen and no
  screenshot) and on a QMP socket the harness had never had, so the boot
  test now types on the emulated keyboard, including two keys held at
  once. Three bugs the unit found were older than it: a mixed 2 MiB
  block in the AArch64 loader's direct map (RAM behind a reserved range
  got device attributes, and the kernel's first unaligned store into it
  faulted), an AHCI fault injection that a second console sink's timing
  exposed as unfaithful, and the rule that a transfer's buffer may not
  be a kernel stack.
- **Floating point and SIMD (done):** `docs/kernel/arch/design.md`
  ("FPU and SIMD state") and `docs/kernel/arch/aarch64/`.
  `printf("%f", 3.14)` printed `?`, and that was the visible end of a
  rule that ran through the tree: the kernel is built
  `-mgeneral-regs-only`, which is right, and so were the libc and every
  user program, which was not. On x86-64 that was a build choice over a
  kernel that was ready; on AArch64 `CPACR_EL1.FPEN` sat at its reset
  value, so every FP instruction trapped at EL0 and EL1 alike. AArch64
  now has a 528-byte state per thread with eager switching, the arm64
  signal frame carries an `fpsimd_context` (so a handler may use the
  vector registers without losing the interrupted code's), the EL2
  backend swaps guest and owner around an entry, and the userland is
  built without the flag with real `%f`, `%e` and `%g` behind it. The
  kernel's own abstention is checked rather than promised —
  `scripts/check-fpregs.sh` disassembles the built image during `make
  analyze` and fails on any vector register outside a named short list,
  because neither architecture can enforce it in hardware: ring 0 and
  EL1 are exactly where the state is saved. Eager stayed, with the
  measurement §21 asks for: the save and restore are about 1 000 ns of a
  21 600 ns switch on AArch64 and 270 of 2 700 on x86-64, against a trap
  for every thread under lazy. What the report got backwards: removing
  the build flag changes nothing by itself — the compiler uses those
  registers when they help, and it was `%f` that gave it a reason.
- **Signals a person can send (done):** `docs/audit/next-subsystem-signals.md`,
  `docs/kernel/process/`. The console unit gave the machine a keyboard,
  and `^C` was dropped in the line discipline: the native personality
  installed no handlers, there were no process groups for a terminal
  interrupt to reach. The native signal ABI chooses the frame a handler
  runs on rather than inheriting one -- magic, blocked mask, registers,
  the FP/SIMD image and the siginfo on the thread's own stack, trusted at
  `sigreturn` no further than its magic, with `sigreturn` in the
  always-allowed list so no syscall filter can turn a caught signal into
  a kill. Sessions and process groups (`pgid`/`sid` read and written only
  under the process table's lock -- one rule for fields always looked at
  across two processes), `kill(-pgid)`, `COSMO_SPAWN_SETPGID` so a child
  is in its group before its first instruction; a session claims the
  console by naming a foreground group and releases it when its leader
  exits. The Linux personality's stubs for the same calls now go through
  the same code. Twelve regression tests, each proved by reintroducing
  its bug; two proofs found the tests vacuous (the boot test's `^C`
  passed with job control disabled because `sleep 5` reaches a prompt on
  its own) and one found the frame carried nothing of AArch64's 520-byte
  FP image because it hard-coded 528.
- **Job control (done):** `docs/audit/next-subsystem-jobcontrol.md`.
  Audit finding #30 closed: the stop signals stop something. A process
  gains `stopped` and a wait queue, a thread gains `sig_must_stop`, and
  the split is the whole design -- the per-thread flag is a *reason to
  look*, the process's own `stopped` is the authority, re-read under the
  lock at the park -- which four rounds of review on the report were spent
  on. Stopping happens at a return to user mode and nowhere else, so no
  kernel lock is held and nothing is frozen mid-kernel; a call cut short
  by a stop is restarted unconditionally. `^Z`, `SIGTTIN`, `SIGTTOU`, an
  orphaned group never stopped (removing that rule wedges the boot), and a
  shell with `&`, a job table, `jobs`/`fg`/`bg`. Two things the report did
  not predict, both found by tests: `SIGCONT` must stay in the ignore
  table (it continues before the default-action table is consulted), and
  the parent's wait scan must not reach into a child's lock -- two process
  locks nested, an order this kernel does not have, which surfaced as a
  global slowdown before it could as a deadlock. Eight regression tests,
  three of which had to be rewritten first because they passed with the
  bug in place.
- **A terminal a program can drive (done):**
  `docs/audit/next-subsystem-termios.md`, `docs/kernel/tty/`. The line
  discipline's echo, canonical assembly and signal characters were fixed
  at boot, which ruled out every full-screen program and every shell with
  history. Four mode flags (`ECHO`, `ICRNL`, `ICANON`, `ISIG`) and two
  numbers (`VMIN`, `VTIME`) over a native structure -- POSIX's other flag
  words and nineteen control characters omitted rather than accepted and
  ignored, so a program asking for what this terminal cannot do fails to
  compile; a raw input path; `/dev/console` and `/dev/tty`; `isatty`,
  `<termios.h>`, and the Linux `TCGETS`/`TCSETS`/`TIOCGWINSZ` over the
  same structure. The shell reads its own line: arrows, `^A`/`^E`/`^U`/
  `^W`, 32 lines of history. The terminal's modes reset when its session
  ends -- the report's leading risk, and it arrived on the first test run.
- **A hypervisor bug, found by poison (done):** two AArch64 crashes the
  terminal chain surfaced once each in tens of boots were one bug: the EL2
  world switch set `SP_EL2` to the top of the vCPU's context page and
  never put it back, so the host's very next `HVC` on that CPU pushed four
  registers into a frame the allocator had already handed to someone else
  -- a page table's last four PTEs, or a text page's last 32 bytes. Found
  by the other half of the change: debug builds now poison every freed
  frame and verify the pattern at the next allocation (invariant M37),
  which turned a 1-in-30 crash into a deterministic panic eight seconds in
  that named the freer. Also `pmm_page_put`'s load-compare-decrement
  became one `fetch_sub`, the `vnode_put` race in another coat.
- **Address-space identifiers (done):** `docs/audit/next-subsystem-asid.md`,
  `docs/kernel/memory/`. Every switch between two processes emptied the TLB,
  and on AArch64 the flush was inner-shareable, so one CPU changing
  process emptied the user TLB of every CPU in the machine. Tags are
  allocated lazily from one bitmap with generations for rollover, so
  exhausting the pool costs what a single switch used to, once per 65,535
  spaces; the tag rides in `TTBR0[63:48]` and the flush is conditional and
  *local*. `vm_space.tlb_cpus` is the CPUs that *may hold* a space's
  translations, left only by a flush; range invalidates stay all-ASID
  because a re-tagged space can still be running under its old tag on
  another CPU; destroy invalidates before it releases, or a tag's next
  owner inherits its translations. x86-64 deliberately unchanged: TCG
  implements PCID on no CPU model, so a tagged path there could be
  exercised nowhere this tree is tested. Six self-tests, including a
  forced rollover that asserts the tag really was reissued before checking
  the byte.
- **GICv3 (done):** `docs/audit/next-subsystem-gicv3.md`,
  `docs/kernel/arch/aarch64/design.md`. The machine section 61 asks for
  next has no GICv2. Two controller generations now sit behind one seam
  (`struct aarch64_irqc_ops`; `gic.c` keeps every line of its GICv2 logic
  behind it), with the seam put in *before* the second driver existed so
  the change that added GICv3 was only new code. `gicv3.c` drives the
  distributor, the redistributors and the system-register CPU interface;
  `gicv3_its.c` is the ITS -- `MAPD`/`MAPC`/`MAPTI`/`INV`/`SYNC`, LPIs
  from 8192, a flat device table -- with the GICv2m frame kept as the
  fallback when firmware describes no ITS, and `arch_irqc_msi_doorbell`
  so the SMMU can let the doorbell page through. The GICv3 machine is the
  first in this tree to bring all sixteen CPUs online, which is what
  proves the SGI target list is built from the right affinity fields.
  `QEMU_GIC=3` and `QEMU_MSI=its|gicv2m|off` select the shapes, and `make
  test-gic` runs them in CI -- added by the next unit, which found that
  since this one merged CI had exercised the GICv2 driver alone.
- **The vGIC: a guest that can be interrupted (done):**
  `docs/audit/next-subsystem-vgic.md`, `docs/kernel/arch/aarch64/design.md`
  ("Giving a guest an interrupt"). `arch_hv_vcpu_set_irq` recorded the
  offer and delivered nothing; now `ICH_*_EL2` state travels in `struct
  hv_ctx` through the world switch, a new `HV_EL2_CALL_VGIC` lets EL1
  reach the GIC's system-register interface and asks `ICH_VTR_EL2` how
  many list registers there are (four on QEMU), and one of them carries
  an interrupt into the guest, whose own `ICC_IAR1_EL1`/`ICC_EOIR1_EL1`
  are redirected by hardware to the virtual interface. Two rules the unit
  paid for: **Active means delivered** -- a hypervisor that waits for
  Invalid re-injects everything a guest was still handling when it exited
  -- and the question after a run is *which* interrupt was taken, not
  *whether*: the register's occupant need not be this entry's offer.
  GICv3-only; on GICv2 the capability says so and the tests skip rather
  than lie. Guest fixtures heartbeat through `hvc`, not `WFI`, because
  `TWI` traps a `WFI` only when it would actually wait -- measured, after
  the first fixture hung the watchdog.
- **The virtual timer: a guest that can be woken by time (done):**
  `docs/audit/next-subsystem-vtimer.md`, `docs/kernel/arch/aarch64/design.md`
  ("The guest's timer"). Measured first: `CNTV_CTL_EL0` read `0x1` in
  the host after a guest armed it, the guest's `ENABLE` live in the host's
  context, because no timer register crossed the switch. Three do now
  (`CNTVOFF_EL2` one value per VM, `CNTV_CVAL`, `CNTV_CTL` saved *before*
  being disarmed because its `ISTATUS` is the only trustworthy account of
  an expiry -- `HV_EXIT_INTR` names no interrupt); `CNTHCTL_EL2` is `0`
  for a guest and the host's saved value otherwise, so a guest may read
  neither the host's uptime nor arm its tick. A guest that waits in `WFI`
  is run again *when* its timer fires: asked 15.6 ms, held 17 ms, fired
  2.6 ms late. Found on the way: an expired timer storms on entry (the
  PPI is disabled locally while an expiry is queued), a leaked timer was
  a hang rather than a failed test (the host's handler now recognises a
  guest's timer past the switch and warns), and the isolation bug-proof
  was defeated by that very safety net until the test read the host's
  register the instant the switch returned. The default GICv2 boot caught
  a hang the GICv3 boots could not: the timer-state fixtures set up the
  GICv3 system-register interface, which faults on a GICv2 host.
- **The virtual distributor: a guest that can run a stock GIC driver
  (done):** `docs/audit/next-subsystem-vdist.md`,
  `docs/kernel/arch/aarch64/design.md` ("The guest's distributor"),
  invariant A24. Measured first: a guest's load from `GICD_TYPER` -- the
  first register every GIC driver reads -- was an `MMIO` exit to an owner
  with nothing behind it. An in-kernel GICv3 distributor per VM, because
  its output is a list-register write an owner in userland cannot reach;
  `GICD`/`GICR` at the addresses `virt` puts them, as the hypervisor's
  own constants; stage-2 faults inside the windows decoded from
  `ESR_EL2.ISS` and completed in the kernel with a new `HV_EXIT_EMULATED`
  ("run again", not a host interrupt); vCPU `i` reads MPIDR Aff0 = `i`
  through `VMPIDR_EL2`, so a driver's redistributor walk finds its own
  frame; routing by the rule that the distributor decides and the vCPU's
  own thread places; the timer PPI now arrives through the redistributor,
  gated by the guest's own enable bit; a guest's `ICC_SGI1R_EL1` write is
  routed by affinity to sibling vCPUs -- a guest can be SMP. Two
  corrections recorded: the report said the SGI write was "not trapped"
  (it was -- no `ICV_SGI1R_EL1` exists, so it traps unconditionally under
  `IMO` -- to an owner that could not route it), and `ICH_HCR_EL2.TC`,
  which the report proposed, traps every register common to both groups,
  `ICC_PMR_EL1` among them, and stopped every interrupt guest before
  "ready". Seven bug-proofs, all deterministic; one was vacuous with the
  neighbouring priority bytes at reset zero until the fixture pre-filled
  them.
- **The guest's console: a PL011 a stock kernel can print to (done):**
  `docs/audit/next-subsystem-vuart.md`,
  `docs/kernel-services/virtualization/design.md` ("The guest's
  console"). Measured first: a guest's store of `'A'` to `UARTDR` was an
  `MMIO` exit to an owner with nothing behind it -- and the exit did not
  even carry the `'A'`. Two things, the first the seam every device after
  it will use: the MMIO exit now describes the access (size, register, a
  write's value, and whether a read sign-extends and into which width of
  register), `vm_device.mmio` goes from a stage-1 stub to a real handler,
  and one function turns a read's result into a register by width and
  sign for the in-kernel and owner-answered paths alike -- proved by a
  test word answering seven load forms before any real device leaned on
  it. Then the device: a PL011 per VM at `0x0900_0000` writing into the
  console ring the VM descriptor already reads; `write()` on that
  descriptor feeds a receive FIFO, and the device raises SPI 33 through
  the distributor -- the first device interrupt it routes that a guest
  did not fake -- level-triggered by the source's own rule (the run loop
  re-raises a line still up before each entry; the device lowers its own),
  and a vCPU asleep in `WFI` is woken by the keystroke. In the kernel by
  the one-exit-per-byte argument that placed the distributor. A bug-proof
  separated two promises that looked like one: with the write not raising
  the line itself the receive test still passes (the next entry re-raises)
  but the sleeping vCPU waits out its deadline -- the immediate raise is
  for the sleeper, the re-raise for the level.
- **The machine a guest is handed: a device tree, the entry convention,
  and PSCI (done):** `docs/audit/next-subsystem-machine.md`,
  `docs/kernel-services/virtualization/design.md` ("The machine a guest
  is handed"). Every device the last four units built sits at an address
  the hypervisor chose, and nothing told a guest what they were: `x0 = 0`
  at entry, and no code in the tree wrote a device tree. Now the layout is
  in the uapi once (`cosmo/hv_machine.h`; three kernel headers became its
  aliases), a device-tree writer this tree owns describes exactly the
  machine the kernel implements in `virt`'s shape and is compiled into
  both `vmctl` and a host tool that puts a blob in the boot archive, and
  `vmctl run --machine` lays the machine out as the arm64 boot protocol
  asks -- RAM at 1 GiB, the image where its Image header says, the tree at
  the first 2 MiB boundary past it and in `x0`. The owner is the firmware:
  PSCI is answered in `vmctl`, and `CPU_ON` creates a vCPU after the VM
  has started and runs it -- in one thread, because the native libc has
  none, through a bounded run (`COSMO_VCPU_RUN_ONE_TICK` returns a new
  `PREEMPTED` exit at the first host interrupt) that the kernel already
  had for its own tests. The tree's first C guest knows nothing of this
  hypervisor: it reads the tree, prints what it found through the UART
  the tree named, asks PSCI its version, brings up the second CPU and
  powers off -- from the kernel's test and from `vmctl` alike. On the way:
  the UART's receive FIFO dropped silently when full and a loaded host
  lost bytes; it refuses now and the owner's write returns short.
- **Booting Linux (done):** `docs/audit/next-subsystem-linux.md`,
  `docs/kernel/arch/aarch64/design.md` ("The features a guest is told it
  has"). The reader whose opinion of the device tree settles it: a stock
  arm64 Linux (Alpine's `vmlinuz-virt`, its raw `Image` extracted) loaded
  by its header and handed our device tree stopped at a trapped read of
  `ID_AA64DFR0_EL1` -- `HCR_EL2.TID3` traps every feature-register read to
  the owner, and the owner modelled none. A feature-register model in the
  kernel (`hv_idregs.c`) answers the whole ID space from the host's own
  registers, masked deny-by-default so a guest is never told it has a
  feature the hypervisor does not isolate. That one model, plus raising
  the per-VM RAM ceiling from 64 to 512 MiB so a modern kernel can run its
  `init`, is the whole distance: **a stock Linux 6.6 now boots on CosmoOS,
  prints its banner and `Machine model: cosmo,virt` -- read from our
  device tree -- through the PL011 we emulate, and reaches the panic a
  diskless machine reaches (`Unable to mount root fs`).** The claim the
  whole hypervisor arc was for: it runs a kernel written for the
  architecture, not for the hypervisor. The Image is not committed (34 MB,
  someone else's binary); the boot test runs when a developer drops one at
  `tests/hv/aarch64/Image` and skips cleanly otherwise, and `el2-guest-idreg`
  is the model's regression net in CI. A root filesystem (virtio-blk) is
  the next unit; the panic is the milestone.
- **A root filesystem a guest mounts (done):**
  `docs/audit/next-subsystem-vblk.md`,
  `docs/kernel-services/virtualization/design.md` ("The root filesystem a
  guest mounts"). The disk that ends the diskless panic is a virtio-blk
  device. The split virtqueue lives in guest memory; the device only
  reaches it through `vm_mem_read`/`vm_mem_write`. `userland/system/vblk.c`
  is the device-side walk and nothing else -- no kernel headers, no
  transport -- so the same function is linked into `vmctl` and exercised
  on the host with no guest and no QEMU: it reads each new head's
  descriptor chain (readable header, writable data buffers, writable
  status), serves the sectors from the disk, and publishes the head on the
  used ring. Every index is bounded by the queue size before it addresses
  memory, a looping chain is refused, a buffer outside guest RAM faults
  through the callback, and a read past the disk completes as an I/O error,
  not a crash. The device is read-only: it offers `VIRTIO_BLK_F_RO` so the
  driver never submits a write, and a `VIRTIO_BLK_T_OUT` that arrives
  anyway completes as unsupported. `vmctl` models the transport at the
  `virtio_mmio@a000000` node the device tree already advertises (SPI 48),
  serves `QueueNotify` from a disk opened read-only, and raises the SPI
  through the guest's distributor on completion; with no `--disk` the node
  reports DeviceID 0 and the guest's driver skips it, so the boot test,
  which runs no disk, is unchanged. The mechanism is proven end to end in
  the kernel harness (`el2-virtq-device`: a guest negotiates the device and
  reads a sector's bytes) and exhaustively on the host (`test_vblk_dev`).
  Mounting a Linux root over it needs a root image and `QEMU_MEM=2G`, the
  same demonstration-not-gate shape the Linux boot has.
- **A writable root (done):** `docs/audit/next-subsystem-vblk-rw.md`,
  `docs/kernel-services/virtualization/design.md` ("A writable root"). The
  read-only root reaches userspace but keeps nothing; this adds the write
  side. The write is the read walk with the data moving the other way, so
  the direction lives entirely in `serve_one`: a read fills its
  device-writable data buffer from the disk, a write (`VIRTIO_BLK_T_OUT`)
  drains its device-readable buffer to the disk, a flush (`T_FLUSH`) makes
  prior writes durable. Every hostile-input bound and the atomic-publish
  rule the read path grew apply unchanged above the direction branch, and
  `disk_write` is bounded to the capacity like `disk_read`, so a write past
  the disk is an I/O error and the file never grows. Read-write is opt-in:
  `--disk` stays read-only (offers `VIRTIO_BLK_F_RO`, writes `UNSUPP`), and
  `--disk-rw` opens the file `O_RDWR` and offers `VIRTIO_BLK_F_FLUSH` and
  not `RO` -- because a guest writing a file the owner meant to keep is
  silent data loss, and a writable device without flush would lie about
  durability. `disk_write` is an `lseek`+`write`; `disk_flush` is `fsync()`
  of the disk file (a new `SYS_fsync` that commits one file, not `sync()`'s
  every mount, so a guest cannot force commits of unrelated host mounts).
  Proven end to end in the harness (`el2-virtq-device`: a guest reads a
  sector, then writes one, flushes, and reads back what it wrote) and
  exhaustively on the host (`test_vblk_dev`: the write round-trip, the
  flush, the hostile write rings). Mounting a stock Linux root read-write
  is the same `QEMU_MEM=2G`-and-a-root-image reproduction the read path's
  Linux boot is, not a CI gate.
- **A guest network interface (done):** `docs/audit/next-subsystem-vnet.md`,
  `docs/kernel-services/virtualization/design.md` ("The shared virtqueue
  walk, and a network interface"). Adding a second device first meant
  lifting the ring walk out of the block device: `userland/system/vq.c` now
  holds the walk and every hostile-input discipline the block device earned
  over its review, and `vblk.c` is one `serve` over it -- proved by the
  block tests passing unchanged. `vnet.c` is the second `serve`: a
  virtio-net device at a second virtio-mmio window (`COSMO_HVM_VIRTIO1_*`,
  SPI 49), transmit draining posted frames to the wire and receive a *pull*
  queue that fills posted buffers only as frames arrive (a serve returning
  0 leaves a buffer available, the generalisation net needed). `vmctl`
  learns `QueueSel` and per-queue state, reports DeviceID 1 and a MAC, and
  offers `VIRTIO_F_VERSION_1` + `VIRTIO_NET_F_MAC`. The wire is the owner's
  and for now a loopback -- a transmitted frame comes back on receive,
  enough to prove both queues, the header and the interrupt end to end, and
  it drops when full rather than growing. Proven in the harness
  (`el2-virtq-net`: a guest transmits a frame and receives it back) and on
  the host (`test_vnet_dev`). The wire's far end, a loopback here, becomes
  the host stack in the next entry.
- **A host bridge, so the guest reaches the host (done):**
  `docs/audit/next-subsystem-tap.md`,
  `docs/kernel-services/virtualization/design.md` ("The host bridge"). The
  guest's NIC looped back to nothing; this connects its far end to the
  host's own stack. `kernel-services/network/tap.c` is a tap -- a `netif`
  whose far end is userland: `transmit` queues a frame for a reader,
  `tap_inject` hands one to `netif_rx`, and the stack does the rest (it
  ARPs on the tap, answers what is addressed to its IP). The owner reaches
  it through `/dev/net/tap`, a character device: read one frame the stack
  sent, write one from the guest (read returns 0 when none waits, so the
  owner polls it like the console; no new syscall). It is backed by one
  `tap0` on `10.0.3.0/24`; a tap is marked never-default (`NETIF_NODEFAULT`),
  so even left up it is never the machine's route to the world. `vmctl --net tap` points the virtio-net wire at the
  channel, the device unchanged. Proven in the harness by the `tap`
  selftest (a frame out the tap read back, an injected ARP answered by the
  stack, the queue capped) and `el2-tap-host` (a guest's ARP request
  crossing virtio-net and the bridge into the real stack, which answers on
  the tap). Reaching beyond the host -- NAT, routing, DHCP, DNS -- is the
  next unit; a stock Linux `ping 10.0.3.1` of the host is the `QEMU_MEM=2G`
  reproduction.
- **Next:** the roadmap's numbered phases and the post-roadmap audit's
  own list are complete, apart from pid renumbering, which the process
  domain deliberately does without and argues against. The constitution's
  **section 60 hardware roadmap** is now done through AHCI — `NVMe`, an
  Intel NIC, USB, AHCI — with the IOMMU unit done earlier and GPU, Wi-Fi
  and Bluetooth explicitly later. What remains named are the follow-ups
  each unit left (NCQ if a real disk shows it pays; the USB hub driver
  and HID are done). The AArch64 follow-ups the EL2 backend left --
  GICv3, ASIDs, FP/SIMD at EL0 -- are done, and the hypervisor has gone
  past them: a guest has a virtual CPU interface, a timer and a
  distributor. What it lacks next is named in the open report below.
  Section **68** is not a list of deferrals: it is the
  instruction to stop after the audit, name one subsystem in a fixed
  shape and wait, which `docs/audit/next-subsystem.md` did for the NIC,
  `docs/audit/next-subsystem-usb.md` for USB (built as the `xhci` and
  `usb_storage` modules) and `docs/audit/next-subsystem-ahci.md` for
  AHCI (built as the `ahci` module).
  `docs/audit/next-subsystem-console.md` did it for the machine's own
  console (built: the framebuffer the UEFI firmware has already lit,
  carried through a version 6 boot protocol into a second console sink,
  and a USB keyboard feeding the same `tty_input` the two UARTs feed).
  `docs/audit/next-subsystem-fpsimd.md` did it for floating point and
  SIMD (built: AArch64 threads own vector state, the signal frame
  carries it, and the userland is no longer compiled to avoid the
  registers every real program uses).
  `docs/audit/next-subsystem-signals.md`, `-jobcontrol.md` and `-termios.md`
  did it for the signals a person can send, for job control and for a
  terminal a program can drive (all built). `-asid.md` did it for
  address-space tags (built). `-gicv3.md`, `-vgic.md`, `-vtimer.md` and
  `-vdist.md` did it for the other interrupt controller and then, one
  piece at a time, for an AArch64 guest's interrupts, timer and
  distributor (all built: a guest can run a stock GIC driver and be SMP).
  `docs/audit/next-subsystem-vuart.md` did it for the guest's console
  (built: a PL011 a stock kernel can print to and be typed at, on a
  device seam that completes an MMIO access by width and sign).
  `docs/audit/next-subsystem-machine.md` did it for the machine a guest is
  handed (built: a device tree, the entry convention, PSCI, and a C guest
  that reads them). `docs/audit/next-subsystem-linux.md` did it for
  booting Linux, and Linux now boots (the feature registers modelled, the
  RAM ceiling raised) to its diskless-root panic -- the reader whose
  opinion of the device tree settles it, and it settled favourably. Design
  documents first, one subsystem at a time.
