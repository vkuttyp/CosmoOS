# Deferred work inventory

Date: 2026-09-14. Tree: `main` at 0318119 (after PR #131, the suite-waits
unit); re-read against 21f4288 (after PR #199) on 2026-09-21, with the
README line numbers below still those of 0318119. Purpose: **the base
for the next §68 reports** -- everything the
repository's own documents defer, delay or set for later, in one place,
cross-checked against the code so that a report proposes something that
is actually open and does not re-propose something already built.

The README's Status section has since moved, verbatim, to
[`docs/history/`](../history/README.md). A `README.md:N` citation below
names line N of the README at 0318119 (`git show 0318119:README.md`).

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
| ~~a listing of live flows for the operator~~ **BUILT (the net-flows unit, `docs/audit/next-subsystem-net-flows.md`)**: `/dev/net/tapctl`'s snapshot version 6 lists every live NAT and firewall flow (opener → peer, NAT identity, established, time left) with the shares and the refusal counters, and `vmctl flows` prints them. The counters were split first, one cause each (a full share, a full table, an ambiguous reverse key, or not a refusal at all). Invariant **N24**: a flow is listed exactly when its share counts it. Measured first by `tools/net-visibility-probe.py`: a guest refused 232 flows while the operator's listing stayed byte-identical and the log silent | README.md:1342 |

### 1.2 Threads, processes, the hypervisor

| item | where deferred |
| --- | --- |
| ~~**`MAP_FIXED` should replace, as POSIX says, instead of returning `-EEXIST`**~~ **BUILT (PR #193)**: `vm_user_map_anon_replace` takes the range in three critical sections with it owned at every instant (the new region goes in under the same lock that clears the old ones, `VM_REGION_QUIESCED` until the teardown is done, so holes it spanned are owned too), both syscall doors use it, `COSMO_MAP_FIXED_NOREPLACE` keeps the old refusal, and libc's punch and retry are gone. Invariant **M40**. The original entry read: -- `space_insert` refuses any overlap, so a caller that wants to turn part of its own reservation into writable memory must `munmap` a hole and `mmap` it back, and the two syscalls have a window in which another thread's `mmap(NULL, …)` can be handed the gap. `cosmo_thread_start` does exactly that to place a guard page below a stack, and lost the race three times on aarch64 CI: `EEXIST` out of a thread start (PR #191). Worked around there with a bounded retry; the repair is atomic replacement in `sys_mmap`, which also deletes the punch. Related: with `SYS_mprotect` below, the carve-out would not need unmapping at all. **Taken up by `docs/audit/next-subsystem-map-fixed.md`**, which found the second door: the Linux personality already replaces (`compat/linux/syscalls.c:951-956`) but in two lock acquisitions, so it has the same window and fails in a way Linux never does | PR #191; `kernel/memory/vmm.c:73`, `libc/src/thread.c` |
| ~~**`SYS_mprotect` for native programs**~~ **BUILT (PR #195)**: `SYS_mprotect` 93 over `vm_user_protect`, the native flags rule kept (an undefined `prot` bit is `EINVAL`, `len` a page multiple), and the kernel synchronising the instruction stream when a range becomes executable, at both doors, because EL0 cannot without `SCTLR_EL1.UCI`. Invariant **M41**. The original entry read: the Linux personality already has it (`lx_mprotect`, `compat/linux/syscalls.c:1012`) over a `vm_user_protect` that is built, tested and used by the ELF loader; only the native ABI has no call. Taken up by `docs/audit/next-subsystem-mprotect.md` | README.md:1486 |
| `cosmo_thread_start` to map read/write and `mprotect` its guard page, dropping reserve-and-replace — one map and one protect instead of a reservation and a `MAP_FIXED`. Deferred by the `mprotect` report on purpose: PR #193 proved the current sequence against four mutations and respending that for one syscall on a cold path needs its own argument | `docs/audit/next-subsystem-mprotect.md`, "Alternatives considered"; `libc/src/thread.c` |
| ~~**native futex requeue**~~ **BUILT (native thread door unit, PR #197)**: `SYS_futex_requeue` 94, the compare form, and `cosmo_cond_broadcast` requeueing under invariant L10 (the herd measured: 0 sleeps on the mutex word against 7). The original entry read: `futex_requeue` is built (`kernel/ipc/futex.c:159`) and the Linux door calls it; `cosmo_cond_broadcast` wakes every waiter and says the herd is what requeue exists to avoid. Taken up by `docs/audit/next-subsystem-native-thread-door.md`, with per-thread signal targeting, as one unit | README.md:1486 (the Linux personality has requeue since milestone 10) |
| ~~**per-thread signal targeting**~~ **BUILT (native thread door unit, PR #197)**: `SYS_thread_kill` 95, `tgkill` with the process implied, `cosmo_thread_kill` in libc. The original entry read: `signal_send_thread` is built (`kernel/process/signal.c:373`) and `lx_tgkill` uses it; the native `kill` is process-scoped. Taken up by `docs/audit/next-subsystem-native-thread-door.md`, with futex requeue | README.md:1486 |
| the device models tested under two guest CPUs (the guest virtio drivers `guest_vblk`/`guest_vnet` exist but run on one vCPU), "named as its own unit" | README.md:1595 |
| the display driver that sets a mode (section 60 "GPU later") | README.md:727 |
| GPU, Wi-Fi, Bluetooth, "later" | constitution §60 (Prompt #2), the hardware roadmap; the README's former Next paragraph |

### 1.3 Standing gaps the README names without scheduling

| item | where stated |
| --- | --- |
| **the VMX backend has never been executed** -- host tests of the pure logic only; needs Intel hardware or KVM | README.md:452 |
| the Linux-guest demonstrations are not CI gates: the Image is not committed; the root filesystem, the writable root and every network unit's `QEMU_MEM=2G` reproduction are manual runs -- and no run has yet shown a Linux guest reaching the real world through the host's NIC | README.md:992, 1022, 1046 |
| ~~`atexit`'s table and the environment remain process-global and unsynchronised~~ **BUILT (PR #191)**: one lock over both in `libc/src/stdlib.c`, `env_count` left unlocked because both mutators call it and the mutex is not recursive, and `exit` never holds it while running a handler. Invariant **L8** counts five now. Both halves are demonstrated: unlocked, `atexit` accepts 33 into a table of 32 and loses handlers, and the environment race **kills the process** (`#GP`, signal 11, three runs of three) once a reader actually walks the reallocated tail and a thread churns the heap so the freed array is reused -- `setenv` frees the array and never a string, so a reader on the stale copy otherwise reads correct pointers out of freed memory. An earlier version of this row said only the `atexit` half was provable. **Taken up by `docs/audit/next-subsystem-libc-shared-tables.md`**, which measured the shape of what was wrong: `setenv` growing the array **did** `free(environ)` while `getenv` might be walking it (a use-after-free in the allocator the same unit locked), `unsetenv` memmoved under readers, and `atexit`'s `g_natexit++` both lost handlers and could write one past a static array. All four are behind the lock now. L8's own wording was part of the defect and has been rewritten: **it used to** enumerate three safe tables and say "all three are done" while the library had five. That old sentence is quoted here only as the record of what was wrong -- **the current L8 counts five**, as the head of this row already says. (Review has read this clause twice as a live claim that L8 still says three; it does not.) | README.md:1734; `docs/libc/invariants.md` |
| ~~the cwd-ref fix is a regression test, not a proof; the seam that would prove it is named and not built~~ **BUILT (the cwd-hold unit, `docs/audit/next-subsystem-cwd-hold.md`)**: a walk held in `walk_parent`'s relative branch with its cwd pointer in hand until the process's `chdir` has published and put, the order enforced by the seam, proved at both doors by `cwd-hold-native` and `cwd-hold-linux`; freed vnodes poisoned in debug builds (V35). Measured first by `tools/cwd-race-probe.py`: with the reference removed at the native `open` and every freed vnode poisoned, the old test caught it in one of five x86-64 boots and none of three AArch64 -- a rate, not a proof; the tree as it was, with nothing poisoned, could not see it at all. `cwdtest`'s four steps keep their regression label | README.md:1637; `docs/audit/next-subsystem-cwd-ref.md` |
| `net-bench` took 71 s once in a hundred boots (x86-64, throughput normal, time lost between rounds; a retransmit backoff after a receive-queue drop is the likeliest mechanism) | README.md:1804; `docs/testing/flakes.md` history |
| the userland test programs' own timing assumptions (`thrtest`, `cwdtest`) | `docs/audit/next-subsystem-suite-waits.md`, deferrals |
| ~~no named pipes and no unix sockets ("both things this kernel does not have")~~ **BOTH BUILT**: the unix socket by the unix-sockets unit (`docs/audit/next-subsystem-unix-sockets.md`, `mknod` and the `VNODE_SOCK` node), the named pipe by the named-pipes unit (`docs/audit/next-subsystem-named-pipes.md`: `VNODE_FIFO` through the same `mknod`, the pipe's ring split from its ends, POSIX's open rules, files that can say whether they would block) | README.md:650 |
| ~~virtio-net's remove drains its queues while `vnet_rx_done` may still run~~ **FIXED (PR #336, device lifecycle Unit 1)**: `vnet-remove-pending` proves missing unmaps and unreclaimed buffers; `vnet-remove-late` proves RX refill after reset, on both architectures. Polls stop before reset, queue teardown synchronizes vectors, independent buffer tables reclaim completed and outstanding RX/TX with every segment unmapped and `rx_posted == 0`. The stack's GONE check already rejected late RX; concurrent writers to the old unlocked count were not proved. Full local matrix and `tools/vnet-remove-probe.py --old` pass on both architectures | [removal report](2026-10-08-vnet-remove-report.md); original finding: `2026-10-08-irqpoll-lockdep-report.md` §6 |
| fault-injection points (`FI_BLK_COMPLETE`, `FI_NET_RX_DUP`, `FI_KMALLOC`) skip interrupt context, so since #334 they can fire in a poll the irqpoll worker runs and never in the same poll run by the handler: which half of a completion stream they see depends on the budget | same, §6 |

### 1.4 Explicitly not done, by decision or measurement

Each has a stated condition for revisiting; a report that reopens one
must say what changed.

| item | the stated reason | where |
| --- | --- | --- |
| device multi-queue, TSO/LRO, jumbo frames, zero-copy socket buffers | "complexity must earn its place"; QEMU's user-mode backend has one queue | README.md:418 |
| IOMMU: interrupt remapping (`intremap=off` in the test machines), AMD-Vi, huge pages, an IOVA cache, PASID/ATS, stream ids above 255, requester-id aliasing behind bridges | not needed by the test machines | README.md:438 |
| AHCI NCQ | measured: four streams reach 87 % of NVMe's aggregate without it; "if a real disk shows it pays" | README.md:716 |
| e1000e checksum offload | would buy two percent (`net-nicbench`) | README.md:690 |
| further USB scatter-gather work (up to 16 segments per request is built) | the USB disk is within noise of NVMe (`blk-bench`) | README.md:708 |
| x86-64 ASIDs (PCID) | TCG implements PCID on no CPU model; nothing here could test it | README.md:842 |
| vGIC on GICv2 hosts | GICv3-only; the capability says so and the tests skip | README.md:876 |
| termios: POSIX's other flag words and nineteen control characters | absent from the native ABI, which masks undefined mode bits; the Linux door accepts a full `termios` and keeps four flags, `VMIN` and `VTIME` | README.md:813 |
| `/sys` | nothing to put in it that `sysctl` does not hold | README.md:675 |
| pid renumbering | the process domain deliberately does without and argues against it (`docs/kernel/security/design.md`, "This is not a pid namespace") | the README's Next paragraph |
| lazy FPU switching | eager measured at ~1 000 ns of a 21 600 ns switch on AArch64, 270 of 2 700 on x86-64 | README.md:763-766 |

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

- NUMA: designed in, not implemented (one `pmm_node`, one direct-map window,
  no distances, no SRAT parsing).
- huge pages for user space; memory deduplication; memory compression;
  swap.
- ~~shared mappings: `MAP_SHARED` is still private; no shared-memory
  primitive of any kind (which also rules out shared futexes across
  processes).~~ **BUILT (the file-regions unit,
  `docs/audit/next-subsystem-file-regions.md`)**: `VM_REGION_FILE` over
  the page cache's own frames at both doors -- shared mappings coherent
  with `read()` and `write()` by construction, private ones
  copy-on-write, demand-paged, `msync` (`SYS_msync` 96, `LX_msync` 26),
  `SIGBUS` past the end, `maxprot`, the first shared memory between two
  processes in this system (invariants **M42--M44**, **V33**). Still
  open from this row: ~~a shared futex across processes
  (`kernel/ipc/futex.c` keys by space)~~ -- **BUILT (the shared-futex
  unit, `docs/audit/next-subsystem-shared-futex.md`)**: the key is what
  the word maps, `(vnode, file offset)` for a `MAP_SHARED` word with a
  reference the waiter holds and a requeue exchanges, the Linux
  `FUTEX_PRIVATE_FLAG` honoured both ways instead of masked out, the
  first wait across two processes in this system (invariant **I7**) --
  `memfd`/`shm_open`, and the ELF loader mapping `PT_LOAD` segments as
  file regions -- named as deferred in that report; the loader and the
  Linux `memfd_create` are built since (below and §2.6). The record of what was wrong, as the report
  corrected it before taking the row: natively there was no file
  mapping at all (`sys_mmap` refused anything not anonymous with the
  comment "file mappings arrive with the VFS"); the Linux personality's
  file mapping was an eager copy that refused
  `MAP_SHARED|PROT_WRITE` with `-EOPNOTSUPP` and gave a read-only
  `MAP_SHARED` mapping a snapshot a later `write()` never reached; and
  the page cache already owned the frames a shared mapping would install (`kernel/include/kernel/pagecache.h`).
  Constitution §14 lists file-backed mappings, shared mappings and
  copy-on-write in the VMM's *must* list, not its "eventually" list. A
  shared futex across processes was named in that report as deferred
  and is built since (the shared-futex unit, above); a native memfd and
  `shm_open` remain deferred (the Linux `memfd_create` is built, §2.6); ~~the loader's segments~~ **BUILT (the elf-shared-text
  unit, `docs/audit/next-subsystem-elf-shared-text.md`)**: a `PT_LOAD`
  that is not writable and has no zero tail is mapped shared over the
  file's page cache, so every process running a program shares one set
  of text frames, and the segment's zero tail is demand-paged rather
  than populated -- 89 pages per additional copy of `init` before, 16
  after, on both architectures. A file being executed is busy
  (`-ETXTBSY`), derived from the page cache's mapping list rather than a
  counter (invariant **P30**); the description of the
  two doors above is the record of what was wrong, and what they do now
  is in `docs/kernel/syscall/api.md` and `docs/compat/linux/api.md`.
- ASLR and KASLR: none; no randomised load base, stack or `brk`.

### 2.3 Scheduler and synchronisation (constitution §20-22)

- only `policy_rr.c` exists; CFS-like fairness, real-time, deadline,
  interactive scheduling and CPU isolation are future policies.
- ~~**no migration** (a thread stays on the CPU chosen at creation;
  confirmed 2026-09-14)~~ — **a migration primitive exists and the
  suite runs under it** (`docs/audit/next-subsystem-percpu-migration.md`):
  `sched_migrate`/`sched_migrate_from` move a READY thread between run
  queues under both locks in increasing CPU-id order, each run queue's
  lock is its own lockdep class so that order is checked (S24), every
  per-CPU read is a declared claim the debug accessors enforce (S25), and
  `make test-chaos` boots the whole suite with a migrator in the tick.
  ~~**No balancer moves threads on its own yet**~~ — **a balancer
  pulls** (`docs/audit/next-subsystem-load-balancer.md`): load counts
  the thread a CPU is running (S29), an idle CPU looks every tick and a
  busy one every 16, and a pull happens when the busiest CPU is two or
  more ahead (S27, S28). It cannot move a thread that is time-slicing
  with another, because such a thread is always preempted and S26
  forbids that; it corrects an imbalance as work becomes runnable. ~~no load balancing~~ — **placement is fixed**
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
  `rq->current` can be in a ready list. ~~**And the tree holds per-CPU
  assumptions nothing declares** — `el2` asserts the hypervisor backend
  owns EL2 "on this CPU" from an unpinned thread — so migration needs an
  audit of those before it can land, not just a working balancer.~~
  **That audit is done** (the percpu-migration unit): a probe in the two
  accessors named 113 sites on x86-64 and 133 on AArch64; seventeen were
  claims a migration would break, `schedule_internal` reading its own
  per-CPU block before the run-queue lock first among them -- the
  corruption above, named -- plus the EL2 hand-back; all are fixed, and
  the accessors now panic in debug builds on a new one.
  **The balancer's one asserting test lost a race under chaos** --
  **BUILT (PR #236)**: its released workers yield, so it asserts the
  contract, and `sched-balance-pair` asserts the mechanism. Taken up by
  `docs/audit/next-subsystem-balance-movable.md`
  (`tools/balance-chaos-probe.py`): a pair of spinners on one CPU is
  never separated -- five hundred refused pulls a second, by S26 -- and
  one chaos move of a released spinner onto a busy CPU makes that pair,
  so `sched-balance-pull` asserted the window rather than the contract.
  **Open, not taken up:** a thread preempted in *user* mode cannot be
  mid-way through a kernel per-CPU access, so S26 need not hold it; two
  compute-bound user threads that came to share a CPU would otherwise
  stay together while another CPU idles. The same probe's user-thread
  measurement found no such pair stuck in twelve boots (one sharing, of
  16 ms, that resolved), so there is no measured defect yet.
- ~~no priority inheritance in `mutex.c`~~ — **BUILT (the priority-inheritance unit, `docs/audit/next-subsystem-priority-inheritance.md`, PR #281)**: a mutex owner is boosted to the highest-priority thread blocked on it and up the chain, under a single `g_pi_lock` (the outermost PI lock; run-queue and wait-queue locks nest under it), and restored on release; `struct thread` gained `base_prio` with `priority` the effective value, and `sched_reprioritize` requeues a ready/running/blocked thread re-checking its CPU against a migration. `prio-inversion` proves it: the three-thread inversion on one CPU now resolves with the high thread acquiring before the medium thread finishes. Measured first by `tools/priority-inheritance-probe.py` (the inverted outcome, before the fix).
- ~~`schedule()`'s preempt-at-restore recursion bounded only by tail
  calls~~ — **BUILT (the restore-loop unit,
  [report](2026-10-06-sched-restore-loop-report.md))**: a reschedule pending
  at `schedule()`'s restore is another pass of a loop in the same frame
  (S31), and the ELF check verifies that structure instead of code
  generation. Found by the raw-pairing unit, whose first wrapper double
  faulted a one-CPU boot.
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
polling (the budget-and-defer half is built for completion handlers:
`kernel/core/irqpoll.c`, 2026-10-08, irq-budget report; received frames
still go through the per-CPU network workers, and nothing polls with
interrupts off), interrupt moderation, busy polling; TCP window scaling, SACK,
timestamps, ECN, fast recovery, Nagle; IP fragmentation and reassembly;
IPv6 routing beyond loopback and ND against a real peer.

### 2.6 Linux compatibility (constitution §40 phases 3-4; Prompt #2 §30)

- `execve` is still `lx_nosys` (compat/linux/syscalls.c:3451) -- by the
  native model's design (spawn, no fork/exec), but a Linux program that
  execs dies.
- missing (re-checked 2026-09-21): ~~`epoll`~~ (**BUILT — the epoll unit, `docs/audit/next-subsystem-epoll.md`, PR #291**: `epoll_create1`/`create`/`ctl`/`wait`/`pwait` as an interest-set kobject (`kernel/io/epoll.c`) composing the readiness operations via the aio ring's multi-wait, level-triggered with `EPOLLONESHOT`; ~~`EPOLLET`~~ (**BUILT — the epollet unit, `docs/audit/next-subsystem-epollet.md`, PR #301**: edge-triggered via per-item `edge`/`armed`, `collect` reporting an edge member only while armed and re-arming it on a drain or a real (non-timeout) wake), ~~auto-remove-on-close deferred~~ (**BUILT — the epoll-close unit, `docs/audit/2026-10-07-epoll-close-report.md`**: the handle table counts an object's slots across every process and the last close removes its registrations, invariant A9), ~~nesting refused~~ (**BUILT — the epoll-callback unit, `docs/audit/2026-10-07-epoll-callback-report.md`**: readiness by callback, a set in a set with `-ELOOP`, a chain of four)), ~~`sendmsg`/`recvmsg`,
  `socketpair`~~ (built with `AF_UNIX` and `SCM_RIGHTS`, the unix-sockets
  unit), `rseq`, ~~`statx`~~ (**BUILT — the statx unit, `docs/audit/next-subsystem-statx.md`, PR #283**: `LX_statx` 332/291 + `lx_statx` marshalling a 256-byte `struct statx` over the existing VFS stat path, `stx_mask` reporting the supported set and omitting the atime/btime the kernel does not keep), ~~`memfd_create`~~ (**BUILT — the memfd unit, `docs/audit/next-subsystem-memfd.md`, PR #289**: `memfd_create` (319/279) making an anonymous ramfs regular file via `ramfs_anon_reg` — born unlinked so it frees on the last close — plus `ftruncate` (77/46) over the vnode truncate op; `mmap`-able through the page cache),
  ~~`eventfd`~~ (**BUILT — the eventfd unit, `docs/audit/next-subsystem-eventfd.md`, PR #285**: a `uint64` counter and wait queue as a kobject on the aio-timer readiness-object pattern (`kernel/io/eventfd.c`), `eventfd2` wired on both arches and `eventfd` on x86-64, `EFD_SEMAPHORE`/`EFD_NONBLOCK` honoured) / ~~`timerfd`~~ (**BUILT — the timerfd unit, `docs/audit/next-subsystem-timerfd.md`, PR #287**: `timerfd_create`/`settime`/`gettime` on the aio-timer timer kobject (`kernel/io/timerobj.c`) given a disarmed create + `settime`/`gettime`, `CLOCK_MONOTONIC`/`REALTIME`/`BOOTTIME`, absolute and periodic timers) / ~~`signalfd`~~ (**BUILT — the signalfd unit, `docs/audit/next-subsystem-signalfd.md`, PR #299**: `signalfd`/`signalfd4` as a readiness kobject (`kernel/io/signalfd.c`) holding a signal mask, reading the current process's pending signals and polling a per-process `signalfd_wqh` the signal path wakes; a consume-by-mask dequeue + the signal core now keeps a blocked ignored signal pending so `signalfd(SIGCHLD)` works; `SIGKILL`/`SIGSTOP` never reportable, non-RT coalescing), ~~shared memory~~ (**BUILT — the shm unit, `docs/audit/next-subsystem-shm.md`, PR #297**: System V `shmget`/`shmat`/`shmdt`/`shmctl` over a reference-counted key/id registry (`kernel/ipc/shm.c`) whose segments are anonymous ramfs files mapped `MAP_SHARED`; `IPC_RMID` frees on the last detach via the record refcount, `shmat` honours an exact address and `SHM_RDONLY`, `shmctl` does `IPC_STAT`/`IPC_RMID`; `SHM_RND`/`REMAP`/`HUGETLB`/`IPC_SET`/namespaces deferred), netlink, ~~`mremap`~~ (**BUILT — the mremap unit, `docs/audit/next-subsystem-mremap.md`, PR #295**: in-place grow/shrink of a whole anonymous mapping via a new `vm_user_remap` primitive that finds, validates and resizes the region under one hold of the space lock (a shrink claims its tail `VM_REGION_QUIESCED` while the teardown runs); `MREMAP_MAYMOVE` never relocates, `FIXED`/file/sub-range are `-EINVAL`);
  ~~`msync`~~ is built (the file-regions unit, `LX_msync` 26).
  `setsockopt`/`getsockopt` exist as stubs: `getsockopt` answers
  `SOL_SOCKET`/`SO_ERROR`, and `SO_PEERCRED` on `AF_UNIX`, only and
  `setsockopt` is `-ENOPROTOOPT` for everything
  (compat/linux/syscalls.c:3005-3060). ~~`sched_getaffinity`~~
  is built (`:2659`); ~~`readlink` (no symlinks)~~ is built with
  symlinks (PR #142). Found on the re-check and not previously listed:
  ~~`pselect6` -- musl's `select` is `pselect6` on both architectures, so
  every `select` caller gets `-ENOSYS`~~ **BUILT (the device-readiness
  unit, `docs/audit/next-subsystem-device-readiness.md`: `select` 23 and
  `pselect6` 270/72 over `io_poll`)**; ~~`sysinfo` (`lx_nosys`)~~ (**BUILT — the sysinfo unit, `docs/audit/next-subsystem-sysinfo.md`, PR #293**: `lx_sysinfo` fills `struct sysinfo` from `pmm_get_stats` (totalram/freeram, mem_unit 1), `clock_now_ns` (uptime) and `process_count`, zeroing the load-average/swap/high fields the system lacks); and ~~a
  real directory fd -- `check_dirfd` returns `-ENOSYS` for any `dirfd`
  but `AT_FDCWD` (`:133-137`), so an `openat` relative to an opened
  directory fails~~ **BUILT (the dirfd unit,
  `docs/audit/next-subsystem-dirfd.md`)**: `at_base` resolves every `*at`
  call from the directory its descriptor names, `renameat` from two
  through `vfs_rename2`, and `fchdir` publishes a directory file's
  recorded name with its vnode (invariant **P31**). Measured first by
  `tools/dirfd-probe.py`: all nine `*at` calls and `fchdir` answered
  `ENOSYS` to a real descriptor, both architectures. All three are now
  built.
- `/proc` and `/sys` compatibility for Linux binaries (the native `/proc`
  holds process facts only); running a real distribution userland
  (phase 4).

### 2.7 Virtualisation (Prompt #2 §29; audit 11.3)

Dirty-page tracking, ballooning, snapshot, live migration, device
passthrough. vCPU registers can be read and written (`sys_vcpu_regs`,
`kernel-services/virtualization/hvsys.c`), but nothing captures full vCPU
and device state (FPU, vGIC, timers, devices), so snapshot and migration
need a UAPI extension first. x86 guest fidelity items not claimed by any
unit: WBINVD/RDPMC/RDTSCP handling (the never-executed VMX backend treats
WBINVD as a no-op and enables RDPMC and MOV-DR exits it does not handle;
SVM intercepts none of them), string I/O, TSC virtualisation, debug
registers, and CPUID leaves the filter in `vcpu.c` passes through
(audit 11.3).

### 2.8 Observability (constitution §55; Prompt #2 §42-43)

Kernel debugger, GDB remote debugging, crash dumps, structured tracing
with per-CPU buffers, performance counters, `ktrace`/`kstat`/
`cosmo-top`/`cosmo-prof`, eBPF-like tracing: none started. Panic
symbolisation is still address-only in the kernel. The boot harness
resolves kernel and module frames on a failed run; module text from the
loader's `module: base` line (2026-10-08, irqpoll-lockdep report). The many `*_stats` structures have
no transport beyond `sysctl` and the self-tests that print them.

### 2.9 Security (constitution §44; Prompt #2 §54)

- secure or measured boot: the kernel and boot archive are unsigned;
  module signing roots trust in an unverified image.
- audit logging: none.
- per-process capabilities beyond handle rights (§54's "file, network,
  device, IPC, VM capability"); network namespaces; cgroups-style
  accounting beyond rlimits.
- kernel stack protector (`-fno-stack-protector`, build/toolchain.mk:36);
  separate interrupt stacks (device interrupts, and every AArch64
  exception, run on the thread stack; x86-64 already takes #DF, NMI, #MC
  and #DB on IST stacks);
  KASAN-like kernel sanitiser builds; a documented speculative-execution
  policy.
- entropy: virtio-rng is the only source; no RDRAND/RDSEED/RNDR, no
  jitter, a pool that never blocks or warns.
- module signature versioning and anti-rollback: any previously signed
  module loads forever (audit 10.2).

### 2.10 Build, packaging, hardware, benchmarks (constitution §49; Prompt #2 §44-45, §58, §61, §65)

- coverage builds and LTO: neither configured.
- packages: SBOM, dependency locking, build sandboxing,
  multi-architecture packages, user-initiated rollback to a prior
  version (a failed install or upgrade already leaves the old state).
- a real-hardware test matrix (AMD, Intel, Apple Silicon): none; the
  unexecuted VMX backend is its visible consequence.
- QEMU CPU-count matrix: CI runs 4 CPUs on both machines; 1 and 8 CPUs
  are run by hand, 16 by hand with `QEMU_GIC=3 QEMU_SMP=16` (`make
  test-gic` runs the default 4).
- benchmarks: `net-bench`, `blk-bench`, `fpu-bench` (including the
  context-switch cost), `net-nicbench`, `read-bench`/`write-bench`,
  `irqrestore-bench`, `clock-cost`, `fb-bench`, the lockdep benches and
  the user suite's `USERBENCH` lines (futex wake, `mmap` first touch per
  page, Unix-socket and FIFO round trips) exist; kmalloc, IPI round trip,
  fsync and metadata latency, and virtualisation (VM exit latency)
  benchmarks do not, nor the baseline-versus-new regression system of
  Prompt #2 §45.

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
| "property tests" and "fuzz tests" for every subsystem; "use fuzzing heavily for packet parsers" | §57, §60 | fuzzers exist for the module ELF, user ELF, package, Linux ABI, virtqueue, cosmofs, LZ4, framebuffer and USB descriptors (`tests/fuzz/`); ~~**no fuzzer for the network packet parsers** (named as a gap in `docs/kernel-services/network/testing.md`)~~ **BUILT (the net-fuzz unit, 2026-10-07, §7.8)**: `fuzz_net_frame`, `fuzz_tcp_segments` and `fuzz_dhcp_dns` run the real protocol layers and the tap services on the host over `tests/fuzz/shim_net.c`, in `make fuzz` and CI, with a corpus from a boot's capture; the first run found a connection stranded in CLOSING for ever by a peer's FIN (fixed, `net-fin-acks-last-data`); the NIC receive descriptors, the tap device file and `netif.c`'s worker are recorded as not host-fuzzable; none for PCI configuration, ACPI tables or VFS paths (Prompt #2 §46); no property-based tests by that name |
| "Power-loss simulation must eventually be part of filesystem testing" | §59 | built (`cosmofs-replay`, milestone 4) |
| "eventually create a hardware test matrix" (AMD, Intel, Apple Silicon) | §61 constitution / Prompt #2 §61 | open (2.10) |
| fault injection: "packet duplication, packet reordering, corrupted metadata, CPU starvation, interrupt storms, device reset, VM exit storms" | Prompt #2 §47 | built: allocation, block submit/complete, demand-page, demand-copy, USB CSW, AHCI command issue, the hypervisor self-check and file readpage (`kernel/core/faultinject.c`), a reordering test, the torn-write replay, an IPI storm test, and ~~packet duplication~~ **BUILT (the net-rx-dup unit, `docs/audit/next-subsystem-net-rx-dup.md`, PR #279)**: `FI_NET_RX_DUP` injected at `rx_common` duplicates a received frame (the copy made before the original is queued, carrying `M_CSUM_OK`), fired per-thread in thread context (a no-op in a driver's interrupt handler); `net-rx-dup` proves the stack's duplicate-frame idempotency — a doubled loopback TCP stream is delivered once in order (sequence dedup), and a doubled ARP request yields two replies but one cache entry; **open: CPU starvation, device reset, VM-exit storms** |
| "Design future support for device add, remove, driver bind, unbind, device reset" | Prompt #2 §41 | built: add/remove for USB and AHCI; driver bind and unbind through the device model -- `driver_register` probes matching devices, `driver_unregister` runs remove and clears bindings (kernel/device/device.c:283-320, tests in `docs/kernel/device/testing.md`). Open: binding or unbinding one device independently of registering its driver outside debug builds (`device_test_bind`/`device_test_unbind` exist under `CONFIG_DEBUG`). ~~a generic device reset operation~~ -- **BUILT (the device-reset unit, `docs/audit/next-subsystem-device-reset.md`, PR #277)**: `device_reset(dev)` + a `reset` op on `struct device_driver`; virtio-blk resets in place on the same `blkdev` (`blk_reset` + the removal teardown and probe rebuild), `device-reset` proves it (3) |
| async I/O "must work for files, sockets, devices, timers, IPC, VM operations" | Prompt #2 §23 | the ring drives any object with a readiness operation and has its own alarm timer; **devices shown since the device-readiness unit** (`docs/audit/next-subsystem-device-readiness.md`: a `READ` and a `POLL` on `/dev/net/tap` park and complete on a transmitted frame, the `devices` section); VM operations not shown by any test; ~~a timer as a submittable object~~ -- **BUILT (the aio-timer unit, `docs/audit/next-subsystem-aio-timer.md`, PR #275)**: `timer_create` returns a timer kobject that rides the ring's POLL/READ path, `aio-timer` exercises it. Still open: VM operations as submittable |
| quiesce performance "16 CPUs, 64 CPUs, 256 CPUs where test infrastructure permits" | Prompt #3 §24 | measured at 1 and 4 CPUs only (lifetime report §6) |
| "TSan-compatible host models where possible" | Prompt #3 §23 | built for lockdep and interrupt dispatch (PRs #304, #305); ~~not done for quiesce (4)~~ **built for quiesce 2026-10-05** (`make host-test-quiesce-tsan`, with negative controls; see the [quiescence memory-order report](2026-10-05-quiesce-memory-order-report.md)) |
| the populate loops that hold the space lock across every page "until milestone 5 adds preemption points" | lifetime report §7.2 | milestone 5 landed; whether it shortened those sections is **not verified** here (`VM_KALLOC_POPULATE` still exists) -- a report touching them checks first |

---

## 3. Audit findings and futures no unit has taken up

`docs/audit/2026-09-post-roadmap-audit.md` (2026-09-05). Its ten
milestones and its "after these" list are all built except zero-copy
networking and container tooling. The following are still true of the
tree.

| item | audit section |
| --- | --- |
| **64-CPU ceiling**: `CONFIG_MAX_CPUS` is 64 and `cpumask_t` is one word (kernel/include/kernel/percpu.h:23-25); xAPIC-only addressing, x2APIC never enabled; a single cross-call slot; no ticket or MCS spinlocks; IRQ affinity spread only by NVMe | 6.3, 5.3 |
| no CPU feature framework (`arch_cpu_has`); no errata table; ~~invariant TSC detected but unused, so cross-CPU timestamps are unsynchronised~~ (the third clause closed by the CPU-clock unit, `docs/audit/next-subsystem-cpu-clock.md`: the bit is read, the offset measured and reported, `clock_since_ns`/`clock_delta_ns` are the rule for subtracting two stamps, and a machine whose counter is not common says so instead of promising) | 6.4, 6.2 |
| ~~the watchdog fires once, from CPU 0, no NMI path, no hard/soft-lockup detection~~ -- **closed by the lockup unit (PR #136)**: an NMI sample path on x86-64, a soft-lockup detector on every CPU and a hard-lockup detector on the next online CPU; "fires once" stays by design (the first block is the diagnosis; a second adds nothing). Open: an NMI-class interrupt on AArch64 (GICv3 pseudo-NMI) | 6.2 |
| sequential AP bring-up, one CPU at a time; on x86-64 with a 10 ms INIT delay per CPU (AArch64 polls for each CPU in 100 us steps) | 6.2 |
| ~~no symlinks in the VFS~~ (a fourth vnode type, `symlink`/`readlink`, a walk that expands with a budget, `O_NOFOLLOW`/`lstat`/`readlink` in both ABIs, both filesystems storing one; PR #142); no dentry cache (every component calls the filesystem); no `(ino, generation)` identity; no mount options string; no bind or overlay stacking; no hard links | 8.3 |
| ~~no fsck~~ (`cosmofs_check`: ten finding classes over the live tree, every snapshot, both allocation maps and the inode map, four of them repaired, run over every image the crash suite replays; PR #144); ~~no checksum algorithm id in the metadata header~~ -- **closed by the metadata-csum-id unit (`docs/audit/next-subsystem-cosmofs-metadata-csum-id.md`, PR #273)**: `cfs_mhdr.pad` becomes `csum_algo`, resolved before the checksum, and an algorithm this build cannot verify is `MHDR_ALGO`, distinct from a bad CRC; `CFS_VERSION` 11, older images read as CRC32C. Authenticating metadata is the enabled future work | 8.5, 8.6 |
| No on-disk orphan list; ~~and no record of a deferred free: a crash strands every block the last transaction freed (the commit publishes the root before applying the frees)~~ -- **the deferred-free half is closed by the unmount-leak unit (PR #148)**: format version 9's `free_root` names a chain of `CFS_KIND_FREELOG` blocks listing what the root freed, written before the root and replayed at mount, so the crash suite's stranded-block total is zero. ~~Still open: an inode unlinked while open is recorded nowhere~~ -- **closed by the orphan unit (PR #152)**: format version 10's `orphan_root` names a chain of `CFS_KIND_ORPHAN` blocks listing the inodes whose last name went while something still held them, written from memory each commit and replayed at the next mount by doing what `cfs_evict` would have done -- queuing the blocks and clearing the slot, so the space returns on that mount's first commit. A record rewritten whole rather than a list edited in place, so nothing here is copy-on-write and an ordinary unlink's add and eviction cancel in memory. It covers directories, which the report's first draft wrongly excluded: a working directory is a referenced vnode. **The row is now closed in both its clauses.** | 8.5 (found by the fsck unit) |
| ~~The same defect the deferred-free record fixes still lives in the snapshot path: `cfs_snapshot_hold_block` appends to a deadlist from phase 7, *after* the root is written~~ -- **closed by the deadlist unit (PR #150)**: the snapshot list and its deadlists are copy-on-write, the append moved in front of the root with its blocks from the commit's existing reservation, and the superseded blocks freed exempt. The guess recorded here was incomplete and the unit says why -- reserve-before-fill alone moves an in-place update in front of the root, where the *old* root can see it. It was also incomplete about the damage: the append was a crash hazard as well as a leak, through the deadlist-head pointer it wrote into the in-place list, which the replay suite found at prefix 125 the first time its workload took a snapshot. `cosmofs-freelog-snapshot`'s bound is now `== 0` | 8.5 (found by the unmount-leak unit) |
| ~~**A metadata block fails its checksum after cosmofs's orphan replay, reliably on aarch64 CI.**~~ **Diagnosed and fixed 2026-09-17.** The chase: the failure was 3 of 3 on CI and 0 of 3 locally on a documentation-only branch, one metadata block at a varying address (88, 103, 104) with every other check class zero. What broke it open was the *other* arch: the same test panicked x86\_64 with `kernel write at 0x0` in `list_remove` <- `cfs_buf_get` <- `freelog_release_previous` <- `cosmofs_sync`, **on the `cfs-wb` thread**, immediately after a forced unmount and remount. cosmofs's writeback thread was started lazily by `note_dirty`, and a mount's own replay dirties buffers -- so the thread was being started from inside the replay, then committing a half-built mount across an `fs->bufs` the mount path walks with `fs->lock` unheld (sound only while nothing else is in the filesystem). Two threads on one intrusive list: a metadata block that would not verify on one arch, a null dereference on the other. The same window let a *failed* mount reach `cfs_destroy`, which frees every buffer and `fs`, with that thread still running. Fixed by gating the spawn on `mount_done` and starting it at the end of `cosmofs_mount`; `cfs_destroy` now stops and joins for itself. Proof: `cosmofs-mount-no-early-writeback` -- whether the thread wins the race is timing, whether it exists during the replay is not. Evidence: runs 35168274142, 35169050744, 35169492332, 35169784060 | orphan replay (PR #152) |
| ~~**`cosmofs-writeback` can fail on a loaded machine, and the test is right**: `cfs_writeback_thread` bumps `fs->wb_commits` after `cosmofs_sync` returns, i.e. after the commit that advanced `fs->sb.generation` has already released `fs->lock`~~ -- **closed (PR #165)**. `cosmofs_sync_counted(mnt, writeback)` takes the count inside the same hold of `fs->lock` that publishes the generation, so the field now has exactly two accesses in the tree and both are under that lock. Bug-proofed deterministically, which the race is not: `lockdep_assert_held` at the increment, so publishing outside the lock panics on the first writeback commit of the boot rather than on the one run in a hundred where a reader lands in the window | none (found by the VMState-layout unit) |
| ~~**Taken up by `docs/audit/next-subsystem-fsck-unchecked.md`** (with the row below; neither is struck until it lands). `cosmofs_check` claims no extent overlaps another inside one inode and that entries are ordered by `lblk`, and checks neither: an overlap is caught only when it makes two claims on one block, and an ordering fault that does not is a wrong file rather than a wrong filesystem. A name repeated inside one directory is likewise undetected, because the pass keeps maps of numbers and detecting it needs a set of strings~~ **BUILT (PR #189)**: runs are checked for ascending `lblk` and for overlap, ordering first so the two classes stay distinct, with the end computed in 64 bits; a repeated name is `dir_dup_name`, found through a fixed 4 KiB bitmap whose hit is confirmed by re-scanning the directory, in a pass that finishes before the walk recurses. The description above is the record of what was wrong | 8.5 (named by the fsck unit) |
| ~~**Taken up by `docs/audit/next-subsystem-fsck-unchecked.md`** (with the row above). `cosmofs_check`'s `chain_cycle` class has no test that manufactures it: a cycle needs a corruption hook that writes structure rather than flipping a field, so the `CFS_CHECK_MAX_CHAIN` bound of 4096 is exercised by nothing. `dir_bad` is reported from six places and two of them are fired by a test (a type that disagrees with its inode, and a slot whose number is not its position); the other four are not: a block pointer outside the pool's range, a snapshot member table whose count does not fit its block, an over-long `namelen`, and a directory reached from two parents~~ **BUILT (PR #189)**: all five have a corruption in `cosmofs_test_corrupt` and a test, and all five reporting paths were correct on their first execution -- the report predicted at least one would not be. The `chain_cycle` test is of the **extent** walker, whose guard is `CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2` and NOT the `CFS_CHECK_MAX_CHAIN` this row names; the other four cycle sites stay untested and one test does not cover five walkers | 8.5 (named by the fsck unit) |
| ~~No operator interface for the filesystem's maintenance passes~~ (`/dev/fsctl`: a mount id that is never reused, a listing scoped to the caller's mount namespace, check and scrub against one id, repair behind a flag, and an unmount that drains a running pass rather than tearing the filesystem down under it; `fsctl(8)`; PR #146) | 8.5 (named by the fsck unit) |
| ~~**A cleanly unmounted cosmofs strands what its last transaction freed**, for the same reason a crashed one does: a commit publishes the new root, then clears the freed blocks' bits in memory and dirties those chunks *for the next commit* -- and at an unmount there is no next commit, so the next mount reads them as allocated and unreachable. Measured at 28 blocks on the boot's own scratch disk, the first real filesystem `fsctl` was pointed at, and at 41 when that unit's test stranded some deliberately instead of depending on the residue~~ -- **closed by the unmount-leak unit (PR #148)**. The guess recorded here was wrong and the unit says why: a second commit at unmount does not converge, because writing the bitmap frees the bitmap, so every commit leaves a `pending_free` for the next one. What closed it is a record the root itself names | 8.5 (found by the fsctl unit) |
| ~~A character device's operations run with the vnode lock held (`file_pwrite` takes it before dispatching), so any device that consults the mount table inverts `mounts -> vnode`~~ -- **closed by the chrdev-lock unit (PR #164)**. The row's own last clause was the answer: a filesystem lock is not held across device I/O at all. Checking it found the larger half the row did not name -- `tty_read` waits with no timeout and `ramfs_lookup` hands out one vnode per device node, so a process blocked at a terminal stopped every other reader and writer of it. `file_pread`/`file_pwrite` now run the `VNODE_CHR` arm outside `vn->lock`, with `lockdep_assert_not_held` at both dispatches (its first use in the tree), and the `vnode-chr` lockdep class split is removed rather than layered over -- reverting the fix with the split gone reproduces the original `mounts -> vnode` panic, which is what says the two were coupled. New invariant V32; test `vfs-chr-write-during-blocked-read` | 8.2 (found by the fsctl unit) |
| ~~`vfs_umount2`'s one-unmount-at-a-time guard was unfired by any test: `follow_mount` refuses to walk to a mount that is unmounting, so a second unmount by path failed while resolving; the door thought to reach it, a relative path resolved from inside the mount, did not exist~~ **BUILT (the mount-rel unit, `docs/audit/next-subsystem-mount-rel.md`)**. `mount` and `umount` resolved every target from the caller's root, ignoring the working directory -- the one exception to P27 (`tools/mount-rel-probe.py`: from `/tmp`, a mount on `mrel` covered `/mrel`). They now pass the cwd to `vfs_mount_at` / `vfs_umount_at`, so a path from inside a mount reaches its root without crossing the mountpoint, and `vfs-umount-once` fires the guard that way: a second unmount while the first drains is `-EBUSY` at once | 8.2 (named by the fsctl unit) |
| ~~The per-test time budget treats `process-user` as a test when it is the whole user-mode suite behind one SELFTEST line -- every fs, net, proc, fpu, trap, priv and svc check init makes, plus a process spawn per tool it drives. On CI it was at **7129 ms of 8000** on `main` before the fsctl unit added to it, so the budget failed the next addition whatever that addition was. Given its own budget (20 s) with the reason stated in the harness; the better answer is for the suite to report per-section timings rather than one line, so a slow section is named instead of the whole suite. ~~ **BUILT (PR #188)**: `init --selftest` drives a table of sections, times each call, and prints `USERTEST: section <name> <ms> ms` plus a total; the harness names the slowest (invariant **F13**). No per-section budget -- the composite 20 s stays the only DURATION that fails a run, though the suite's self-consistency (a truncated run, a count mismatch, a missing or unknown section) does. The first measurement contradicts the check counts: `svc` is two fifths of the suite from 34 checks and nine sleeps, while `proc` with 235 checks is smaller. What follows is the record of what was wrong. **Taken up by `docs/audit/next-subsystem-usertest-sections.md`**, which measured it: on CI the line has since reached **8284 ms**, past the 8000 ms every ordinary test is held to. The sections do print `usertest: ... ok` prose, but it is NOT a boundary -- two of the nine print mid-section and `trap_selftest`'s is followed by the UMIP check, so reading the last one as "this section finished" names the wrong section (caught in review of the report's first draft, which claimed otherwise). Hence the timing line is emitted by the driver around each call, not by the section | none (found by the fsctl unit) |
| ~~Nothing in the tree can attempt an unprivileged open~~ -- **the row was stale when it was written down and the lifetime-windows unit corrected it (PR #154)**. `init --unpriv-test` drops to uid 1000 and is refused `mount`, `umount`, a mount namespace, a uts namespace, `sethostname`, `kill` of root's process, `klog`, a reserved port, a 0600 device, a 0700 directory, a 0644 file and the sticky bit on `/tmp`, with the permitted cases asserted beside them. What was actually missing was two doors added *after* that suite: `/dev/fsctl` and `/dev/net/tapctl`, which it now tries | 14.2 (named by the fsctl unit) |
| hotplug: no CPU hotplug, no PCI rescan, no power management; only USB and AHCI notice a removal at runtime -- PCI drivers have `.remove` callbacks, but PCI removal is reached only through the test hooks `pci_test_remove`/`pci_test_rebind` | 10.5 |
| DMA on non-coherent hardware: the audit's "no driver calls `dma_unmap` or `dma_sync_for_cpu`" is no longer true -- every driver unmaps, and NVMe syncs its completion queue before reading it (drivers/nvme/nvme.c:248). NVMe also syncs its submission queue and PRP lists for the device, and the virtqueue syncs its ring for the device (drivers/virtio/virtqueue.c:191). What remains: the virtqueue reads its used ring, and e1000e, AHCI and xHCI read their device-written rings and buffers, with no `dma_sync_for_cpu`; e1000e, AHCI and xHCI sync nothing for the device either. Adequate on coherent QEMU, exposed by the first non-coherent SoC | 13.2, 10.2 (re-checked 2026-09-14) |
| AArch64 hardening: ~~`SCTLR_EL1.WXN` cleared and never set~~ (set on every CPU from the kernel's tables on, proved by `make test-wxn`, PR #140); UAO, E0PD, BTI, PAC unused; ~~a user-triggerable SError panics the kernel~~ -- **closed by the async-error unit** (`docs/audit/next-subsystem-async-error.md`), which found x86-64's `#MC` had the same gap: every SError reached `aarch64_trap_entry`'s `default` arm (relabelled `ARCH_TRAP_GENERAL_PROTECTION`) and vector 18 was unregistered, so both panicked even for an error the hardware had **corrected**. Now classified and dispatched as `ARCH_TRAP_ASYNC_ERROR` (invariant I-ARCH-16): corrected is counted and returned from, everything else panics naming the class and the syndrome. **No process is killed** -- review refused that in the report, because an asynchronous abort's frame names the context interrupted at delivery, not the one that caused it; attribution needs the RAS error records and is a unit of its own. Two things the building found: `HCR_EL2.VSE` is inert without `AMO` (the host runs `HCR_EL2 = RW` alone), and **EL1 runs with `PSTATE.A` masked for the kernel's whole life**, so the kernel never takes an asynchronous abort while running -- EL0, entered with DAIF clear, is the live path. Whether EL1 should unmask `A` is recorded as I-ARCH-16's gap, not settled; no device-tree parsing for the host (ACPI only); PSCI variations untested | 13.2, 13.3 |
| ~~SMEP/SMAP/UMIP absence silently accepted; `mmap`/`mount`/`umount` accept unknown flag bits~~ (the `hardening:` boot line and the guard boot `make test-guard`; unknown bits `-EINVAL` in `mmap`/`mount`/`umount`/`open`; PR #140) | 14.2 |
| **`net-harness`: the host side is built (PR #177); what is open is why slirp's host-side connect stalls.** The heading this row carried for three weeks -- "the twelve bytes were never written" -- was falsified by its own contents twice over: PR #170's x86-64 job sent all twelve, and sightings twenty-three and twenty-four settled that the harness's blind accept, not the guest's send path, was what could not be seen. The history below is kept in the order it was learned, because each framing was the best available at the time and the record of being wrong is the useful part. The guest's instrumentation answered on two of its own pull request's CI jobs, one per architecture: `client failed: connect 0, sent -104, recv -1, sndbuf free 65536 before, 65536 after send, 65536 after read (outstanding 0 then 0), state 0, segs_out +0 retransmits +0 refused +0 rsts_in +0` on x86-64, and the same line with `rsts_in +1` on aarch64. **-104 is ECONNRESET** (`errno.h:47`) and **state 0 is TCP_CLOSED** (`tcp.h:38`), so `ksock_sendto` failed: nothing was queued, nothing transmitted, and the connection was already reset when the guest wrote to it -- while the host had *accepted* it a second earlier, so slirp's own connect to 127.0.0.1 had succeeded. Every earlier framing of this row, including "twelve bytes that never arrive", describes a symptom of something that had already happened. **The question is now: what resets an established connection between `ksock_connect` returning and the next statement?** **The aarch64 job adds `rsts_in +1`**: an inbound RST, accepted in sequence -- this stack implements RFC 5961 §3 (`tcp.c:2000-2011`), so a reset anywhere but exactly `rcv_nxt` is a challenge ACK and is never counted, and the accepted one sets `pcb->error = -ECONNRESET` and ends the pcb. So the reset came *off the wire*, not from the guest's own stack. Who sent it is still not named: `tcp_get_stats` is machine-wide, so `+1` does not say the reset was this pcb's, and the x86-64 job's `+0` for the same failure is the window starting *after* the connect rather than a run without a reset. **PR #170's own CI then broke the pattern**: on a documentation-only branch, its x86-64 job printed `sent 12 ... sndbuf free 65536 before, 65524 after send, 65524 after read (outstanding 12 then 12), segs_out +1 retransmits +0 rsts_in +1, recv -104`. The guest queued the bytes, put a segment on the wire, and they were never acknowledged -- so "the twelve bytes were never written" describes three sightings and not the fourth, and the send path is ruled out as the defect. `retransmits +0` is *not* evidence of a retransmission bug here: a reset that kills the pcb before the timer fires leaves it flat. The locus is now **an established connection to slirp is reset -- sometimes before the guest writes, sometimes after a segment is on the wire -- and the payload never reaches the host's accepted socket**. **Both instrument gaps are now closed by the socket-verdict unit**: `tcp_get_stats` is sampled *before* `ksock_connect`, each step is timed, and the line carries `pending error %d` from `ksock_error` -- the pcb's own verdict rather than a machine-wide counter. **And the moved window answered on PR #171's own aarch64 CI**: `connect -104 in 1381 ms, ..., pending error -104, segs_out +3 retransmits +1 rsts_in +1 (counters from before the connect)`. The **connect itself** was reset -- every earlier instrumented sighting said `connect 0` -- and `pending error -104` is the first per-pcb verdict read rather than inferred. `ECONNRESET` rather than `ECONNREFUSED` is set only by a reset accepted on a *synchronized* connection (`tcp.c`, RFC 5961 §3), so the handshake completed and the reset arrived before the connecting thread ran again; `retransmits +1` and 1381 ms are one SYN retransmission. **This unifies the shapes**: across seven instrumented sightings the only thing that differs is how far the guest got before the reset landed -- connect, send, or an unacknowledged segment. The constant is an inbound reset on an established connection to slirp, while slirp's own host-side socket connects fine, which is why the host's `accept` keeps succeeding and then reading nothing. **Why slirp resets it is not established and is outside this kernel**, which the twelve-bytes report named as a possible result from the start. Eighteen sightings to 2026-09-17 (`docs/testing/flakes.md`, "The count"), nine instrumented, seven carrying `rsts_in +1`. A SYN-retransmission pattern in the first two moved-window runs was withdrawn when the third had none. Reproduces on x86-64 locally one boot in twenty-one; the tally is `docs/testing/flakes.md`, "The count". ~~**Taken up by `docs/audit/next-subsystem-nettest-accept.md`**~~ **BUILT (PR #177): the host-side half of this row is closed.** The harness now identifies the guest's connection by the request it delivers, keeps a roster of every other connection that reached the port, gives each connection its own receive budget, and reports the roster in the failure line instead of `TimeoutError`. ~~**Taken up again by `docs/audit/next-subsystem-nettest-probe.md`**~~ **BUILT (PR #182)**: the harness now records what slirp's own host-side connection was DOING: its TCP state (`ESTABLISHED` vs `CLOSE_WAIT`), **how that connection ended, in four classes** (`closed`, `error` with its errno, `deadline`, `wrong-data`) -- both outcomes reach the roster as `0 byte(s)` today, and the loop's `except OSError: chunk = b""` would record a RESET as an orderly close, which is the case under investigation -- and a timed probe through the same slirp to the guest's echo port, whose FAST reading is the informative one (a slow one implicates slirp or the guest and cannot separate them). A write probe was proposed first and dropped: a write into `CLOSE_WAIT` succeeds, so it cannot tell an open peer from a closed one. A host-side packet capture would be the right measurement and needs root (`/dev/bpf0` is root-only), so it is left to a human with `sudo` rather than designed into a test. **ANSWERED ON THE PROBE'S FIRST OUTING (sighting 30)**: slirp reset the guest's half of the connection and left the host's half **ESTABLISHED, open and silent**, while answering a probe through the same slirp in **1 ms**. So it is a per-connection failure inside slirp -- not a stall (slirp was healthy), not a foreign connection (one connection, every time), and not this kernel (both halves now accounted for). What is NOT named is the line of code; the next measurement is a host-loopback capture, which needs root, or slirp's own source -- both outside this tree. Earlier: the instrument narrowed it on its first outing: sighting twenty-three's roster named **exactly one connection, carrying nothing**, so it is not a foreign connection reaching the port. With the guest reporting `connect 0 in 1116 ms, segs_out +3 retransmits +1` for the second sighting running, the locus is **slirp's own host-side connect**, and what is unnamed is why that connect stalls about a second and then fails. That report took the **host** half rather than the guest's: the harness *had* listened with a backlog of one and accepted once, blindly, so it could not tell the guest's back-connection from any other connection to that port and reported `TimeoutError` when it met one. It no longer does either. **Demonstrated 2026-09-18**: occupying that single slot before QEMU starts reproduces the signature on the first boot -- host `accept` succeeds, 0 of 12 bytes, guest `recv -104` with `rsts_in +1` -- and the capture shows slirp acknowledging the twelve bytes into its own buffer, never delivering them, and resetting the guest ten seconds later, with the guest correct from first SYN to final reset. A healthy back-connection, captured from a passing boot, is answered in **150 us** and completes in **52 ms**; there was no such baseline before. Two side facts measured and closed: a backlog of one makes a second connect **hang silently** rather than refuse, and `free_port` is clean (0 collisions in 3000 triples, range 49152-65535). The adversary was injected, so the reproduction names the mechanism the harness could not report rather than the cause on CI -- which sighting twenty-three then ruled out directly, as above **AND THE TEST NO LONGER FAILS FOR IT (PR #218, `docs/audit/next-subsystem-nettest-retry.md`)**: the diagnosis above stays open and unowned -- it is in slirp -- but the guest's back-connection now runs the exchange up to three times on fresh sockets, so one reset from a third party no longer fails a test of this kernel. The bound is a failure, not a fallback; every attempt prints the diagnostic block the three units above built; both outcome lines name the attempt, so a recovered boot is still a countable sighting and `docs/testing/flakes.md` counts it. The host harness needed no change, which PR #177's roster had already made true. `make test-harness-retry` boots a build whose first attempt is broken from inside the guest, so the retry is exercised every time rather than one boot in twenty | none (found by the hardening unit; localised by the nettest-deadline and twelve-bytes units) |
| One firmware in CI: both CPU models are booted there (the default and the guard boot, PR #140) but only against Debian's AAVMF. The VHE handover path (`HCR_EL2.E2H` set at hand-over) exists because that build differs from the EDK2 a developer has locally, and the other build's path -- the plain `_EL1` writes -- is exercised by every CI boot but by no CI *firmware* variation. A firmware matrix in CI, or a documented minimum EDK2, is unbuilt | 13.3 (found by the hardening unit) |
| The IOMMU walker (`kernel/iommu/pt.c`) builds one four-level root, a level-0 start the architecture allows only above 42 bits of output; an SMMU of 42 bits or less is left unregistered at probe with a `WARN`, the machine booting without DMA remapping on it, rather than programmed with a start level it rejects (PR #140), and the concatenated level-1 root it would need is unbuilt -- untestable on QEMU's 44-bit SMMU; the hypervisor's stage-2 (`hv_s2_layout`) has the rule and the shape | 13.2 (found by the hardening unit) |
| ~~VFS: `close()` cannot report write-back errors~~ -- **closed by the file-path unit (PR #138)**: the page cache records, each open file hears once by `fsync` or `close` (a `flush` hook on the I/O object type), a named file's pages lost at release are counted; with it the 8.2 LOW "`vnode_release` writes back dirty pages of an `nlink==0` file" | 8.2 (MEDIUM, not in any milestone) |
| ~~`read` returns at most 1 KiB per call through the stack bounce buffer~~ -- **closed by the file-path unit (PR #138)**: a bounce sized to the request up to 64 KiB, one object call, shared by both personalities; the read/write bandwidth benchmark the audit asked for exists (`read-bench`, `write-bench`, `USERBENCH`) | 4.2 (MEDIUM) |
| ~~**`struct cosmo_vcpu_regs` is 496 bytes on AArch64 and 448 on x86-64**, while its own comment and `tests/host/test_hv.c:75` say both are 448~~ -- **closed by the VMState-layout unit (PR #162)**. Not by resizing: the header's own preamble calls these structures stable, x86-64's 448 is correct, and the AArch64 ABI has been 496 since the EL2 backend -- the documentation was what was wrong. Each real size is now asserted by `_Static_assert` in `uapi/cosmo/syscall.h`, so the check runs in every translation unit on every architecture rather than in one host binary that compiled whichever block the build host matched. Removing the stale host assertion also unblocked the **fourteen host suites ordered after `test_hv`**, which had never run on an arm64 host and all pass. `hv-vcpu-regs-roundtrip` is new | none (found by the fsck unit, whose host-test run was on an arm64 host) |
| coverage: no instrumented build; no line coverage of the self-tests | 16.2 |
| ~~`chdir` through a symbolic link published the link's spelling with the target's vnode, at both doors: `getcwd` named the link rather than the directory, and a later relative `chdir("..")` normalised lexically against that spelling and could publish a name unrelated to the vnode it reached; the dirfd unit had `fchdir` refuse a directory with no coherent name, and the coherent answer was thought to need a dentry cache or parent pointers~~ **BUILT (the cwd-name unit, `docs/audit/next-subsystem-cwd-name.md`, invariant P32)**. Measured first at both doors by `tools/chdir-link-probe.py` (after `chdir` through a link and `chdir("..")`, `getcwd` named `/tmp` while the process stood in `/tmp/clp`), and answered without either structure: the walk keeps the name it took (`vfs_lookup_named`, `vfs_open_named`). `chdir`, a spawn's `cwd` and a directory file's name are that path, so `fchdir` of a directory opened through a link succeeds with its own path. The same probe found procfs's process directories refusing `..`; they answer it now, and `/proc/self` is a symbolic link to the reader's pid | none (found in review of the dirfd unit) |
| ~~`sys_mmap`'s non-fixed path chooses under one hold and inserts under another~~ **BUILT (the mmap-place unit, `docs/audit/next-subsystem-mmap-place.md`)**: `vm_user_map_anon_free`/`vm_user_map_file_free` choose and insert under one hold, both doors use them, invariant **M46**, `mmap-place-race` plus a racer at each door. The record of what was wrong: `sys_mmap`'s non-fixed path, at both doors, chooses a range under one hold of the space lock (`vm_user_find_free`) and inserts it under another (`vm_user_map_anon`/`vm_user_map_file`), so two threads asking for a placement at once can be handed the same hole and the loser's insert is `-EEXIST` -- reached userland as a thread start failing with `EEXIST` (`thrtest`, 2026-09-23, `docs/testing/flakes.md`). The MAP_FIXED unit fixed replace-as-two-holds one branch over and removed the libc retry that had been absorbing this race too. Measured by `tools/mmap-place-probe.py` | none (found by the cwd-hold unit's rebase boot) |
| ~~**An exited process keeps its address space until its last reference drops** (found by the proc-settle unit, `docs/audit/next-subsystem-proc-settle.md`): only its handles close at exit (`process_last_thread_gone`); `vm_space_destroy` runs in `process_release`. The native `waitpid` returns once the child is `EXITED`, which the reaper sets before it drops the thread's reference, so a program that waits for a child and then writes the child's binary can get `-ETXTBSY`, and the child's memory stays allocated after its status is collected. Forced with the probe's reap delay: `elf-txtbsy` fails that way on both architectures; no natural sighting. Linux releases the address space in `do_exit`. Moving the teardown to exit needs an audit of everything that reads `p->space` after exit~~ **BUILT (the exit-space unit, `docs/audit/next-subsystem-exit-space.md`, PR #253)**: the space goes first in `process_last_thread_gone`, before `EXITED` is published (P34), and a dying space is torn down without per-chunk shootdowns, stepping over absent tables (M47); 0 of 1,800 rewrites refused unforced | proc-settle report |

---

## 4. The quiesce report's remaining risks and debt

The 2026-10-03 renewed lock-discipline audit found two defects in the
already-built validator, rather than a missing milestone: unbounded
~~printing of a truncated cycle path~~ and ~~graph names pointing into freed
module rodata~~. **CLOSED (PR #302)** in the October hardening pass, with
ASan/UBSan path/storage regressions and a real module fixture. See
`2026-10-03-lock-discipline-audit.md` and `2026-10-03-lockdep-report.md`.
**CLOSED (PR #303):** ~~transitive IRQ-safe/unsafe dependency
validation~~, ~~IRQ trylock classification~~, ~~timer-cancellation callback-lock
checks for observed active callbacks~~, and ~~spinlock irqrestore state
validation~~. Section 7 records the completed diagnostic and validation
increments in PR #304. Remaining: NMI/#MC validator writer reentrancy,
global held-state snapshots across CPUs and threads, raw
`arch_irq_restore` ownership/pairing, and callback wait relationships beyond
observed timer callback paths. Direct class checks and ordinary dependency
edges must not be presented as proof of those broader relationships.

The same validation found **UP suite prerequisites**: unchanged HEAD failed
cwdtest's progress assertion on x86-64 and AArch64; AArch64 additionally
failed two clock tests whose helper required two CPUs. Both prerequisites
are corrected while preserving their assertions: the mover rendezvous makes
progress observable, and the clock test uses a local monotonic bracket on
UP. A LOCKDEP=0 boot also showed `rlimit-unpriv`'s assumed 80-write burst
taking long enough for the log bucket to refill (exit 19); the unchanged
assertions pass on rerun.
These failures and analyzer warnings are detailed in the October report;
none is silently counted as passing validation.

`docs/audit/2026-09-lifetime-quiesce-report.md` §7-8 (2026-09-05). Its
"NEXT SUBSYSTEM", lockdep, was built as milestone 3. Still standing:

- ~~**grace-period latency is tick-bound** (a 4-8 ms floor);
  `synchronize_quiesce` polls; the wake-on-publish design that would
  remove the floor was not built.~~ -- **the poll is gone (the
  quiesce-wake unit)**: `synchronize_quiesce` blocks on a queue with a
  `TICK_NS / 2` deadline and is woken by a CPU publishing from a context
  that holds nothing (invariant Q18). Measured on four idle CPUs:
  **3.73-3.81 ms a grace period, against 4.29-7.55 ms polling** -- about
  half, and the spread collapses.
  **The row's premise was half wrong and the unit says so**: the floor is
  not the sleep. A grace period is not over in microseconds; it is over
  when the other CPUs reach a quiescent point, which while they are
  halted means their next tick, and that ~3.75 ms is untouched. What the
  poll added on top -- a 2 ms request serviced at the next 4 ms tick,
  twice -- is what went. The wake could not go where the report put it
  (inside the scheduler: the machine dies at five seconds) and the trap
  returns alone were not enough (an idle CPU is halted, so its publish
  comes from the idle loop). All five synchronous callers benefit:
  `interrupt_unregister`, module unload, `netif_unregister`, the
  receive-hook removal, and the `call_quiesce` batch worker.
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
  ~~**Taken up by**~~ **BUILT (the virtio-removal unit,
  `docs/audit/next-subsystem-virtio-remove-inflight.md`)**: a second
  virtio-blk (`QEMU_RMDISK`; `vdb` on q35, `vdc` on `virt`) attached
  last on both machines, `virtio-remove-inflight` driving
  `pci_test_remove` with the driver's slots occupied by construction --
  64 found in flight, 64 completed `-EIO`, nothing after the removal's
  own boundary stamp -- and `pci_test_rebind` bringing the disk back
  with its sector intact. **The row is closed in every clause.**
- ~~**What the straggler kick is worth is an open question**, added by
  that unit rather than struck by it.~~ -- **ANSWERED (PR #179): it
  works, but barely.** About 220 kick IPIs sent and **1 publish
  attributed** over eight boots, four per architecture, the one on
  AArch64 -- so by the decision rule the report fixed before measuring
  the kick stays and deletion is off the table, on evidence thin enough
  that the follow-up should widen the sample first. A first version of
  the measurement said four per cent and was wrong: it counted publishes
  by CPUs that had already published the target epoch, which advance
  nothing. Attribution now requires the publish to have MOVED this CPU's
  epoch. **The population the kick's own comment
  named is not the reason**: the adversary was built as designed and
  showed that a CPU whose tick keeps landing inside a short read-side
  section publishes *without* a kick, because `schedule()` publishes at
  entry and the covered tick still sets `need_resched`. The publish was
  never confined to the trap return. Which population the one attributed
  publish came from is the open part now, and it is a question the
  counter can answer and argument could not.
  The report, `docs/audit/next-subsystem-straggler-kick.md`, took the
  *measurement* rather than any of the three
  outcomes: nothing in the tree counts a kick that **worked**, so no
  counter would change if the kick were replaced by a no-op, and the
  choice between deleting it, bounding it and proving it cannot be made
  on the evidence that exists. It also names a latent defect found while
  reading it: the kick sent `IPI_RESCHEDULE` (PR #179 replaced it with a
  dedicated `IPI_QUIESCE_KICK`), whose documented contract
  is "target re-evaluates `need_resched` on interrupt return" and whose
  handler comment says the sender set `need_resched` under a run-queue
  lock -- **the quiesce caller does neither**, and wants only the trap
  return. The hazard is on the **send** side: a handler that returned
  early on `!need_resched` would not neuter the kick, because the trap
  tail evaluates the quiescent point independently of the handler. What
  would is a send **suppressed because the target's `need_resched` is
  clear** -- "nothing to reschedule there, do not interrupt it", the
  natural optimisation for an IPI documented as re-evaluating that flag.
  The quiesce caller never sets it, so every kick would be suppressed
  with no test to notice. The looser forms do not follow: skipping when
  the flag is already set would still send, and coalescing preserves the
  first interrupt. A CPU publishes at interrupt
  return only when `preempt_count == 0`, so the kick cannot help a CPU
  spinning inside a read-side section -- which is the case its own
  comment named until this unit corrected it. It fires only for a CPU
  pending past two ticks, and the one population it can help (a CPU
  whose periodic tick keeps landing inside a short disabled region) is a
  phase coincidence no deterministic test can arrange. Deleting it,
  bounding it, or proving it are three different units
  (`docs/audit/next-subsystem-lifetime-windows.md`).
- ~~**quiesce ordering verified by review and sanitizers only**: no TSan model, no
  litmus tests (Prompt #3 §23 asked for them "where possible"). The lockdep
  graph/held-stack model completed in PR #304 does not close this lifetime
  protocol gap.~~ **BUILT 2026-10-05**: six litmus tests checked by herd7
  against RC11 (`make litmus`, with reachability witnesses and eight
  negative controls) and the epoch-core host model under TSan with two
  negative builds (`make host-test-quiesce-tsan`), both in CI. The litmus
  model found a real ordering hole -- a CPU coming online could be missed
  by a grace period while its reader missed the unlink -- fixed by two
  `seq_cst` fences (W1b, Q0). See the [quiescence memory-order report](2026-10-05-quiesce-memory-order-report.md).
- **unexplained**: the AArch64 virtio-console flake seen once in four
  runs on 2026-09-05 (the console file lacked the last line while the
  serial log was complete).
- **~~small debts~~ — the first two were not, and are now closed**:
  ~~`nd_flush`/`arp_flush` drop in-flight resolutions silently when an
  interface goes; ARP and ND entries hold bare interface pointers and
  rely on the flushes in `netif_unregister`.~~ **As of PR #184 both
  flushes count what they drop** (`arp_stats.pending_dropped` and the new
  `ip_stats.nd_pending_dropped`, which is a different struct — one fix
  was invisible to the other), **and both retry paths hold a
  `netif_get` across the send**, taken under the table lock and released
  after it (invariant **N22**). Entries still hold a bare pointer *by
  design*, and N22 records why: the flush clears them, so a reference
  each would turn a missing flush from a dangling pointer into a leak
  without fixing the dangling pointer. The description of the defect
  below is kept in the past tense it deserves — it is the record of what
  was wrong, not a statement about the tree.
  (`MODULE_MAX_LIVE`'s fixed 32-slot array and the zombie-module reaping
  below were untouched by that unit; PR #186 closed both.)
  ~~**Taken up by `docs/audit/next-subsystem-arp-netif-ref.md`**~~
  **BUILT (PR #184), and this is what it was**: the retry paths in
  `arp_age` and `nd_age` **copied** that bare pointer out from under the
  table lock, released the lock, and then dereferenced it --
  `send_arp` reads `nif->mac` and `nif->ip4.addr`. The flush at
  `netif_unregister` step 5 did not close that window and the `input_one`
  barrier at step 4 did not either, because `age_work` re-arms on a
  one-second timer and a fresh one could start after the barrier and
  before the flush. It **was** a **use-after-free in a transmit path**
  reached by every tap teardown. `netif.h:81` states the reference rule
  only for pointers lookups RETURN, and ARP and ND never looked the
  interface up -- it arrives as an argument and is kept, which is why
  that rule did not reach them and why **N22** now states the one that
  does;
  ~~`MODULE_MAX_LIVE` is a fixed 32-slot array; zombie modules are
  reaped only by a later `module_unload` of the same name.~~ **~~Taken up by
  `docs/audit/next-subsystem-module-zombie-reap.md`~~** **BUILT (PR
  #186); §4's small debts are now closed.** Every `module_load` and
  every `module_unload` now sweeps the zombie list by identity
  (invariant **M24**), and exhausting the slot array returns `-ENOSPC`
  from a slot reserved before `init()` runs. What follows is in the
  past tense it deserves -- it is the record of what was wrong, not a
  statement about the tree.

  The zombie half was understated here: a zombie keeps its whole image
  AND its dependency pins (`drop_deps` runs at the free, not the
  unload), so one that was never collected blocked unloading every
  module it depended on, for good. Nothing called the reaper; a name
  reused by a replacement hid the zombie, because `module_unload` finds
  the live module first; and `find_zombie_locked` returns the FIRST
  name match, which the reap then removes -- so N zombies of one name
  needed N of those calls, and nobody made even the first. (An earlier
  version of this row said the second was unreachable by any call. That
  was wrong: it read the first-match lookup without checking that the
  reap `list_remove`s what it finds.) It survived because the happy
  path was tested and passed (`selftest_module_unload_busy`): the
  mechanism worked, and there was no policy that invoked it. The slot
  array's defect was separately that exhaustion **panicked** where
  `-ENOSPC` existed; it was NOT caused by zombies, which hold no slot
  (`unpublish` runs at unload step 1, before the zombie is made --
  checked, because the report's first draft assumed a connection).

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
  bitset waits, the wall clock, the AArch64 table;
- the Linux fd and IPC families: `eventfd`, `timerfd`, `signalfd`, `epoll`
  with `EPOLLET`, `memfd_create`, System V shared memory, in-place
  `mremap`, `sysinfo`, `statx`; Unix-domain sockets and named pipes;
- symlinks, `fsck` and `fsctl`; file-backed shared mappings, shared
  futexes and shared ELF text; priority inheritance; generic device reset
  (virtio-blk); USB scatter-gather.

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

## 7. Lockdep milestone follow-ups (2026-10-03)

Completed work from the October lockdep session is struck through below.
PRs #303 through #308 are merged. PR #305 includes counter consistency,
mutex/new-edge measurements, interrupt writer serialization, thread mutex
snapshots, dense/full-capacity graph measurements, kernel writer/IPI tests,
and the lockup deadline-read correction. PR #306 adds unhandled-IPI and
interrupt-boundary coverage, deterministic graph-search bounds and first-use
acquisition measurements. PR #307 merges the contention-cleanup,
queued-mutex, ELF-fixture, raw-lock re-entry and panic-output rows below.
PR #308 merges the October 4 observed-spin rows; its remote CI run
37187562158 passed on both architectures after a startup-dependency fix.
Evidence and validation limits are in
[the lockdep report](2026-10-03-lockdep-report.md) and the
[observed-spin continuation](2026-10-04-spin-contention-report.md).

| Completed item | Implementation and scope |
|---|---|
| ~~Transitive IRQ-safe/unsafe dependency validation and IRQ trylock classification~~ | **BUILT (PR #303)**: IRQ dependency graph checks, beyond direct class checks. |
| ~~Spinlock IRQ-state restoration validation~~ | **BUILT (PR #303)**: validates tracked irqsave/irqrestore state; raw architecture IRQ ownership/pairing remains open. |
| ~~Timer-cancellation callback-lock checks~~ | **BUILT (PR #303)**: profiles learn locks from observed active timer callback executions; unobserved paths and other callback waits remain open. |
| ~~UP validation prerequisites in cwd progress and clock tests~~ | **FIXED (PR #303)**: explicit mover rendezvous and local monotonic clock brackets preserve the assertions on one CPU. |
| ~~Concurrent lockdep graph and held-stack host models~~ | **BUILT (PR #304)**: ASan/UBSan and TSan cover graph publication, serialized writers, diagnostic readers, and bounded atomic held-stack snapshots. Kernel interrupt entry and callback protocols are outside these models. |
| ~~Consistent normal graph diagnostics~~ | **BUILT (PR #304)**: counts and edges print from a private graph copied under the raw writer lock. Panic reports print held-lock diagnostics, not a graph dump. |
| ~~Accurate panic thread and IRQ/preemption context~~ | **BUILT (PR #304)**: reports the actual context instead of always claiming boot; deliberate crash tests require that context. |
| ~~Bounded remote CPU spinlock-stack snapshots~~ | **BUILT (PR #304)**: one atomic snapshot attempt returns a consistent private copy or unavailable, without waiting on the target. Thread stacks and global state are not covered. |
| ~~Read-only held-stack snapshot validation from x86 NMI~~ | **BUILT (PR #304)**: real NMIs exercise a held graph lock and a busy stack writer. Delivery timeout retains the handler and live argument, releases test locks, and fails stop explicitly; injected first/second timeouts validate cleanup. This does not establish NMI writer reentrancy. |
| ~~Matched debug LOCKDEP=0/1 warmed spin-path measurements~~ | **BUILT (PR #304)**: empty, spin, irqsave, and nested paths report min/median/max on both architectures under QEMU. No performance threshold or native-hardware claim. |
| ~~Excessive initial user-stack builder scratch on the kernel stack~~ | **FIXED (PR #304 review/CI follow-up)**: private heap scratch reduces the local AArch64 builder frame from 5,328 to 192 bytes; allocation failure follows process cleanup. The protection-capable boot and post-self-test workload pass locally. |
| ~~Consistent lockdep statistics snapshots~~ | **BUILT (PR #305)**: all counter updates and the snapshot copy share the graph raw lock. Operations can still be in progress; held stacks are not frozen. |
| ~~Matched debug LOCKDEP=0/1 warmed mutex-path measurements~~ | **BUILT (PR #305)**: adds private mutex lock/unlock and successful trylock/unlock paths. Contended waits and priority inheritance remain unmeasured. |
| ~~New-edge core search measurements on sparse chains~~ | **BUILT (PR #305)**: private 16/64/256-node graphs exercise allowed insertion, cycle rejection, and transitive IRQ-conflict rejection. Setup is outside every sample; these are core costs, not cold-cache or complete acquisition timings. |
| ~~Concurrent interrupt-table writer and diagnostic data races~~ | **FIXED (PR #305)**: per-vector raw writer serialization prevents competing registrations/removals; atomic count/name reads support concurrent diagnostics. Actual-source host tests cover publication and writer races; dispatch stays lock-free and record reuse still requires a grace period. |
| ~~Bounded thread mutex-stack snapshots~~ | **BUILT (PR #305)**: atomic single-writer publication and capacity-aware reads with caller-owned thread lifetime. Panic diagnostics copy each local stack or report unavailable. Separate CPU/thread snapshots do not form a global view. |
| ~~Dense DAG core search measurements through the class limit~~ | **BUILT (PR #305)**: chain/dense 16/64/256/320-node cases validate allowed insertion, cycle rejection, and IRQ-bridge rejection. Only subclass zero is active; these measurements do not establish worst-case bounds. |
| ~~All-subclass graph search measurements at full capacity~~ | **BUILT (PR #305)**: 1,280-node chain/dense cases cover all subclasses, with explicit IRQ endpoint checks and sanitizer coverage of full BFS queues. Worst-case latency bounds remain open. |
| ~~Kernel interrupt writer, IPI and grace-period reuse regression~~ | **BUILT (PR #305)**: 16 rounds race two registrations/removals, validate the winning handler via real IPI, and wait before reuse. Dispatch follows registration; arbitrary entry interleavings remain open. |
| ~~Stale lockup response mask after delayed deadline read~~ | **FIXED (PR #305)**: both polling APIs read expiry before collecting responses. A controlled real-IPI/NMI probe reproduces the old missing-mask failure and validates the fix on both architectures; the five-millisecond deadline remains unchanged. |
| ~~Real unregistered-vector delivery and concurrent unhandled totals~~ | **BUILT/FIXED (PR #306)**: real IPIs exercise unhandled dispatch, repeated delivery after EOI, and handled reuse. Architecture warning totals use atomic increments. Hardware-spurious vectors and fatal exceptions are separate paths. |
| ~~Interrupt dispatch bounds and synchronous-removal error-path coverage~~ | **BUILT (PR #306)**: actual-source host tests check invalid dispatch and oversized-init panic diagnostics, logical/full-table boundaries, preserved registrations on failure, and synchronous-removal wrapper ordering. ASan/UBSan, TSan and three failing mutation controls validate the tests; real panic shutdown and epoch completion are outside these host shims. |
| ~~Deterministic graph-search work bounds~~ | **BUILT (PR #306)**: host-only counters in shared search helpers check queue, bitmap and parent-walk bounds on full-capacity graphs and composite IRQ checks. Dense/cyclic traversal attains 25,600 adjacency-word reads; a full chain attains 2,559 parent steps. A duplicate-scan control fails the bound. This does not bound wall-clock latency or raw-lock contention. |
| ~~Uncontended first-acquisition measurements against the live graph~~ | **BUILT (PR #306)**: public spin, irqsave, nested-spin and mutex acquisitions include first class/usage/edge checks and held-stack publication, then compare reuse. Three fresh samples per path consume 18 classes. Matched debug LOCKDEP=0/1 boots cover both architectures; timings remain descriptive QEMU observations, not worst-case or native-hardware bounds. |
| ~~Contention-test lock/timer cleanup on a missed callback window~~ | **FIXED (PR #307)**: release the spinlock, synchronously cancel the stack timer and join the completed holder before returning failure. Readiness and exit guards fail stop with the thread retained. Old/fixed missed-timer probes validate state on both architectures; x86 probes validate both timeout diagnostics. The timing window is addressed by the PR #308 rows below; stopped-owner spin waits remain open. |
| ~~Controlled queued-mutex acquisition measurements~~ | **BUILT (PR #307)**: verify the waiter entered the real mutex queue before a controlled 1 ms owner hold and release; two warmups and nine samples time public acquisition through ownership. Tests check protected-data handoff and lifetime-safe queue/exit failure paths. These are total wait times, not isolated lockdep or priority-inheritance overhead. |
| ~~Console-dependent lifetime of ELF inspection fixtures~~ | **FIXED (PR #307)**: `elf-text-ro` and `elf-data-private` use `init --spin` until explicit kill/wait cleanup. Their former `--block` child could read console input and exit before address-space inspection; a process reference alone does not preserve that space. Protection and data assertions remain intact. |
| ~~Unbounded same-CPU lockdep raw-lock re-entry~~ | **FIXED (PR #307)**: the acquiring CAS publishes CPU ownership in the raw word. Re-entry fails stop without stealing or clearing the interrupted owner's lock. Direct probes and real x86 NMI exercise the diagnostic, including a busy held-stack writer. Arbitrary NMI/#MC tracking and cross-CPU wait cycles remain open. |
| ~~Tracked log-ring and VirtIO queue locking during panic output~~ | **FIXED (PR #307)**: irreversible panic mode skips ring writes and the VirtIO console transport, retaining serial/framebuffer output. This prevents diagnostic recursion into an interrupted validator and waiting for those lock owners. Held-ring probes cover both architectures with lockdep enabled and disabled; general sink faults and a VirtIO panic transport remain outside this fix. |
| ~~NAT expiry test racing unfinished flood packets~~ | **FIXED (PR #307 follow-up)**: `net-nat` counts flood outcomes relative to its baseline and asserts completion before aging. Lifetime counters survived `nat_flush`, allowing four earlier translations to hide four pending packets. Controlled real-worker probes reproduce the old post-sweep entries and validate the fix on both architectures; a withheld-tail control fails before aging. Production NAT timeouts and expiry/quota assertions are unchanged. |
| ~~IRQ routing test assuming five deliveries within 50 ms~~ | **FIXED (PR #307 follow-up)**: wait for five atomic-counted deliveries with a 1 s migration-safe deadline instead of sampling after a fixed delay. Missing delivery still fails, while the existing mask checks and deferred cleanup remain. Slow-source and negative probes are documented in [interrupt testing](../kernel/interrupt/testing.md). |
| ~~Composite NIC benchmark and chaos boot outgrowing default timing limits~~ | **ADJUSTED (PR #307 follow-up)**: CI completed the two-interface benchmark in 8,360 ms and all 422 self-tests, but exceeded the default watchdog and shell deadlines. The unchanged benchmark workload now has a shared 20 s watchdog/harness budget; chaos boots have 240 s total. Ordinary boot limits and signal-response checks remain. See the [audit evidence](2026-10-03-lockdep-report.md). |
| ~~Contention-test reliance on a fixed holder/callback timing window~~ | **FIXED (PR #308)**: a self-test-only observer publishes actual failed spin exchanges. The holder waits for the callback's observed contention; early callbacks rearm. A nested IRQ spin wait must restore the outer observation. Early-callback, short-hold and lost-restoration controls validate the protocol on both architectures; cleanup and masked-IRQ guard probes cover failure paths. Arbitrary stopped owners remain outside the bound. |
| ~~Controlled contended plain-spin and irqsave acquisition measurements~~ | **BUILT (PR #308)**: a pinned remote waiter must fail an exchange before the owner holds another 1 ms and releases. Two warmups and nine samples per path verify exclusion, ownership, protected-data handoff and IRQ/preemption state. Matched debug LOCKDEP=0/1 boots cover both architectures; UP explicitly skips. These are total acquisition times under QEMU, including observer cost. Broader workloads and native-hardware bounds remain open. |

Still open for the next milestone:

- General NMI/#MC lockdep writer reentrancy and cross-CPU wait cycles;
  same-CPU raw-lock re-entry now fails stop. Reader tests and the deliberate
  writer-failure probe do not permit arbitrary tracked acquisitions or
  establish AArch64 NMI delivery.
- Simultaneous global held-state snapshots across CPUs and threads;
  individual CPU/thread stacks and counter snapshots are consistent separately.
- ~~Callback wait dependencies beyond observed active timer callback paths.~~
  *2026-10-05: timer callback waits are in the lock graph (callback classes,
  PR #312, [report](2026-10-05-lockdep-callback-classes-report.md)).*
  **Built 2026-10-07**: completion waits are in the graph through completion
  classes (L21), and the unexecuted paths are a listing the runner prints
  after every debug suite; see the
  [completion-waits report](2026-10-07-lockdep-completion-waits-report.md)
  and §7.3.
- ~~Raw `arch_irq_restore` ownership and pairing.~~ **Built 2026-10-06**: per-context
  save stacks with restore and thread-exit checks under lockdep; see the
  [raw-pairing report](2026-10-06-lockdep-irq-pairing-report.md).
- Worst-case wall-clock search latency, first-acquisition graph-size sweeps,
  broader spin/mutex contention workloads, priority-inheritance waits, and native-hardware
  lockdep overhead measurements.
- Kernel interrupt-entry and callback concurrency validation beyond the
  host graph/held-stack and interrupt-publication tests, kernel writer/IPI
  and unregistered-delivery regressions, and bounded x86 NMI reader test.
- ~~Two-CPU full-suite validation: the VirtIO removal overlap needs a third
  CPU with its current placement helper; `sched-spread` and
  `sched-balance-pair` failures observed on two CPUs remain unattributed.
  See the [October 4 report](2026-10-04-spin-contention-report.md).~~
  **Resolved 2026-10-05:** all three were test defects (no kernel change);
  the full suite now passes at one to four CPUs on both architectures. See
  the [two-CPU validation report](2026-10-05-two-cpu-validation-report.md).

### 7.1 Flake triage follow-ups (2026-10-06)

Report: [`2026-10-06-flake-triage-report.md`](2026-10-06-flake-triage-report.md).

| Item | Outcome |
|---|---|
| ~~`net-nicbench` over its budget (three sightings; 20 s budget from PR #307)~~ | **DIAGNOSED and RESTATED**: the UDP rate is the host's per-datagram `sendto` through QEMU's user-mode network, which ARP never pays; a host state, on for ten consecutive boots of unchanged `main` and then off. The phase is a 500 ms window with a per-send histogram, the driver's share, ring occupancy and the sender's switches; the budget is the default 8 s again. The host's trigger for the slow mode is **not attributed**. |
| ~~`timer-cancel-sync`'s lower bound measured from the test thread's return (two sightings)~~ | **FIXED**: `irq-sync` and `timer-cancel-sync` measure from the callback's own entry stamp; `tools/sync-lower-bound-probe.py` fails the old measurement and passes the new on both architectures. |
| ~~TLB shootdown `acknowledged by N of M` with no word on which CPU (three sightings)~~ | **BUILT** (x86-64; AArch64 has no wait): a per-CPU acknowledgement mask; at the unchanged deadline each silent CPU's thread, `irq_depth`, `preempt_count`, last tick, NMI sample and held locks are printed and the panic names it. `tools/tlb-shootdown-diag-probe.py` forces a CPU to hold its acknowledgement. |

### 7.2 Neighbour table keying (2026-10-07)

Report: [`2026-10-07-neighbour-per-interface-report.md`](2026-10-07-neighbour-per-interface-report.md).

| Item | Outcome |
|---|---|
| ~~ARP table and ND cache keyed by address alone: one interface's resolution found another's entry (no request of its own; retries on the other interface; the packet out of the wrong interface to the wrong MAC); `arp_lookup` took no interface~~ | **FIXED**: entries are (interface, address) in both tables (N25); `arp_lookup`/`arp_delete` name the interface; down flushes an interface's entries as removal does; `net-arp-per-interface`, `net-nd-per-interface`, `tools/arp-per-interface-probe.py --old`. |
| ~~`net-nicbench` reported `udp not measured` on an unresolved gateway (PR #318)~~ | **TIGHTENED**: a failure again, the entry being this interface's own; the line carries the request count that separates a lost reply from an entry already present. The 2026-10-06 sightings remain unattributed between the two. |
| `ipv4_route`'s tie for two up interfaces on one subnet (first registered wins) | Recorded as a policy without a knob; not changed. |
| ~~Two windows in "a down interface holds no entries": the input paths read `NETIF_UP` before the table lock, the resolve paths not at all~~ | **CLOSED (follow-up to PR #319)**: the flag is read under the table lock in input and resolve, a resolve on a down interface returns `-ENETUNREACH` with the packet counted dropped, every flag writer is a release store; `net-neigh-down-race` and `tools/neigh-down-race-probe.py --old <race>`, one mode per race (report §8). |

### 7.3 Completion waits and the coverage listing (2026-10-07)

Report: [`2026-10-07-lockdep-completion-waits-report.md`](2026-10-07-lockdep-completion-waits-report.md).

| Item | Outcome |
|---|---|
| ~~`wait_for_completion` on a completion another context signals was outside the lock graph~~ | **BUILT**: one lockdep class per completion name, keyed through the completion's own spinlock class (no new field; module ABI v4 unchanged); the wait records held mutex → completion, a thread-context `complete()` records completion → held mutex, deferred until the thread's next event and discarded when that is its own wait for the same object; `LOCKDEP_R_COMPLETION` from whichever side closes a cycle; interrupt and callback signallers record nothing by the `might_sleep` argument. `lockdep-completion`, `tools/lockdep-completion-probe.py`. The tree reported no violation. |
| ~~Callback paths never executed contributed no edges, invisibly~~ | **MEASURED**: `timer_setup` records every callback function and `run_expired` marks it run; completion classes record waits and signals; the self-test runner prints both listings after the suite. One timer callback of the debug suite never runs (`delack_timer`); one production completion class is polled rather than waited for (`xhci-first-scan`), and the NVMe no-vector fallback is a polled path inside an exercised class. Judged in the report; the tests that would drive them are a plan item. |
| ~~`LOCKDEP_MAX_CLASSES` at 384 with a debug boot at 382~~ | **RAISED** to 512 (the graph 512 KiB in `LOCKDEP=1` builds); the host bounds follow the macros. |
| A completion signaller that takes and releases a mutex before `complete()` | Recorded, not modelled: no production instance (the survey names every signaller's locks); crossrelease's history caught it at the price of its false positives. |
| A worker thread's barrier depends on every item ahead of it | Recorded: the network worker's items take spinlocks only; the mechanism if that changes is the callback class applied to the worker. |

### 7.4 The completion-waits tidy-up (2026-10-07)

Report: [`2026-10-07-completion-waits-tidy-up-report.md`](2026-10-07-completion-waits-tidy-up-report.md).

| Item | Outcome |
|---|---|
| ~~`delack_timer` never ran in a boot~~ | **FOUND AND FIXED**: the delayed acknowledgement could not fire -- the receive path's output after every segment sent the owed ACK at once and cancelled the timer it had just armed. `ack_now` separates "wanted now" from "owed"; `net-tcp-delack` sees the odd segment acknowledged from the timer; the listing reports 0 callbacks never run. |
| ~~`xhci-first-scan` polled under `g_controllers_lock`; the NVMe admin fallback polled~~ | **FIXED**: both wait (`wait_for_completion_timeout`); `FI_NVME_ADMIN_POLL` forces the NVMe path and `nvme-admin-poll` runs it under lockdep. |
| ~~`fw.c`, `ipv4.c`, `nat.c` read `nif->flags` once per check~~ | **FIXED (N26)**: one reading per interface per packet, passed down; `net-netif-flags`, `tools/netif-flags-probe.py --old`. |
| ~~The class table's headroom was invisible until a boot overflowed it~~ | **BUILT**: the post-suite dump prints the peak against `LOCKDEP_MAX_CLASSES` and warns at 90%. |
| A completion signaller blocked in another wait before its `complete()` | Recorded in the design document and the report: the classes are acquired and never held, so the chain through a second wait is not detected; no production instance closes it. |

### 7.5 The delayed acknowledgement against a Nagle peer (2026-10-07)

Report: [`2026-10-07-delack-nagle-report.md`](2026-10-07-delack-nagle-report.md).

| Item | Outcome |
|---|---|
| ~~Does the delayed ACK that now fires (§7.4) cost a Nagle peer doing write-write-read up to 40 ms a request?~~ | **MEASURED, CLOSED**: no shape in which the application reads pays the timer -- the owed acknowledgement leaves with the read, unconditionally (N27), where Linux's `tcp_cleanup_rbuf` withholds it in pingpong mode and so has the stall itself. `net-tcp-nagle-peer` builds the Nagle peer on loopback (a reading server and a 5 ms-busy one; medians bounded at 20 ms); the harness runs a write-write-read exchange both ways on every boot (`NODELAY` median bounded at 25 ms); `tools/delack-nagle-probe.py --old` boots the pre-b76e4536 rule. QEMU's user-mode backend has no Nagle toward the guest (libslirp `tcp_output.c`), so the harness's Nagle batches measure the host kernel and are reported, not bounded. |

### 7.6 epoll interest removal on the final close (2026-10-07)

Report: [`2026-10-07-epoll-close-report.md`](2026-10-07-epoll-close-report.md).

| Item | Outcome |
|---|---|
| ~~A registered descriptor closed without `EPOLL_CTL_DEL` kept its object alive and evaluated (epoll v1's one deviation): a closed TCP socket in a set never sent its FIN~~ | **FIXED (A9)**: `kobject.handles` counts an object's handle-table slots across every process; the close that empties the last one calls `epoll_last_handle_closed`, which removes the object's registrations from every set (items linked on the object's `watchers` list under one global mutex, taken outside `ep->lock`). `dup`/inheritance keep the registration; an add after the last close is refused; a set released with entries unlinks them first; a waiter asleep on the member is woken so the release follows the close. `epoll-close`, `lxtest` (`LXEPOLLCLOSE`), `epoll_musl`, `tools/epoll-close-probe.py --old`. |
| ~~Nested epoll (`-ELOOP` loop detection, depth limit)~~ | **BUILT by §7.7** (the epoll-callback unit): readiness by callback, nesting with `-ELOOP`, a chain of four sets. |
| A handle in flight in an unread unix message | Recorded as a gap: not a descriptor for A9, so an object whose only remaining reference rides in a message loses its registrations at its last slot's close. |

### 7.7 epoll readiness by callback, and nesting (2026-10-07)

Report: [`2026-10-07-epoll-callback-report.md`](2026-10-07-epoll-callback-report.md).

| Item | Outcome |
|---|---|
| ~~Every `epoll_wait` snapshotted and pinned every member and parked a wait entry on each member's queue: O(registered) per call~~ | **FIXED (A10)**: `struct wait_entry` gained a callback kind; every item owns one on each queue its member's directions wake, for the item's life; a wake links the item onto the set's ready list under `ep->rlock` and wakes the set's queue; a wait sleeps on that queue alone and walks the ready list. Level items are re-queued after a report, edge and one-shot items are not; a wake during the walk is kept. `epoll-scale` bounds the 1024-member figures against the 1-member ones; the report has before and after. |
| ~~A set could not be a member of a set~~ | **BUILT (A11)**: the outer's item hooks the inner's queue; forwarded wakes take the outer's two spinlocks with a lockdep subclass equal to the chain depth (`nests`, Linux's `ep_poll_safewake`); self `-EINVAL`, a loop or a chain longer than `EPOLL_MAX_NESTS` `-ELOOP`, decided under the watch lock (four sets at first; five, Linux's limit, since the follow-up PR -- the Depth row below). `epoll-nest`, `LXEPOLLNEST`, `epoll_musl`; `tools/epoll-callback-probe.py --old no-loop-check`. |
| ~~The last close of every object took the global epoll watch lock~~ | **FIXED**: `kobject.watched`, set under the lock before the add's handle-count check, read after the decrement; the two fences make it a store-buffering pair proved by `tests/litmus/epoll/watched.litmus` (both controls flip). `epoll-close-bench` has the closes per second (the last close of a never-registered object, a non-last close, the removal path). |
| A callback in flight when its item is removed | **Closed**: the entries leave their queues under those queues' locks before the item is freed; a queue not owned by the member (a process's signalfd queue) detaches its entries and waits a grace period before it is freed (`waitqueue_detach_callbacks`, Linux's POLLFREE); `epoll-wake-race`; `tools/epoll-callback-probe.py --old no-unhook`. |
| `init --block` read the console: a key the HID harness typed during `process-spawn`'s 50 ms ended it (the "known `--block` flake") | **FIXED**: it reads a pipe of its own. Found when this unit's tests moved `process-spawn` into the typing window. |
| Depth | ~~A chain of four sets, not Linux's five: lockdep's four subclasses size its order graph.~~ **Five since the follow-up PR** (Linux's documented limit); lockdep has five subclasses (800 KiB of order graph, 512 before). A capped subclass was rejected: it would report a false recursion at the fifth level. |
| `wake_one` walked the whole list for callback entries, so a contended mutex unlock grew with the waiters | **FIXED in the follow-up PR**: callback entries have a list of their own; `wake_one` runs it whole and stops at its one thread. `mutex-wake-bench`, `tools/wake-one-probe.py`. |

### 7.8 Network packet-parser fuzzing (2026-10-07)

Plan §12; `docs/audit/2026-10-07-net-fuzz-report.md`. Three host fuzz targets
over the real network sources and `tests/fuzz/shim_net.c`.

| Follow-up | Status |
|---|---|
| ~~A peer's FIN that acknowledges the last segment in flight, arriving after the host closed with window-held data unsent, left the connection in CLOSING for ever (pcb, 128 KiB of buffers and the table slot leaked; the peer never gets a FIN)~~ | **FIXED**: the FIN branch of `tcp_input` runs the output; `net-fin-acks-last-data`; `tools/net-fuzz-probe.py --old fin-output`. |
| The forwarder masquerades and relays a guest's UDP datagram whose length field disagrees with the IP length (Linux's conntrack marks it INVALID and skips NAT) | Recorded (report §5): no memory is read by the field, the private source is rewritten; a policy difference, not a defect. |
| Forwarding re-stamps the IP id and drops IP options (`output_on` builds a fresh header) | Recorded; fragments are refused on input, so nothing depends on the id. |
| ~~`tcp_close` on an ownerless pcb the network already ended puts it twice~~ | **MADE DEFENSIVE (the net-fuzz-oracles unit, 2026-10-08)**: `pcb_kill_locked` runs once; still unreachable from the socket layer; `fuzz_tcp_segments` seed 16. |
| Not fuzzable on the host: virtio-net and e1000e receive descriptors (a device model's shape), the tap device file, `netif.c`'s worker and steering, anything two CPUs race over | Recorded in the plan (§12) and the network testing notes. |
| ~~Where coverage stopped (report §5): the IPv6 output path, `udp.c`'s send side, the configuration-driven half of `fw.c` and `nat.c`~~ | **BUILT (the net-fuzz-oracles unit, 2026-10-08, `docs/audit/2026-10-08-net-fuzz-oracles-report.md`)**: IPv6 sockets and UDP sends in `fuzz_net_frame`, the new `fuzz_net_config`; found and fixed `nat_pf_clear` keeping its rules' translations (`net-pf-clear`) and a tap's release purging its guest before the tap was gone (`net-tap-release-order`). |

## 8. Device lifecycle Unit 1 follow-ups (2026-10-08)

These are source-audit suspicions and missing tests, **not reproduced
defects**. No other driver was changed. See the path-by-path table in
[the removal report](2026-10-08-vnet-remove-report.md#audit-of-other-drivers).
Prove each concern with a deterministic interleaving or fault before fixing.

| Follow-up | Evidence and proof still needed |
|---|---|
| ~~virtio-rng completion after reset~~ | **PROVEN AND FIXED (device-lifecycle Unit 2, 2026-10-09)**: `vrng_completed` after the reset boundary reposted once; `tools/virtio-rng-repost-probe.py --old` failed only the proof marker on x86-64 and AArch64. `vrng_post` now closes publication under `post_lock` before reset. See the [Unit 2 report](2026-10-09-device-lifecycle-unit2-report.md). |
| Timeout versus late block submission | NVMe checks `c->dead` before mapping, not again under the queue lock; `controller_die` drains while tables/polls remain. Virtio-blk timeout also needs an accepted-submit/locked-drain interleaving check. Park a submit between its dead check and publication; prove whether block-layer gating already excludes it. |
| Failed hardware stop acknowledgements | **PARTLY CLOSED (device-lifecycle Unit 2, 2026-10-09):** NVMe disable failure is proven; fixed boots retain SQ/CQ/PRP DMA, the active mapping and bio, while `tools/nvme-disable-ack-probe.py --old` fails only the proof on x86-64 and AArch64. AHCI remove's port-stop failure is proven by `tools/ahci-stop-ack-probe.py --old` on both architectures (`dma_frees=12`); the fix retries once and retains controller/port DMA, mappings and bios on persistent failure. Still open: AHCI COMRESET failure and other stop/recovery call sites, xHCI halt/Stop Endpoint/Disable Slot, and e1000e RX/TX disable. See the [Unit 2 report](2026-10-09-device-lifecycle-unit2-report.md). |
| ~~AHCI probe rollback before IRQ synchronization~~ | **REFUTED (device-lifecycle Unit 2, 2026-10-09)**: the deterministic failure after ports start latched `AHCI_IS=0x3f` while `GHC.IE=0`; no handler ran and no disk or command had been published on either architecture. `fail_ports` is reachable before worker creation, global interrupt enable, and disk probing. See the [Unit 2 report](2026-10-09-device-lifecycle-unit2-report.md). |
| xHCI endpoint/slot stop failure and callback retirement | Cancel flushes after stop/dequeue error; disable-device ignores stop/Disable Slot errors then frees endpoint resources. An already-retired callback runs outside `x->lock`; cancel's already-gone branch joins IRQ and current poll. Inject command failures and park that retired callback through disconnect before claiming a defect. |
| Network active-worker removal gap | **REFUTED (device-lifecycle Unit 2, 2026-10-09):** `netif-remove-worker` parks an already-dequeued `input_one` after the quiesce read section, confirms unregister reached its barrier after purge, and observes that unregister stays blocked until release. Both x86-64 and AArch64 passed 455 self-tests plus the network harness (100/100 rounds). See the [Unit 2 report](2026-10-09-device-lifecycle-unit2-report.md). |
| USB storage reset/cancel failure propagation | `usbs_remove` ignores `usb_cancel` result; timeout records reset result but clears recovering/broken afterwards. Prove state/ownership under failed HCD cancellation and BOT recovery. |
| virtio-net probe failure injection and active worker wait | The synthetic peer proves held completions at reset and tracks every outstanding RX/map/chain segment. Add actual probe-allocation/registration faults and a callback held on another CPU to prove waiting, beyond deterministic reset-time delivery. |
| virtio-net receive before interface registration | `vnet_probe` sets DRIVER_OK/posts RX before filling the interface operations/name/MTU and calling `netif_register`. `rx_common` admits any non-GONE interface; the failed-registration path has no RX-worker purge/barrier. Inject a frame before registration plus registration failure and establish whether an escaped `rcvif` outlives the private object. This ordering is unchanged in Unit 1; no such interleaving was reproduced. |
| `waitpid(WCONTINUED)` after a quick child exit | During the AArch64 Unit 2 old-behavior probe on 2026-10-09, `signal-stop` check 9 failed: the child exited with status 7 before the parent returned status 9. The child continues and exits immediately; `child_event_locked` tests `EXITED` before `CONTINUED`, so the continued event may be hidden. Recorded in `docs/testing/flakes.md`; outside Unit 2 and awaiting scope decision. |
| module-unload timeout assertion compares different clocks | During the x86-64 AHCI stop-fix boot on 2026-10-09, `module-unload-busy` failed `waited >= 50000000ULL`; four following zombie tests cascaded. The timeout uses quantized global ticks when `clock_is_common()` is false, while the test measures with `clock_since_ns()`. Recorded in `docs/testing/flakes.md`; outside Unit 2 and awaiting scope decision. |
| ~~virtio-net TX ownership lookup cost~~ | **ADDRESSED during PR #336 review:** a private free list supplies ownership records; the queue cookie points directly to the record. Submission, completion and failed-publication rollback take constant time without a transport API or mbuf layout change. The synthetic peer checks exhaustion, reverse-order completion and reuse. No throughput improvement is claimed without matched-LOCKDEP benchmarks. |

## 9. Analyzer investigation follow-ups (2026-10-09)

See [the analyzer report](2026-10-09-analyzer-gate-report.md). These two
source-established self-test cleanup defects were discovered outside the
current diagnostic list. The user authorized deterministic proofs and separate repairs in PR A
on 2026-10-09. Results are recorded in that report as the probes run.

| Finding | Evidence and work remaining |
|---|---|
| `cosmofstest.c:engine_mount` leaks a registered RAM device on setup failure | **Proven and fixed in `abc8bc63`.** On both architectures, `tools/cleanup-path-probe.py --old` failed at format, mkdir and mount injection with `released=0`; fixed mode reported `released=1` at all stages. The test checks `gone` and the creator-reference retirement while holding its own inspection reference. |
| `devtest.c:selftest_nvme` leaks a worker buffer when thread creation fails | **Proven and fixed in `80fb43db`.** On both architectures, `--old` reported `allocated=1 hits=1 started=0 released=0`; fixed mode reported `released=1`. `thread_create_on` does not consume the entry argument on failure. |
| Analyzer gate and Oct. 3 AArch64 count delta | **Gate implemented on the PR A branch.** Current full Clang 21.0.3 runs leave eight reasoned baseline entries per architecture (19/27 removed on x86-64; 20/28 on AArch64). The Oct. 3 source snapshot analyzed with today's compiler emits 28 AArch64 diagnostics, including the ten items in the reported increase; the old compiler identity and plist reports are unavailable, so the 19-to-28 count change cannot be attributed to source or toolchain. See the dated [analyzer report](2026-10-09-analyzer-gate-report.md), including the full historical list, current dispositions, and gate probe. |
