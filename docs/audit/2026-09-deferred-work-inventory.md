# Deferred work inventory

Date: 2026-09-14. Tree: `main` at 0318119 (after PR #131, the suite-waits
unit). Purpose: **the base for the next §68 reports** -- everything the
repository's own documents defer, delay or set for later, in one place,
cross-checked against the code so that a report proposes something that
is actually open and does not re-propose something already built.

Sources read in full: `README.md` (the Status section, lines 75-1933),
`docs/audit/2026-09-post-roadmap-audit.md`,
`docs/audit/2026-09-lifetime-quiesce-report.md`, and the three prompts
under `prompts/` (the constitution, Prompt #2, Prompt #3). Where an item
comes from a document written on 2026-09-05, it was re-checked against
the tree by `grep` on 2026-09-14 and is listed only if still true. README
line numbers are as of this commit.

**How to use this file.** A §68 report picks from sections 1-4 and says
which entry it is closing. Section 5 is what must *not* be proposed
again. An item that a unit closes should be struck through here in that
unit's documents commit, with the PR number, so the inventory stays the
truth.

---

## 1. Named follow-ups the README's Status entries leave open

These are the deferrals stated in the units' own entries. The README's
closing "Next" paragraph used to gather them; this file replaces that
paragraph (same pull request), and the README now points here.

### 1.1 The network arc

| item | where deferred |
| --- | --- |
| per-interface host chains (all real links share one chain) | README.md:1232, 1303, 1342, 1387, 1440 |
| rate-limit and logging targets in the filter | same lines |
| IPv6 filtering | same lines |
| full TCP state tracking in the filter | same lines |
| IPv6 NAT | README.md:1112 |
| hairpin / NAT-reflection, IPv6 DNAT | README.md:1160 |
| an L2 bridge; per-guest limits beyond the NAT quota | README.md:1203 |
| the control channel's remaining settings: forwarding and masquerade on/off, the resolver, the tap up and down | README.md:1183; `docs/audit/next-subsystem-netctl.md` "the ABI the later network settings will ride" |
| a general DHCP server, a caching resolver, DHCPv6, DNS-over-TCP, DNSSEC | README.md:1139 |
| ICMP errors for a UDP flow in the host's reply state ("no consumer exists yet") | README.md:1342 |
| a listing of live flows for the operator | README.md:1342 |

### 1.2 Threads, processes, the hypervisor

| item | where deferred |
| --- | --- |
| `SYS_mprotect` for native programs | README.md:1486 |
| native futex requeue | README.md:1486 (the Linux personality has requeue since milestone 10) |
| per-thread signal targeting | README.md:1486 |
| the device models tested under two guest CPUs (needs a guest-side virtio driver), "named as its own unit" | README.md:1595 |
| the display driver that sets a mode (section 60 "GPU later") | README.md:727 |
| GPU, Wi-Fi, Bluetooth, "later" | constitution §60 (Prompt #2), the hardware roadmap; the README's former Next paragraph |

### 1.3 Standing gaps the README names without scheduling

| item | where stated |
| --- | --- |
| **the VMX backend has never been executed** -- host tests of the pure logic only; needs Intel hardware or KVM | README.md:452 |
| the Linux-guest demonstrations are not CI gates: the Image is not committed; the root filesystem, the writable root and every network unit's `QEMU_MEM=2G` reproduction are manual runs -- and no run has yet shown a Linux guest reaching the real world through the host's NIC | README.md:992, 1022, 1046 |
| `atexit`'s table and the environment remain process-global and unsynchronised | README.md:1734; `docs/libc/invariants.md` |
| the cwd-ref fix is a regression test, not a proof; the seam that would prove it is named and not built | README.md:1637; `docs/audit/next-subsystem-cwd-ref.md` |
| `net-bench` took 71 s once in a hundred boots (x86-64, throughput normal, time lost between rounds; a retransmit backoff after a receive-queue drop is the likeliest mechanism) | README.md:1804; `docs/testing/flakes.md` history |
| the userland test programs' own timing assumptions (`thrtest`, `cwdtest`) | `docs/audit/next-subsystem-suite-waits.md`, deferrals |
| no named pipes and no unix sockets ("both things this kernel does not have") | README.md:650 |

### 1.4 Explicitly not done, by decision or measurement

Each has a stated condition for revisiting; a report that reopens one
must say what changed.

| item | the stated reason | where |
| --- | --- | --- |
| device multi-queue, TSO/LRO, jumbo frames, zero-copy socket buffers | "complexity must earn its place"; QEMU's user-mode backend has one queue | README.md:418 |
| IOMMU: interrupt remapping (`intremap=off` in the test machines), AMD-Vi, huge pages, an IOVA cache, PASID/ATS, stream ids above 255, requester-id aliasing behind bridges | not needed by the test machines | README.md:438 |
| AHCI NCQ | measured: four streams reach 87 % of NVMe's aggregate without it; "if a real disk shows it pays" | README.md:716 |
| e1000e checksum offload | would buy two percent (`net-nicbench`) | README.md:690 |
| USB scatter-gather | the USB disk is within noise of NVMe (`blk-bench`) | README.md:708 |
| x86-64 ASIDs (PCID) | TCG implements PCID on no CPU model; nothing here could test it | README.md:842 |
| vGIC on GICv2 hosts | GICv3-only; the capability says so and the tests skip | README.md:876 |
| termios: POSIX's other flag words and nineteen control characters | omitted rather than accepted and ignored | README.md:813 |
| `/sys` | nothing to put in it that `sysctl` does not hold | README.md:675 |
| pid renumbering | the process domain deliberately does without and argues against it (`docs/kernel/security/design.md`, "This is not a pid namespace") | the README's Next paragraph |
| lazy FPU switching | eager measured at ~1 000 ns of a 21 600 ns switch on AArch64, 270 of 2 700 on x86-64 | README.md:689 |

---

## 2. Set for later by the constitution and Prompt #2, still open

Checked by grep on 2026-09-14; "none" means no implementation was found
outside tests.

### 2.1 Constitution §68 "what not to implement yet"

GPU drivers, Wi-Fi, Bluetooth, the full NVMe feature set, a distributed
filesystem, containers (the primitives exist -- rights, roots, domains,
mount and uts namespaces, the syscall filter, rlimits, the service
manager -- the tooling does not), eBPF, advanced graphics, a desktop
environment, Wayland, Docker or Kubernetes compatibility, full Linux
compatibility, NUMA, live migration, nested virtualization. USB and
AHCI are the two entries from that list now built.

### 2.2 Memory (constitution §13-15; Prompt #2 §8)

- NUMA: designed in, not implemented (one node, one direct-map window,
  no topology structure, no SRAT parsing).
- huge pages for user space; memory deduplication; memory compression;
  swap.
- shared mappings: `MAP_SHARED` is still private; no shared-memory
  primitive of any kind (which also rules out shared futexes across
  processes).
- ASLR and KASLR: none; no randomised load base, stack or `brk`.

### 2.3 Scheduler and synchronisation (constitution §20-22)

- only `policy_rr.c` exists; CFS-like fairness, real-time, deadline,
  interactive scheduling and CPU isolation are future policies.
- no load balancing and no migration (a thread stays on the CPU chosen
  at creation; confirmed 2026-09-14).
- no priority inheritance in `mutex.c`.
- no `rwlock` in the kernel.
- the Epoch abstraction (`quiesce`) is used for lifetimes; not yet for
  routing tables or protocol lookup structures as §22 asks.

### 2.4 Storage (constitution §32; Prompt #2 §17)

Built: snapshots, mirrors, read repair, scrub, compression, encryption,
many-member pools. Open: writable clones, rollback, boot environments,
incremental send/receive, RAID-Z-like parity, striping, device
replacement, resilvering (a device that missed a commit is left out of
the mirror and never brought back), hot spares, deduplication.

### 2.5 Networking (constitution §35; Prompt #2 §20-22)

Zero-copy paths, device multi-queue, TSO/LRO, jumbo frames, NAPI-style
polling, interrupt moderation, busy polling; TCP window scaling, SACK,
timestamps, ECN, fast recovery, Nagle; IP fragmentation and reassembly;
IPv6 routing beyond loopback and ND against a real peer.

### 2.6 Linux compatibility (constitution §40 phases 3-4; Prompt #2 §30)

- `execve` is still `lx_nosys` (compat/linux/syscalls.c:1892) -- by the
  native model's design (spawn, no fork/exec), but a Linux program that
  execs dies.
- missing: `epoll`, `sendmsg`/`recvmsg`, `socketpair`, real
  `setsockopt`/`getsockopt`, `sched_getaffinity`, `rseq`, `statx`,
  `memfd_create`, `eventfd`/`timerfd`/`signalfd`, shared memory,
  netlink, `readlink` (no symlinks), `mremap`/`msync`.
- `/proc` and `/sys` compatibility for Linux binaries (the native `/proc`
  holds process facts only); running a real distribution userland
  (phase 4).

### 2.7 Virtualisation (Prompt #2 §29; audit 11.3)

Dirty-page tracking, ballooning, snapshot, live migration, device
passthrough. Nothing captures vCPU state, so snapshot and migration
need a UAPI extension first. x86 guest fidelity items not claimed by any
unit: WBINVD/RDPMC/RDTSCP interception, string I/O, TSC virtualisation,
debug registers, CPUID leaks (audit 11.3).

### 2.8 Observability (constitution §55; Prompt #2 §42-43)

Kernel debugger, GDB remote debugging, crash dumps, structured tracing
with per-CPU buffers, performance counters, `ktrace`/`kstat`/
`cosmo-top`/`cosmo-prof`, eBPF-like tracing: none started. Panic
symbolisation is still address-only. The many `*_stats` structures have
no transport beyond `sysctl` and the self-tests that print them.

### 2.9 Security (constitution §44; Prompt #2 §54)

- secure or measured boot: the kernel and boot archive are unsigned;
  module signing roots trust in an unverified image.
- audit logging: none.
- per-process capabilities beyond handle rights (§54's "file, network,
  device, IPC, VM capability"); network namespaces; cgroups-style
  accounting beyond rlimits.
- kernel stack protector (`-fno-stack-protector`, build/toolchain.mk:36);
  separate interrupt stacks (interrupts run on the thread stack);
  KASAN-like kernel sanitiser builds; a documented speculative-execution
  policy.
- entropy: virtio-rng is the only source; no RDRAND/RDSEED/RNDR, no
  jitter, a pool that never blocks or warns.
- module signature versioning and anti-rollback: any previously signed
  module loads forever (audit 10.2).

### 2.10 Build, packaging, hardware, benchmarks (constitution §49; Prompt #2 §44-45, §58, §61, §65)

- coverage builds and LTO: neither configured.
- packages: SBOM, dependency locking, build sandboxing,
  multi-architecture packages, rollback of upgrades.
- a real-hardware test matrix (AMD, Intel, Apple Silicon): none; the
  unexecuted VMX backend is its visible consequence.
- QEMU CPU-count matrix: CI runs 4 CPUs on both machines; 1 and 8 CPUs
  are run by hand, 16 only under `make test-gic`.
- benchmarks: `net-bench`, `blk-bench`, `fpu-bench`, `net-nicbench`
  exist; memory (kmalloc, page fault, mmap), scheduler (context switch,
  wakeup, IPI round trip), fsync and metadata latency, and virtualisation
  (VM exit latency) benchmarks do not, nor the baseline-versus-new
  regression system of Prompt #2 §45.

### 2.11 The other explicit "later" and "eventually" statements

Every remaining occurrence of *later*, *eventually*, *future* and *not
yet* in the three prompts, so that none is lost between the sections
above; the built ones are kept here so the coverage is visible.

| statement | where | state |
| --- | --- | --- |
| "framebuffer only later" | constitution §6 | built (the console unit) |
| "ASLR eventually" | §15 | open (2.2) |
| huge pages, deduplication, NUMA, memory compression "but do not implement them initially" | §14 | open (2.2) |
| "Design NUMA support into the abstraction, but do not implement NUMA initially" | §13 | open (2.2) |
| snapshots: "Later support: writable clones, rollback, boot environments, incremental send/receive" | §32 | open (2.4) |
| "The architecture should eventually permit" zero-copy networking | §35 | open (2.5) |
| Linux phases 3 and 4 | §40 | open (2.6) |
| "Eventually support Intel VT-x" | §42 | built, never executed (1.3) |
| packages: "rollback eventually"; "a SQLite metadata database if appropriate" | §47 | rollback of upgrades open (2.10); a text database was chosen instead of SQLite, a decision |
| "Rust may later be introduced selectively for memory-sensitive components"; "C#/.NET may be used later for host development tools, package tooling, image builders, debugging tools" | §50 | open by design; no second language has been introduced |
| observability "Eventually: kernel debugger, GDB remote debugging, crash dumps, tracing, performance counters, eBPF-like tracing" | §55 | open (2.8) |
| "property tests" and "fuzz tests" for every subsystem; "use fuzzing heavily for packet parsers" | §57, §60 | fuzzers exist for the module ELF, user ELF, package, Linux ABI, virtqueue, cosmofs, LZ4, framebuffer and USB descriptors (`tests/fuzz/`); **no fuzzer for the network packet parsers** (named as a gap in `docs/kernel-services/network/testing.md`), none for PCI configuration, ACPI tables or VFS paths (Prompt #2 §46); no property-based tests by that name |
| "Power-loss simulation must eventually be part of filesystem testing" | §59 | built (`cosmofs-replay`, milestone 4) |
| "eventually create a hardware test matrix" (AMD, Intel, Apple Silicon) | §61 constitution / Prompt #2 §61 | open (2.10) |
| fault injection: "packet duplication, packet reordering, corrupted metadata, CPU starvation, interrupt storms, device reset, VM exit storms" | Prompt #2 §47 | built: allocation, block submit/complete, demand-page, demand-copy, USB CSW (`kernel/core/faultinject.c`), a reordering test, the torn-write replay, an IPI storm test; **open: packet duplication as an injection, CPU starvation, device reset, VM-exit storms** |
| "Design future support for device add, remove, driver bind, unbind, device reset" | Prompt #2 §41 | built: add/remove for USB and AHCI; driver bind and unbind through the device model -- `driver_register` probes matching devices, `driver_unregister` runs remove and clears bindings (kernel/device/device.c:222-260, tests in `docs/kernel/device/testing.md`). Open: binding or unbinding one device independently of registering its driver, and a generic device reset operation (3) |
| async I/O "must work for files, sockets, devices, timers, IPC, VM operations" | Prompt #2 §23 | the ring drives any object with a readiness operation and has its own alarm timer; VM operations and a timer as a submittable object are not shown by any test -- unverified |
| quiesce performance "16 CPUs, 64 CPUs, 256 CPUs where test infrastructure permits" | Prompt #3 §24 | measured at 1 and 4 CPUs only (lifetime report §6) |
| "TSan-compatible host models where possible" | Prompt #3 §23 | not done (4) |
| the populate loops that hold the space lock across every page "until milestone 5 adds preemption points" | lifetime report §7.2 | milestone 5 landed; whether it shortened those sections is **not verified** here (`VM_KALLOC_POPULATE` still exists) -- a report touching them checks first |

---

## 3. Audit findings and futures no unit has taken up

`docs/audit/2026-09-post-roadmap-audit.md` (2026-09-05). Its ten
milestones and its "after these" list are all built except zero-copy
networking and container tooling. The following are still true of the
tree.

| item | audit section |
| --- | --- |
| **64-CPU ceiling**: `CONFIG_MAX_CPUS` is 64 and `cpumask_t` is one word (kernel/include/kernel/percpu.h:22); xAPIC-only addressing, x2APIC never enabled; a single cross-call slot; no ticket or MCS spinlocks; IRQ affinity spread only by NVMe | 6.3, 5.3 |
| no CPU feature framework (`arch_cpu_has`); no errata table; invariant TSC detected but unused, so cross-CPU timestamps are unsynchronised | 6.4, 6.2 |
| the watchdog fires once, from CPU 0, no NMI path, no hard/soft-lockup detection | 6.2 |
| sequential AP bring-up with a 10 ms delay per CPU | 6.2 |
| no symlinks in the VFS; no dentry cache (every component calls the filesystem); no `(ino, generation)` identity; no mount options string; no bind or overlay stacking | 8.3 |
| no fsck; no checksum algorithm id in the metadata header | 8.5, 8.6 |
| hotplug: no CPU hotplug, no PCI rescan, no power management; only USB and AHCI remove devices | 10.5 |
| DMA on non-coherent hardware: the audit's "no driver calls `dma_unmap` or `dma_sync_for_cpu`" is no longer true -- every driver unmaps, and NVMe syncs its completion queue before reading it (drivers/nvme/nvme.c:248). NVMe also syncs its submission queue and PRP lists for the device, and the virtqueue syncs its ring for the device (drivers/virtio/virtqueue.c:191). What remains: the virtqueue reads its used ring, and e1000e, AHCI and xHCI read their device-written rings and buffers, with no `dma_sync_for_cpu`; e1000e, AHCI and xHCI sync nothing for the device either. Adequate on coherent QEMU, exposed by the first non-coherent SoC | 13.2, 10.2 (re-checked 2026-09-14) |
| AArch64 hardening: `SCTLR_EL1.WXN` cleared and never set; UAO, E0PD, BTI, PAC unused; a user-triggerable SError panics the kernel; no device-tree parsing for the host (ACPI only); PSCI variations untested | 13.2, 13.3 |
| SMEP/SMAP/UMIP absence silently accepted; `mmap`/`mount`/`umount` accept unknown flag bits | 14.2 |
| VFS: `close()` cannot report write-back errors | 8.2 (MEDIUM, not in any milestone) |
| `read` returns at most 1 KiB per call through the stack bounce buffer | 4.2 (MEDIUM) |
| coverage: no instrumented build; no line coverage of the self-tests | 16.2 |

---

## 4. The quiesce report's remaining risks and debt

`docs/audit/2026-09-lifetime-quiesce-report.md` §7-8 (2026-09-05). Its
"NEXT SUBSYSTEM", lockdep, was built as milestone 3. Still standing:

- **grace-period latency is tick-bound** (a 4-8 ms floor);
  `synchronize_quiesce` polls; the wake-on-publish design that would
  remove the floor was not built.
- **the network worker runs below default priority** (priority 40,
  kernel-services/network/netif.c:725) on the sender's CPU. The report
  assigned "a priority decision" to milestone 8, which did per-connection
  locking and left the priority. The suite-waits unit's proofs met
  exactly this: a flood is processed only after the sending thread first
  sleeps.
- **woken-thread latency**: a thread woken by a lower-priority thread
  runs only at the next tick or preemption point; the deferred-preemption
  flag consumed at `arch_irq_restore` was assigned to the lockdep
  milestone and not done; there is still no preemption check on syscall
  return.
- **never exercised by a test**: the straggler IPI (Q6), the
  `blk_submit`/`blk_unregister` window (Q11), the TCP
  timer-callback/free race (N-L3), runtime hot-unplug of virtio devices.
- **ordering verified by review and sanitizers only**: no TSan model, no
  litmus tests (Prompt #3 §23 asked for them "where possible").
- **unexplained**: the AArch64 virtio-console flake seen once in four
  runs on 2026-09-05 (the console file lacked the last line while the
  serial log was complete).
- **small debts**: `nd_flush`/`arp_flush` drop in-flight resolutions
  silently when an interface goes; ARP and ND entries hold bare
  interface pointers and rely on the flushes in `netif_unregister`;
  `MODULE_MAX_LIVE` is a fixed 32-slot array; zombie modules are reaped
  only by a later `module_unload` of the same name.

---

## 5. Already built -- do not re-propose

The mistakes this section prevents have happened: a report that proposes
one of these will be sent back.

- the audit's ten milestones (critical fixes, lifetime/quiesce, lockdep,
  verification, uaccess fixups + user VMM regions, access control +
  rlimits, the transaction engine, network hardening + per-connection
  locking, async I/O + NVMe, Linux stage 2), receive scaling, the IOMMU,
  the VMX seam, AArch64 EL2, snapshots, pools, redundancy, compression,
  encryption;
- the container primitives: handle rights, per-type rights, roots,
  domains, mount and uts namespaces, the syscall filter, rlimits, the
  service manager (`svc`), `/proc`;
- the hardware roadmap through AHCI: NVMe, e1000e, USB (xhci, mass
  storage, hub, HID keyboard), AHCI, the framebuffer console;
- FP/SIMD at EL0 and in the signal frame; signals a person can send; job
  control; termios; ASIDs; GICv3 and the ITS; the vGIC, virtual timer and
  virtual distributor; the guest PL011; the machine (device tree, PSCI);
  booting Linux; virtio-blk root read-only and writable; virtio-net; the
  tap bridge; forwarding + masquerade NAT; DHCP + DNS proxy; DNAT; the
  runtime control channel; a tap per guest; the four firewall chains and
  the host's reply state; a TCP verdict;
- native threads and a futex; a thread pointer and per-thread `errno`;
  `__thread` with `PT_TLS`; a thread per vCPU and `SYS_vcpu_stop`; the
  condition variable; the cwd reference; the suite's waits, the
  load-sensitive list and the harness note;
- for the Linux personality: signals, threads via `clone`, PIE and
  `PT_INTERP`, private file-backed `mmap`, `poll`, futex requeue and
  bitset waits, the wall clock, the AArch64 table.

---

## 6. Reading the inventory into a report

Three questions a report answers before picking from the above, in the
constitution's order (correctness, cleanliness, observability, security,
portability, performance):

1. Is it a **correctness gap reachable today** (sections 1.3, 3, 4) or a
   **feature set for later** (sections 1.1-1.2, 2)? The former comes
   first unless the user says otherwise.
2. Does the tree have a way to **test** it deterministically (§70)? The
   VMX backend and the real-world guest reproduction do not, which is
   why they are gaps rather than units.
3. Which entry here does the report **close**, by name? Strike it
   through in the documents commit.
