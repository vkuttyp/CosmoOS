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
  x86-64, `x0`–`x30` with the EL1 system state on AArch64; 448 and 496
  bytes respectively) and two exits joined the set (`WFI`, `SYSREG`). Then stage-2
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
  and a unix socket — were both things this kernel did not have then
  (both exist now: the unix socket since the unix-sockets unit, the
  named pipe since the named-pipes unit; `svc`'s shape stands on its
  own reasons);
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
  has started and runs it -- at the time, in one thread, because the
  native libc then had none, through a bounded run
  (`COSMO_VCPU_RUN_ONE_TICK` returns a new `PREEMPTED` exit at the first
  host interrupt) that the kernel already had for its own tests. **That
  single-thread loop is history**: the thread-per-vCPU unit below replaced
  it, each vCPU now runs untimed on a thread of its own, and the flag
  survives only in the ABI and the kernel's own tests
  (`docs/kernel-services/virtualization/design.md`, "More than one vCPU in
  one thread, and why it stopped being one thread"). The tree's first C
  guest knows nothing of this
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
  sent, write one from the guest (read returned 0 when none waited, so the
  owner polled it like the console; since the device-readiness unit a
  blocking open's read waits for a frame and `vmctl` opens it
  `O_NONBLOCK` to keep polling; no new syscall). This unit backed it with
  one persistent `tap0` on `10.0.3.0/24` (a tap per open came later -- see the
  multi-guest entry); a tap is marked never-default (`NETIF_NODEFAULT`),
  so even left up it is never the machine's route to the world. `vmctl --net tap` points the virtio-net wire at the
  channel, the device unchanged. Proven in the harness by the `tap`
  selftest (a frame out the tap read back, an injected ARP answered by the
  stack, the queue capped) and `el2-tap-host` (a guest's ARP request
  crossing virtio-net and the bridge into the real stack, which answers on
  the tap). Routing and NAT past the host are done in the next entry; DHCP
  and DNS remain. A stock Linux `ping 10.0.3.1` of the host is this unit's
  `QEMU_MEM=2G` reproduction.
- **The guest reaches beyond the host: IP forwarding and masquerade NAT
  (done):** `docs/audit/next-subsystem-nat.md`,
  `docs/kernel-services/network/design.md` ("Forwarding and NAT"). The tap
  boxed the guest into a two-node network with the host; this lets its
  packets leave. Three pieces: `ipv4_route` now prefers a *connected* subnet
  by longest prefix (so a reply for the guest routes to the tap, not the
  default NIC); `ipv4_input` **forwards** a datagram that is not for the host
  when it arrived on a `NETIF_FORWARD` interface -- routed, the TTL
  decremented (an ICMP time-exceeded at zero, RFC 1812), re-emitted -- the
  gate per-ingress so the flag is the tap's and never the NIC's, and the
  host is no router for its real link; and `kernel-services/network/nat.c`
  **masquerades** a forwarded flow whose source is not on the egress subnet,
  rewriting the source to the egress address and the TCP/UDP port (or ICMP
  echo id) to a value a bounded conntrack table lends, fixing the transport
  checksum incrementally (RFC 1624) and rewriting the reply -- and an ICMP
  error quoting a NAT'd packet -- back to the guest. The table is bounded
  and its entries expire; a full table drops new flows. The guest's tap turns
  both flags on when an owner opens `/dev/net/tap` (no new syscall, no
  writable sysctl). Proven in the harness by `net-route` (longest-prefix
  connected routing), `net-forward` (a datagram forwarded TTL-1-lower, a
  TTL-1 time-exceeded, a non-forwarding ingress that stays a non-router) and
  `net-nat` (UDP/TCP/ICMP round trips masqueraded and restored with valid
  checksums, an ICMP error translated back, the table bounded and expiring).
  A stock Linux guest with the tap as its gateway reaching the host's network
  is the `QEMU_MEM=2G` reproduction. The filtering firewall over this
  forwarding path is done (its own entry below); IPv6 NAT is a later unit.
  (Inbound port-forwarding, once listed here as next, is done -- see the
  entry below.)
- **The guest configures itself: DHCP and a DNS proxy (done):**
  `docs/audit/next-subsystem-dhcp-dns.md`,
  `docs/kernel-services/network/design.md` ("Autoconfiguring the guest").
  The guest reached the world only once configured by hand; now a stock
  guest with its DHCP client on learns everything from the host.
  `kernel-services/network/tapsvc.c` is the tap's autoconfiguration service,
  started when the VM attaches. Its **DHCP server** runs at the frame level
  (a tap input filter, since the guest has no address yet and a tap is
  `NETIF_NODEFAULT`, so a routed broadcast cannot reach it): it answers
  DISCOVER/REQUEST for the tap's single guest slot (`<subnet>.15`) with the
  gateway as router and DNS and a lease, NAKs a wrong address, refuses a
  second client, and sends each reply out the tap with `ether_output` per
  the client's broadcast flag at both layers (RFC 2131 §4.1). Its **DNS
  proxy** is a `ksock` UDP relay on `<gateway>:53`: it rewrites each query's
  transaction id to a value unique in a bounded, expiring table, forwards to
  the `fw_cfg` upstream (`opt/cosmo/resolver`) as the host's own traffic, and
  restores the guest's id on the answer -- record-type-agnostic, SERVFAIL
  with no upstream, dropping when the table is full. No new syscall, no
  writable control surface. Proven by `tap-filter` (the ingress/egress
  mechanism), `net-dhcp` (DORA, NAK, the broadcast-flag reply, a refused
  second client) and `net-dns` (a relayed round trip with id rewrite and
  restore, two queries sharing an id, SERVFAIL, the bounded expiring table).
  A stock Linux guest autoconfiguring `eth0` and resolving a name is the
  `QEMU_MEM=2G` reproduction; a general DHCP server, a caching resolver,
  DHCPv6, and DNS-over-TCP/DNSSEC are later units.
- **The guest is reachable from outside: inbound port forwarding (done):**
  `docs/audit/next-subsystem-dnat.md`,
  `docs/kernel-services/network/design.md` ("Inbound port forwarding: DNAT").
  Masquerade let the guest reach out; a service it *runs* was invisible.
  `nat.c` gains destination NAT: a static port-forward table (`fw_cfg`
  `opt/cosmo/portforward`, `proto:hostport:guestaddr:guestport`, a wildcard
  host-address bind, read on VM attach) maps a host port to a guest
  address/port. A TCP/UDP connection to the host on a forwarded port -- not a
  masquerade reply -- is rewritten to the guest and forwarded out the tap,
  authorized by the rule rather than `NETIF_FORWARD`; the guest's reply is
  rewritten back to what the client dialed (`host:P`), with precedence over
  masquerade, so `ipv4_forward` now runs `nat_out` on every natable forwarded
  packet. DNAT and masquerade share the one bounded, expiring conntrack table
  (a kind flag), so inbound state a remote can create is bounded as outbound
  state the guest can. No new syscall, no writable control surface. Proven by
  `net-dnat` (a client SYN forwarded to the guest, the SYN-ACK un-DNAT'd back
  from `host:P`, a UDP round trip, an unruled port kept local, the table
  bounded and expiring). A stock Linux guest running a service reached from
  the host is the `QEMU_MEM=2G` reproduction. The writable control surface
  and the filtering firewall are done (their own entries below);
  hairpin/NAT-reflection and IPv6 DNAT are later units.
- **Configuring the guest's network at runtime (done):**
  `docs/audit/next-subsystem-netctl.md`,
  `docs/kernel-services/network/design.md` ("A runtime network control
  channel"). Everything above was fixed at boot from read-only `fw_cfg`; this
  adds the one writable control surface the arc deferred. `/dev/net/tapctl`
  is a privileged character device (mode `0600`, separate from the frame
  channel): a `write` submits one versioned, fixed-layout `struct
  cosmo_netctl` command (`FORWARD_ADD` / `FORWARD_DEL`), applied whole or
  refused (short / unknown version / unknown opcode / out-of-range change
  nothing); a `read` returns the live rules as a versioned snapshot. The
  commands reach the port-forward table, now with `nat_pf_del` (reaps the
  rule's conntrack entries so a removed forward stops an in-flight flow),
  `nat_pf_list`, and a tightened `nat_pf_add` (the target must be on the
  guest tap's own subnet, and `(proto, host_port)` is a unique key, a
  duplicate `-EEXIST`). Contained as the read-only surfaces were: privileged,
  every command a range-checked struct, touching only the guest's forwards
  and never the host's own network; no new syscall. Proven by `net-tapctl`
  (add through the device takes effect and lists, delete reaps the flow,
  duplicate/off-tap/short/bad-version refused). `vmctl port-forward
  add|del|list` drives it; exposing and hiding a guest service on a running
  Linux guest is the `QEMU_MEM=2G` reproduction. The channel is designed to
  carry the tap's other settings (forwarding/masquerade toggles, the
  resolver) in later units.
- **From one guest to many: a tap per guest (done):**
  `docs/audit/next-subsystem-multiguest.md`,
  `docs/kernel-services/network/design.md` ("Many guests"),
  `docs/kernel-services/vfs/design.md` ("Per-open character devices"). The
  network served one guest; now every open of `/dev/net/tap` is a guest of
  its own. The VFS gains a per-open character-device lifecycle
  (`chrdev_ops.open`/`release` and a `priv` slot on `struct file`, release on
  the file's last reference); `/dev/net/tap`'s `open` creates a tap on its
  own subnet from a pool (`10.0.(3+k).0/24`, up to `TAP_MAX_GUESTS` = 8),
  its `release` stops the service, purges the guest's NAT state and destroys
  the tap, and a ninth open is refused. `tapsvc` is now an instance per tap
  (its DHCP lease and a DNS proxy bound to its own gateway); `nat.c` gains a
  per-guest quota, uplink-only masquerade (so guests reach each other with
  real addresses), and `nat_guest_purge`. `vmctl` and the guest are
  unchanged, so two `vmctl` runs are two machines. Proven by `vfs-chrdev-open`
  (the lifecycle) and `net-multiguest` (eight taps on eight subnets, the
  ninth refused, frame isolation, guest-to-guest un-masqueraded, a close that
  tears down and purges only its own). Two stock Linux guests at once is the
  `QEMU_MEM=2G` reproduction. The firewall forbidding inter-guest traffic is
  the next entry (done); an L2 bridge and per-guest limits beyond the NAT
  quota are later units.
- **A forwarding firewall: who may reach whom (done):**
  `docs/audit/next-subsystem-firewall.md`,
  `docs/kernel-services/network/design.md` ("A forwarding firewall"). The
  multi-guest unit routed guests to each other and deferred a policy
  forbidding it; this is that policy. `kernel-services/network/fw.c` is a
  stateful FORWARD-chain filter called from `ipv4_forward` after anti-spoof
  and routing and before NAT, so rules see the datagram as the guest sent
  it. Each guest owns an ordered first-match rule list (direction, protocol,
  destination prefix, destination port → accept/drop) and a default verdict
  per direction; the defaults close the deferred gap -- inter-guest **drop**,
  guest-to-uplink **accept**. It is stateful only where it must be: a
  masqueraded reply is delivered by `nat_in` and never re-enters
  `ipv4_forward` (its conntrack entry is its state), while a guest-to-guest
  flow -- un-NAT'd, both halves through the chain -- is recorded in a bounded,
  per-guest-quota flow table so its reply is admitted without a reverse rule;
  ICMP is stateful for echo only, on the echo id. A rule's identity is its
  whole match tuple (the control write returns only a byte count, so there is
  no id to hand back), ordered by an explicit index; rules bind to a guest by
  address and are attached/purged with the tap under one lock, so an add
  cannot race a teardown. `/dev/net/tapctl` moves to ABI version 2 with
  `FILTER_ADD`/`DEL`/`POLICY` and a filter section appended to the snapshot;
  `vmctl filter add|del|policy|list` drives it; no new syscall. Proven by
  `net-firewall` (default drop, one-rule hole, stateful return incl. echo by
  id, first-match ordering and delete-by-tuple, a rule outliving its handle
  but not its guest, the listing round trip and every rejection). The INPUT
  chain, the host chain and the OUTPUT chain are done (their own entries
  below), so all four chains exist; rate-limit/log targets, IPv6 and full
  TCP state are later units.
- **The INPUT chain: what a guest may ask of the host (done):**
  `docs/audit/next-subsystem-input-chain.md`,
  `docs/kernel-services/network/design.md` ("The INPUT chain"). The
  forwarding firewall decided which *other machines* a guest may reach; this
  decides which of the **host's own services** it may reach. Before it,
  `ipv4_input` handed everything `nat_in` declined straight to the host's
  ICMP/UDP/TCP handlers -- a guest could reach any host listener through its
  gateway (or the host's uplink address) and forge its source doing so. Now
  `fw_input_verdict` runs for a guest tap's datagram to the host, **after
  `nat_in` declines** and before the transport demux: the anti-spoof first
  (the source must be the tap's guest), then the guest's `TO_HOST` rules --
  a **third direction on the same engine**, not a second rule list -- else
  its `TO_HOST` default, **DROP**. The tap's own services are **seeded as
  real, deletable rules** at attach (`udp gateway/32 :53`, `icmp gateway/32
  type 8`), not hard-coded holes; stateless, since the host's reply passes no
  filter and later segments match by port. **The ICMP selector is a type**:
  for ICMP a rule's `dst_port` is the ICMP type (`0xffff` = any), so the echo
  seed admits echo-request alone and a guest's echo reply or
  Need-Fragmentation cannot reach the echo hook or `ipv4_pmtu_update`. ABI
  version 3 (`DIR_TO_HOST`, `policy_to_host`, the type meaning); `vmctl
  filter ... host ...` with a protocol-aware selector. Proven by `net-input`
  (seeds reach the host and nothing else does, a rule opens a port per
  datagram, the seeds are deletable, echo reply and need-frag dropped by type,
  forged sources dropped as spoofed, per-guest and flippable policy, the
  listing round trip). The host chain for the uplink and the OUTPUT chain
  that filters the host's replies to guests are both done (their own entries
  below).
- **The host chain: what the world may ask of the host (done):**
  `docs/audit/next-subsystem-host-input.md`,
  `docs/kernel-services/network/design.md` ("The host chain"). FORWARD
  decided which machines a guest may reach and INPUT which of the host's
  services a guest may reach; this decides which of the host's services the
  **world** -- anything arriving on the uplink -- may reach, the fourth and
  last ingress→egress pair the stack serves short of the host's own egress.
  The same engine, consulted from a third place: the **host is a policy
  object** (`guest_addr 0` on the existing control channel, permanent, kept
  outside the guest table so no datagram's source can name it), holding
  rules of a **fourth direction**, `FROM_UPLINK`, that alone may carry a
  **source prefix** -- the field a world-facing rule cannot do without and
  the one a guest's rule may not have. `fw_host_verdict` runs in `ipv4_input`
  for the uplink's datagram to the host **after `nat_in` declines**, so a
  DNAT'd connection is never re-gated (now provable: a port-forwarded SYN
  reaches the guest under a host default DROP), with default **ACCEPT** --
  today's behaviour; the operator drops by source/protocol/port or flips the
  default. One thing closes with no rule: the **off-link invariant** -- a
  datagram arriving on any non-loopback link must be for that link's own
  address or a broadcast, else `rx_offlink`; this shuts the per-guest DNS
  proxies that were open resolvers from the real network and the
  loopback-bound services the uplink could reach (the martian check looks
  only at the source). And a DROP is **quiet**: the chain never models TCP
  state -- a DROP on TCP/UDP marks the datagram `M_FW_QUIET` and the
  transport, which owns acceptability, delivers it only into an existing
  connection or a connected UDP socket and **answers nothing** otherwise: no
  SYN-ACK, RST, challenge ACK, window ACK or port-unreachable. In TCP the
  gate is `batch_send`, the sole emitter, freeing a still-quiet batch; the
  `quiet` bit clears at five acceptance points found by walking every `goto
  out` in `tcp_input` (after the last rejection check, so dup-ACKs, window
  updates, data and FINs keep their output; the `SYN_SENT` completion, so
  the host's own connect completes; a valid reset; the SYN-cache completion;
  a retransmitted FIN in `TIME_WAIT`), the RFC 5961 budget and the keepalive
  clock are touched only for an accepted segment, and no SYN-cache entry is
  made under the flag. ABI version 4 (`DIR_FROM_UPLINK`, `src_addr/src_prefix`,
  `policy_from_uplink`, the host record listed first); `vmctl filter ... host
  world PROTO SRC DST PORT VERDICT`. Proven by `net-hostinput` (default
  accept, a sourced drop by prefix, quiet delivery on established and
  outbound connections against every probe shape and every rejection check,
  no budget or keepalive side effect, dup-ACK/window-update/data/FIN output
  kept, a valid reset applied, ICMP by type, off-link drops, DNAT never
  re-gated, scope refusals, the listing round trip) with fourteen bug-proofs.
  Its reply state and the OUTPUT chain are done (their own entries below);
  per-interface host chains, rate-limit/log targets and IPv6 are later
  units.
- **The host's own flows: reply state for the host chain (done):**
  `docs/audit/next-subsystem-host-state.md`,
  `docs/kernel-services/network/design.md` ("The host's own flows"). The
  host chain shipped with default **ACCEPT** because it had no reply state,
  and three of the host's own facilities depended on that: the **DNS proxy**
  (its upstream socket is unconnected *by design* so it can authenticate the
  sender, so quiet delivery's connected-socket rule freed the answer and
  every guest would have lost DNS under a hardened host), the host's **own
  pings**, and its **TCP path-MTU discovery** (the Need-Fragmentation errors
  were freed before `icmp_needfrag` saw them, blackholing large segments).
  This is that state, in the shape the stack already had: `fw_host_record`
  records the host's **UDP sends and ICMP echo requests** in the FORWARD
  chain's flow table at **`ipv4_output`** -- the one door every
  host-originated datagram passes and no forwarded one does -- when the
  egress is a real, non-guest link, and `fw_host_verdict` consults that
  state **before its rules**, admitting the **reverse** tuple as the FORWARD
  chain does. **TCP is deliberately not recorded** (quiet delivery already
  admits its segments through the connection), so `g_fw_lock` stays off the
  uplink's hottest send path, and a one-entry cache refreshes the repeated
  tuple without walking the table. The table is split by share --
  `FW_FLOW_GUEST_POOL` 256 (the guests' 8 x 32, unchanged) plus
  `FW_FLOW_QUOTA_HOST` 64 -- so a flood starves only its own initiator, and a
  send is never refused for the firewall's sake. **Exactly one tuple** is
  opened per flow and intent is not modelled: a second datagram on it inside
  the window is admitted too, the socket's own validation being the second
  line. **ICMP is the consumer's decision, not the firewall's**: under a DROP
  every ICMP message is delivered `M_FW_QUIET` to `icmp_input`, the single
  dispatch point, which runs *only* the Need-Fragmentation path -- and that
  path already accepts nothing TCP does not confirm (RFC 5927) -- freeing
  everything else (`icmp_quiet_dropped`) without building a reply or
  spending the host-wide echo-reply budget. No ABI change and nothing to
  configure: state is earned by what the host sends. Proven by
  `net-hoststate` (an unconnected client's reply, one tuple and its three
  negatives, the host's ping, a refused echo request answering nothing,
  path-MTU discovery under DROP, the DNS proxy end to end, refresh and
  expiry, forwarded and loopback sends recording nothing, the share, and the
  hardened default) with thirteen bug-proofs. Reply state for a UDP flow's
  ICMP errors, per-interface host chains, rate-limit/log targets and IPv6
  are later units; the OUTPUT chain and the listing of live flows are done
  (their own entries below).
- **The OUTPUT chain: what the host itself may send (done):**
  `docs/audit/next-subsystem-output-chain.md`,
  `docs/kernel-services/network/design.md` ("The OUTPUT chain"). The
  filter's fourth and last chain, and the only one whose subject is this
  machine: with it, every path through the stack is under policy --
  guest→guest, guest→world, guest→host, world→host and now host→anywhere.
  `fw_output_verdict` runs in `ipv4_output`, the one door every
  host-originated datagram passes, **after the route** (the egress is the
  scope a rule may name) and **before the link**, and a DROP frees the
  datagram, counts `ip_stats.tx_filtered` and returns **`-EPERM`** -- the one
  chain whose refusal is *spoken*, because the refused party is a local
  socket that already reads errors, where the other three hide the host from
  strangers and silence is the point. `udp_sendto` and `icmp_send_echo`
  propagate it; **TCP stalled instead**, since `batch_send` ignored output
  errors -- documented, asserted and left to its own unit rather than
  half-built, and that unit is the next entry. One new direction, `FW_DIR_OUTPUT`, on the host object alone,
  plus an egress **scope** in the rule (`world` / `guest` / `any`): a
  destination prefix can name a guest's subnet, but the tap pool reassigns
  `10.0.(3+k).0/24` as guests come and go, so a prefix rule follows whoever
  inherits the subnet while a scope keeps meaning what it said -- which is
  what finally makes "the host may not answer guests on this port"
  expressible, the case the INPUT and host chains both named and neither
  could say. The **source is resolved once** in `ipv4_output` and handed to
  the verdict, the host chain's flow read and `output_on`, so a
  source-prefix rule judges the address the wire carries rather than the
  zero an unbound sender passed. `nat_in`'s deliveries to guests pass this
  door too and are scope-guest traffic -- intended, and the unit's sharp
  edge. Loopback passes no chain; the default is **ACCEPT**, argued from
  what a default DROP would cut. ABI version 5: the `scope` byte comes from
  the filter command's and rule record's reserved bytes (no size change)
  while the per-guest policy record grows 8 → 12 for `policy_output`, since
  version 4 spent its last spare. Proven by `net-output` (the default, the
  `-EPERM`, all three scopes and the scope as identity, the host's reply to a
  guest silenced, no reply state for a refused send, a non-verdict failure
  not counted as one, loopback exempt, TCP's documented stall, a DNAT'd
  delivery, the hardened default, the scope discipline and the control round
  trip) with nine bug-proofs -- including the two that keep the fast path
  honest, a rule and a policy flip each having to invalidate it -- and one
  honest non-proof: the verdict's position relative to the flow read turns
  out not to be observable, because the flow record is already conditional on
  the send succeeding. A verdict TCP's callers can see became the
  next unit; per-interface chains, rate-limit/log targets, IPv6 filtering and
  full TCP state tracking are later ones.
- **A verdict TCP's callers can see (done):**
  `docs/audit/next-subsystem-tcp-verdict.md`,
  `docs/kernel-services/network/design.md` ("A refused segment is the
  connection's business"). The OUTPUT chain's own incompleteness, closed:
  that unit made a refusal *spoken* for `udp_sendto` and
  `icmp_send_echo` and left TCP deaf, because `batch_send` -- the sole
  emitter, and by design the only code that touches the link after the
  per-connection lock is dropped -- discarded `ipv4_output`'s return. A
  rule that refused a connection's segments was therefore invisible to
  `connect`, `send` and `poll`, and the socket waited out eight
  retransmissions with a doubling RTO, **about three minutes**, to be told
  `-ETIMEDOUT`: a network that did not answer rather than a machine that
  decided not to ask. `batch_send` now returns what became of the batch and
  `output_result` carries a refusal back into the connection after the
  flush, under the connection's own lock with nothing else held. **Only
  `-EPERM`** counts, and only because it is the chain's verdict: a rule
  matches the whole tuple and every segment of a connection carries the
  same tuple, so a rule that refused one will refuse them all -- a decision,
  not the loss the retransmit timer repairs -- which makes `-EPERM` part of
  the interface between the IP layer and TCP and leaves every other output
  error discarded as before. What follows depends on the state, in RFC 1122
  §4.2.3.9's shape: an **opening** connection is aborted (the same three
  lines a valid reset uses, which is also what wakes a blocking `connect`,
  whose wait watches the state and not the error), a **synchronized** one
  **records** the verdict and is left standing -- the peer's half still
  arrives, and a rule the operator deletes a moment later should leave a
  connection to resume, which it does because **a flush in which a segment
  reaches the link clears a recorded verdict**. `tcp_connect` is the one
  site that *returns* the refusal rather than only recording it, its caller
  being right there, so a nonblocking connect fails outright instead of
  reporting an open already abandoned. Seven flush sites own a connection;
  five were syscall-context functions with no unwind pair and each keeps its
  own return contract -- `tcp_send` keeps the count it accepted, because
  those bytes are queued and will be retransmitted, so the verdict is read
  by the next call. The unit's sharpest finding is that **a refused SYN-ACK
  has nothing to tell**: a passive open's half-open lives in the listener's
  SYN cache and the child pcb is only created when the ACK completes, so
  `SYN_RCVD` comes only from a simultaneous open -- there is no connection
  to abort and no caller who has heard of it, and what is left is hygiene,
  dropping the entry instead of holding one of sixty-four slots for eight
  seconds for a connection the machine has decided not to answer. Two
  consequences are stated rather than hidden: a recorded verdict reaches
  `recv` too, once the bytes already buffered are drained, because a pending
  error belongs to the socket and not to a direction; and an application
  cannot send its way out of one, so the clearing segment is always the
  timer's or one the peer's own traffic asks for. Proven by `net-tcpverdict`
  (nine steps, one uplink tap) with eight bug-proofs, and one
  discrimination **argued and not proved** exactly as the report promised in
  advance: no TCP segment can reach a non-verdict output error in this
  stack, so keying the helper on any negative error changes no test's
  outcome -- the experiment was run and changed none. Per-interface chains,
  rate-limit and logging targets, IPv6 filtering and full TCP state
  tracking in the filter remain later units.
- **Native threads and a futex (done):**
  `docs/audit/next-subsystem-threads.md`,
  `docs/kernel/process/design.md` (§12 "Native threads"). The one place
  the compat layer was a **superset** of the machine rather than a
  translation of it: this kernel has run threads across every CPU since
  milestone 10, with per-thread signal masks and a futex, and a *native*
  program could reach none of it -- the only door was the Linux
  personality's `clone(CLONE_THREAD)`, so a `musl` binary with
  `pthread_create` could use every CPU in the machine and a CosmoOS binary
  could not. Five system calls close it and add no mechanism:
  `SYS_thread_create` (a `struct cosmo_thread`, as `SYS_spawn` takes a
  `cosmo_spawn`), `SYS_thread_exit`, `SYS_thread_self`, `SYS_futex_wait`
  and `SYS_futex_wake`; `SYS_exit` needed no change, having meant *the
  process* since `process_exit` and `process_thread_exit` became two
  functions. **Joining is not a system call**: `clear_tid` names a word the
  kernel fills with the new thread's id before it can run and zeroes and
  futex-wakes at exit, so a join is a futex wait in libc, the kernel keeps
  no table of unreaped threads, and the same word answers "has it
  finished?" with no call at all. Creation is ordered by its failure modes
  -- everything before the link fails with nothing created, everything
  after it with `process_thread_abandon` -- and the **entry contract** is
  part of the interface, a thread being entered at a function with no
  `call` having happened: x86-64 gets a zeroed return slot so the entry
  sees `rsp % 16 == 8` as SysV promises, AArch64 gets `sp` at `stack_top`
  and `x30` zero, and a `return` therefore faults at address 0 and ends the
  process, which libc's trampoline is there to prevent. The unit's real
  finding was where a *behaviour* lived: the `clear_child_tid`
  zero-and-wake was the Linux personality's `thread_exit` hook, so a native
  thread's joiner was never woken until it moved into
  `process_thread_exit` -- the "second door" lesson from a third side, not
  a check missing from the front door but work the side door owned
  privately. Two more corrections came from building it: the stack probe is
  **uniform across architectures**, because with it x86-only AArch64
  accepted a create with an unmapped stack and killed the thread on its
  first push; and **libc was not thread-safe inside** -- invariant L8 had
  named this day in advance, and two thirds of what it asked for landed
  here after review pushed back on merely documenting the hazard: the
  **allocator** and **stdio** now take one lock each (an unlocked free
  list is the one hazard that corrupts memory silently, and a whole
  `printf` is one critical section), while **`errno` stayed one global**
  until the unit below, because per-thread `errno` needed a thread pointer
  the machine did not have. Proven by
  `tests/native/thrtest` -- from userland, because a kernel
  self-test cannot create a *user* thread -- with eleven bug-proofs, four of
  which sent the test back for a stronger assertion rather than the code. `SYS_mprotect`, futex
  requeue and per-thread signal targeting were later units, and all three have since landed (#195 and the native thread door unit); `vmctl`'s
  conversion to a thread per vCPU, which was the reason for all of this,
  landed in the unit two entries below.
- **A thread pointer, and `errno` per thread** (`docs/audit/next-subsystem-errno-tls.md`,
  `docs/kernel/process/design.md` §13). The last third of what invariant L8
  owed. The kernel had kept a thread pointer per thread all along and
  `SYS_thread_create` took one for a *new* thread, but nothing let a thread
  that already exists set its own -- so a process's first thread could never
  have one. **`SYS_set_tls` (87)** is that and only that, and libc keeps a
  128-byte block behind the pointer holding `errno` and a cached tid: a
  `.bss` object for the first thread, installed before `__stdio_init` and
  before `main`, and the page above each created thread's stack, freed by
  the join's own `munmap`. `errno` becomes `(*__errno_location())` and all
  **25 writers across ten files compile unchanged**, because every one
  assigns through the name. There is deliberately **no fallback** for a
  thread without a block: on x86-64 there cannot be one, since reading
  `%fs:0` with a zero base faults at address zero before any check could
  run -- so the contract is that a thread made by a raw `SYS_thread_create`
  with `tls = 0` must not call libc, and `cosmo_tcb_install` is its way out.
  Two things this unit taught, both from its own failures: the block needs
  `aligned(16)` **on the type**, because the struct leads with a pointer and
  one program out of the suite landed at an 8-mod-16 address -- caught in
  one line by the exit-127 branch that "cannot happen"; and the `self` word
  is load-bearing on exactly one architecture, which only a *pair* of runs
  shows (unwritten, AArch64 passes everything and x86-64 loses every process
  to SIGSEGV). Review added a third: **`SYS_set_tls` must be always-allowed
  by the syscall filter**, beside `exit`, `sigreturn` and `thread_exit`,
  because every native program installs its block before `main` -- a filter
  omitting it killed every child of a filtered process during startup, and
  the inherited-filter test could not see it, since its child is *expected*
  to die of `SIGSYS` and a death in startup wears the same status. The
  kernel log said `number 87 is outside its filter` where it used to name
  the call the test was about; `init --filter inherit-start` now asserts
  that a child of a filter naming only spawn and wait reaches its own
  `main`. The tid cache became **lazy** for the same reason: read during
  startup it made installing the pointer two syscalls, the second also
  denied. Proven by `thrtest` steps 12-16 and six bug-proofs, one of
  which failed to fail and sent the test back for an assertion about the
  *layout* rather than about `errno`, plus one more for the filter. `strerror` and `getcwd(NULL)` stay
  shared -- now fixable, since there is somewhere per-thread to put them --
  and compiler `__thread` with ELF `PT_TLS` remains a later unit that this
  one is the prerequisite for.
- **A thread per vCPU, and a way to stop one** (`docs/audit/next-subsystem-vcpu-threads.md`,
  `docs/kernel-services/virtualization/design.md`). The consumer the thread
  arc was built for. `vmctl --machine` ran a guest's vCPUs on **one thread,
  a tick each**, and two corrections existed only because of that: an array
  tracking which vCPU had never had a turn end on its own terms, and a
  `SYSTEM_OFF` **held for up to 64 turns** while such a sibling ran. The
  design document already called them what they were -- "not PSCI
  semantics" -- and both are now gone: each vCPU has its own thread and runs
  untimed, so a four-CPU guest gets four CPUs' worth of progress instead of
  four slices of one thread's, and `vmctl: peak concurrent vcpus: 2` is a
  required marker that the round-robin could never have produced.
  **The threads were the cheap half.** Nothing could stop a vCPU already
  inside `arch_hv_vcpu_run` except a fatal signal to the whole process, so
  the unit adds the kick KVM has and this tree did not: **`SYS_vcpu_stop`
  (88)** sets a sticky flag and IPIs the host CPU the vCPU is on, and the
  run leaves with `COSMO_VM_EXIT_STOPPED` having done nothing to the guest.
  Its handshake (`kernel/hvkick.h`) is **Dekker's** and is `SEQ_CST` on both
  store-load pairs, because release/acquire permits exactly the reordering
  that lets both sides miss -- the one place in this design where the
  cheaper ordering is wrong in a way no amount of testing on one
  architecture would show. The publication lives in the arch backends,
  inside the interrupt-disabled region around the entry, because that is
  what makes a late IPI stay pending and become an exit rather than a
  missed kick.
  The lifecycle is a word per vCPU -- parked, starting, running, quit -- in
  which **quit is absorbing** and parked and running cycle, because a
  `CPU_OFF` is not the end of a vCPU and its thread goes back to its park.
  Every transition is a compare-and-swap, so a `CPU_ON` racing a
  `SYSTEM_OFF` cannot revive a vCPU nobody waits to stop, and shutdown both
  **waking** the parked threads
  and **kicking** the running ones, because the kick reaches a thread inside
  the kernel and nothing else. The main thread is a supervisor rather than a
  joiner: `join` blocks, and the console ring drops its oldest bytes, so a
  main thread that joined first would lose the guest's output.
  Nine review rounds on the report caught **ten design errors before any of
  it was built**, seven of them in the lifecycle -- including two deadlocks
  and the memory-ordering bug above. **Building it, and reviewing the
  build, found six more** -- three of them bugs in the first draft of the
  threaded owner, which is the honest number:
  *(a)* MMIO answers were **erased**, because the run's exit structure
  carries the answer to the last exit *into* the next run and the new loop
  zeroed it each time round, so every virtio register read completed as
  zero -- and nothing gated catches that, since the machine-mode guests use
  no virtio;
  *(b)* `CPU_ON` wrote its target's registers **before** asking whether the
  target was already running, which is why the lifecycle gained a
  `STARTING` state to claim a vCPU before writing it;
  *(c)* `CPU_OFF` left the vCPU **unstartable**, exiting its thread and
  leaving the word `RUNNING`, so a later `CPU_ON` answered `ALREADY_ON`
  forever -- the run set this replaced allowed a restart, and the threaded
  version had silently taken it away;
  *(d)* `CPU_ON` had to become **synchronous**, and then twice over: waiting
  for the target's *thread* still lost `cpu1: up` on a loaded CI runner,
  because a thread can exist without its guest having executed. It now
  waits for the first run to have *returned*, which is what PSCI already
  meant by "powered on and executing" -- and the CI failure was turned into
  a local proof by standing a 40 ms sleep in for the loaded host;
  *(e)* a second stop-check in the run loop was **redundant** and deleted;
  *(f)* **removing the kick's IPI entirely fails no test**, because a host
  timer tick exits a spinning guest anyway -- liveness comes from the tick
  plus the sticky flag, and the IPI buys promptness. Proven by
  `guest_dtb`, `guest_offspin` at `-c 2` and `-c 3`, `guest_psci_race`, the
  peak-concurrency marker and the `hv-vcpu-stop` self-test, with bug-proofs
  for each -- and two of those proofs recorded as *not* failing on demand,
  which is the part a reader needs most. The device models take one lock
  each, taken by whoever enters from outside and never held across a run;
  testing them under two guest CPUs needs a guest-side virtio driver and is
  named as its own unit.

- **The reference a path walk never takes**
  (`docs/audit/next-subsystem-cwd-ref.md`). Every relative-path system
  call read `process_current()->cwd` with no lock and **no reference**,
  then handed the raw pointer to a walk that can block, while another
  thread's `chdir` swapped it and dropped what could be the last
  reference -- and `vnode_put` on the last reference unhashes the vnode
  and frees it, the mount's hash being a weak cache that holds none of its
  own. `process_cwd_get()` returns it referenced and
  `process_cwd_snapshot()` returns it **with its path from one
  acquisition**, which is what `chdir` and `spawn` need because they
  publish both; taking the two separately is correct twice and wrong
  together, leaving a process that reports one directory through `getcwd`
  while resolving relative paths in another.
  **Renaming the field to `cwd_locked` found more than the report's
  `grep` had**: twenty unsafe sites in three files rather than sixteen in
  two, including all of `kernel/process/spawn.c`, which every process
  creation goes through and which already contained the
  publish-two-reads defect a review had just found in this unit's
  *design*. Three sites already had the discipline -- both doors' `getcwd`
  and the child-inherits-the-parent's-cwd path -- so the unit finishes a
  rule rather than inventing one, and invariant **P29** states it with a
  census of which per-process fields are mutable.
  **Two claims in the report were wrong and are corrected in it.** On
  `ramfs` a directory entry pins its child, so a `chdir` race alone frees
  nothing -- reaching the free needs the directory *removed* too, measured
  as zero frees in the first three test steps and 228 in the fourth. And
  the frame poisoner it named cannot see a `kzalloc`'d vnode.
  **The test is a regression, not a proof**, which is said plainly: every
  reverted-fix run passed. (That entry also said a run with each freed
  vnode poisoned passed, and explained it by the walk never being inside
  the few instructions where the free lands. The cwd-hold unit below
  measured otherwise: with the poison at `vnode_release`, the test
  catches the bug in some boots -- a rate, not a proof, and not never.) Two of the test's own designs could not have failed
  either -- `../X` discards the component the paths differ in, names
  differing in one byte cannot show a tear, and the coherence step's
  sibling moves could not express the two-acquisition defect at all since
  normalising and looking up `../NAME_A` from either sibling both give
  `DIR_A`. A fourth failed on a *correct* kernel, because two threads
  issuing relative moves can walk the process out of the subtree. All four
  fixed; the proofs still pass, which is now evidence about the window
  rather than about the inputs -- a distinction that only became available
  once the inputs were right. The fix stands on its ordering argument, and the seam that
  would prove it is named.

- **The wait, written once** (`docs/audit/next-subsystem-condvar.md`).
  `cosmo/thread.h` could start a thread, end one, join one and exclude
  one, and had no way to **wait for something another thread will do**.
  `cosmo_cond_t` is one word -- a sequence number every signal increments
  -- with `wait`, `timedwait`, `signal` and `broadcast` over the futex the
  threads unit already built. **No kernel change and no new syscall**:
  `SYS_COUNT` stays 89, `futex_wake`'s count was already unbounded so a
  broadcast is one call, and a `timeout_ns` of 0 already meant no timer.
  The report offered that as a falsifiable check on its own design and it
  held.
  **The lost-wakeup guarantee lives in a window no arrangement of threads
  can reach**, and two drafts of its test proved it the hard way: the
  first had the signaller finish before the waiter ever called
  `cosmo_cond_wait`, and the second relied on the wait's own `unlock` to
  release a blocked signaller -- which makes that thread *runnable*, not
  *running*, so the waiter reaches `futex_wait` first. libc therefore
  ships a one-shot probe, `__cosmo_cond_probe`, NULL in every real program
  and **taken with an atomic exchange rather than read** so it cannot fire
  inside a later test's waiter. It is compiled in unconditionally, because
  a seam that exists only in a test build proves things about a binary
  nobody runs, and the §70 gate weighs what a writable function pointer in
  every process costs against that. The evidence it was worth it: the same
  read-after-unlock bug that hangs the seam-armed test for the full
  180-second deadline **passes in 75.9 s** when the test uses a signaller
  thread instead.
  **The report's premise for its other caller was wrong.** It counted five
  hand-rolled waits and committed to converting four; three were
  `vmctl`'s, and all three are `futex_wait` on a **single atomic word**
  (`park[cpu]`, `ran[c]`, `live`) with lock-free compare-exchange around
  them -- which is the futex's own job, not a hand-rolled condition
  variable. That was measured, not assumed: the simplest was converted,
  built and booted before being reverted at **+14/−5**, six lines becoming
  fifteen and a mutex appearing around a lock-free counter, with no
  correctness or clarity gained. `vmctl` is unchanged; the two waits in
  `thrtest` that were genuinely wrong -- a 2,000,000-iteration loop count
  standing in for a duration, and an unbounded `cosmo_yield()` spin -- are
  gone. Converting them also exposed a wrong assertion in the unit's own
  first draft: it checked that `signal` wakes **exactly one** waiter,
  which the interface does not promise, since spurious wakeups are
  permitted and the number of threads a signal makes runnable is not
  observable without a race. It asserts the guarantee a caller depends on
  instead -- one ticket of work offered to four waiters is taken exactly
  once, however many wake. Invariant **L9** states both halves of the
  contract, and `thrtest` steps 18 to 22 check them.

- **`__thread`, and the TLS image a program brings with it**
  (`docs/audit/next-subsystem-pt-tls.md`). The unit the per-thread `errno`
  was built as a prerequisite for. The compiler emitted thread-local
  storage all along -- local-exec relocations, `.tdata`/`.tbss` -- and
  nothing in the system loaded it. **The report's central claim was wrong
  and building it is what showed that**: it said such a program would
  "link, load and run" with silent corruption, and in fact `ld.lld`
  refuses the link, because `user.ld` declared no `PT_TLS` for the
  `STT_TLS` symbols to live in. I had verified that the *compiler* emits
  the relocations and asserted the link would succeed without trying it.
  The failure was the good kind, loud, and the unit is a feature to add
  rather than corruption to stop.
  **The kernel learns nothing about thread-local storage.** The native
  auxiliary vector gains the standard `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`
  trio, from values `elf_info` already computed for the Linux door, and
  libc reads its own `PT_TLS` -- no new syscall, no new structure, and the
  loader untouched. Two earlier drafts carried the template worse: one grew
  `struct cosmo_procinfo`, which would have overflowed an older binary's
  buffer since `sys_procinfo` writes `count * sizeof` with the kernel's
  `sizeof`; one declared ELF's 64-bit sizes as `uint32_t`.
  The image is placed per architecture because the ABIs differ:
  `[ image ][ block ]` with variables *below* the thread pointer on
  x86-64, and `[ block ][ ABI head ][ image ]` with them *above* on
  AArch64 -- where the first variable sits at TP+16, inside the
  `reserved[112]` the previous unit had promised to programs and called
  permanent. That promise is withdrawn and replaced: `__thread` is how a
  program gets per-thread storage now.
  **What proves the offset formula is an initialiser, not the ABI
  documents**: `__thread int x = 0x5eed` read back in four threads, since
  the linker resolved that address relative to the thread pointer and only
  reading it back shows that libc and the linker agree. Four more things
  only running could find: the startup order gained exactly one exception
  (the auxiliary vector must be found before the block can be sized, and
  the errno unit's bug-proof keeps everything else after it); an **empty**
  `PT_TLS` is the common case, with `memsz` 0 and `p_align` 0, and
  refusing that alignment killed every program in the system; the first
  thread's static storage is deliberately **generous**, because libc's own
  `strerror` buffer is `_Thread_local` and an exact block would make every
  program `mmap` at startup -- which a syscall filter that does not name
  `mmap` would turn into a dead process, the same trap `SYS_set_tls` fell
  into one unit earlier; and the kernel's `mmap` wants a page multiple,
  which every other caller in libc satisfies by accident.
  Invariant **L8 is finished**, and one of its two remaining items never
  needed doing: `getcwd(NULL)` `malloc`s per call and hands the buffer to
  its caller, so it was safe from the moment the allocator took its lock --
  the invariant named it for two units after that stopped being true.
  `strerror`'s was the only libc function returning a pointer to a
  static, which a `grep` now confirms rather than a list kept by hand.
  Review corrected a broader version of that sentence: it is **not** the
  only writable static left. `atexit`'s table and the environment remain
  process-global and unsynchronised, which is a gap now named in
  `docs/libc/invariants.md` and stated to callers in `cosmo/thread.h`,
  and which moving `strerror` behind the thread pointer could not fix
  because it is process state rather than per-thread state. The header validation is a
  pure function in a file of its own, tested on the host with
  **seventeen** tables no linker would emit -- where the report had
  proposed a single crafted binary in the boot archive.
  **Review then found three defects in the built code and a fourth in the
  build.** A `PT_TLS` whose `p_vaddr + p_filesz` *wrapped* the address
  space passed the containment test, because that test is an ordering of
  sums and a sum that wraps defeats it -- and the accepted `p_vaddr` was
  what `memcpy` read from; `cosmo_tcb_storage()` charged for one of the
  placement's two roundings, so a program aligned more strictly than the
  block's own offset had `cosmo_tcb_install` refuse exactly the size the
  function documents; and a missing program-header table was read as "this
  program has no TLS" rather than as "this cannot be answered", which is a
  process whose `__thread` variables are never initialised and whose
  storage was sized as if it had none. The fourth is the one worth
  remembering: **three of those bug-proofs passed with the bug
  reintroduced.** `tests/host/test_libc.c` is one translation unit that
  `#include`s the sources it tests, and `host.mk` listed its prerequisites
  by hand -- a list that had fallen one behind, so editing `tlsscan.c`
  rebuilt nothing and `host-test` re-ran the previous binary and said ok.
  The list is now `-MMD` output: a list nobody maintains cannot fall
  behind.

- **The suite waits for the property, not for time**
  (`docs/audit/next-subsystem-suite-waits.md`). Eight self-test failures
  across five tests in one day, every one on correct code: `nettest.c`
  slept a fixed interval and then counted, and on a loaded host the count
  was about the host. **The report's proof lever was wrong and the first
  step found out**: slowing the host stretches the sleep and the work
  together and tips nothing; slowing the *work* -- a delay per frame in
  `lo_transmit`, injected by the proof and restored -- fails the
  unconverted tests at exactly the two lines that flaked and passes the
  converted ones, which wait longer and get the right answer. `settle` is
  gone: twenty-four `wait_until` sites over twelve predicates, each on a
  single unsynchronised field or a monotone sum of monotonic ones, the
  multi-field claim left *after* the wait, the result `CHECK`ed under
  `warn_unused_result`, and a wait past half its budget reported. Review
  found four waits that returned before the thing asserted -- "scheduled"
  where "delivered" was meant, a stale baseline, dropped expiries, a
  two-field predicate against the file's own rule -- the same defect one
  level up, all fixed. **The ten time bounds were classified one at a
  time**: four restated into something observable (parallelism watched
  from CPU 0 instead of a ratio of counts; the wake IPI counted on its
  target instead of a 2 ms latency, and removing that IPI is noticed by
  nothing else in the suite; clock pairs bracketed and re-read instead of
  tolerated; the kicker joined and the run's return ordered after its
  kick), four removed because the failure they named is a hang the
  watchdog already reports, and two widened and labelled `LOAD-SENSITIVE`
  (`sleep`, `el2-guest-timer-ontime`) -- which, with the limiter test's
  residual below, are the three entries on
  `docs/testing/flakes.md`, which says what each asserts, why nothing
  observable replaces it, what is deliberately not listed, and the rule
  for joining. The boot harness reads that table and names a failing test
  against it in its report -- a label, not a retry, and a missing or empty
  list is reported too. **As run, the twenty-boot repetition the report
  priced at an hour found one more**: `net-icmp-limit` once in forty boots,
  at the limiter line -- the ICMP limiter's fixed one-second window had its
  boundary inside the burst, a phase the test never controlled and the
  conversion had carried over intact. The test now makes the phase known
  (fill the window and probe until an echo is refused, then until one is
  replied, flood into the fresh window) and waits on echoes *decided*, not
  received -- and, after review caught the first draft taking any accepted
  probe as a fresh window, a probe must be *refused* before the accepted
  one counts. Forty boots on the fixed tree, twenty per architecture, all
  pass; forty more on the refusal-first tree pass every assertion, with one
  x86-64 boot tripping `net-bench`'s 8 s budget at 71 s with normal
  throughput -- a slowness between the bench's rounds that this unit names
  as a follow-up rather than lists as a flake. Suite time is unchanged
  within its ±2 s spread.

- **A wake that preempts** (`docs/audit/next-subsystem-wake-preempt.md`).
  Every wake in the kernel happens under an interrupt-disabling spinlock,
  and `spin_unlock_irqrestore` re-enables preemption *before* interrupts,
  so the documented preemption point at `preempt_enable` never fired for
  a same-CPU wake: a woken higher-priority thread ran at the next tick,
  up to 4 ms later, at all fifty-three wake sites. `preempt_point()` is
  the same four-condition predicate at the other moment it can become
  true, called from each architecture's `arch_irq_restore` after the
  enable -- invariant S8's fourth point, covering the wait-queue wakes,
  the thirteen direct `sched_wake` callers and the bare interrupts-off
  regions alike, with no change at any unlock. Three kernel tests read
  the waker's flag from the waiter's first statement (the waiter now
  runs 20-77 µs after the wake, before that statement), and a debug
  sysctl whose *read* is the system call under test shows a wake made
  inside a call preempting before the call returns -- read, because this
  kernel's sysctl is read-only, one of the as-built differences the
  report records. Proved by removing the point (all four fail on both
  arches), placing it before the enable (the same), and putting it in
  the unlock only (exactly the bare-region test fails). **Then the
  decision milestone 8 left**: the network worker ran at 40, below
  default, which is why `net-bench` delivered 512 of 10 000 UDP sends on
  every boot; five boots per setting per architecture under the report's
  rule vetoed 31 (the worker preempting its feeder collapses the one-flow
  TCP figure, and one x86-64 boot hung with the feeder starved -- a
  latent spin now on the inventory) and chose the default: every TCP
  figure within the old spread or above it, and 9 300 to 10 000 of
  10 000 delivered.

- **A hang that names its program counter**
  (`docs/audit/next-subsystem-lockup.md`). The wake-preempt unit's
  measurement found a hang it could not diagnose -- the network worker
  one priority above its feeder, one x86-64 boot in five stopped in
  `net-steer` with the worker `running` for eight seconds -- and the
  watchdog's dump carried everything about that CPU but where it was.
  The kernel had the answer every tick and discarded it. Now every tick
  stores the interrupted PC and its time (two stores), and on request
  every other online CPU records its own frame and stack into its own
  per-CPU buffer, in its own handler, with no lock and no printing: an
  NMI on x86-64 (a new LAPIC delivery mode, answered on the paranoid
  path with any registered handler still dispatched), the ordinary
  `IPI_SAMPLE` on AArch64, where a CPU with interrupts masked is
  reported as "no answer, last tick N ms ago" rather than guessed at.
  One reporter at a time, claimed with a compare-and-swap and never
  spun for; one total wait bound. The scheduler dump prints each CPU's
  tick sample and age and a live `run_ms`; the self-test watchdog prints
  every CPU's sample. Two detectors run from the tick -- a soft lockup on
  a CPU's own tick (no switch while something is runnable) and a hard
  lockup seen by the next online CPU in the mask (no tick), each once per
  episode -- and the harness forbids a real report and symbolises a
  report's addresses with the kernel ELF. Seven kernel tests (a spinner
  in a known function whose sampled PC must lie in it; through an
  interrupt mask with the outcome stated per architecture; the
  single-reporter rule; both detectors at a lowered threshold; the quiet
  control) and a host test of the watcher rule over masks with holes,
  every one bug-proofed by injection. Found by building: an AArch64 leaf
  function had no frame record at `-O1`, so a walk from inside one
  skipped its caller -- the AArch64 kernel now keeps leaf frame pointers;
  and a CPU with interrupts masked cannot acknowledge a TLB shootdown,
  whose waiter panics after a second. Then the spin: with the worker's priority overridable from the command line, the hang reproduced on the seventh boot at 31 and the dump named it in one block -- eight samples of the worker, every one in its wait condition or its dequeue, none in a packet: the receive queue's count said non-empty over a list that was empty. The cause was a second enqueue of an mbuf already on the queue, from the reorder test's loopback filter (a held copy in a plain global that two CPUs could both take), which cut the list behind it; at priority 32 the spinning worker had gone unnoticed -- one boot in five burning a CPU since that test landed. Fixed at the stack (a queued mbuf is refused a second enqueue, counted and said once; `net-mbufq-double`) and in the test (its state under a lock). Five boots at 31 on each architecture: no hang. The five-boot check at 31 then found a second, older hang -- the keepalive test black-holing the handshake's last segment before the worker had sent it, leaving its server in `accept` forever -- fixed by waiting for the passive side to be established. Ten boots at 31 after both fixes, five per architecture: no hang.

- **A read that fills its buffer, and an error that reaches close**
  (`docs/audit/next-subsystem-file-path.md`). The audit's two MEDIUM
  findings on the file path, still as found: `read` returned at most
  1 KiB per call through a stack bounce sized to a console line (a
  64 KiB read was sixty-four system calls, and the Linux personality
  inherited it), and `close` could not report a write-back error (the
  last-reference write-back dropped its result; the vnode's release
  dropped the pages themselves, uncounted). Now the bounce is sized to
  the request -- the stack for a kilobyte and less, the heap above it
  up to 64 KiB, degrading to the stack chunk when the heap refuses --
  one object call per read, so a pipe or tty keeps its semantics and a
  file fills its buffer; both personalities share it. A write-back
  failure is recorded where it is seen, by the page cache's own sync
  under its own lock; each file open at the time is told once, by
  `fsync` or by `close` (a `flush` hook on the I/O object type, called
  before the handle's reference goes; the handle closes regardless);
  a named file's pages dropped at its vnode's release are counted and
  said once, an unlinked file's neither written back nor counted --
  the rule `cosmofs-reserve`, which fills a disk and unlinks, decided.
  Seven kernel tests on cosmofs over the RAM block device with the
  block fault injection scoped to the test thread, each bug-proofed by
  injection, a user-mode read that fills 64 KiB in one call, and the
  audit's missing read/write bandwidth benchmark: the object path reads
  a ramfs file at 44 MiB/s with 1 KiB requests and 317 with 64 KiB on
  x86-64, the whole syscall from user mode at 38 and 110 (PR #138).

- **A guard that is proved, not assumed**
  (`docs/audit/next-subsystem-hardening.md`). The kernel's guard on its
  own access to user memory -- `stac`/`clac` with SMAP, PAN on AArch64
  -- was a no-op on both CI CPU models, so an unbracketed access passed
  every boot: booting the unchanged image on `cortex-a76` failed 5 of
  265 (four ASID tests reading user pages outside the bracket, and the
  `el2` test because the stage-2 walk started at a level the
  architecture forbids below 43 bits of physical address, the boot
  self-check disabled the backend, and the disabled backend kept EL2's
  vectors). Now `make test-guard` boots the same image on a
  protection-capable model per architecture and the harness requires
  the kernel's own `hardening:` line whole, the `uaccess-guard`
  self-test's "guard live" and, on x86-64, `usertest: umip: enforced`;
  the default boot stays the control and carries the `WARN` naming what
  is absent. The three faults are fixed: the bracket in the ASID tests;
  a pure, host-tested `hv_s2_layout` rule (level 0 above 42 bits, level
  1 from concatenated root pages at or below; the SMMU driver refuses
  what the IOMMU walker cannot build); and `arch_hv_disable`, which
  needed a call of the switch's own (`HV_EL2_CALL_HANDBACK`), since the
  stub's calls are gone once the switch owns EL2 -- proved by
  `hv-disabled` with an injected self-check failure. The native ABI's
  `mmap`, `mount`, `umount` and `open` refuse an unknown flag bit with
  `-EINVAL`. `SCTLR_EL1.WXN` is set on every AArch64 CPU from the
  kernel's own tables on, and `make test-wxn` executes a deliberate W+X
  page and requires that panic. 267 self-tests on both architectures on
  both CPU models, the `hv` suite included on `cortex-a76`; the guard
  instructions cost nothing measurable under TCG (PR #140).

- **A name that points somewhere else**
  (`docs/audit/next-subsystem-symlink.md`). The VFS had three vnode
  types and no way to offer a fourth: no `symlink` or `readlink` in
  `vnode_ops`, no call that would reach them, and no type for `stat` to
  report. The Linux personality was worse than absent -- `lstat` was
  aliased to `stat` and `newfstatat` read `AT_SYMLINK_NOFOLLOW` and
  dropped it, so a program that asks specifically not to follow a link
  was told about the target. Now a link is a node: the walk expands one
  the moment it meets it, with a budget of eight and the component count
  that already bounded it, carrying two path buffers from a single
  allocation that a walk without links never takes. A relative target
  resolves against the directory the link lives in; an absolute one
  restarts at the calling process's root, so a link cannot name its way
  out of a root the way a leading slash cannot -- proved by a child
  rooted at a jail writing through an absolute-target link and landing
  inside its own root. `open` is a loop, so `O_CREAT` creates the target
  of a dangling link and `O_NOFOLLOW` refuses a link named last while
  saying nothing about the ones in between. ramfs keeps the target in
  the node and cosmofs in the file's own block, written inside the
  transaction that publishes the entry and given back whole on any
  failure before it; format version 8 gates creation, because an older
  kernel would read a link as a regular file whose contents are a path.
  Three native calls (`SYS_COUNT` 89 → 92), six Linux entry points, and
  `ls` showing a link with its target. 273 self-tests on both architectures (PR #142).

- **The blocks nobody can reach**
  (`docs/audit/next-subsystem-fsck.md`). cosmofs could tell you every
  block was still what it wrote, and nothing could tell you the blocks
  added up. `cosmofs_check` walks the live tree, every snapshot, both
  allocation maps and the inode map under the mount's lock and compares
  what it reached with what the filesystem believes: ten finding
  classes, four repaired because each has one right answer, the rest
  reported because setting the bit of a block that is reachable and free
  may hand out a block in use, and choosing which of two inodes keeps a
  shared block is data loss. A snapshot shares blocks with the live tree
  on purpose, so the union map and the live-generation map are separate
  and only the second can report a cross-link. Pointed at the crash
  suite's replayed images, it found that **every crash strands space**,
  not only the unlinked-but-open file the report predicted: a block
  freed in a transaction keeps its bit until the commit after the one
  that made the new root durable, which is correct for crash safety and
  costs the previous generation's copy-on-write casualties. Measured
  across 199 replayed prefixes: 162 leaked, worst 18 blocks, 1912 in
  all, each reclaimed and clean afterwards. Repair is an argument from
  absence, so it runs only on a walk that is sure of itself: one
  unreadable directory block reports every file named inside it as an
  orphan whose blocks are leaked, and repairing that image would destroy
  them. 281 self-tests on both architectures; seventeen bug-proofs, two
  of which exposed tests that could not fail (PR #144).

- **The pass nobody can run**
  (`docs/audit/next-subsystem-fsctl.md`). cosmofs had two maintenance
  passes -- the scrub, which repairs a rotted mirror, and the structural
  check built the week before -- and every caller of either was a
  self-test. The check was compiled only into debug builds because
  nothing in a release kernel could have called it; the scrub had no
  such gate and shipped as dead code. Three things were missing, and
  none of them was the pass. A mount had no name: it carries no
  identifier, and a path is not one, because the same mount sits at
  different paths in different namespaces and a path names different
  mounts over time. Nothing pinned a mount: unmount infers busyness from
  the vnode hash, which a walk holding a reference does not appear in.
  And there was no channel. So: an id handed out in order and never
  reused; an unmount that **drains** a running pass rather than refusing
  or tearing down under it, terminating because the flag it sets first
  stops a new pass starting; and `/dev/fsctl` (0600), a versioned
  fixed-layout command whose result belongs to the open file that asked,
  with the listing scoped to the caller's mount namespace and the root
  filesystem emitted by name because it deliberately holds no namespace
  reference. Two optional entries on `struct fs_type` let the VFS learn
  that a filesystem has a pass without learning what one is. `fsctl(8)`
  lists, checks, scrubs and repairs; a check that *finds* something
  still exits zero, because a script cannot otherwise tell a broken
  filesystem from a question it could not ask. The first boot panicked
  on a lock-order inversion -- a device operation runs under the vnode
  lock, and the mount table is taken the other way round -- which is now
  a separate lock class and an inventory row. And the first real
  filesystem the tool was pointed at was not clean: **28 leaked blocks
  on the boot's own scratch disk**, because a clean unmount strands what
  its last transaction freed, exactly as a crash does, which nobody had
  measured because nothing could look. 286 self-tests on both
  architectures, debug and release; twelve bug-proofs, one of which
  passed and became an inventory row (PR #146).

- **The commit after the last one**
  (`docs/audit/next-subsystem-unmount-leak.md`). Two units in a row found
  this and neither fixed it. A cosmofs commit publishes its new root and
  *then* clears the freed blocks' bits in memory, marking those bitmap
  chunks for the next commit -- which is correct, because a block the old
  root still names cannot be freed before the new one lands, and which at
  an unmount means there is no next commit. So every unmount, on every
  filesystem, lost the space its last transaction freed: the fsck unit
  measured 1912 blocks across 199 replayed crash prefixes, and the fsctl
  unit found the sharper half when the first real filesystem an operator
  could point a tool at turned out to have 28 stranded blocks after a
  *clean* unmount -- and 41 more when that unit's test stranded some
  deliberately rather than depending on the residue it found. The obvious fix does not converge -- writing the
  bitmap frees the bitmap, so a second commit leaves a third's worth of
  work -- so format version 9 gives the superblock a `free_root` naming
  a chain of blocks that records what this root freed, written before
  the root and made true by it, and replayed by the next mount before
  the bitmap is trusted for allocation. The ordering is the design and
  both halves are load-bearing: everything that allocates must happen
  before the bitmap fixpoint, or the root publishes a bitmap that does
  not know about the record's own blocks and the allocator eats them;
  and the set to record is not final until the fixpoint has run, because
  it frees every chunk it copies. Reserve before, fill after. Two of the
  plan's claims were wrong and the build says so in the report: moving
  the snapshot filter ahead of the fixpoint corrupts a snapshot's member
  table, so the filter stayed and the *question* was extracted instead;
  and "a superseded record is freed outside the filter" was not true of
  the code, because a deferred free goes on the list phase 7 filters --
  found by writing the test that argument never had. Four tests passed
  their own bug-proof and were rebuilt before they measured anything,
  one of them three times. Review then found three failure paths where
  the happy path was right and the unhappy one was not: a commit that
  failed after reserving the record's blocks kept them, an over-reserved
  block had nowhere to be recorded when the last chain block came out
  exactly full, and a record whose count was past what a block holds was
  read as empty. The third corrected the report as written -- refusing a
  root is not refusing the filesystem, because cosmofs keeps two and
  falls back a generation to a whole one. The crash suite's
  stranded-block total is now zero and its weakened assertion is gone.
  297 self-tests on both architectures, debug and release (PR #148).

- **The list the root does not name**
  (`docs/audit/next-subsystem-snap-deadlist.md`). The record of what a
  transaction freed fixed the frees and not the holds. A block a
  snapshot still occupies was appended to that snapshot's deadlist from
  the commit's release loop -- *after* the root was published -- by a
  call that allocated a block the published bitmap did not know about
  and dirtied two blocks for a commit that an unmount never makes. The
  snapshot list was also the one metadata chain in the filesystem
  rewritten **where it lay**, so a crash between that write and the next
  root left the surviving root naming a list belonging to a transaction
  that never happened. The inventory row called for the free record's
  reserve-before-fill treatment; that alone would have traded a leak for
  a corruption, because an append moved in front of the root under an
  in-place update is an append the *old* root can see. So the snapshot
  list and its deadlists are now copy-on-write, published by the same
  superblock write as everything else -- `snap_root` names the list and
  nothing else does -- with the blocks taken from the commit's existing
  reservation, the superseded ones freed exempt on the rule the free
  record established, and the verdict "does a snapshot hold this" taken
  once per freed block instead of once for the record and again for the
  release loop, which now clears bitmap bits and nothing else. **The
  report's own analysis was wrong in one place and the crash suite said
  so on its first run**: the release loop's append *was* a crash hazard,
  not through its entries but through the pointer it wrote into the
  in-place list -- a deadlist head neither written nor allocated under
  the surviving root, `unreadable` and reachable-and-free at once, at
  prefix 125, block 23. A control with the change disabled failed
  identically, which is what said the defect pre-dated the unit. Nothing
  had ever tested it, because **the crash suite had never taken a
  snapshot**; it does now, and asks per prefix a question the structural
  checker cannot -- a deadlist's entries are claimed non-live, so one
  block on two lists is not a duplicate claim and not a finding. 334
  prefix images, 0 blocks stranded, 1312 deadlist entries examined, none
  duplicated. The previous unit's weakened bound, `alloc_not_seen <= 4`,
  is `== 0`. No format change: the on-disk shapes are unchanged and only
  where their blocks live differs, so `CFS_VERSION` stays 9. One of the
  new tests passed its own bug-proof and was rebuilt: comparing
  `snap_root` across two commits compares equal on a filesystem that
  copies perfectly, because the allocator hands the superseded block
  straight back, so the measurement is taken across one commit instead.
  (PR #150).

- **The name is gone and the handle is not**
  (`docs/audit/next-subsystem-orphan.md`). The last clause of a row two
  units in a row named and neither took. `cfs_unlink_common` removes a
  name and zeroes the link count; the blocks are released later by
  `cfs_evict`, when the VFS drops the last reference -- and a commit can
  land between the two, leaving a durable filesystem with an inode whose
  extents are intact and which no directory entry reaches. A crash or a
  forced unmount after that lost the inode and its blocks for good,
  because no mount reconsidered them: `cosmofs_check` found them and an
  operator with the repair flag got them back, which is the workaround
  with a person in it that the free-record unit had already refused. The
  tree carried a test whose comment called this "the leak the design
  admits by omission" and whose assertions described it. Format version
  10 gives the superblock an `orphan_root` naming a chain of inodes the
  root still owes, written before the root and replayed at the next
  mount by doing what `cfs_evict` would have done -- which means queuing
  the blocks and clearing the slot, so the space comes back when that
  mount's first commit publishes it, as an ordinary eviction's does. **It is a record, not a
  list**, and that is the design: the set is derived, so each commit
  writes it whole and nothing is edited on disk -- unlike the snapshot
  list, nothing here is copy-on-write -- and an ordinary unlink's add
  and eviction cancel in memory, so a filesystem holding nothing open
  across a commit writes no record at all. Review corrected the plan
  twice: **directories reach this state too**, because a working
  directory is a referenced vnode and an empty one can be removed under
  it, and the replay must not touch the parent's link count, which
  `rmdir` already decremented; and the cancel is not a guarantee,
  because the filesystem lock is dropped before the VFS drops the last
  reference. The build corrected it twice more: `cfs_inode_read` calls
  an inode with no links absent, so the first replay reclaimed nothing
  until it used the raw read the structural check already had; and the
  reclaim lands on the mount's first commit rather than on the mount,
  because the root still names those blocks -- and a replay that fails
  after queuing them must fail the mount *and* clear the queue, or the
  older-root fallback commits frees that root's inodes still name. Two test oracles measured
  the wrong thing before they measured anything -- one compared against
  a filesystem that had no record either, and the next asserted sixteen
  blocks for a 64 KiB file that compresses to five. The crash suite now
  holds a handle across a sync: 410 prefix images, 0 stranded, and with
  the replay disabled it fails at prefix 213 reporting one orphan, which
  is what says the workload is not vacuous. 312 self-tests on both
  architectures, debug and release (PR #152).

- **Four windows nothing has ever raced**
  (`docs/audit/next-subsystem-lifetime-windows.md`). The first unit in
  six that is not cosmofs. The lifetime and quiescence report states its
  ordering argument as a table of claims and then says how each was
  checked -- a host model under sanitizers, plus review -- and lists as a
  risk that ordering is "verified by review and sanitizers, not by a
  model checker". That is good evidence for the algorithm and none at
  all for the four places where the algorithm meets a driver, a socket
  or a device, because nothing had ever run the other side:
  `straggler_ipis` was incremented in one place and read in none;
  `blk_unregister` was called by two tests and in both the device was
  quiescent; `timer_cancel_sync` was tested on a probe, which is
  evidence for the primitive and not for four uses of it; `vpci_remove`
  ran only on module unload. Seven tests race them now, each holding its
  window open with a hook rather than a stopwatch. **Three of the four
  windows are closed and the fourth is narrowed**: removal is asserted
  to be the whole unbind -- the driver's hook *and* the model's
  bookkeeping, since the hook alone leaves a bound device holding a
  dangling `drvdata` -- but on a device of the test's own, because the
  machine's live virtio-blk is the scratch disk the filesystem tests run
  on and a boot-time suite that removes it destroys the run. Driving
  `vpci_remove` itself with real I/O outstanding needs a virtio device
  dedicated to removal in the test machine, and stays an inventory row.
  The numbers, from one run: 6 kicks from that waiter over a 32 ms
  wait with the spinner unhelped, 5749 units of work on a third CPU while
  one stalled the waiter, 15 accepted and 349784 refused across the
  unregister window, an unregister that spun 6118 times for a submitter
  parked inside the driver, a cancel that spun 96 times for a callback
  holding a pcb it had not yet taken a reference on. **The review was
  worth more than the run.** It caught three oracles that measured the
  wrong thing: a kick credited with a completion it could not have
  caused, a drain window no submitter ever occupied, and a hold placed
  after the callback takes its reference -- which would have measured
  reference counting and passed with `timer_cancel_sync` stubbed out --
  plus a removal hook that would have left a bound device with a
  dangling `drvdata`. **And the unit's own findings came before it
  ran**: a CPU publishes at interrupt return only when `preempt_count`
  is zero, so the straggler kick cannot help the spinner its comment
  named, and what it is worth for the population it *can* help is
  unproven because arranging that is a phase coincidence. The comment is
  fixed and the question is filed. No use-after-free, no bio reaching a
  detached driver, no hung unregister -- which the report said in
  advance it would report as such and keep the tests. Also corrected:
  the inventory row claiming nothing here could attempt an unprivileged
  open was stale, and what was left of it was two doors added after that
  suite. 319 self-tests on both architectures, debug and release
  (PR #154).

- **Two timestamps and no rule about subtracting them**
  (`docs/audit/next-subsystem-cpu-clock.md`). `clock_now_ns()` is a
  per-CPU clock on x86-64 that the whole tree treated as a machine-wide
  one, and `has_invariant_tsc` -- the one fact that says whether the
  counter is even usable that way -- had been detected at boot since this
  kernel had an x86 port and read by nothing. **The sweep was the unit.**
  Planned as step 6, a re-check of step 1, it was run first as a grep
  rather than a re-reading and changed the plan twice. The report called
  `sched_dump` "the one place with a hand-rolled guard"; there were
  three, written independently, each for the same reason and none
  referring to the others. And rewriting the block-timeout scan as
  `clock_since_ns(stamp)` re-reads the clock per bio *inside a spinlock*,
  against a `now` that moves underneath the comparison -- a regression
  step 1 had already written before the grep caught it -- so
  `clock_delta_ns(now, stamp)` is the primitive and `clock_since_ns` its
  fresh-read form. The classification rule is not the one the report
  implied either: a thread that sleeps between two clock reads wakes on a
  different CPU, so a plain `t0` in a local variable is a foreign stamp,
  and that is nearly every timing assertion in the suite (**that premise
  was itself wrong, and the thread-migration unit below says so: this
  kernel pins a thread to one CPU for life, so those stamps were
  same-CPU when they were swept; **and true again since the
  percpu-migration unit below, which is when threads began to move**) -- 42 such
  sites, 20 shared-state ones, 4 in userland the report had not noticed,
  1 deliberately left plain (the tick cost, where saturating would hide a
  counter going backwards on one CPU) and 4 that are not elapsed times at
  all; 66 changed in all. **Then the gate fired on the first machine it met.** QEMU's
  x86-64 TCG does not advertise an invariant TSC and refuses to be asked
  to (`-cpu ...,+invtsc`: "TCG doesn't support requested feature"), so
  the only x86-64 machine this project runs on is one where the kernel
  must decline to promise -- while its counters demonstrably agree, 0 ns
  outside the bracket over 2400 handshakes. It declines anyway, because
  the promise is about the hardware's contract and not about what happens
  to work today. The report's fallback for that case does not exist: PIT
  channel 2 is a one-shot calibration gate, not a free-running counter,
  and there is no HPET driver. So the kernel keeps the TSC, still
  monotonic per CPU, and gives up the cross-CPU claim instead. **The
  measurement runs anyway** -- gating it on the bit would have made it
  dead code on every machine here -- and reports what it found without
  acting on it. Its first run reported an uncertainty of ±0 ns, which is
  a promise no measurement can make: the narrowest bracket had width
  zero, the counter not having advanced across a handshake that certainly
  took real time. The bound is floored at one counter tick now, and a
  test keeps three cases apart permanently: 0 only when nothing was
  measured, unbounded only when the kernel has declined, otherwise at
  least one tick. **And the unit found a live dependency on the property
  it was defining**: `blk-unregister-drain` asserted an order between two
  events on two different CPUs by comparing their timestamps, which is
  exactly what the kernel had just stopped promising -- it uses an atomic
  sequence number now and depends on no clock at all. Ten new tests;
  the two cross-CPU oracles pass trivially on every machine here, so the
  injection that would fail them runs in CI rather than in a terminal.
  Review then spent five rounds on one finding the report had scoped out
  -- that a *deadline* is a timestamp too, so one built on one CPU and
  tested on another is unsound where the offset is unbounded -- and was
  right to keep asking. It is fixed: a machine-wide tick advanced by a
  designated CPU, with ownership taken over by another when its owner
  stops ticking, so the counter cannot stop while any CPU still ticks.
  Two earlier shapes were built and reverted first, and the test that
  ships with the third creates exactly the failure that killed the
  first. What does not run anywhere available: the applied correction,
  since no machine here both has a per-CPU counter and advertises it as
  invariant.
  329 self-tests on both architectures, debug and release (PR #156).

- **A thread that could never move, on a CPU chosen once**
  (`docs/audit/next-subsystem-thread-migration.md`). Half of this unit
  shipped and the half it is named for did not, which is the result
  rather than an excuse. `pick_cpu` compared with a strict `<`, so ties
  went to the lowest-numbered CPU — and because `nr_running` counts only
  what is runnable *now*, and a kernel thread is blocked almost all of
  its life, the queues had usually drained to zero between creations and
  every CPU tied. **So every thread created on an idle machine went to
  CPU 0, and nothing ever moved it.** Measured: 8 of 14 threads and 94%
  of context switches there. Ties rotate now, which fixes the placement
  half; the test had to be built around a surprise, since threads created
  back-to-back already spread (each raises its target's count) and only
  threads that *block* pile up. **The balancer was built, worked, and was
  removed.** It moved threads correctly — CPU 0 to 6 of 14, CPU 2 doing
  ten times the switches — and made three of four aarch64 boots fail,
  once with seven concurrency tests at once, against four of four passing
  without it. Seven together is corruption, not timing, and it was not
  found; what was ruled out is written down so the next attempt starts
  past it. Removed rather than left behind a flag, because a balancer
  switched on only by its own test closes nothing and reintroduces the
  instability wherever it is switched on. **The failure was worth more
  than the feature.** lockdep cannot check a two-run-queue lock order,
  because both are one class and the nesting annotation only says
  "deliberate" — an invariant claimed otherwise and is corrected.
  `rq->current` *can* be in a ready list, which the report and two
  comments called structurally impossible, caught by an assertion kept
  only because the property lived in another file. And the tree holds
  per-CPU assumptions nothing declares: `el2` asserts the hypervisor
  backend owns EL2 "on this CPU" from an unpinned thread, and was correct
  only while threads could not move. 330 self-tests on both
  architectures, debug and release (PR #158).

- **A writeback thread inside a mount that was still replaying.** Not a
  unit: the deferred-work inventory's one open defect, chased on
  request. `cosmofs-orphan-reserved` failed on three of three CI runs
  and none of three local ones, on a documentation-only branch, always
  as one metadata block that would not verify, at a different address
  each time. What opened it was the other architecture: the same test
  panicked x86\_64 with a kernel write at zero in `list_remove` <-
  `cfs_buf_get` <- `freelog_release_previous` <- `cosmofs_sync`, running
  on `cfs-wb`. cosmofs starts its writeback thread lazily, on the first
  dirty buffer, so that a read-only mount has none to join -- and a
  mount's own replay dirties buffers, so the thread was being started
  from inside the replay. It then takes only `mount.sync_lock`, which
  the mount path does not hold, finds `mnt->unmounted` false, and
  commits a half-built filesystem across an `fs->bufs` that the mount
  walks with `fs->lock` unheld, because until `cosmofs_mount` returns
  nothing else is supposed to be in the filesystem. Two threads on one
  intrusive list: an unverifiable metadata block on one architecture, a
  null dereference on the other. The same window let a *failed* mount
  reach `cfs_destroy`, which frees every buffer and the filesystem, with
  that thread still running. The rule is now that no autonomous
  committer exists before the mount is live: the spawn is gated on
  `mount_done` and `cosmofs_mount` starts the thread at the end if the
  replay left anything dirty, and `cfs_destroy` stops and joins for
  itself rather than trusting its caller. Whether that thread wins the
  race is timing and whether it exists during the replay is not, so
  `cosmofs-mount-no-early-writeback` claims the second: it counts the
  replay's dirty marks, which must not be zero or the test is asking
  nothing, and how many of those found a thread already running, which
  must be zero -- that count is the rule. With the gate reverted it
  fails on exactly that count, while `cosmofs-orphan-reserved` still
  passes here -- which is what the three local runs had been saying all
  along. 331 self-tests on both
  architectures, debug and release (PR #160).

- **An invariant written down thirteen times, checked once, and false in
  half the tree** (`docs/audit/next-subsystem-vcpu-regs-size.md`).
  `struct cosmo_vcpu_regs` is the VMState, and its size was stated in
  thirteen lines across eight files, all of which said 448 bytes. It is
  448 on x86-64 and **496 on AArch64**, and has been since the EL2
  backend landed. The one check lived in a host test that compiles
  whichever block the *build host* matches, so on the x86-64 CI runner
  it passed and could not fail: the invariant's own check never
  compiled the block that violated it. The first plan was to resize both
  blocks to a shared 512. Review asked what protects a caller built from
  the older header, and the answer was nothing -- `SYS_vcpu_regs` takes
  no size and no version and copies `sizeof` both ways -- which led three
  lines above the struct, to the header's own preamble: *"This is user
  ABI: numbers and structures here are stable."* x86-64's 448 is correct
  and stable; the AArch64 ABI is 496. **The documentation was what was
  wrong**, so the unit resizes nothing: it asserts each real size with
  `_Static_assert` in the UAPI header, where the check runs in every
  translation unit on every architecture instead of in one host binary,
  and corrects the twelve lines that were wrong. The cross-architecture
  equality rule is gone and bought nothing -- every caller and copy uses
  `sizeof`. Deleting the stale host assertion had a second effect worth
  more than the first: `test_hv` is ninth of twenty-three in
  `HOST_TESTS` and the target stopped there, so **fourteen host suites
  had never run on an arm64 machine**; they run now, and all pass.
  `hv-vcpu-regs-roundtrip` is new, because nothing had ever checked that
  a register file survives a set and a get: it asserts every field back,
  and asserts the four the backends normalise against their documented
  rules, so removing the `rflags` masking fails the test that documents
  it. 332 self-tests on both architectures, debug and release (PR #162).

- **A filesystem lock held across a device that sleeps**
  (`docs/audit/next-subsystem-chrdev-vnode-lock.md`). `file_pread` and
  `file_pwrite` took the vnode's mutex and dispatched to the character
  device inside it. The inventory row called that a lock-order problem
  and it was one; the larger half is that one of those devices sleeps.
  `tty_read` waits for a line with no timeout, and `ramfs_lookup` hands
  out **one vnode per device node**, so a process blocked at a terminal
  held the lock every other opener of that terminal needs — not slower,
  stopped, until somebody typed. Nothing had ever failed, and the reason
  is worth stating: kernel messages never touch the VFS, the shell
  harness has one reader whose background jobs print nothing while it
  waits, and no other character device sleeps. The one that does is the
  one nothing writes to concurrently, which stops being true the moment
  there is a second session. The `VNODE_CHR` arm now runs with **no
  filesystem lock** — it guards nothing there, and a device's other
  entry points already ran outside it (`ops->open` from `file_run_open`,
  `ops->release` from `file_release`), so this is read and write being
  brought into line with open and release rather than a new rule. It is
  **checked rather than stated**: `lockdep_assert_not_held` sits at both
  dispatches and fires for every character device in every debug build,
  including ones not written yet — a macro that was defined in the tree
  and called from nowhere, which is most of why the row survived. The
  `vnode-chr` lockdep class split, added after `/dev/fsctl` panicked on
  its first boot, is **removed rather than layered over**: it was sound
  and answered the wrong question, since what made a mount-table lookup
  an ordering at all was the lock being held across the device. Three
  proofs: the new `vfs-chr-write-during-blocked-read` fails at
  `writer_returned` in 511 ms before and passes in 17 ms after;
  reverting the fix with the split gone reproduces the original
  `mounts -> vnode` panic, which is what says the two were coupled; and
  with both in place lockdep reports only the six its own suite asks
  for. The test releases its blocked reader on every path before
  asserting, because a test that proves a deadlock by deadlocking is a
  hung boot. New invariant V32. 333 self-tests on both architectures,
  debug and release (PR #164).

- **A count published after the thing it counts.** Not a unit: an
  inventory row this session's VMState work turned up, fixed on request.
  `cfs_writeback_thread` incremented `fs->wb_commits` after
  `cosmofs_sync` returned — after that function had dropped `fs->lock` —
  while `cosmofs_stats` reads the counter and `sb.generation` together
  under it. For the few instructions in between, a reader saw a
  filesystem that never existed: the new generation with the old count,
  which is exactly the pair `cosmofs-writeback` asserts. It failed once
  on aarch64 CI and looked like the two genuine flakes beside it; the
  log is what separated them, because it failed after **80 ms** rather
  than at its 2-second deadline with `committed generation 2` printed
  above it, so the commit had happened and host load explains nothing.
  The count is now taken inside the same hold of `fs->lock` that
  publishes the generation, and the field has exactly two accesses in
  the tree with both under that lock. The race is not deterministic and
  the proof is: `lockdep_assert_held` at the increment means publishing
  it outside the lock panics on the first writeback commit of the boot
  rather than on the one run in a hundred that catches it. 333
  self-tests on both architectures, debug and release (PR #165).

- **A harness that could not say why its own exchange failed**
  (`docs/audit/next-subsystem-nettest-deadline.md`). `net-harness`
  failed seven times in a fortnight, on both architectures, on CI and
  locally, including on branches that add a single Markdown file — and
  two documents had concluded it was host-dependent and unbounded. It is
  neither, but **this unit does not claim to have found the cause, and
  an earlier draft of its report did**. The obvious candidate was a real
  defect: the harness armed a 120-second `accept()` deadline in
  `NetTest.__init__`, which runs *before QEMU is launched*, and closed
  the listener when it expired — so a guest connecting later had its
  connection completed by QEMU's user networking and answered by
  nothing, which is exactly the `ksock_connect` returning 0 with no echo
  that every sighting recorded. Measured, the back-connection lands at
  **72 %** of the boot, which projects onto CI's 140–146 second boots at
  101–105 seconds against a 120-second deadline: a margin of fifteen to
  nineteen seconds. Thin, and not shown to be crossed — across sixteen
  aarch64 jobs a 145.6-second boot passed and a 145.8-second one failed,
  so boot length does not predict the outcome. What the unit *did* find
  is why nobody could tell: **the harness recorded none of those
  numbers.** Every sighting produced `TimeoutError('timed out')` and
  nothing about when the guest connected, how much budget remained, or
  whether the port was open. So the deadline is fixed — bound early,
  accepted after the guest reports ready, one budget derived from the
  run's `--timeout` — and every run now prints its timings whether it
  passes or fails. `tests/boot/test_nettest_deadline.py` (in `make
  host-test`) holds four properties in under a second, including that an
  expired deadline leaves nothing listening, which is why expiry was
  fatal rather than late; restoring the deadline to the constructor
  fails it by name with `got 120.0`. **The inventory row stays open**:
  it is struck when a sighting with the new timings shows the deadline
  was the cause and the fix ended it, and not before (PR #167).

- **Twelve bytes that never arrive** (`docs/audit/next-subsystem-twelve-bytes.md`).
  The guest half of the instrumentation the previous unit added to the
  host. `net-harness` prints `client failed (%d)` with the **connect's**
  result, so every one of its failures has reported that the step which
  worked, worked — the same defect PR #167 removed from the other side
  of the same wire. It now says what `ksock_sendto` and `ksock_recvfrom`
  returned, and asks the connection rather than the global counters:
  `tcp_send_space` before the send, after it and after the read, because
  data sits in the send buffer until it is **acknowledged**, and that is
  per-connection where `tcp_get_stats` counts this connect's own SYN and
  every other socket's traffic. Four outcomes, and they are exclusive:
  never queued; queued and never acknowledged with retransmissions
  climbing; queued and never acknowledged with retransmissions **flat**,
  which would be a defect here whatever else is true; or the send buffer
  **drains** — the data was acknowledged and the host still saw nothing,
  which is what QEMU's user-mode networking being a *proxy rather than a
  wire* makes possible, since it acknowledges into its own buffer before
  writing onward. **This unit is step 1 and stops there.** Twenty-one
  local boots produced one failure — the rate is one in twenty-one, not
  the one in three an earlier draft claimed from a single observation,
  and six boots under CPU load did not raise it. So the instrument ships
  and the next failure reports itself, as #167's did within the hour —
  **and it did, on two of this unit's own CI jobs, one per
  architecture**: `sent -104`, which is `ECONNRESET`, with the send
  buffer untouched across all three samples, `segs_out +0` and the pcb
  `TCP_CLOSED`. So `ksock_sendto` failed and the twelve bytes were never
  written: the connection had already been reset, while the host had
  accepted it a second earlier. Every framing of this defect so far,
  this unit's own title included, describes a symptom of something that
  had already happened, and the question is now what resets an
  established connection between `ksock_connect` returning and the next
  statement. The aarch64 job narrows it one step further with
  `rsts_in +1` — an inbound RST accepted in sequence, so the reset came
  off the wire rather than from this stack — while the x86-64 job's
  `+0` for the same failure is the instrument's window starting after
  the connect, not a run without a reset. Who sent it is still not
  named, because `tcp_get_stats` is machine-wide and the pcb's own
  pending error is never read; that is the next unit's first step. The
  inventory row stays open, narrowed. 333
  self-tests on both architectures, debug and release (PR #169).

- **A socket can be asked what went wrong.** The stack has always known:
  `pcb->error` is set at six sites with four errnos — reset, refused,
  timed out, refused by the firewall — and `struct socket` carries a
  second field, commented *"pending asynchronous error"*, that **nothing
  ever wrote**. `sock_set_error` had zero callers while five sites tested
  the field, and behind that dead setter sat a real bug: `ksock_accept`
  read `return take_error(s) ? take_error(s) : -EINVAL`, and `take_error`
  clears as it reads, so the second call answered 0 — `accept` returning
  **success** with its out-parameter unassigned, live the moment anyone
  called the setter. Meanwhile `icmp_input` consumed only
  fragmentation-needed and echo, so this host **sent** ICMP
  port-unreachables and had never **received** one, and a connected UDP
  socket talking to a closed port waited forever. And no caller could ask
  for a verdict at all: `SYS_ioready` says *that* a socket is broken and
  never which way, the native ABI had no socket-option call, and the
  Linux door refused every `getsockopt` while `setsockopt` returned **0
  for every `SOL_SOCKET` option and did nothing** — so a program setting
  `SO_RCVTIMEO` was told it worked and then blocked forever. This unit
  makes the verdict a thing you can ask for: `ksock_error` reads it once
  (clearing by compare-exchange, so two readers cannot both be told the
  same error) and takes **no lock**, because three of its five callers hold `s->lock`
  and two do not and the writer runs in packet-receive context where a
  mutex cannot be taken — which is a rule the field never had and the
  reason the first draft of the design would have recursed on a
  non-recursive mutex. `udp_error_notify` gives the field its first
  writer, from an `icmp_input` branch that reuses the quoted-header parse
  `icmp_needfrag` already had and delivers only to a **connected** socket
  whose whole four-tuple the message quotes (RFC 5927: the bar N16 sets
  for a reset). `SYS_getsockopt` (92) carries `SO_ERROR` — one option,
  positive errno, cleared by the read — with the Linux door forwarding to
  the same kernel path rather than growing its own, and `setsockopt`
  refusing what it does not implement. Invariant N21. Five bug-proofs,
  each shown to fail for its stated reason: the double call makes
  `accept` return the wrong thing, cutting the delivery makes the UDP
  socket wait, dropping connected-only lets an unconnected socket take
  another flow's error, giving the accessor the mutex stops the kernel,
  and a delivery that commits by errno alone destroys a second verdict of
  the same value that nobody had been told — which is why the pending
  error is one 64-bit word carrying a generation as well as an errno, and
  why a syscall peeks, copies, and only then commits the clear rather
  than taking the verdict and putting it back. `net-harness` now prints the pending error and samples its
  counters *before* the connect, which is what PR #169's window was too
  late for. 337 self-tests on both architectures, debug and release
  (PR #171).

- **A hardware error the CPU corrected no longer kills the machine.** An
  SError on AArch64 and a machine check on x86-64 are the same class — a
  fault the CPU could not attribute synchronously — and both ended in
  `panic`. Every SError reached `aarch64_trap_entry`'s `default` arm,
  which **relabelled the frame `ARCH_TRAP_GENERAL_PROTECTION`**, so a
  machine stopped by an asynchronous abort reported a general protection
  fault; and vector 18 had no handler at all, so a machine check panicked
  through `arch_trap_unhandled`. Both now classify: `arch_async_error_class`
  answers *corrected* only for a syndrome that positively says so — on
  AArch64 `ID_AA64PFR0_EL1.RAS` non-zero with `ESR_EL1.IDS` clear and
  `AET = CE`; on x86-64 **at least one valid bank**, every valid bank
  `UC == 0`, no `PCC` or `OVER`, and `RIPV`, across all `MCG_CAP.Count`
  banks. That first x86 clause is not redundant: without it "every valid
  bank is clean" is true of *no banks*, and a machine check carrying no
  record would read as corrected. Everything else panics, naming the
  class and printing the syndrome. **No process is killed** — an
  asynchronous abort's frame names the context interrupted when the error
  was *delivered*, not the one that caused it, so nothing here may choose
  a victim; attribution needs the RAS error records and is a unit of its
  own. Invariant **I-ARCH-16**. Five bug-proofs, each shown to fail for
  its stated reason — the vacuous bank rule, the missing-FEAT_RAS rule,
  the SError vector back in the panic arm (`KERNEL PANIC: exception in an
  unsupported vector slot 7 (EC 0x2f)`, dead in 8.8 s), the unregistered
  machine-check vector, and a bank scan that stopped at 32 and pronounced
  on the prefix — which is the vacuous rule again in a different
  disguise, a check ranging over less than it claims. Two things the building found and
  the report had not: `HCR_EL2.VSE` is inert without `AMO`, since the
  host runs `HCR_EL2 = RW` and nothing else; and **EL1 runs with
  `PSTATE.A` masked for the kernel's entire life** (`daifset #0xF` at
  boot; the only unmask anywhere is `daifclr, #2`, which is IRQ), so the
  kernel never takes an asynchronous abort while running — EL0, entered
  with DAIF clear, is the live path. Whether EL1 should unmask `A` is
  recorded as I-ARCH-16's gap rather than settled here. A real corrected
  SError, injected through `HCR_EL2.VSE`, is taken at EL1 and execution
  continues on the guard boot (`cortex-a76`, which has FEAT_RAS;
  `cortex-a72` does not, and the test says so rather than passing
  quietly). 339 self-tests on both architectures, debug and release
  (PR #173).

- **A grace period ends when the last CPU publishes, not at the next
  sleep boundary.** `synchronize_quiesce` polled — `thread_sleep_ns(TICK_NS
  / 2)` in a loop — so nothing told the waiter that the last CPU had
  published one microsecond after it went to sleep, and a 2 ms request is
  serviced at the next 4 ms tick. It blocks on a queue with a deadline
  now, woken by a CPU passing a quiescent point: **about 3.9 ms a grace
  period against 4.3–7.5 ms**, on four idle CPUs, with the spread
  collapsing as well as the mean. Every synchronous caller gains it —
  `interrupt_unregister`, module unload, `netif_unregister`, the
  receive-hook removal, and the `call_quiesce` batch worker, which is why
  the deferred form never escaped the floor either.
  **The report's premise was half wrong and the unit says so**: a grace
  period is *not* over in microseconds. It is over when the other CPUs
  reach a quiescent point, which while they are halted in
  `arch_cpu_wait_for_interrupt` means their next tick — and that ~3.75 ms
  is untouched. What went is the polling overshoot on top of it.
  Two more of the design's claims died in the building. The wake cannot
  live in `quiesce_note_quiescent`: that runs inside the scheduler, where
  the AP bring-up path publishes holding a run-queue lock with interrupts
  off, and waking from there reaches `schedule_internal`'s assertion —
  the machine dies five seconds into boot. And the two trap returns are
  not enough, because an idle CPU is halted, so the publish that finishes
  a grace period comes from `idle_main`. Three wake sites, all of them
  contexts that hold nothing and call `schedule()` a line or two later.
  The deadline stays, and is the correctness argument rather than a
  hedge: `pending == 0` is still the whole condition, so a missed or
  spurious wake costs one re-check and a defect in the wake path can only
  make the wait longer. Invariant **Q18**, and a new `wait_event_timeout`
  — the tree's first timed wait — with its own three-arm test. The
  assertion is that the wake **fires** — wakes delivered to a queued
  waiter, identically zero unless the path runs — and not that a grace
  period was fast; three earlier versions asserted timing in disguise and
  none of them was sound. Found next door and fixed: a thread
  killed while sleeping cancelled its stack timer with `timer_cancel`,
  which only promises the callback will not *start*. 341 self-tests on
  both architectures, debug and release (PR #175).

- **The harness can say which connection it accepted.** `net-harness`
  failed repeatedly over three weeks, on both architectures, on CI
  and locally, several times on branches that change no code — and said
  only `TimeoutError`. The cause was on the *host* side, where three
  earlier units had not looked: `nettest.py` listened with a backlog of
  one and accepted exactly once, blindly, treating whatever it dequeued
  first as the guest's. It never checked. **The failure was reproduced
  deterministically**: occupying that single backlog slot before QEMU
  starts reproduced the signature on the first boot, and the packet
  capture shows slirp acknowledging the guest's twelve bytes into its
  own buffer, never delivering them, and resetting the guest ten seconds
  later — the guest correct from first SYN to final reset, which is the
  outcome `nettest.c`'s own comment predicted before it was ever
  observed. The rule now: **the guest's connection is the one that
  delivers `cosmo hello\n`, and every other connection is evidence that
  gets reported.** A backlog of eight, a select loop over every
  connection that arrives, a per-connection receive budget measured from
  its own accept, and a failure line carrying a roster — each peer, when
  it was accepted, bytes read, a thirty-two-byte preview — in place of
  "connection accepted at 90.9s", which was a time without an identity.
  A connection that arrives and never delivers the request is still a
  failure, so the deeper backlog cannot turn a real guest fault into a
  pass.
  Two side facts closed by measurement: a backlog of one makes a second
  connect **hang silently** on this host — the SYN dropped, no refusal —
  so a stale connection both won the accept and stalled the real one;
  and `free_port` is clean, zero collisions in three thousand triples.
  A baseline the thread never had: a healthy back-connection is
  SYN-ACKed in **150 µs** and completes in **52 ms**, which is what made
  sighting twenty-two's `connect 0 in 1031 ms` readable at all.
  **And it answered on its own CI, against the hypothesis that built
  it.** Sighting twenty-three landed on this pull request's aarch64 job
  and the roster said `1 connection(s): … 0 byte(s)` — **exactly one
  connection, carrying nothing** — and sightings twenty-four and
  twenty-five said it again, three times running. So the wild trigger is
  *not* a foreign connection: the stale-slot reproduction reproduces the
  symptom without being the cause. The guest's connect succeeds but
  takes 787 ms to 1116 ms against a 150 µs baseline, and a
  SYN-retransmission constant read off the first three sightings was
  **withdrawn** when the fourth answered the first SYN with
  `retransmits +0`. The locus is **slirp's own host-side connect**, and
  the guest's side is fully accounted for. Still unnamed: why that
  connect stalls and then fails. Host-side only: no kernel change, no new API, no
  self-test registry entry. Eight host tests that run in about six
  seconds without booting anything, and the bug-proof is that the
  stale-slot case fails against the old harness with exactly the wild
  symptom — `back_error: timeout('timed out')`, zero of twelve bytes,
  9.9 s (PR #177).

- **The straggler kick is worth something, and now there is a number.**
  After 8 ms of waiting, `synchronize_quiesce` sends an IPI to every CPU
  still pending, up to eight times. Nothing counted a kick that
  *worked*: `straggler_ipis` counts kicks sent, so **no number in the
  tree would have changed if the kick were replaced by a no-op**, and
  the code said as much — "an open question rather than a measured
  fact". A kick that worked is now defined as narrowly as it can be, a
  publish that happened in that kick's **own trap return**, and counted
  there: a kind of its own (`IPI_QUIESCE_KICK`), a per-CPU flag its
  handler sets, and each architecture's interrupt tail reading **and
  clearing it unconditionally** before deciding whether it may publish,
  so the flag cannot outlive the trap that set it and claim a later
  publish. Invariant **Q19**.
  **The answer: it works, but barely — one attributed publish in eight
  boots, about 220 kick IPIs, and that one on AArch64.** The decision
  rule was written down before the measurement, and it says a counter
  that rises means the kick stays; deletion, which the report called the
  likely outcome three times, is off the table — on evidence thin enough
  that the follow-up should widen the sample first. **A first version of
  this said four per cent and was wrong**: it counted publishes by a CPU
  that had *already* published the target epoch, which are correct,
  cheap, and advance nothing. Attribution now requires the publish to
  have **moved** this CPU's epoch, which took the rate from 7-in-161 to
  1-in-220.
  **And the population the kick's own comment named is not the reason.**
  The adversary was built as designed — phase-locking a short read-side
  section over the target CPU's tick — and showed the opposite of what
  it was built to show: that CPU publishes *without* a kick, because
  `schedule()` publishes at entry and the covered tick still sets
  `need_resched`, so the `preempt_enable` ending the section that hid
  the tick publishes a moment later. The publish was never confined to
  the trap return, which is the premise the story rested on.
  **That is a local result and CI does not reproduce it reliably** —
  there the covered CPU has been seen both publishing while its ticks
  were hidden and still pending after a hundred milliseconds, on both
  architectures — so the test now reports the outcome rather than
  asserting it, and whether the variation is the population or a
  non-portable adversary is unsettled (invariant **Q19**). Where the
  the one attributed publish came from is now a question the counter can
  answer and argument could not.
  The other half of the measurement is the half that makes it
  attribution rather than a tally: `quiesce-kick-spinner` takes eight
  kicks inside a read-side section and publishes **none** of them. A
  counter that only goes up is not attribution. 343 self-tests on both
  architectures, debug and release (PR #179).

- **The harness can say what the connection it accepted was doing.** The
  accept unit answered *which* connection arrived; it has answered the
  same way every time since — one connection, carrying nothing — and
  nothing was recorded about that connection itself, because the harness
  closed it in a `finally` without asking. Now every connection carries
  **how it ended**: `closed` (an orderly FIN), `error` **with its
  errno**, `deadline`, or `wrong-data`. Those were all `0 byte(s)`
  before, so no sighting could say whether slirp closed its end or the
  harness merely timed out — the most valuable bit the roster lacked.
  On Linux it carries the connection's TCP state too, separating slirp
  holding an open socket and never forwarding from slirp having closed
  it. On the failure path only, a timed probe through the **same slirp**
  to the guest's echo service asks whether that path was answering at
  all; the reading is asymmetric and the line says so, since a slow
  answer implicates slirp *or* the guest and cannot separate them.
  **Nothing is written to the accepted socket**: a write into
  `CLOSE_WAIT` succeeds, so it cannot tell an open peer from a closed
  one — that was the first design and it could not have discriminated.
  Neither could the second, which flattened a reset into an orderly
  close, in the one case this defect is known to involve. The bug-proof
  is that an open, a closed and a resetting peer must read **three
  different ways**, asserted rather than assumed.
  A host-side packet capture would be the better measurement and needs
  root, so it is left to a human with `sudo` rather than designed into a
  test.
  **It answered on its first outing.** Sighting thirty, on this unit's
  own CI: the guest's half was reset (`sent -104`) while slirp's
  host-side half was **`ESTABLISHED`, open and silent** — ended by the
  harness's deadline, not by a FIN or a reset — and a probe through the
  *same* slirp answered in **1 ms to connect and 1 ms to echo**. So
  slirp tore down one half of this connection and orphaned the other
  while remaining perfectly responsive to everything else. That is a
  **per-connection failure inside slirp**: not a stall, not a foreign
  connection, and not this kernel, whose side has been fully accounted
  for since the socket-verdict unit. It does not name a line of code —
  it names the component and the shape, which thirty sightings had not.
  343 self-tests on both architectures, debug and release (PR #182).

- **An ARP retry can no longer outlive the interface it points at.** ARP
  and ND entries held a **bare** `struct netif *` and took no reference.
  Their ageing passes copy that pointer out from under the table lock,
  release the lock, and then dereference it — `send_arp` reads
  `nif->mac`. Meanwhile `netif_unregister` flushes those tables and drops
  what can be the last reference. **Nothing closed that window**: the
  per-CPU barrier a step earlier is for the receive path, by its own
  comment, and `age_work` re-arms every second — so a retry could begin
  *after* the barrier and read the interface after it was freed. The code
  survived by the timer not having fired.
  Both retry lists now take `netif_get` as they copy and `netif_put`
  after the send, which is sound because the flush takes the same lock:
  an entry present under it means the unregister's flush has not run, so
  the registry's reference is still held. Invariant **N22**, with the
  sweep it came from — `tapsvc` holds one too and is safe by explicit
  ordering, which the invariant now records rather than assumes.
  Entries deliberately do **not** hold one each: the flush already
  clears them, so per-entry references would turn a missing flush from a
  dangling pointer into a leak without fixing the dangling pointer.
  Flushing now also **counts what it drops** — `pending_dropped` for ARP
  and a new `nd_pending_dropped` for ND, which keeps its statistics in a
  different struct, so one fix was invisible to the other.
  The tests park a retry in that one-unlock window and run
  `netif_unregister` to completion against it, rather than racing two
  threads: the driver's release must not have run, and the reference
  count must show the retry's hold. 347 self-tests on both
  architectures, debug and release (PR #184).

- **A module zombie is collected without being asked for.** A module
  whose objects outlive its unload keeps its whole image mapped **and
  its dependencies pinned** — `drop_deps` runs at the free, not the
  unload — so one left behind blocks unloading everything beneath it.
  The only reaper was `module_unload` of the zombie's own name: a
  request nobody had a reason to make, which a replacement loaded under
  that name hid entirely, and which took one call per zombie when a
  name had more than one. **It survived because the happy path was
  tested and passed** — the mechanism worked, and nothing invoked it.
  A sweep now frees every zombie whose objects have gone, by identity
  rather than by name, at the top of a load and on **every** exit from
  an unload — including the `-EBUSY` a pinned dependency returns, which
  is the one action someone takes on discovering the pin. The ordering
  is the point: an explicit `module_unload("name")` is a real request
  and must still find its own zombie, so the named paths run first and
  the sweep never steals it. Invariant **M24**, with
  the two properties such a walk needs — removal-safe iteration, and an
  **acquire** load of `live_objects` so module text is never unmapped
  ahead of the final release.
  A load also reserves its publish slot **before** `init()` runs, so
  exhausting `MODULE_MAX_LIVE` returns `-ENOSPC` instead of panicking
  after the module is already initialised, linked and counted — where
  returning an error would have been worse than the panic.
  352 self-tests on both architectures, debug and release (PR #186).

- **A slow section is named instead of the whole suite.**
  `process-user` was one `SELFTEST` line standing for the entire
  user-mode suite — nine sections, about 540 checks in them and 54 more
  outside any section — so when it was slow nothing said which part
  was, and its budget had to be widened twice blind. On CI it reached
  **8284 ms**, past the 8000 ms every ordinary test is held to,
  surviving only on its own 20 s composite budget.
  `init --selftest` now drives a **table** of sections and times each
  call, printing `USERTEST: section <name> <ms> ms` and a total; the
  harness parses them and names the slowest beside the per-test
  summary. The table rather than ten bracketed calls is the point: the
  driver is the only caller, so a section cannot be added without a
  line — where the suite's existing `usertest: … ok` prose lines were
  a convention two of the nine had already stopped honouring. **No
  per-section budget**, because rationing a section that got more
  thorough is the defect this replaces; the numbers are for
  attribution. The first measurement disagrees with the source: `svc`
  is two fifths of the suite from **34** checks and nine sleeps
  waiting on service state, while `proc` with 235 checks is smaller —
  time here is spawning and waiting, not checking. Invariant **F13**,
  34 host checks in `tests/boot/test_usertest_sections.py`.
  352 self-tests on both architectures, debug and release (PR #188).

- **The checker checks what the format promises.** `cosmofs_check`
  took three of the format's invariants on trust and had five
  reporting paths that nothing had ever made fire — code whose
  behaviour was unknown rather than merely uncovered. Runs are now
  verified to **ascend by `lblk` and not overlap**, checked as they
  stream past with one `prev` per inode and no new memory; ordering is
  tested first and the overlap test skipped for a pair that fails it,
  because every descending pair also begins inside its predecessor and
  the two are different repairs. The end of a run is computed in 64
  bits, so a run near the 2³²-block bound cannot wrap and hide a real
  overlap. A **name repeated in one directory** is reported through a
  fixed 4 KiB bitmap whose hit only means *maybe*: the directory is
  re-scanned to confirm, so a collision costs a re-scan and never a
  wrong finding — and the names pass completes **before** the walk can
  recurse, or a subdirectory clears its parent's sheet. The five
  unfired paths each gained a corruption and a test; all five worked
  first time, against the report's own prediction that one would not.
  Thirteen classes, every one of which a test can now make fire.
  360 self-tests on both architectures, debug and release (PR #189).

- **The two tables threads left behind are locked.** When native
  threads arrived, `malloc.c` and `stdio.c` took locks and `errno`
  became thread-local; `stdlib.c`'s environment and `atexit` list were
  left as they were, and invariant **L8** enumerated three safe tables
  and said "all three are done" while the library had five. `setenv`
  growing the environment calls `free(environ)` while `getenv` may be
  walking it — **a use-after-free in the allocator that same unit
  locked**, reached through a table it was locked to protect — and
  `atexit`'s `g_atexit[g_natexit++]` both lost handlers and could
  write past a static array, because the bound check and the
  increment were separate. One lock now covers both tables, `exit`
  never runs a handler while holding it, and `getenv`'s returned
  pointer stays valid because `setenv` **leaks** the string it
  replaces — deliberate, **unbounded** in the number of overwrites,
  and recorded with its cost so it is not tidied away; a case holds a
  returned pointer across an overwrite and reads it back after the
  heap has been reused at that block's size, so tidying the leak away
  now fails a test rather than nothing.
  Eight cases in `thrtest`, and **the use-after-free is reproduced
  rather than argued**: unlocked, with a thread churning the heap so
  the freed array is reused, the process dies with a `#GP` at the
  same address on three runs of three — a reliable reproduction
  rather than a deterministic one, since nothing forces the
  interleaving. Getting there took two
  corrections — the reader had to look up a name placed *after* the
  padding so it actually walks the array being reallocated, and the
  block had to be reused before the stale pointers in it could be
  wrong. `setenv` frees the array and never a string, so a reader on
  the stale copy otherwise reads correct pointers out of freed
  memory. Unlocked `atexit` separately accepts **33 registrations
  into a table of 32** and loses two handlers. Two fixes outside
  `stdlib.c` came out of building it, both recorded where they live:
  the shell's AND-OR lists were right-associative, so `A && B || C`
  with a failing `A` ran **neither** branch and `/etc/rc.test` could
  never print `SHTEST: FAIL n` (`docs/userland/invariants.md` U11);
  and `cosmo_thread_start` builds a stack by punching a hole in a
  reservation and re-mapping it `MAP_FIXED`, which another thread's
  `mmap` can take in between — `EEXIST` out of a thread start, three
  times on aarch64 CI, now retried while the real repair (a
  `MAP_FIXED` that replaces, as POSIX says) is filed in the
  inventory. 360 self-tests on both
  architectures, debug and release (PR #191).

- **`MAP_FIXED` replaces, as POSIX says, and the range is owned while
  it does.** `space_insert` refused any overlap, so a fixed mapping
  over live memory returned `-EEXIST` and a caller wanting to convert
  part of a range it already owned had to `munmap` a hole and `mmap`
  it back — two syscalls with the range belonging to nobody in
  between, which `vm_user_find_free` will hand to the next
  `mmap(NULL, …)`. `cosmo_thread_start` does exactly that to place a
  guard page below each stack and **lost the race three times on
  aarch64 CI**, getting `EEXIST` out of a thread start; the previous
  unit shipped a bounded retry and filed the real repair.
  `vm_user_map_anon_replace` is that repair: three critical sections,
  because the page-table teardown takes the space lock itself once
  per chunk, with the new region put in under the
  same lock that clears the old ones, marked `VM_REGION_QUIESCED`
  and still claimed across it, so **one region owns the whole
  interval — holes included — at every instant**. An earlier version
  claimed only the regions that existed and left a spanned hole for
  the allocator to hand out, which review caught as a route to a
  kernel panic. The
  claim is an ownership claim — a user fault on such a region
  installs nothing and retries, a kernel fault inside a copy reports
  `-EFAULT`, `munmap` refuses it with `-EBUSY`, and a per-space mutex
  serialises replacements — so the finishing swap cannot fail, which
  matters because every fallible step and the whole page accounting
  happen before the first change. Both doors use it: the native
  `mmap` now conforms, and the Linux personality, which already
  replaced but as an unmap followed by a map (**the same window, one
  door further in**), no longer has it. `COSMO_MAP_FIXED_NOREPLACE`
  keeps the old refusal, because "place this only if the range is
  free" is a real request and the self-tests assert it. libc drops
  the punch, the retry and `STACK_MAP_ATTEMPTS`. Invariant **M40**;
  `vm-replace` and `vm-replace-race`, each bug-proofed by four
  mutations — and one of those mutations found a vacuous case of my
  own, a refusal whose range could not split and so could not show
  that a failure changes nothing. 362 self-tests on both
  architectures (PR #193).

- **`mprotect`, which one personality already had.** The Linux
  personality has been able to change a mapping's protection since
  milestone 10 — `lx_mprotect` over `vm_user_protect`, which the ELF
  loader also uses — and the native ABI could not: this tree's usual
  second-door bug, inverted. `SYS_mprotect` (93) is the native door,
  keeping the native rules `mmap` and `munmap` keep (an undefined
  `prot` bit is `EINVAL` rather than ignored, `len` is a page multiple
  rather than rounded) and translating for the VM layer, which decides
  W^X, holes and claimed ranges. The part that only came out of review
  of the report: **user code cannot make freshly written bytes
  executable on AArch64**, because `dc cvau`/`ic ivau` need
  `SCTLR_EL1.UCI` and this kernel does not set it — so the kernel
  synchronises the instruction stream when a range gains `PROT_EXEC`,
  at both doors, for every JIT and not just a test (invariant M41).
  Honest about what QEMU can show: TCG invalidates translated code on
  write, so the write-then-execute self-test proves the path runs
  without trapping, not coherence — and removing the PAN bracket
  around the maintenance did not fault under the guard boot either, so
  the bracket is kept for the rule, not for a proof. Hardware is where
  both would be settled. `MAP_FIXED`
  replacement can never substitute for this call — it returns
  demand-zero memory — and libc's thread stacks deliberately keep the
  reserve-and-replace sequence #193 proved (PR #195).

- **The two thread calls the native door still lacked.** After
  `mprotect`, `README` still listed futex requeue and per-thread signal
  targeting as what native threads went without -- both built since
  milestone 10, both reachable only through the Linux personality.
  `SYS_futex_requeue` (94, the compare form only: the native ABI is new
  and need not carry the race glibc abandoned `FUTEX_REQUEUE` for) and
  `SYS_thread_kill` (95, `tgkill` with the process implied, so another
  process's thread is `ESRCH` by construction) close the gap. The unit's
  substance was in libc: `cosmo_cond_broadcast` now wakes one waiter and
  moves the rest onto the mutex, and the report's review found that a
  requeue is not a drop-in for a three-state mutex whose unlock wakes
  only when it finds 2 -- so the condition records its mutex (two words,
  published `SEQ_CST` against `seq`) and waiters relock through the
  contended path, which makes the one waiter a broadcast wakes the head
  of a chain that every unlock carries (invariant L10,
  `docs/libc/invariants.md`). The report's third rule, the broadcaster
  reading the word after the requeue, was removed by its own bug-proof
  passing, and with it the only load through the recorded pointer. The
  measurement libc's own comment
  had deferred: eight waiters, one broadcast, **no** sleep on the mutex
  word against seven for wake-all, on both architectures. Building it
  found a kernel bug: a requeue of a word onto itself re-pushed each
  waiter to the tail of the list being walked, an unbounded loop with
  interrupts off that any program could ask for; counted in place now,
  and that count is how `thrtest` knows its waiters are asleep rather
  than merely arrived. Eight `thrtest` steps, every rule bug-proofed by
  removal with a bounded join reporting the hang; the syscall fuzzer
  gets both calls, `thread_kill` with signal 0 only. Report:
  `docs/audit/next-subsystem-native-thread-door.md` (PR #197).
- **A device removed while it was busy, at last on a real one.** The
  lifetime-and-quiescence work named four windows nothing had ever
  raced; three got an adversary and the fourth — a virtio device
  removed with I/O outstanding — was narrowed to the *unbind
  transition* on a synthetic device, because the machine's only
  virtio-blk is the scratch disk every filesystem test runs on. The
  reason was a machine, not a mechanism: the machine now carries a
  second, 4 MiB virtio-blk that exists to be removed (`QEMU_RMDISK`,
  attached after every function the documentation numbers, so nothing
  moved). `virtio-remove-inflight` fills the driver's slot table by
  construction — a hook leaves the device's finished requests
  unconsumed, because a QEMU device answers in microseconds and the
  natural race finds **nothing** in flight, every run, on both
  architectures — removes the function while a submitter on another CPU
  keeps going, and asserts the protected object: every accepted bio
  completed exactly once with `0`, `-EIO` or `-ENODEV`, the `-EIO`
  count *equal* to what the remove found (64 of 64: no double
  completion, no stranded slot), and nothing completed after the
  boundary the removal stamps inside itself — a stamp the caller takes
  afterwards can be beaten by a callback that completes later and
  numbers itself earlier. Then it brings the disk back with
  `pci_test_rebind` and reads the sector written before the removal,
  which is what says the removal left the hardware sane and what lets
  the test leave the machine as it found it. Nine mutations, including
  `blk_unregister` dropped from the driver's remove (a kernel page
  fault) and a leftover slot completed twice. **And it found the defect
  it was built to find, through review rather than through the test:**
  `vblk_remove` read and cleared the driver's slot table -- with no
  lock, and *before releasing the queue's interrupt* -- then freed the
  ring and the DMA pool that a completion handler would be walking. A
  reset stops the device; it says nothing about a handler already
  inside the driver. The kernel has had the answer all along
  (`synchronize_irq`, and a handler is a quiesce read-side section):
  NVMe releases its vectors before freeing its queues, xHCI calls
  `synchronize_irq` by hand, AHCI disables its interrupt before tearing
  its ports down, and **virtio-blk was the only one of the four in the
  wrong order**. The fix is the order -- `virtq_free`, which releases
  the vector, now precedes the slot walk (invariant **Q11b**) -- and
  the third test pass holds a read-side section across the teardown and
  asserts the removal entered it, the section ended, and only then did
  the walk begin. A first attempt built a private barrier in the driver
  instead, on the mistaken belief that the kernel had no
  `synchronize_irq`; the review that found the defect found that too,
  and the mechanism it wanted was already there. Found on the way and
  repaired here because it blocked the gate: six checks in
  `lockuptest.c` asserted `thread_count() == before` the instant a join
  returned, and the count falls at the reaper, not at the join —
  `docs/testing/flakes.md`. Report:
  `docs/audit/next-subsystem-virtio-remove-inflight.md` (PR #199).
- **File-backed regions: the mappings the constitution requires.** The
  constitution's §14 *must* list has file-backed mappings, shared
  mappings and copy-on-write, and the VMM had two region kinds and none
  of the three: the native `mmap` refused every file mapping with a
  comment that had outlived the VFS ("file mappings arrive with the
  VFS"), and the Linux personality copied a file eagerly into an
  anonymous region, refusing `MAP_SHARED|PROT_WRITE` and handing a
  read-only `MAP_SHARED` mapping a snapshot no later `write()` ever
  reached. Now a file is mapped as a `VM_REGION_FILE` region over **the
  page cache's own frames**: a shared mapping and `read()`/`write()` are
  one frame, coherent in both directions with nothing to synchronise;
  a private mapping is copy-on-write, per page; every page is
  demand-paged. The fault runs in two phases under the order
  `vnode → pagecache → vm_space` with the install **under the cache
  mutex**, so a truncate or a write-back on the same file is serialised
  against it without a third mechanism, and the region is found again
  by what it maps -- vnode, index, sharing -- before anything is
  installed. Building it found that both filesystems trim the cache
  *before* they lower the size, so the cache keeps a bound for that
  window. Dirtiness is a write fault, not a hardware bit: a shared
  page's PTE gains write only in the fault handler and write-back lowers
  it before the bytes are read for the disk. A cache frame is referenced
  once per PTE; reclaim skips a referenced frame; truncate unmaps before
  it frees; and every process exit checks `file_pages == 0` as it
  checks `anon_pages`. `SIGBUS` past the end or on a read that fails;
  `maxprot` so a shared mapping of a read-only fd cannot be made
  writable; `msync` at both doors (`SYS_msync` 96, `LX_msync` 26);
  `COSMO_MAP_SHARED`/`COSMO_MAP_PRIVATE` with the native refusal rules
  (`SHARED|ANONYMOUS` is `EINVAL`: there is no `fork`, so nobody to share
  with). The dynamic linker's mappings became demand-paged copy-on-write
  mappings of the same files, with the existing `lxtest` dynamic cases
  as their regression suite. An `mmap` section of `init --selftest`
  proves the coherence both ways, **the first shared memory between two
  processes in this system** (a spawned child writes a byte the parent
  reads), copy-on-write per page, the rights, the native rules, `SIGBUS`
  past the end and under a truncate (children, by status), the
  address-space limit, a leak cycle whose exit runs the count check,
  and on cosmofs that `MS_SYNC` writes exactly the dirtied page and a
  write after it faults and is written again; three kernel tests hold a
  fault between its phases through an event-driven seam while another
  thread installs the same page, unmaps the range, or replaces it with
  another file (nothing installed, one frame, the other file's byte),
  make a cache miss's read fail under a mapping (`SIGBUS`), and pin a
  frame against a forced reclaim. Nine mutations, each run: the
  write-back not lowering PTEs and the fault marking nothing dirty fail
  the same section on *different* checks (the counter tells them
  apart); a frame installed without its reference is caught by the
  poisoner with the section's own byte in the dump; a truncate that
  frees before unmapping dies on the frame-count assertion; the bound alone survived, as the report declared in advance (the window is inside a filesystem's truncate and no seam sits there); the re-find by kind alone let a held fault install one file's page under another file's name, which only the third held-fault variant could show; `maxprot` ignored and `SHARED|ANONYMOUS` accepted each failed their probe. **And self-review found what the report had not**: `mprotect` on a private mapping raised a read-only cache frame to writable, so a write after it went through to the file; the rule is per frame now -- a cache frame's PTE never gains write from `mprotect`, only a copy-on-write copy does -- with its own check and its own mutation. Review of the build found three more, each fixed with a check: the copy-on-write path that replaces a present frame did not consult `COSMO_RLIMIT_MEM`; a `write()` into a page some mapping executes from changed instructions with no cache maintenance, so the cache now synchronises by the frame's kernel alias; and the Linux door validated no mapping type.
  Invariants **M42**--**M44**, **V33**; 366 self-tests on both
  architectures. Report: `docs/audit/next-subsystem-file-regions.md`
  (PR #201).
- **A futex keyed by what the word maps.** The file-regions unit gave
  two processes one page and left them no way to wait on it: the futex
  was keyed by address space, so a sleeper on a word in a shared page
  was invisible to a wake from another process on the same word --
  nothing refused, the wake found nobody -- and the Linux door masked
  out `FUTEX_PRIVATE_FLAG`, the one bit that says which kind a futex is,
  so a musl process-shared mutex in a shared page was the program that
  broke. Now a futex's identity is what the word maps: the space and the
  address for a word in the process's own memory, the vnode and the
  file offset for a word in a `MAP_SHARED` file mapping, with the one
  vnode reference the waiter holds for the vnode its current key names.
  The VMM classifies under the space lock and skips the walk entirely in
  a process with no shared mapping, so libc's mutex pays one load; the
  Linux flag is honoured both ways; the native calls always classify,
  on the native rule that the kernel does not ask the program what it
  can see. The whole change to the futex is its hash and two match
  lines; the sequences and the compare-then-enqueue that L4 rests on
  are untouched, and a requeue that changes the key exchanges the
  reference -- one add of as many references as waiters moved, under
  the bucket locks, the old ones put after them -- which three review rounds on the report
  sharpened from "the reference travels with the waiter" to a rule that
  survives an unbounded chain of requeues with one slot. The `mmap`
  section proves the two-process wait (**the first wait across two
  processes in this system**, the parent counting the child asleep
  through a requeue of the word onto itself, a count that crosses the
  boundary only if the key does), the wake before the sleep, private
  stays private, unmap under a waiter, requeue across kinds and onto a
  shared word that then goes, and a double requeue through two files
  with the first released between; `lxtest` proves the flag both ways
  on a shared page with clone threads. Building it found that the
  file-backed page fault ran with interrupts masked, as every trap
  enters -- the anonymous arm never minded, the file arm sleeps and
  shoots down, and the file-regions unit's own tests never contended
  the cache mutex, so the assert never fired until this unit's did:
  the arm now runs with interrupts as the interrupted context had them.
  Seven mutations, each run: the key by space alone and the counter's
  test inverted each lose the two-process wake (the child times out,
  the count never crosses); the flag still masked fails all four
  `lxtest` checks; the counter not decremented panics at the first exit
  of a process that shared; the old references not put after a requeue
  leak the first file's vnode, which the double-requeue's page count
  sees; the moved waiters' references not taken panics on a released
  vnode; and the reference not taken at classification survives, as the
  report declared in advance. Bench: a wake costs 2.0 / 3.9 us with no
  shared mapping on x86-64 / AArch64, 3.9 / 6.6 us with one, and a
  cross-process round trip 132 / 145 us. Invariant **I7**.
  Report: `docs/audit/next-subsystem-shared-futex.md` (PR #203).
- **The count that could wrap, and the hold that held only at the door.**
  `virtio-remove-inflight`'s held pass found **0** in flight twice in
  CI, on branches that touch no driver, with every bio completed `0`
  and the pass over in 30 ms. The cause was the test's own count: the
  submitter counted an accept after `blk_submit` returned while the
  completion callback on the other CPU had already counted the
  completion, so `accepted - completed` -- two unsigned words --
  wrapped for that instant, the wait for a full table exited at once,
  and the remove walked an empty table. The accept is counted before
  the submit now, and the pass asserts `completed <= accepted` on every
  turn of its loops, completed read first and both atomically, from
  the test thread while the submitter runs on the other CPU; the worst
  case of the old order fails that assertion within a millisecond
  (PR #206). Reading the first sighting also found a real hole in the
  test's seam, closed on the way (PR #204): the hold that parks a
  device's finished requests was checked once at `vblk_done`'s entry,
  so a handler already inside its pop loop when the hold landed kept
  popping. The check runs before every pop now, and a fourth pass,
  `held-inside`, builds that moment rather than racing for it -- every
  hold in it stored from a completion callback, the parked requests
  known to be finished at the device by an exact seam (`unconsumed`:
  used entries not yet popped) instead of a wait; with the check back
  at the door it fails deterministically. The hole was real and was
  first taken for the cause; the second sighting, on the driver with
  the hole closed, is what named the count. `docs/testing/flakes.md`;
  `docs/kernel/device/testing.md`; the #199 report's banner, items 11
  and 12.
- **Unix domain sockets: a name in the filesystem, and a handle that
  rides in a message.** Since the service manager was built the README
  has said "a named pipe and a unix socket — are both things this
  kernel does not have"; the second exists now, and the `mknod` the
  first needs with it. A third family in the socket object
  (`COSMO_AF_UNIX`, dispatched inside the socket layer, so no door or
  rights table learns a new kind): a stream connection is the pipe's
  bounded queue twice, a datagram socket one such queue, a listener a
  backlog-bounded queue of connections made at `connect` -- the client
  writes before anyone accepts and the bytes wait. A name is a
  `VNODE_SOCK` node made by the new `mknod` vnode operation (ramfs;
  cosmofs has no on-disk type and refuses), mode 0755, owned by the
  caller, the node's mode the access control for connecting, `open`
  answering `ENXIO`; or an abstract name keyed by the caller's **root**,
  so a jail sees only its own. A message carries bytes, a name and
  handles: a handle rides under spawn's transfer rule, factored into
  one function both callers use, and arrives with the rights the sender
  named, installed in order until the first refusal (`HTRUNC`); a unix
  socket does not ride (the cycle Linux garbage-collects is refused,
  and the collector named as the unit that would lift it). `socketpair`,
  `SO_PEERCRED`, `sendmsg`/`recvmsg`; the Linux door's `AF_UNIX`,
  `sockaddr_un` in its three forms, `SCM_RIGHTS`, `MSG_CTRUNC`,
  `socketpair`. **Building it found two things.** A waiter on another
  socket must hold no reference to it: a connector blocked on a full
  backlog that held the listener kept it alive past its last handle,
  and the wait depended on a release the wait prevented -- so such
  waiters sleep on one generation queue and resolve the name again
  (invariant **I8**). And the Linux door's `pipe2` and `openat`
  installed handles with no owner rights, so no Linux descriptor could
  ever have been passed; they carry them now. Six self-tests (a stream
  end to end, datagrams, names, handles, a two-CPU close race,
  readiness), a `unix` section of the user suite (a child on the other
  end of a pair, a file handle across it, `SO_PEERCRED` naming the
  child, a uid-1000 child refused by the node's mode, a jailed child
  reaching neither the abstract name nor the path), `lxtest` rows;
  nine mutations, each caught by a named test or, for the reference
  rule, by the deadlock it exists to prevent. Bench: a one-byte round
  trip to a child costs 121 / 147 us over a unix pair on x86-64 /
  AArch64 against 140 / 181 over two pipes. Invariant **I8**; 372
  self-tests on both architectures. Report:
  `docs/audit/next-subsystem-unix-sockets.md` (PR #207).
- **Named pipes: a pipe with a name, and files that can be waited
  on.** The other half of the sentence the unix-sockets unit left: a
  `VNODE_FIFO` node made by the new `SYS_mknod` (100) through the same
  `mknod` vnode operation (ramfs; cosmofs and procfs refuse), behind
  which sits the pipe's ring -- split from its two end objects, which
  became one client of it, with `ipc-pipe` unchanged as the proof. The
  ring is made by the first open and freed by the last release, and
  its reader and writer counts are the live **opens** of each side, so
  the pipe's own end-of-file and `EPIPE` rules follow from them
  (invariant **I9**). Open is POSIX's: read-only waits for a writer
  (one that opened since the reader joined, even if it has closed
  again by the time the reader runs: the other side's open generation,
  not its count, which lost exactly that writer),
  write-only for a reader, `O_NONBLOCK` (`0x0800`, new to the native
  `open`) makes the first return at once and the second `ENXIO`,
  `O_RDWR` never blocks; the wait is killable, and an open that fails
  -- killed, refused, out of memory -- takes back its count and any
  ring it made, because the VFS runs release only for an open that
  succeeded. Non-blocking mode is per open, as POSIX has it. For it,
  **a `struct file` can now say whether it would block**: three
  optional `vnode_ops` (`ready`, `poll_wq`, `set_nonblock`) that the
  file kobject type delegates to, so `ioready`, `setnonblock`, `poll`
  and the async ring work on a FIFO handle; `chrdev_ops` gained the
  same three and no existing device sets them yet. The Linux door's
  `mknodat` (`S_IFIFO`; `S_IFSOCK` `EINVAL`, the rest `EPERM`) and an
  `O_NONBLOCK` that reaches the kernel; libc `mkfifo`; a `mkfifo`
  coreutil and a FIFO in the shell test (a pipeline whose two commands
  meet on the name, since this shell runs `&` in the foreground without
  job control and opens redirections before it spawns). One kernel
  self-test (`ipc-fifo`: both open orders from a second thread, the
  non-blocking rules, per-open mode, two readers and two writers, an
  unlinked FIFO, and a real process killed inside its open, the ring
  counted before and after every case), a `fifo` section of the user
  suite, `lxtest` rows; nine mutations, each caught by a named check.
  Bench: a one-byte round trip to a child costs 167 / 197 us over
  two FIFOs on x86-64 / AArch64 against 150 / 182 over two pipes in
  the same runs: the same ring, plus the file layer. 373 self-tests on
  both architectures. Report: `docs/audit/next-subsystem-named-pipes.md`
  (PR #209).
- **A migration that can land: declared per-CPU claims, a lock order
  lockdep can see, and a migrator that moves one thread.** Thread
  migration was built and removed once because the tree held per-CPU
  assumptions nothing declared; this unit is the prerequisite, not the
  balancer (`docs/audit/next-subsystem-percpu-migration.md`). The rule
  (scheduler S25): a per-CPU answer is kept only while the thread cannot
  move -- preemption disabled, interrupts off, interrupt context, or an
  affinity of one CPU -- and `preempt_disable()` is the migration
  barrier, since a thread with preemption disabled is never READY and
  only READY threads move. Debug builds check the rule in `this_cpu()`
  and `arch_cpu_id()` and panic naming the call site; `raw_this_cpu()` /
  `raw_cpu_id()` are for the current thread, an asserted-zero count, or a
  diagnostic, each with its reason; `PERCPU_WARN=1` lists every site
  once instead (the sweep's form). The sweep named 113 sites on x86-64
  and 133 on AArch64; seventeen were claims a migration would break,
  `schedule_internal` reading its own per-CPU block before the run-queue
  lock first among them -- the corruption that removed the first
  balancer, named -- and the EL2 hand-back on AArch64; all are fixed by
  making the read happen where the rule holds. Each run queue's lock is
  its own lockdep class (a static name table), so two of them in
  increasing CPU-id order is an order lockdep checks (S24, real now).
  `sched_migrate(t, cpu)` and `sched_migrate_from(from, to, &moved)`
  move a READY, non-current thread under both locks and say *which*
  check refused; the woken-before-blocked window (READY, queued, still
  `rq->current`) is refused by identity (S26). `make test-chaos` boots a
  `SCHED_CHAOS=1` debug kernel whose tick moves a ready thread to
  another CPU every fourth tick, on every CPU, through the whole suite
  and the user-mode sections, requiring its tally line; CI runs it on
  both architectures. Five tests (`percpu-claim`, `lockdep-rq-order`,
  `sched-migrate`, `sched-migrate-refuses`, `sched-migrate-stress`: about
  3,500 moves in 200 ms on x86-64, 8,000 on AArch64), 380 self-tests on
  both architectures. Not in this unit: the balancer, which is the next.
  (PR #213)
- **A balancer that pulls, and a load that can see the thread already
  running.** The migration primitive moved threads only when a test or
  the chaos migrator asked; nothing in a shipped build moved one. This
  unit is the policy (`docs/audit/next-subsystem-load-balancer.md`).
  Measured first: twice as many threads as CPUs, created one at a time
  so the rotation places them one per CPU, then every other one released
  -- the runnable set lands two-deep on half the CPUs, the other half
  stays idle for the whole run, and the machine does 53% of the work it
  could on x86-64 and 60% on AArch64 (40-47% lost across four boots).
  `sched_cpu_load(c)` is that queue's `nr_running` plus the thread it is
  running unless that is its idle thread (S29): `nr_running` counts the
  ready list and `schedule` dequeues what it runs, so a CPU saturated by
  one thread reported the same zero as an idle one, to `pick_cpu` as
  much as to anyone. Balancing is a **pull** (S27) from `sched_tick`
  after its own unlock: an idle CPU with an empty queue looks every
  tick, a busy one every `SCHED_BALANCE_TICKS` (16, 64 ms), and one
  thread moves when the busiest CPU is at least two ahead (S28) -- one
  is the steady state of an odd thread count, and a CPU running one
  thread with an empty queue is at load 1, so its thread is never
  dragged to an idle CPU to arrive cold. The scan reads other queues
  without their locks and is a hint; `sched_migrate_from` re-decides
  under both locks and picks the thread itself. **It cannot move a
  thread that is time-slicing**: two compute-bound threads on one CPU
  alternate by preemption, so the one in the queue always carries
  `THREAD_FLAG_PREEMPTED` and S26 forbids moving it -- found by the
  hysteresis test, which passed under a deliberately broken threshold
  because the thread it wanted moved could never move. So the balancer
  corrects an imbalance as work becomes runnable, not once it has
  settled into alternation. `SCHED_BALANCE=0` compiles it out and is how
  the tests prove it; the boot prints its tally and `sched_dump`'s
  per-CPU line carries the load. Four tests and a benchmark
  (`sched-load`, `sched-balance-pull`, `sched-balance-hysteresis`,
  `sched-balance-affinity`, `bench-balance`: the alternate round reaches
  99-108% of the *balanced* round -- the same threads unpinned, spread
  because creation order happened to do it, which differs from it in
  exactly one thing -- against the 53% the report measured without the
  balancer; the pinned figure is reported beside it and is not what the
  assertion is against).
  (PR #216)
- **A back-connection that survives one reset.** `net-harness` failed
  seven CI jobs on 2026-09-22 alone, across four pull requests and
  `main`, two of which changed only documentation -- and the defect is
  not in this kernel. QEMU's user-mode networking resets the guest's
  half of one connection while keeping its own half open and answering a
  probe through the same instance a millisecond later; the guest is
  correct from first SYN to final reset, verified against a packet
  capture. Three units localised it and what is left is in slirp's
  source (`docs/audit/next-subsystem-nettest-retry.md`). So the guest's
  back-connection now runs the exchange up to three times on fresh
  sockets. The bound is a **failure**, not a fallback: exhausting it
  fails the test exactly as one reset did, printing
  `client failed every attempt (3 of 3)`. Every attempt prints the full
  diagnostic block the earlier units built, and both outcome lines name
  the attempt -- `client ok (attempt 2 of 3)` -- so a recovered boot is
  still a sighting the same grep finds, and `docs/testing/flakes.md`
  counts it among the recovered ones rather than losing it. A retry that
  hid the flake would be worse than the flake. The host harness needed
  no change: since PR #177 it accepts eight connections and picks the
  one that delivers the request. `make test-harness-retry` builds a
  `HARNESS_BREAK=1` image whose first attempt is shut down from inside
  the guest after its connect, so the retry runs on every boot of that
  build instead of one in twenty, and the runner requires both halves --
  that an attempt was broken and that a later one carried the exchange.
  (PR #218)
- **A program's text belongs to the file, not to each process that runs
  it.** The loader mapped every `PT_LOAD` as anonymous populated memory
  and copied the image in, so two processes running one binary held two
  complete copies of its text and every page of every segment was
  allocated whether or not it was ever touched
  (`docs/audit/next-subsystem-elf-shared-text.md`). Measured on `init`
  by spawning copies and subtracting free-frame counts: **89 pages per
  copy, the fourth costing exactly what the first did**. A `PT_LOAD`
  that is not writable and has no zero tail now comes from the file's
  page cache, shared, so every process running the program maps the same
  frames -- with `maxprot` excluding `W`, which is what stops a later
  `mprotect` from making shared text writable. The segment's zero tail
  is demand-paged rather than populated, because an anonymous page
  arrives zero. **89 pages per copy became 40, then 16**, identically on
  both architectures. Shared text makes a running program's instructions
  the file's, so a write to a file being executed is `-ETXTBSY`
  (invariant **P30**) -- and *busy* is a property of the page cache's
  mapping list, not a counter beside it: the record is linked there when
  the mapping is made and unlinked before its vnode reference goes, and
  the check is taken under that list's lock by a writer already holding
  the vnode's, so the answer and the write are atomic against a mapping
  being created. `VM_MAP_TEXT` is passed by the loader and by nothing
  else, because a program that maps a file executable and writes to it
  deliberately is a different thing the page cache already serves. Four
  tests: one physical frame for two address spaces, the per-copy cost,
  `PROT_WRITE` refused on shared text with the zero tail reading as
  zero, and all three doors a file's contents can change through -- a
  write, a truncate, and a writable shared mapping -- refused while a
  program runs and allowed once it exits, with a private writable
  mapping allowed throughout as the control and the text mapping
  refused in the other order too. Demand-paging the zero tail also gave a multi-threaded program
  its first pages two threads could fault at once, and the anonymous
  fault panicked when they did: the flags a fault carries are the
  hardware's snapshot from when the trap was raised, so both threads
  believed the page absent and the loser mapped over the winner. The
  page table is the authority now -- asked under the space lock
  immediately before the install, exactly as the file-backed fault has
  always re-found its region (invariant **M45**, test
  `vm-anon-fault-race`). (PR #221)
- **A held walk: the working-directory race becomes a proof**
  (`docs/audit/next-subsystem-cwd-hold.md`). The cwd-ref unit fixed a
  use-after-free with a reference taken for the length of the walk, and
  guarded it with a test that passed when the fix was removed -- a
  window a few instructions wide against a whole path walk. Measured
  first: with the bug put back at the native `open` and every freed
  vnode poisoned, the test catches it in **one x86-64 boot of five** and
  no AArch64 boot of three, which corrects that unit's record that a
  poisoned run passes, and says why: nothing in the tree poisoned a
  freed vnode, so a walk that outlived its reference read a plausible
  directory. Now it reads `0x5a`, in debug builds, as freed frames
  already do. And the interleaving is no longer left to chance: a seam
  in `walk_parent`'s relative branch -- the one line every relative walk
  from every caller shares, since `vfs_open` never enters `resolve()` --
  holds a walk of the armed process with its pointer in hand until that
  process's `chdir` has published and put, with `chdir` waiting for the
  hold **before** it publishes so the held walk has necessarily captured
  the directory being replaced. Both waits are killable and bounded. The
  swapper records the old directory's count before its put: **two** in
  the pass that removed the directory (the process's and the walk's) on
  a correct kernel, one with the fix removed, where the walk then resumes
  on the poison and the kernel panics by name. Two racers, one per door
  -- the Linux door had no test of this at all -- each run once per pass
  with the seam rearmed, every claim a count the seam read rather than a
  flag the code under test set. **Two mutations survived the first round
  and both said the test was weak, not the rule dead**: the release
  order derived on the walk's resume could not tell a release a few
  instructions before the put from one after it, because the woken walk
  loses that race every time, so the releasing side now reads the count
  at the instant it releases; and no racer had the swapper arrive first,
  so a third pass starts the walker only once the seam itself says the
  swapper is waiting inside `chdir`. A `chdir` in a single-threaded
  process registers no swapper, because it has no walk to race.
  Invariant **V35**; P29 gains its proof. (PR #224)
- **`tcp-pcb-timer-free` could park its callback above its own armer,
  with nothing left to release it.** Twice on 2026-09-23, in the same
  slot after `net-lo-udp`: a hard lockup at ten seconds once, a TLB
  shootdown unanswered at one second once. The armer arms a 1 ms timer
  and must exit on its CPU before the next tick; when it did not, the
  callback spun in interrupt context above it, the test's join of the
  armer -- made before the releaser existed -- never returned, and the
  releaser was never made. Made deterministic by having the armer linger
  two ticks on purpose, which reproduced the hard lockup to the line on
  the first boot; fixed by creating the releaser before arming and
  joining the armer after the release, with the callback's CPU recorded
  and asserted. (PR #225)
- **A placement is inserted under the hold that chose it.** `mmap(NULL,
  ...)` at either door chose a range under one hold of the space lock and
  inserted it under another, and the range belonged to nobody in
  between. Measured by running the syscall's own two calls from kernel
  threads on one space: **about half of all concurrent placements lost**,
  on both architectures, with `-EEXIST` for a request that named no
  address -- which is how a `thrtest` thread start failed on a
  documentation-only rebase, once the `MAP_FIXED` unit had removed the
  libc retry that used to absorb it. Both doors now choose and insert in
  one hold (`vm_user_map_anon_free`, `vm_user_map_file_free`), two
  attempts from the hint and then the base with `-ENOMEM` the only
  answer that moves between them; the file form inserts its region
  claimed before its record goes on the vnode's list, because truncate
  reads the base from there. `vm_user_find_free` stays, advisory, for
  the two callers that may use it. Invariant **M46**, the fixed path's
  M40 made whole; `mmap-place-race` and a racer at each door, rate-based
  and said so, at a rate that cannot hide. (PR #227)
- **A directory descriptor names a directory.** Every `*at` call in the
  Linux personality answered a real directory descriptor with `ENOSYS`,
  on the stated premise that the VFS could not resolve from one -- it
  always could, every entry point takes a start -- and `fchdir` was not
  in the table. Measured before the change: `openat`, `newfstatat`,
  `faccessat`, `readlinkat`, `symlinkat`, `mkdirat`, `mknodat`,
  `unlinkat`, `renameat` and `fchdir` all refused, on both
  architectures, which is every tree walker failing the moment it
  descends. One resolver replaces the refusal at nine sites and
  references the descriptor's directory before letting go of the
  descriptor (V35), demanding the handle's `READ` right to look a name up
  and `WRITE` to change an entry, so a directory handle delegated
  read-only stays read-only -- including what is opened through it (a
  directory opened at either door carries both, masked by the rights of
  the descriptor it was opened through); `renameat` resolves its two names from two directories through
  `vfs_rename2`; a directory file remembers the path it was opened by, at
  both doors, **only if that name walked with no symbolic link reaches the
  same directory** (since the cwd-name unit, PR #232, the name its open's
  own walk took, so a directory opened through a link has one), and
  `fchdir` publishes the name with the vnode as `chdir` does. Review of the first build found the missing rights, a
  child directory opened through a narrowed one regaining them, the
  incoherent name and a failed `chdir` stranding the held-walk seam's
  swapper; each has a test. Invariant **P31**; `lxtest` checks every call
  against a real descriptor by where its effect lands, not merely that it
  succeeded. (PR #229)
- **`tcp-pcb-timer-free` holds only its own connection's callback.** Its
  test hook held the next TCP timer callback of *any* connection, and
  once, on the dirfd unit's branch, a connection an earlier test had
  left behind fired first and was held in the test's place: the test's
  close had nothing to wait for and the test failed after five seconds
  with its CPU's tick stalled. `tcp_test_hold_callback` now takes the
  pcb to hold, `timer_kick` holds only that pcb's callback and counts
  any other it lets through, and the test arms a decoy connection's
  timer one tick before its own on every boot, so the sighting's shape
  is certain rather than waited for. With the hook back to "any pcb",
  the decoy is held and the test fails. (PR #230)
- **A working directory's name is the path the walk took.** `chdir`
  published a lexical normalisation of its argument with the vnode the
  walk reached; through a symbolic link the two named different
  directories, and after `chdir` through a link and `chdir("..")`
  `getcwd` answered `/tmp` while the process stood in `/tmp/clp` --
  measured at both doors on both architectures
  (`tools/chdir-link-probe.py`). The walk now keeps the name it took
  when asked (`vfs_lookup_named`): each component entered as a
  directory, links replaced by where they led, `..` removing one. `chdir`,
  a spawn's `cwd` and a directory file's name for `fchdir` all publish
  it, so `fchdir` of a directory opened through a link now succeeds with
  its own path where the dirfd unit refused it. The same probe found
  `chdir("..")` from `/proc/<pid>` was `ENOENT`; procfs's process
  directories answer `..` now, and `/proc/self` is a symbolic link to the
  reader's pid, so no name in the tree means different directories to
  different processes. Invariant **P32**. (PR #232)
- **Mount and unmount name what the caller's path names.** Every path
  call resolves a relative path from the working directory except two:
  `mount` and `umount` resolved their target from the root, so from
  `/tmp` a mount on `mrel` covered `/mrel` -- measured on both
  architectures (`tools/mount-rel-probe.py`) -- and the matching unmount
  removed the same wrong mount, so return codes looked fine. Found while
  taking up an inventory row that wanted a test for the
  one-unmount-at-a-time guard "through a relative path from inside the
  mount": that path did not exist. `vfs_mount_at` and `vfs_umount_at`
  take a start and the two calls pass the cwd; `vfs-umount-once` then
  fires the guard for the first time, with a held maintenance pass
  keeping the first unmount in its drain and a second from inside
  refused `-EBUSY` at once. P27 names the two calls. (PR #234)
- **The balancer's test asserts what the balancer promises.**
  `sched-balance-pull` failed five CI runs in two days under the chaos
  migrator, on branches that did not touch the scheduler. Measured: every
  spread takes under 40 ms and a miss never recovers; the miss is two
  spinning workers alternating on one CPU, and a spinning pair built on
  purpose is never separated -- an idle CPU is refused some five hundred
  times a second, because S26 forbids moving a thread that may have been
  preempted between the two instructions of a per-CPU access -- while a yielding pair is separated in
  milliseconds. The test had been asserting a race: that the balancer
  pulls a spinner before its first preemption, which one chaos move lost
  by the rules. Its released workers now yield, so it asserts the
  contract, and `sched-balance-pair` asserts both halves of the
  mechanism. No kernel code changed. (PR #236)
- **`sched-balance-pair` read a worker that had not run as "on CPU 0".**
  After #236 merged, the test failed once on aarch64 with its spinning
  pair "separated": an instrumented run found three such rounds in 25,
  each after 0 ms and with no migration at all. A worker's recorded CPU
  started at zero and was first written in its loop, so a worker still
  on its way there looked like one on CPU 0. It now starts at a value no
  CPU has, the premise waits for both workers to have written the
  pair's CPU, and the pull test's counter skips an unrun worker (which
  had been a possible false pass). Sixty rounds then: none apart. (PR #237)
- **The lockup sampler's bound is a count of waits, not a stopwatch.**
  `lockup-sample-busy` asserted that two CPUs that cannot answer cost
  the sampler one 5 ms timeout, by timing it: under 7 ms. It failed ten
  times in eight days at 88-241 ms. Measured (`tools/lockup-busy-probe.py`):
  quiet, one sample takes 5.00-5.12 ms; under host load the excess is
  time the virtual CPU did not run, most of it in one gap between two
  clock reads. And on x86-64 the "masked" targets answered by NMI, so the
  claim had never been exercised there. The sampler now reports, per
  call, what it did -- claim attempts, waits armed, the interval armed
  (`lockup_sample_all_info`) -- and can be asked, for that call alone, to
  send the ordinary interrupt so masked targets cannot answer on either
  architecture; the test checks one wait of exactly the timeout, and the
  loser's "refused at once" as an order plus a single claim attempt. The
  remaining time bounds are 1 s hang guards. (PR #239)
- **The aarch64 console no longer stops taking input.** Three times on
  CI, an aarch64 release boot's shell stopped echoing mid-line right
  after a job event and never recovered. Provoked on purpose
  (`tools/console-stall-probe.py`), it stalled in 13 of 20 boots. Looked
  at from outside, the guest was idle and ticking, and the PL011's
  receive FIFO was full with its interrupt enabled but none pending. The
  receive handler drained the FIFO and then cleared the interrupt, so a
  character arriving between the two lost its interrupt, and QEMU raises
  none for the characters behind it. The handler now clears first and
  drains after (T16). A new self-test, `console-rx-clear`, uses the
  PL011's loopback to put a byte into the FIFO just after the drain, and
  fails with the old order on every boot. The shell harness's release
  boots also run six cycles of a background job exiting as a line is
  typed. (PR #241)
- **The operator can see why a guest's flows are refused.** A guest that
  fills its share of NAT's table or the firewall's has every new flow
  dropped. Before this, nothing an operator could read changed when that
  happened (`tools/net-visibility-probe.py`): a guest was refused 232
  flows while the control snapshot stayed byte-identical and the log
  silent, and the counters were read only by the self-tests. The
  `/dev/net/tapctl` snapshot, now version 6, lists every live NAT and
  firewall flow, opener to peer, with its NAT identity, established flag
  and time left, beside the shares and the refusal counters. `vmctl
  flows` prints them. The counters were split first, one cause each:
  three of them had lumped a full share with a full table, and one
  counted something that was not a refusal at all. Port-forwarded TCP
  flows can now become established, and only when the handshake completes
  in order: the guest's SYN-ACK, then the client's ACK. A client's
  unsolicited ACK can no longer hold a guest's share for the long timeout.
  Before, they never became established, and the table kept them for 30 s
  instead of 300. Invariant N24: a flow is listed
  exactly when its share counts it. (PR #243)
- **Devices that can be waited on: readiness for the terminal and the
  tap, and `select` for the Linux door.** The named-pipes unit gave a
  `struct file` and `chrdev_ops` the three readiness operations and
  wired no device; this unit wires the two that block. `/dev/console`
  and `/dev/tty` answer what the console object answers
  (`tty_read_ready`, the readers queue), and `/dev/tty` for a caller
  with no controlling terminal answers `ERROR`, which is what its
  read's `ENXIO` looks like to a poller. `/dev/net/tap` gains a wait
  queue its transmit wakes and **a read that blocks unless the open is
  non-blocking** -- the one contract change, from "0 when none, the
  owner polls" to Linux's tun; `vmctl` opens it `O_NONBLOCK` and is
  otherwise unchanged. A device's per-open non-blocking mode is the
  open file's `O_NONBLOCK` flag through two small VFS helpers, so
  nothing is allocated per open. With that, an asynchronous `READ` or
  `POLL` on the tap parks until a frame is transmitted, which is the
  constitution's "async I/O must work for devices" shown for the first
  time. The Linux door gains `select` (x86-64) and `pselect6` over
  `io_poll` -- musl's `select` is `pselect6`, so every `select` caller
  had been getting `ENOSYS` -- with Linux's own set membership and one
  documented deviation: the except set (`POLLPRI`) is always clear,
  because no object in this tree reports a priority event. Two kernel
  self-tests (`tty-devready`, `tap-ready`: readiness against the read,
  a poll woken by a fed line and by a transmitted frame, per-open mode,
  a process killed inside the tap's read releasing the tap only after),
  a `devices` section of the user suite, `lxtest` rows including a
  `select` over the tap and a UDP socket; eleven mutations, each caught
  by a named check. Bench: a frame written reaches a `READ` parked on
  the tap in 315 / 165 us on x86-64 / AArch64, against the 2 ms
  poll interval, floored to a tick, that `vmctl`'s loop imposes on every
  host-to-guest frame today; `pselect6` costs 4371 / 4373 ns per call
  on one descriptor against `ppoll`'s 16423 / 14205. Invariants **V34**
  (vfs) and **N23** (network); 375 self-tests on both architectures.
  Report: `docs/audit/next-subsystem-device-readiness.md` (PR #211).
- **Next:** the roadmap's numbered phases and the post-roadmap audit's
  own list are complete, apart from pid renumbering, which the process
  domain deliberately does without and argues against
  (`docs/kernel/security/design.md`, "This is not a pid namespace"). Every
  §68 report under `docs/audit/next-subsystem-*.md` has been built and
  has its entry above. What each unit deferred, what the constitution and
  the audits set for later, what is deliberately not done and on what
  condition it would be revisited, and what must not be proposed again
  are gathered in one place and cross-checked against the tree:
  **`docs/audit/2026-09-deferred-work-inventory.md`**. The next report is
  chosen from it and names the entry it closes. Section **68** is not a
  list of deferrals: it is the instruction to stop after the audit, name
  one subsystem in a fixed shape and wait. Design documents first, one
  subsystem at a time.
