# Remaining milestone work

Status snapshot: 2026-10-05, after merging [PR #308](https://github.com/vkuttyp/CosmoOS/pull/308);
§2's two-CPU items updated by the two-CPU validation increment the same day.

This plan consolidates the remaining work from the
[deferred-work inventory](audit/2026-09-deferred-work-inventory.md), the
[post-roadmap audit](audit/2026-09-post-roadmap-audit.md), and subsequent
subsystem reports. The inventory's §5 records the original ten roadmap
milestones as built. The entries below are their follow-ups and later
capabilities, not a proposal to rebuild those milestones.

The spin-contention continuation merged with PR #308 (`24ed5c45`). Its
implementation commit is `95dc0286`; `e324a62b` fixes the startup dependency
that failed the first remote x86 boot (run 37186753332). Remote CI run
37187562158 passed on x86-64 and AArch64. Evidence is in the
[October 4 report](audit/2026-10-04-spin-contention-report.md).

## How to use this plan

- **Integration:** implementation exists, but review, CI or merge remains.
- **Implementation:** a documented capability or extension remains open.
- **Validation:** evidence, diagnosis or coverage remains incomplete; this
  does not by itself establish a production defect.
- **Conditional/deferred:** intentionally postponed or dependent on a
  demonstrated need, architectural decision or suitable hardware.

Unchecked items are outstanding within their stated scope. They are not
all commitments for the current lockdep milestone. The source documents
contain historical descriptions as well as current status; inspect the
current implementation before starting any item. In particular, an older
claim that memfd, edge-triggered epoll, shared futexes, priority inheritance,
runtime VirtIO removal or device reset is missing has been superseded.

For each increment, record its scope and baseline, implement and validate
the smallest useful change, and update this plan and the inventory with
the completion evidence and PR. Keep failures and limitations explicit.

## 1. Lock discipline and concurrency hardening

Sources: [inventory §7](audit/2026-09-deferred-work-inventory.md#7-lockdep-milestone-follow-ups-2026-10-03),
[lockdep design](kernel/lockdep/design.md),
[lockdep testing](kernel/lockdep/testing.md), and the
[spin-contention report](audit/2026-10-04-spin-contention-report.md).

- [x] **Integration — spin-contention increment.** Merged with PR #308 after
  remote CI run 37187562158 passed on both architectures. The increment
  observes failed spin exchanges, checks nested IRQ wait restoration, and
  measures plain and irqsave acquisition handoff. Its CI follow-up makes
  both contention protocols rendezvous with IRQs enabled before either
  participant masks them; no shootdown or test budget changed. See the
  [PR #308 follow-up](audit/2026-10-04-spin-contention-report.md#pr-308-ci-follow-up-runnable-is-not-ready).
- [ ] **Implementation/validation — NMI/#MC writers and cross-CPU waits.**
  Establish safe validator behavior for general NMI/machine-check writer
  entry and cross-CPU raw-lock wait cycles. Existing same-CPU re-entry
  fail-stop behavior and bounded NMI reader tests cover narrower contracts.
- [ ] **Implementation — global held-state snapshots.** Provide a defined
  consistency model across CPU spinlock stacks and thread mutex stacks;
  separately consistent snapshots do not form one simultaneous global view.
- [ ] **Implementation — callback-wait dependencies.** Extend coverage beyond
  locks learned from observed active timer callbacks, including other
  synchronous callback waits and paths not yet observed.
- [ ] **Implementation/validation — raw IRQ pairing.** Track or validate
  ownership and pairing of raw `arch_irq_save`/`arch_irq_restore` operations
  beyond the checks already applied to tracked spinlock wrappers.
- [ ] **Validation — interrupt and callback interleavings.** Broaden actual
  kernel entry/concurrency coverage beyond host graph models, publication
  tests, writer/IPI regressions and bounded x86 NMI reader tests.
- [ ] **Validation — performance and scaling.** Measure broader spin/mutex
  contention, priority-inheritance waits, first-acquisition graph-size
  sweeps, worst-case wall-clock search latency and native-hardware costs.
  Current controlled acquisition times include owner hold and observer cost.
- [ ] **Conditional/deferred — stopped-owner spin waits.** Decide whether a
  separately scoped diagnostic or recovery contract is needed. Test-side
  guards do not bound a primitive spin wait when its owner stops progressing.

## 2. Immediate suite failures and timing investigations

Sources: [October 4 validation report](audit/2026-10-04-spin-contention-report.md),
[flake history](testing/flakes.md), and inventory §§1.3, 3 and 7.

- [x] **Validation/fix — two-CPU VirtIO removal test.** Its placement helper
  assigns the nonpreemptible read-side holder and remover to the same CPU
  when there is no third CPU, preventing the required overlap. Correct the
  test arrangement or explicitly define its supported CPU-count premise.
  *Completed 2026-10-05 — test bug, two layers; two CPUs suffice.* The
  removal now runs on the test thread with the holder on another CPU; a
  submitter preempted inside `blk_submit` behind the holder also kept
  `blk_unregister`'s (correct) drain waiting, so the submitter is parked
  first. Five stamps prove the overlap. No kernel change. See the [two-CPU report](audit/2026-10-05-two-cpu-validation-report.md), §§5–6.
- [x] **Validation — two-CPU scheduler failures.** Diagnose `sched-spread`
  placing all eight workers on one CPU and the observed `sched-balance-pair`
  failure. The spin increment did not establish their cause or compare them
  against a baseline two-CPU boot.
  *Completed 2026-10-05 — both test bugs; no scheduler defect.* Unchanged
  `main` also failed `sched-spread` at three CPUs: its bound predates S29,
  under which the running creator's CPU never ties. Two CPUs have no tie to
  observe, so the test skips there with that reason. `sched-balance-pair`'s
  2 ms poll expired on every tick before `balance_tick` on the only
  receiving CPU, so it never scanned as idle (x86-64 too). See the [two-CPU report](audit/2026-10-05-two-cpu-validation-report.md), §§7–10.
- [ ] **Validation — syscall-fuzz duration excursions.** Investigate the
  recorded AArch64 budget failures, including 8,240 ms against 8,000 ms during
  the spin increment. An unchanged-image retry passed; that does not explain
  the slowdown or justify weakening the assertions.
  *2026-10-05: not reproduced* — 3,545–3,921 ms in 35 AArch64 boots at one to
  four CPUs, boots run one at a time; cause still unestablished ([two-CPU report](audit/2026-10-05-two-cpu-validation-report.md), §13).
- [ ] **Validation — other timing assumptions.** Revisit the remaining
  `thrtest`/`cwdtest` assumptions and the rare long `net-bench` run with
  evidence that distinguishes scheduling delay from a mechanism failure.
  See the [suite-waits report](audit/next-subsystem-suite-waits.md).
  *2026-10-05:* `thrtest`'s counts are workload sizes with clock-deadline
  waits; `cwdtest --held` still bounds seam waits by 200,000 yields (a count
  standing in for a duration), not failing in 68 boots; `net-bench` took
  1.2–3.5 s throughout ([two-CPU report](audit/2026-10-05-two-cpu-validation-report.md), §13).
- [ ] **Validation — slirp connection resets.** The guest-side network test
  now makes up to three attempts and the host harness records each exchange, but the specific slirp defect remains unidentified.
  Continue with host-loopback capture or slirp source investigation; do not
  relabel the existing diagnosis as a kernel send-path defect. See the
  [network retry report](audit/next-subsystem-nettest-retry.md).
- [ ] **Validation — AArch64 virtio-console output loss.** Explain the
  historical missing final console line while the serial log was complete.
  This remains an unexplained observation, not a demonstrated root cause.
  *2026-10-05: not reproduced* in 35 AArch64 captures ([two-CPU report](audit/2026-10-05-two-cpu-validation-report.md), §13).

## 3. Lifetime and quiescence

Sources: [inventory §4](audit/2026-09-deferred-work-inventory.md#4-the-quiesce-reports-remaining-risks-and-debt),
[lifetime report](audit/2026-09-lifetime-quiesce-report.md), and
[quiescence design](kernel/quiesce/design.md).

- [ ] **Validation — memory ordering.** Add suitable TSan host models and
  memory-order litmus tests for the lifetime protocol itself. The existing
  threaded epoch-core host test runs under ASan/UBSan only, and the lockdep
  graph/held-stack host models do not close this gap.
- [ ] **Validation — scaling and long disabled regions.** Measure at higher
  CPU counts and identify long preemption-disabled sections, including VM
  population loops. Recheck what later VMM work already shortened before
  proposing preemption points. Counts beyond 64 require separate CPU-mask work.
- [ ] **Validation — straggler-kick attribution.** Widen the sample and
  identify which execution contexts benefit from kick IPIs; per-CPU counts of
  kick-driven publications already exist. Preserve the
  distinction between an IPI sent and a publication that advances the epoch.
  See the [straggler-kick report](audit/next-subsystem-straggler-kick.md).
- [ ] **Conditional/deferred — further grace-period latency reduction.**
  The polling delay was removed; the remaining idle-CPU tick floor is a
  different problem. Any further change needs measurements and a compatible
  quiescent-point design. See the [wake report](audit/next-subsystem-quiesce-wake.md).

## 4. Scheduler, synchronization and SMP

Sources: inventory §§2.3 and 3, [scheduler design](kernel/scheduler/design.md),
and [SMP design](kernel/smp/design.md).

- [ ] **Conditional/deferred — additional scheduling policies.** Define
  requirements for fairness, real-time, deadline and interactive policies,
  plus CPU isolation, beyond the existing fixed-priority round-robin policy.
  Its 64 priority levels have no user-facing priority or policy syscalls.
- [ ] **Conditional/deferred — placement and idle-balance policy quality.**
  Two policy observations from the [two-CPU report](audit/2026-10-05-two-cpu-validation-report.md) (§§8–9), within documented rules:
  `pick_cpu`'s tie rotation favours the CPU after a busy creator's (2:1 at
  three CPUs), and a CPU whose own sleeper expires on every tick never
  balances as idle. Change only with workload measurements.
- [ ] **Validation/implementation — user-mode preempted-thread migration.**
  Assess relaxing the restriction for threads preempted in user mode, where
  interrupted kernel per-CPU accesses are absent. Existing measurements did
  not establish a persistently stranded user-thread pair. See the
  [balance-movable report](audit/next-subsystem-balance-movable.md).
- [ ] **Conditional/deferred — kernel rwlocks.** Introduce a reader/writer
  primitive only with a concrete workload, ownership rules and lockdep model.
- [ ] **Implementation — quiescent lookup structures.** Assess extending the
  existing epoch abstraction to routing/protocol lookups where the original
  architecture requested it; lifetime use alone does not implement those paths.
- [ ] **Conditional/deferred — CPU scalability.** Address the 64-bit CPU mask
  ceiling, x2APIC, the single cross-call slot and broader IRQ affinity;
  evaluate ticket/MCS locks and parallel AP bring-up only with measurements.

Priority inheritance, migration and a pull balancer already exist. The open
PI item here is measurement in §1, not implementation of donation itself.
See the [priority-inheritance report](audit/next-subsystem-priority-inheritance.md).

## 5. Memory and native thread setup

Sources: inventory §§1.2 and 2.2, [memory design](kernel/memory/design.md),
and the [mprotect report](audit/next-subsystem-mprotect.md).

- [ ] **Implementation — ASLR/KASLR.** Define randomized executable, stack,
  heap and kernel placement together with an adequate entropy strategy.
- [ ] **Conditional/deferred — user huge pages.** Extend mapping, fault,
  protection and teardown semantics with tests for mixed page sizes.
- [ ] **Conditional/deferred — NUMA.** Add topology discovery, node-aware
  allocation and placement; the present single-node abstraction is a foundation.
- [ ] **Conditional/deferred — swap, compression and deduplication.** Treat
  these as separate memory-management projects with reclaim, lifetime and
  accounting contracts, rather than flags on current allocation paths.
- [ ] **Conditional/deferred — thread guard-page setup.** Consider mapping
  read/write and protecting the guard page instead of reserve-and-replace.
  The current sequence is tested; changing a cold path needs its own benefit.

File-backed shared mappings, shared futexes, shared ELF text, memfd and
System V shared memory are built. Their remaining compatibility extensions
are listed in §8 rather than being described as absent shared-memory support.

## 6. VFS and storage

Sources: inventory §§2.4 and 3 and the
[post-roadmap audit](audit/2026-09-post-roadmap-audit.md), §8.

- [ ] **Implementation — VFS lookup and identity.** Add a name (dentry) cache
  and a defined inode/generation identity where needed, with invalidation and
  lifetime rules. A per-mount vnode cache keyed by inode number exists, but
  every lookup still calls the filesystem.
- [ ] **Implementation — VFS namespace features.** Add hard links, mount
  options beyond read-only, bind mounts and overlay stacking as independently
  validated units.
- [ ] **Implementation — snapshot derivatives.** Extend existing snapshots
  to writable clones, rollback and boot environments with explicit crash semantics.
- [ ] **Implementation — incremental send/receive.** Define the stream format,
  ancestry validation and interruption/recovery behavior for snapshot transfer.
- [ ] **Implementation — pool layouts.** Add fixed-width striping and
  RAID-Z-like parity with failure, replay and repair coverage beyond existing
  mirrors. Multi-member pools already spread allocations to the emptiest member.
- [ ] **Implementation — pool maintenance.** Support device replacement,
  resilvering and hot spares; a member that missed commits currently needs a
  mechanism to catch up before rejoining the mirror.
- [ ] **Conditional/deferred — storage deduplication and metadata authentication.**
  Evaluate their separate space, integrity, key-management and recovery costs.
  Existing checksums and encryption do not establish authenticated metadata.

## 7. Networking

Sources: inventory §§1.1, 1.4 and 2.5 and
[network design](kernel-services/network/design.md).

- [ ] **Implementation — filter scope and actions.** Add per-interface host
  chains, rate-limit/logging targets, IPv6 filtering and fuller TCP state tracking.
- [ ] **Implementation — translation and guest connectivity.** Add IPv6 NAT
  and DNAT, hairpin/NAT reflection, an L2 bridge, and guest limits beyond NAT
  quotas. IPv4 masquerade and port-forward DNAT, with control-channel rules,
  already exist.
- [ ] **Implementation — runtime controls.** Expose the remaining forwarding,
  masquerade, resolver and tap up/down settings through the control channel.
- [ ] **Implementation — DHCP/DNS extensions.** Evaluate a general DHCP server,
  caching resolver, DHCPv6, DNS-over-TCP and DNSSEC beyond the present proxy service.
- [ ] **Implementation — TCP/IP extensions.** Add window scaling, SACK,
  timestamps, ECN, fast recovery and Nagle behavior; three-duplicate-ACK fast
  retransmit already exists. Implement IP fragmentation and reassembly with
  resource bounds and hostile-input tests; IPv4 fragments are currently dropped.
- [ ] **Implementation/validation — IPv6 beyond the local link.** Implement
  global routing (the route lookup returns none beyond loopback, link-local and
  multicast) and router discovery, then exercise neighbor discovery against a
  real peer. Link-local neighbor discovery exists.
- [ ] **Conditional/deferred — faster data paths.** Measure the case for
  zero-copy buffers, device multiqueue (software flow steering to per-CPU
  receive queues exists), TSO/LRO, jumbo frames, NAPI-style
  polling, interrupt moderation and busy polling. QEMU's user-mode backend
  constrains what can be demonstrated; complexity must earn its place.

## 8. Linux compatibility and event/IPC objects

Sources: [inventory §2.6](audit/2026-09-deferred-work-inventory.md#26-linux-compatibility-constitution-40-phases-3-4-prompt-2-30)
and [Linux compatibility API](compat/linux/api.md).

- [ ] **Conditional/deferred — execve.** Replacing a process image requires
  an architectural extension to the native spawn model, not just a syscall-table entry.
- [ ] **Implementation — rseq and netlink.** These need, respectively,
  per-thread registration/abort semantics and a new socket-family protocol surface.
- [ ] **Implementation — socket options.** Add backing behavior for supported
  options; preserve explicit errors for options whose semantics remain unimplemented.
- [ ] **Implementation — epoll lifetime and nesting.** Add removal on final
  descriptor close and nested sets with cycle detection. Edge-triggered operation
  is already built. Sources: [epoll](audit/next-subsystem-epoll.md) and
  [EPOLLET](audit/next-subsystem-epollet.md).
- [ ] **Implementation — mremap extensions.** Support relocation and separately
  define fixed, file-backed and sub-range cases beyond whole anonymous
  in-place resizing. See the [mremap report](audit/next-subsystem-mremap.md).
- [ ] **Implementation — memfd extensions.** Add sealing and the associated
  fcntl semantics; huge-page support depends on the memory work in §5.
  See the [memfd report](audit/next-subsystem-memfd.md).
- [ ] **Implementation — System V shared-memory extensions.** Address address
  rounding/remapping, huge pages, IPC_SET, locking/info operations, separate
  owner and creator identities and IPC namespaces. Mode-bit permission checks
  and creator-or-root removal exist. See the
  [shared-memory report](audit/next-subsystem-shm.md), Risks.
- [ ] **Implementation/validation — broader Linux environment.** Extend
  Linux-facing `/proc` and `/sys` behavior and validate a real distribution
  userland. Existing syscall subsets do not establish full Linux compatibility.

## 9. Virtualization

Sources: inventory §§1.2, 1.3 and 2.7 and
[post-roadmap audit §11](audit/2026-09-post-roadmap-audit.md).

- [ ] **Validation — VMX execution.** Run the existing backend on suitable
  Intel hardware or KVM infrastructure; pure host logic tests do not prove VM entry.
- [ ] **Validation — guest concurrency.** Exercise device models with multiple
  guest CPUs using an appropriate guest-side VirtIO driver. Multi-vCPU AArch64
  guests already run under `vmctl run --machine -c`; the VirtIO test guests
  are single-vCPU.
- [ ] **Validation — reproducible Linux guest demonstrations.** Make guest
  image/root-filesystem provisioning reproducible and suitable for CI; demonstrate
  a guest reaching the external network through the host's real NIC.
- [ ] **Implementation — x86 guest fidelity.** Resolve documented interception,
  string-I/O, TSC virtualization, debug-register and remaining CPUID-exposure
  gaps, including WBINVD/RDPMC/RDTSCP behavior. CPUID leaves are already
  filtered; VMX enables MOV-DR and RDPMC exits that it does not yet handle.
- [ ] **Implementation — dirty tracking and ballooning.** Define memory ownership,
  guest coordination and accounting needed for these services.
- [ ] **Conditional/deferred — VM snapshots and live migration.** Extend the
  UAPI to capture/restore complete vCPU and device state before attempting
  snapshot or transfer; vCPU register get/set already exists.
- [ ] **Conditional/deferred — device passthrough.** Establish DMA and interrupt
  isolation, assignment lifetime and reset behavior on appropriate hardware.
- [ ] **Conditional/deferred — nested virtualization.** Requires a separately
  scoped guest/host virtualization model; it is outside the current milestone.

## 10. Security and isolation

Sources: inventory §§2.9 and 3 and [security design](kernel/security/design.md).

- [ ] **Implementation — secure/measured boot.** Establish trust for the kernel
  and boot archive; module signing alone starts from an unverified boot image.
- [ ] **Implementation — audit logging.** Define security events, access controls,
  storage/transport and behavior under resource pressure.
- [ ] **Implementation — stronger isolation/accounting.** Extend capabilities
  beyond handle rights, add network namespaces and cgroups-style accounting
  beyond current rlimits; coordinate IPC namespaces with §8. Mount and UTS
  namespaces and process domains already exist.
- [ ] **Implementation/validation — stack and memory hardening.** Evaluate kernel
  stack protection, broader interrupt-stack isolation and kernel sanitizer builds.
  x86-64 already uses IST stacks for double fault, NMI, machine check and
  debug; AArch64 takes EL1 exceptions on the current stack.
- [ ] **Implementation — entropy sources.** Supplement virtio-rng with supported
  CPU or other sources and define health, seeding and unavailable-entropy behavior.
- [ ] **Implementation/validation — speculative-execution policy.** Document
  supported CPUs, threat assumptions and the mitigations that must be enabled/tested.
- [ ] **Implementation — module anti-rollback.** Add signature versioning and
  policy for rejecting obsolete signed modules without breaking recovery workflows.

## 11. Observability

Sources: inventory §2.8 and [diagnostics design](kernel/diagnostics/design.md).

- [ ] **Implementation — kernel debugging.** Add a defined debugger/GDB remote
  workflow appropriate to supported architectures and execution contexts.
- [ ] **Implementation — crash dumps and symbols.** Preserve useful crash state
  and improve kernel-side symbolization; host postprocessing is a separate capability.
- [ ] **Implementation — tracing and counters.** Build structured per-CPU tracing
  and performance-counter support with bounded overhead and safe diagnostic readers.
- [ ] **Implementation — operator tools.** Provide transports and interfaces for
  `ktrace`, `kstat`, `cosmo-top` and `cosmo-prof`-style inspection beyond existing
  sysctl values and self-test output.
- [ ] **Conditional/deferred — eBPF-like tracing.** Treat program verification,
  execution isolation and attachment lifetime as prerequisites, not just a tracing UI.

## 12. Verification, CI and performance evidence

Sources: inventory §§2.10, 2.11 and 3,
[verification testing](verification/testing.md), and [flake history](testing/flakes.md).

- [ ] **Implementation — missing fuzz/property coverage.** Add network packet
  parser, PCI configuration, ACPI table and generated VFS-path fuzzing, plus
  property-based tests beyond the randomized lockdep graph oracles. Preserve the
  existing parser and syscall fuzzers; the latter draws paths from a fixed list.
- [ ] **Implementation/validation — remaining fault injection.** Cover CPU
  starvation, device-reset faults and VM-exit storms. Generic device reset and
  packet-duplication injection already exist; these are broader failure scenarios.
- [ ] **Implementation — coverage and LTO builds.** Add useful coverage collection
  and assess LTO as a separate configuration with its own build/boot validation.
- [ ] **Validation — CPU and firmware matrices.** Expand routine CPU-count
  coverage, include the two-CPU prerequisites in §2, and test AArch64 firmware
  variants or document a supported minimum EDK2/AAVMF version.
  *2026-10-05:* the two-CPU prerequisites are done; full suites pass locally
  at one to four CPUs on both architectures ([two-CPU report](audit/2026-10-05-two-cpu-validation-report.md)).
  CI now also boots the debug suite with two CPUs on both architectures
  (`make test-smp2`). One- and three-CPU boots remain local-only.
- [ ] **Validation — physical hardware matrix.** Establish repeatable AMD, Intel
  and Apple Silicon testing where supported; state which firmware/device paths
  are actually exercised rather than inferring hardware success from QEMU.
- [ ] **Implementation/validation — broader benchmarks.** Measure allocation,
  page faults, mmap, IPI round trips, fsync, filesystem metadata and VM exits,
  and build baseline-versus-change regression tracking. Existing network, block,
  FPU/context-switch, futex wakeup, IPC round-trip, VFS read/write, balancer
  and lockdep measurements remain useful baselines.

## 13. Devices, architecture and portability

Sources: inventory §§1.4, 2.11 and 3 and [device design](kernel/device/design.md).

- [ ] **Implementation/validation — non-coherent DMA.** Audit device-written
  rings/buffers and submission synchronization in VirtIO, e1000e, AHCI and xHCI.
  Coherent QEMU success cannot prove the required cache-maintenance behavior.
- [ ] **Implementation — hotplug and power management.** Add CPU hotplug, PCI
  rescan and power-management lifecycles beyond existing device-removal support.
- [ ] **Implementation — independent bind/unbind.** Expose per-device operations
  without requiring registration or removal of the whole driver. Per-device
  test bind/unbind hooks exist only in debug builds.
- [ ] **Conditional/deferred — IOMMU variants.** Extend interrupt remapping,
  AMD-Vi, huge pages, IOVA caching, PASID/ATS, larger stream IDs and bridge
  requester aliases only with a target and validation path. Small-output-width
  SMMUs additionally need the appropriate concatenated level-1 root layout.
- [ ] **Implementation/validation — AArch64 portability.** Address host device-tree
  boot support, PSCI variations, UAO/E0PD/BTI/PAC and pseudo-NMI delivery as
  separately scoped architecture work.
- [ ] **Validation/implementation — asynchronous hardware errors.** Use RAS
  records for causal attribution and decide whether EL1 should unmask asynchronous
  aborts. Corrected-error handling is already built; delivery context alone cannot
  identify a guilty process. See the [async-error report](audit/next-subsystem-async-error.md).
- [ ] **Implementation — CPU feature and errata framework.** Centralize supported
  feature discovery and errata policy rather than adding scattered assumptions.
- [ ] **Conditional/deferred — display and wireless hardware.** Add a mode-setting
  display driver, GPU drivers, Wi-Fi and Bluetooth when suitable hardware and a
  bounded initial use case are available. The framebuffer console already exists.

## 14. Packaging, platform tooling and async I/O

Sources: inventory §§2.1, 2.10 and 2.11 and [package design](pkg/design.md).

- [ ] **Implementation — package provenance and reproducibility.** Add SBOMs,
  dependency locking, build sandboxing and multiarchitecture package handling.
  Package archives are already built deterministically.
- [ ] **Implementation — upgrade rollback.** Define multi-package transactions,
  recovery of interrupted installs and the relationship to filesystem boot
  environments. A single package install already stages files and commits its
  record before renaming them into place.
- [ ] **Implementation — container tooling.** Build usable tooling over the
  existing primitives. The service manager already composes roots, mount/UTS
  namespaces, domains, rlimits and handle rights per service; syscall filtering
  is not yet part of it. Broader isolation gaps are listed in §10.
- [ ] **Implementation/validation — async VM operations.** Define and exercise
  submittable VM operations through the async-I/O interface; existing file,
  socket, device and timer readiness does not demonstrate this contract.

## 15. Deliberate deferrals and decisions to revisit only with evidence

Sources: inventory §§1.4, 2.1 and 2.11. Items already described above retain
their deferred status here; this section does not schedule them for implementation.

| Deferred item or decision | Condition or rationale for revisiting |
|---|---|
| Advanced graphics, desktop environment and Wayland | Require a display/input architecture and a concrete product scope beyond the existing console; GPU work is in §13. |
| Distributed filesystem | A separate consistency, failure and network-storage project; local storage follow-ups are in §6. |
| Full Linux compatibility | Pursue specific applications and semantics incrementally through §8; do not promise arbitrary distribution compatibility from syscall counts. |
| Docker/Kubernetes compatibility | Requires much broader Linux and container semantics than current primitives/tooling; §14 is a smaller prerequisite. |
| Full NVMe feature set | Extend the functioning driver for demonstrated device/workload requirements, rather than implementing the entire specification in advance. |
| eBPF and eBPF-like tracing | Needs a security and lifetime model; tracing infrastructure in §11 is a prerequisite. |
| NUMA, huge pages, memory compression/deduplication | Memory projects in §5 remain deferred until a workload and test infrastructure justify them. |
| Live migration and nested virtualization | Require the state/UAPI and execution-model work in §9. |
| Device multiqueue, TSO/LRO, jumbo frames and zero-copy sockets | The inventory requires measured benefit and notes the limits of the current QEMU network backend; see §7. |
| AHCI NCQ | Existing measurements reached about 87% of NVMe aggregate throughput with four streams; revisit with evidence from a real disk. |
| e1000e checksum offload | Prior measurement suggested roughly a 2% benefit; establish a worthwhile workload before adding complexity. |
| x86 PCID | The inventory records no usable TCG CPU model for validation; obtain a hardware or virtualization test path first. |
| vGIC on GICv2 hosts | Existing virtual GIC support is GICv3-only and advertises that capability; add GICv2 support only for a supported target. |
| Remaining termios flags/control characters | Unsupported settings are currently masked off silently. Implement actual semantics when needed, or report them as unsupported. |
| Native `/sys` | Previously deferred because sysctl held the available information; Linux `/sys` compatibility is a distinct requirement in §8. |
| PID renumbering | The process-domain design deliberately avoids it; reopening requires an architectural argument, not merely a missing-feature label. |
| Lazy FPU switching | Eager switching was measured as a modest part of context-switch cost; revisit with new performance evidence and security analysis. |
| Rust or C#/.NET adoption | These were optional language/tooling directions, not missing kernel capabilities; introduce another language only for a concrete benefit. |
| SQLite package metadata | A text database was chosen; SQLite is an alternative to reassess if requirements outgrow that decision, not an outstanding deliverable. |

## Suggested next increments

1. ~~Establish a trustworthy two-CPU validation baseline and address the
   VirtIO/scheduler failures without weakening their intended contracts.~~
   Done 2026-10-05 ([two-CPU report](audit/2026-10-05-two-cpu-validation-report.md)); CI boots two CPUs
   (`make test-smp2`) so it stays done.
2. Add quiescence memory-order models and targeted negative controls.
3. Extend callback-wait and raw IRQ-pairing coverage within the established
   locking and lifetime architecture.
4. Select later features from the sections above by demonstrated correctness
   impact, user need and available validation; keep conditional deferrals explicit.

This ordering is a proposal for subsequent increments, not a claim that all
remaining projects belong in the current lockdep milestone.
