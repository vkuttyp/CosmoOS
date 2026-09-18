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
| ~~ICMP errors for a UDP flow in the host's reply state ("no consumer exists yet")~~ -- **closed by the socket-verdict unit**. The consumer exists: `icmp_quoted_flow` (the parse `icmp_needfrag` already had, now shared) feeds `icmp_unreach`, which maps net/host/proto/port to ENETUNREACH/EHOSTUNREACH/ECONNREFUSED and calls `udp_error_notify` -- a pcb matching the whole four-tuple **and connected**, which is the entire RFC 5927 argument and the bar N16 sets for a reset. `sock_set_error` has its first caller and `struct socket::error` its first writer. Proved by `net-sockerr-udp` and `net-sockerr-spoof` (six messages differing in one field of the quoted four-tuple change nothing; the seventh is delivered). TCP takes no ICMP hard error, by decision: RFC 1122 §4.2.3.9 forbids the obvious implementation | README.md:1342 |
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
- **no migration** (a thread stays on the CPU chosen at creation;
  confirmed 2026-09-14). ~~no load balancing~~ — **placement is fixed**
  (`docs/audit/next-subsystem-thread-migration.md`): `pick_cpu` rotates
  its ties, so a thread created on an idle machine is no longer always
  born on CPU 0, which was the measured cause of 8 of 14 threads and 94%
  of context switches landing there.

  **Migration itself was built and removed**, and the next attempt
  should read that report's as-built before starting. A pull balancer
  moved threads correctly — CPU 0 went to 6 of 14, CPU 2 did ten times
  the context switches — and made three of four aarch64 boots fail, once
  with seven concurrency tests at once, where four of four pass without
  it. The corruption was not identified. Ruled out: `sched_wake`'s
  unlocked `t->cpu` read, `list_remove` leaving a stale node, per-CPU
  fault accounting, and per-CPU interrupt routing. Found on the way:
  lockdep cannot check a two-run-queue lock order (one class), and
  `rq->current` can be in a ready list. **And the tree holds per-CPU
  assumptions nothing declares** — `el2` asserts the hypervisor backend
  owns EL2 "on this CPU" from an unpinned thread — so migration needs an
  audit of those before it can land, not just a working balancer.
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
| no CPU feature framework (`arch_cpu_has`); no errata table; ~~invariant TSC detected but unused, so cross-CPU timestamps are unsynchronised~~ (the third clause closed by the CPU-clock unit, `docs/audit/next-subsystem-cpu-clock.md`: the bit is read, the offset measured and reported, `clock_since_ns`/`clock_delta_ns` are the rule for subtracting two stamps, and a machine whose counter is not common says so instead of promising) | 6.4, 6.2 |
| ~~the watchdog fires once, from CPU 0, no NMI path, no hard/soft-lockup detection~~ -- **closed by the lockup unit (PR #136)**: an NMI sample path on x86-64, a soft-lockup detector on every CPU and a hard-lockup detector on the next online CPU; "fires once" stays by design (the first block is the diagnosis; a second adds nothing). Open: an NMI-class interrupt on AArch64 (GICv3 pseudo-NMI) | 6.2 |
| sequential AP bring-up with a 10 ms delay per CPU | 6.2 |
| ~~no symlinks in the VFS~~ (a fourth vnode type, `symlink`/`readlink`, a walk that expands with a budget, `O_NOFOLLOW`/`lstat`/`readlink` in both ABIs, both filesystems storing one; PR #142); no dentry cache (every component calls the filesystem); no `(ino, generation)` identity; no mount options string; no bind or overlay stacking; no hard links | 8.3 |
| ~~no fsck~~ (`cosmofs_check`: ten finding classes over the live tree, every snapshot, both allocation maps and the inode map, four of them repaired, run over every image the crash suite replays; PR #144); no checksum algorithm id in the metadata header | 8.5, 8.6 |
| No on-disk orphan list; ~~and no record of a deferred free: a crash strands every block the last transaction freed (the commit publishes the root before applying the frees)~~ -- **the deferred-free half is closed by the unmount-leak unit (PR #148)**: format version 9's `free_root` names a chain of `CFS_KIND_FREELOG` blocks listing what the root freed, written before the root and replayed at mount, so the crash suite's stranded-block total is zero. ~~Still open: an inode unlinked while open is recorded nowhere~~ -- **closed by the orphan unit (PR #152)**: format version 10's `orphan_root` names a chain of `CFS_KIND_ORPHAN` blocks listing the inodes whose last name went while something still held them, written from memory each commit and replayed at the next mount by doing what `cfs_evict` would have done -- queuing the blocks and clearing the slot, so the space returns on that mount's first commit. A record rewritten whole rather than a list edited in place, so nothing here is copy-on-write and an ordinary unlink's add and eviction cancel in memory. It covers directories, which the report's first draft wrongly excluded: a working directory is a referenced vnode. **The row is now closed in both its clauses.** | 8.5 (found by the fsck unit) |
| ~~The same defect the deferred-free record fixes still lives in the snapshot path: `cfs_snapshot_hold_block` appends to a deadlist from phase 7, *after* the root is written~~ -- **closed by the deadlist unit (PR #150)**: the snapshot list and its deadlists are copy-on-write, the append moved in front of the root with its blocks from the commit's existing reservation, and the superseded blocks freed exempt. The guess recorded here was incomplete and the unit says why -- reserve-before-fill alone moves an in-place update in front of the root, where the *old* root can see it. It was also incomplete about the damage: the append was a crash hazard as well as a leak, through the deadlist-head pointer it wrote into the in-place list, which the replay suite found at prefix 125 the first time its workload took a snapshot. `cosmofs-freelog-snapshot`'s bound is now `== 0` | 8.5 (found by the unmount-leak unit) |
| ~~**A metadata block fails its checksum after cosmofs's orphan replay, reliably on aarch64 CI.**~~ **Diagnosed and fixed 2026-09-17.** The chase: the failure was 3 of 3 on CI and 0 of 3 locally on a documentation-only branch, one metadata block at a varying address (88, 103, 104) with every other check class zero. What broke it open was the *other* arch: the same test panicked x86\_64 with `kernel write at 0x0` in `list_remove` <- `cfs_buf_get` <- `freelog_release_previous` <- `cosmofs_sync`, **on the `cfs-wb` thread**, immediately after a forced unmount and remount. cosmofs's writeback thread was started lazily by `note_dirty`, and a mount's own replay dirties buffers -- so the thread was being started from inside the replay, then committing a half-built mount across an `fs->bufs` the mount path walks with `fs->lock` unheld (sound only while nothing else is in the filesystem). Two threads on one intrusive list: a metadata block that would not verify on one arch, a null dereference on the other. The same window let a *failed* mount reach `cfs_destroy`, which frees every buffer and `fs`, with that thread still running. Fixed by gating the spawn on `mount_done` and starting it at the end of `cosmofs_mount`; `cfs_destroy` now stops and joins for itself. Proof: `cosmofs-mount-no-early-writeback` -- whether the thread wins the race is timing, whether it exists during the replay is not. Evidence: runs 35168274142, 35169050744, 35169492332, 35169784060 | orphan replay (PR #152) |
| ~~**`cosmofs-writeback` can fail on a loaded machine, and the test is right**: `cfs_writeback_thread` bumps `fs->wb_commits` after `cosmofs_sync` returns, i.e. after the commit that advanced `fs->sb.generation` has already released `fs->lock`~~ -- **closed (PR #165)**. `cosmofs_sync_counted(mnt, writeback)` takes the count inside the same hold of `fs->lock` that publishes the generation, so the field now has exactly two accesses in the tree and both are under that lock. Bug-proofed deterministically, which the race is not: `lockdep_assert_held` at the increment, so publishing outside the lock panics on the first writeback commit of the boot rather than on the one run in a hundred where a reader lands in the window | none (found by the VMState-layout unit) |
| **Taken up by `docs/audit/next-subsystem-fsck-unchecked.md`** (with the row below; neither is struck until it lands). `cosmofs_check` claims no extent overlaps another inside one inode and that entries are ordered by `lblk`, and checks neither: an overlap is caught only when it makes two claims on one block, and an ordering fault that does not is a wrong file rather than a wrong filesystem. A name repeated inside one directory is likewise undetected, because the pass keeps maps of numbers and detecting it needs a set of strings | 8.5 (named by the fsck unit) |
| **Taken up by `docs/audit/next-subsystem-fsck-unchecked.md`** (with the row above). `cosmofs_check`'s `chain_cycle` class has no test that manufactures it: a cycle needs a corruption hook that writes structure rather than flipping a field, so the `CFS_CHECK_MAX_CHAIN` bound of 4096 is exercised by nothing. `dir_bad` is reported from six places and two of them are fired by a test (a type that disagrees with its inode, and a slot whose number is not its position); the other four are not: a block pointer outside the pool's range, a snapshot member table whose count does not fit its block, an over-long `namelen`, and a directory reached from two parents | 8.5 (named by the fsck unit) |
| ~~No operator interface for the filesystem's maintenance passes~~ (`/dev/fsctl`: a mount id that is never reused, a listing scoped to the caller's mount namespace, check and scrub against one id, repair behind a flag, and an unmount that drains a running pass rather than tearing the filesystem down under it; `fsctl(8)`; PR #146) | 8.5 (named by the fsck unit) |
| ~~**A cleanly unmounted cosmofs strands what its last transaction freed**, for the same reason a crashed one does: a commit publishes the new root, then clears the freed blocks' bits in memory and dirties those chunks *for the next commit* -- and at an unmount there is no next commit, so the next mount reads them as allocated and unreachable. Measured at 28 blocks on the boot's own scratch disk, the first real filesystem `fsctl` was pointed at, and at 41 when that unit's test stranded some deliberately instead of depending on the residue~~ -- **closed by the unmount-leak unit (PR #148)**. The guess recorded here was wrong and the unit says why: a second commit at unmount does not converge, because writing the bitmap frees the bitmap, so every commit leaves a `pending_free` for the next one. What closed it is a record the root itself names | 8.5 (found by the fsctl unit) |
| ~~A character device's operations run with the vnode lock held (`file_pwrite` takes it before dispatching), so any device that consults the mount table inverts `mounts -> vnode`~~ -- **closed by the chrdev-lock unit (PR #164)**. The row's own last clause was the answer: a filesystem lock is not held across device I/O at all. Checking it found the larger half the row did not name -- `tty_read` waits with no timeout and `ramfs_lookup` hands out one vnode per device node, so a process blocked at a terminal stopped every other reader and writer of it. `file_pread`/`file_pwrite` now run the `VNODE_CHR` arm outside `vn->lock`, with `lockdep_assert_not_held` at both dispatches (its first use in the tree), and the `vnode-chr` lockdep class split is removed rather than layered over -- reverting the fix with the split gone reproduces the original `mounts -> vnode` panic, which is what says the two were coupled. New invariant V32; test `vfs-chr-write-during-blocked-read` | 8.2 (found by the fsctl unit) |
| `vfs_umount2`'s one-unmount-at-a-time guard is unfired by any test: `follow_mount` refuses to walk to a mount that is unmounting, so a second unmount by path fails while resolving and never reaches it. It is reachable by a relative path resolved from inside the mount, which does not traverse the mountpoint, and no test opens that door | 8.2 (named by the fsctl unit) |
| The per-test time budget treats `process-user` as a test when it is the whole user-mode suite behind one SELFTEST line -- every fs, net, proc, fpu, trap, priv and svc check init makes, plus a process spawn per tool it drives. On CI it was at **7129 ms of 8000** on `main` before the fsctl unit added to it, so the budget failed the next addition whatever that addition was. Given its own budget (20 s) with the reason stated in the harness; the better answer is for the suite to report per-section timings rather than one line, so a slow section is named instead of the whole suite | none (found by the fsctl unit) |
| ~~Nothing in the tree can attempt an unprivileged open~~ -- **the row was stale when it was written down and the lifetime-windows unit corrected it (PR #154)**. `init --unpriv-test` drops to uid 1000 and is refused `mount`, `umount`, a mount namespace, a uts namespace, `sethostname`, `kill` of root's process, `klog`, a reserved port, a 0600 device, a 0700 directory, a 0644 file and the sticky bit on `/tmp`, with the permitted cases asserted beside them. What was actually missing was two doors added *after* that suite: `/dev/fsctl` and `/dev/net/tapctl`, which it now tries | 14.2 (named by the fsctl unit) |
| hotplug: no CPU hotplug, no PCI rescan, no power management; only USB and AHCI remove devices | 10.5 |
| DMA on non-coherent hardware: the audit's "no driver calls `dma_unmap` or `dma_sync_for_cpu`" is no longer true -- every driver unmaps, and NVMe syncs its completion queue before reading it (drivers/nvme/nvme.c:248). NVMe also syncs its submission queue and PRP lists for the device, and the virtqueue syncs its ring for the device (drivers/virtio/virtqueue.c:191). What remains: the virtqueue reads its used ring, and e1000e, AHCI and xHCI read their device-written rings and buffers, with no `dma_sync_for_cpu`; e1000e, AHCI and xHCI sync nothing for the device either. Adequate on coherent QEMU, exposed by the first non-coherent SoC | 13.2, 10.2 (re-checked 2026-09-14) |
| AArch64 hardening: ~~`SCTLR_EL1.WXN` cleared and never set~~ (set on every CPU from the kernel's tables on, proved by `make test-wxn`, PR #140); UAO, E0PD, BTI, PAC unused; ~~a user-triggerable SError panics the kernel~~ -- **closed by the async-error unit** (`docs/audit/next-subsystem-async-error.md`), which found x86-64's `#MC` had the same gap: every SError reached `aarch64_trap_entry`'s `default` arm (relabelled `ARCH_TRAP_GENERAL_PROTECTION`) and vector 18 was unregistered, so both panicked even for an error the hardware had **corrected**. Now classified and dispatched as `ARCH_TRAP_ASYNC_ERROR` (invariant I-ARCH-16): corrected is counted and returned from, everything else panics naming the class and the syndrome. **No process is killed** -- review refused that in the report, because an asynchronous abort's frame names the context interrupted at delivery, not the one that caused it; attribution needs the RAS error records and is a unit of its own. Two things the building found: `HCR_EL2.VSE` is inert without `AMO` (the host runs `HCR_EL2 = RW` alone), and **EL1 runs with `PSTATE.A` masked for the kernel's whole life**, so the kernel never takes an asynchronous abort while running -- EL0, entered with DAIF clear, is the live path. Whether EL1 should unmask `A` is recorded as I-ARCH-16's gap, not settled; no device-tree parsing for the host (ACPI only); PSCI variations untested | 13.2, 13.3 |
| ~~SMEP/SMAP/UMIP absence silently accepted; `mmap`/`mount`/`umount` accept unknown flag bits~~ (the `hardening:` boot line and the guard boot `make test-guard`; unknown bits `-EINVAL` in `mmap`/`mount`/`umount`/`open`; PR #140) | 14.2 |
| **`net-harness`: the twelve bytes were never written. Localised to a reset, PR #169, still open.** The guest's instrumentation answered on two of its own pull request's CI jobs, one per architecture: `client failed: connect 0, sent -104, recv -1, sndbuf free 65536 before, 65536 after send, 65536 after read (outstanding 0 then 0), state 0, segs_out +0 retransmits +0 refused +0 rsts_in +0` on x86-64, and the same line with `rsts_in +1` on aarch64. **-104 is ECONNRESET** (`errno.h:47`) and **state 0 is TCP_CLOSED** (`tcp.h:38`), so `ksock_sendto` failed: nothing was queued, nothing transmitted, and the connection was already reset when the guest wrote to it -- while the host had *accepted* it a second earlier, so slirp's own connect to 127.0.0.1 had succeeded. Every earlier framing of this row, including "twelve bytes that never arrive", describes a symptom of something that had already happened. **The question is now: what resets an established connection between `ksock_connect` returning and the next statement?** **The aarch64 job adds `rsts_in +1`**: an inbound RST, accepted in sequence -- this stack implements RFC 5961 §3 (`tcp.c:2000-2011`), so a reset anywhere but exactly `rcv_nxt` is a challenge ACK and is never counted, and the accepted one sets `pcb->error = -ECONNRESET` and ends the pcb. So the reset came *off the wire*, not from the guest's own stack. Who sent it is still not named: `tcp_get_stats` is machine-wide, so `+1` does not say the reset was this pcb's, and the x86-64 job's `+0` for the same failure is the window starting *after* the connect rather than a run without a reset. **PR #170's own CI then broke the pattern**: on a documentation-only branch, its x86-64 job printed `sent 12 ... sndbuf free 65536 before, 65524 after send, 65524 after read (outstanding 12 then 12), segs_out +1 retransmits +0 rsts_in +1, recv -104`. The guest queued the bytes, put a segment on the wire, and they were never acknowledged -- so "the twelve bytes were never written" describes three sightings and not the fourth, and the send path is ruled out as the defect. `retransmits +0` is *not* evidence of a retransmission bug here: a reset that kills the pcb before the timer fires leaves it flat. The locus is now **an established connection to slirp is reset -- sometimes before the guest writes, sometimes after a segment is on the wire -- and the payload never reaches the host's accepted socket**. **Both instrument gaps are now closed by the socket-verdict unit**: `tcp_get_stats` is sampled *before* `ksock_connect`, each step is timed, and the line carries `pending error %d` from `ksock_error` -- the pcb's own verdict rather than a machine-wide counter. **And the moved window answered on PR #171's own aarch64 CI**: `connect -104 in 1381 ms, ..., pending error -104, segs_out +3 retransmits +1 rsts_in +1 (counters from before the connect)`. The **connect itself** was reset -- every earlier instrumented sighting said `connect 0` -- and `pending error -104` is the first per-pcb verdict read rather than inferred. `ECONNRESET` rather than `ECONNREFUSED` is set only by a reset accepted on a *synchronized* connection (`tcp.c`, RFC 5961 §3), so the handshake completed and the reset arrived before the connecting thread ran again; `retransmits +1` and 1381 ms are one SYN retransmission. **This unifies the shapes**: across seven instrumented sightings the only thing that differs is how far the guest got before the reset landed -- connect, send, or an unacknowledged segment. The constant is an inbound reset on an established connection to slirp, while slirp's own host-side socket connects fine, which is why the host's `accept` keeps succeeding and then reading nothing. **Why slirp resets it is not established and is outside this kernel**, which the twelve-bytes report named as a possible result from the start. Eighteen sightings to 2026-09-17 (`docs/testing/flakes.md`, "The count"), nine instrumented, seven carrying `rsts_in +1`. A SYN-retransmission pattern in the first two moved-window runs was withdrawn when the third had none. Reproduces on x86-64 locally one boot in twenty-one; the tally is `docs/testing/flakes.md`, "The count" | none (found by the hardening unit; localised by the nettest-deadline and twelve-bytes units) |
| One firmware in CI: both CPU models are booted there (the default and the guard boot, PR #140) but only against Debian's AAVMF. The VHE handover path (`HCR_EL2.E2H` set at hand-over) exists because that build differs from the EDK2 a developer has locally, and the other build's path -- the plain `_EL1` writes -- is exercised by every CI boot but by no CI *firmware* variation. A firmware matrix in CI, or a documented minimum EDK2, is unbuilt | 13.3 (found by the hardening unit) |
| The IOMMU walker (`kernel/iommu/pt.c`) builds one four-level root, a level-0 start the architecture allows only above 42 bits of output; an SMMU of 42 bits or less is left unregistered at probe with a `WARN`, the machine booting without DMA remapping on it, rather than programmed with a start level it rejects (PR #140), and the concatenated level-1 root it would need is unbuilt -- untestable on QEMU's 44-bit SMMU; the hypervisor's stage-2 (`hv_s2_layout`) has the rule and the shape | 13.2 (found by the hardening unit) |
| ~~VFS: `close()` cannot report write-back errors~~ -- **closed by the file-path unit (PR #138)**: the page cache records, each open file hears once by `fsync` or `close` (a `flush` hook on the I/O object type), a named file's pages lost at release are counted; with it the 8.2 LOW "`vnode_release` writes back dirty pages of an `nlink==0` file" | 8.2 (MEDIUM, not in any milestone) |
| ~~`read` returns at most 1 KiB per call through the stack bounce buffer~~ -- **closed by the file-path unit (PR #138)**: a bounce sized to the request up to 64 KiB, one object call, shared by both personalities; the read/write bandwidth benchmark the audit asked for exists (`read-bench`, `write-bench`, `USERBENCH`) | 4.2 (MEDIUM) |
| ~~**`struct cosmo_vcpu_regs` is 496 bytes on AArch64 and 448 on x86-64**, while its own comment and `tests/host/test_hv.c:75` say both are 448~~ -- **closed by the VMState-layout unit (PR #162)**. Not by resizing: the header's own preamble calls these structures stable, x86-64's 448 is correct, and the AArch64 ABI has been 496 since the EL2 backend -- the documentation was what was wrong. Each real size is now asserted by `_Static_assert` in `uapi/cosmo/syscall.h`, so the check runs in every translation unit on every architecture rather than in one host binary that compiled whichever block the build host matched. Removing the stale host assertion also unblocked the **fourteen host suites ordered after `test_hv`**, which had never run on an arm64 host and all pass. `hv-vcpu-regs-roundtrip` is new | none (found by the fsck unit, whose host-test run was on an arm64 host) |
| coverage: no instrumented build; no line coverage of the self-tests | 16.2 |

---

## 4. The quiesce report's remaining risks and debt

`docs/audit/2026-09-lifetime-quiesce-report.md` §7-8 (2026-09-05). Its
"NEXT SUBSYSTEM", lockdep, was built as milestone 3. Still standing:

- **grace-period latency is tick-bound** (a 4-8 ms floor);
  `synchronize_quiesce` polls; the wake-on-publish design that would
  remove the floor was not built.
- ~~**the network worker runs below default priority**~~ -- **closed by
  the wake-preempt unit (PR #134)**: decided by measurement at the
  default priority (`docs/kernel-services/network/design.md`, "The
  worker's priority").
- ~~**woken-thread latency**~~ -- **closed by the wake-preempt unit
  (PR #134)**: the fourth preemption point in `arch_irq_restore`; a
  wake inside a system call is shown to preempt before the return, so no
  syscall-return point is needed.
- ~~**a latent spin in the network worker that a priority above its feeder
  exposes**~~ -- **closed by the lockup unit (PR #136)**: reproduced at
  31 with the tool the unit built, diagnosed from the sample (the
  worker's receive queue with a count over an empty list), caused by a
  second enqueue of an mbuf already on the queue from the reorder test's
  loopback filter; the stack now refuses a second enqueue
  (`net-mbufq-double`) and the filter's state is under a lock
  (`docs/audit/next-subsystem-lockup.md`, "The spin, found"); the five-boot check at 31 then found and fixed a second hang, the keepalive test's black hole raised before the handshake's last segment had left (its server never woke from `accept`).
- ~~**never exercised by a test**: the straggler IPI (Q6), the
  `blk_submit`/`blk_unregister` window (Q11), the TCP
  timer-callback/free race (N-L3), runtime hot-unplug of virtio
  devices.~~ -- **closed by the lifetime-windows unit (PR #154)**, with
  one clause narrowed rather than closed. Six tests now race these:
  `quiesce-straggler` (and `-system`, `-idle`), `blk-submit-unregister`,
  `blk-unregister-drain`, `tcp-pcb-timer-free` and
  `device-remove-busy`. None found a defect in the mechanisms; the unit
  found two things about them anyway, below. **Still open**: removing a
  *live virtio* device with real I/O outstanding, which needs a virtio
  device dedicated to removal in the test machine -- the machine's
  virtio-blk is the scratch disk the filesystem tests run on, so a
  boot-time suite that removes it destroys the run.
- **What the straggler kick is worth is an open question**, added by
  that unit rather than struck by it. A CPU publishes at interrupt
  return only when `preempt_count == 0`, so the kick cannot help a CPU
  spinning inside a read-side section -- which is the case its own
  comment named until this unit corrected it. It fires only for a CPU
  pending past two ticks, and the one population it can help (a CPU
  whose periodic tick keeps landing inside a short disabled region) is a
  phase coincidence no deterministic test can arrange. Deleting it,
  bounding it, or proving it are three different units
  (`docs/audit/next-subsystem-lifetime-windows.md`).
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
