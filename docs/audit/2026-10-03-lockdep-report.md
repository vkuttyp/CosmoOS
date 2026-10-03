# Lockdep hardening report — 2026-10-03

## 1. Executive summary

The requested subsystem already existed. This pass audited and extended
it rather than building a second validator. Two memory-safety defects were
reproduced against the unchanged core under ASan and corrected: truncated
cycle diagnostics reading beyond their buffer, and class names retained
after their source memory was freed. Both reproducers pass against the
fixed core. The first hardening increment is implemented and tested;
this report does **not** declare every completion criterion in the new
prompt satisfied. Remaining validator model gaps and UP suite failures
are recorded below.

## 2. Baseline state

HEAD at investigation: `b1512a8477993216960dea20d19fe548a55b51db`.
User changes to reviewer.yaml and prompts/Next-Milestone.md were preserved.
macOS arm64, GNU make and LLVM cross-compilation, QEMU TCG. Unmodified
SMP debug boots passed 416 selftests on each architecture (x86 121.0 s,
AArch64 127.2 s). Host C sanitizer suites and Python harness unit tests
passed. Initial socket-bind permission failures were sandbox restrictions
and were rerun with approval. Logs: `out/lockdep-baseline-{host,x86,arm}.log`.

UP coverage was expanded after implementation and compared with an
unchanged HEAD checkout under `out/lockdep-baseline-tree`; see §15 for
the distinction between passing lockdep tests and failing complete boots.

## 3. Locking architecture discovered

Spinlocks (plain, irqsave, nested, try), sleeping mutexes with priority
inheritance, wait queues, semaphores, completions, futex buckets, atomics,
references, preemption control, IRQ masking/depth and epoch quiescence.
No kernel rwlock. Native userland condition variables use futexes.
Canonical rules: `docs/kernel/lockdep/locking-rules.md`. The initial audit
is `2026-10-03-lock-discipline-audit.md`.

`2026-10-03-lock-sites.tsv` records 232 direct and helper initialization
or definition sites, with file and line. It is a searchable source-site
inventory, not a claim that lexical matching enumerates every runtime
lock instance or proves each callback order.

## 4. Existing problems found before implementation

The path search returned the complete path length but copied only eight
nodes to its caller; the reporter indexed the small buffer with that
complete length. The graph also kept name pointers permanently, including
driver names in unloadable module rodata. Raw addresses reused for another
module could silently become the old class key. Class-cache publication,
IRQ usage and bitmap hot-path reads also mixed shared plain access with
serialized writes. The old VFS/futex bugs in the prompt are already fixed,
and the scheduler now has ordered runqueue pairs rather than one class.

## 5. Lockdep architecture

Preserved the existing allocation-free raw lock, class table, graph,
pre-acquisition check and post-ownership push. No scheduler policy change,
new reclamation mechanism or replacement lock API. `LOCKDEP=0/1` now maps
to CONFIG_LOCKDEP through ordinary make configuration; defaults remain
debug enabled / release disabled. Separate OUT directories avoid stale
objects because build rules do not track changed command-line flags.

## 6. Lock-class model

Name **contents plus kind** identify a class. This implements the prior
documented intent that equal names share a family without relying on
linker string pooling, source addresses or module mappings. Each record
owns 64 bytes of name storage. Names longer than 63 characters are rejected
with a metadata-overflow report, never truncated into another key. NULL
normalizes to `?`. Cached class indices use acquire/release atomic access.
Original names must still outlive their lock's use by primitive diagnostics.

## 7. Dependency graph implementation

Unchanged 320 classes × four subclasses, 1280 bitmap nodes and breadth-first
cycle search. Hot edge reads and updates are atomic; graph modification
and search remain serialized by the raw lock. Path output now returns the
number of entries actually stored, supports zero-capacity/NULL output and
self reachability. Detection is not truncated; kernel diagnostics retain
the last eight path nodes and label this limit explicitly.

`2026-10-03-lock-order.tsv` records the final SMP observations by architecture:
1197 x86 edges, 1210 AArch64 edges, 1288 in their union. Kahn traversal
found each graph and their union acyclic. These include test classes and
observed acquisition attempts; they are not exhaustive workload proof.

## 8. Atomic-context model

Existing might_sleep checks preempt_count or irq_depth, covering spinlocks,
explicit preemption disabling and quiesce readers. Existing blocking and
faulting-copy boundaries remain. IRQ masking alone is not part of this
predicate; no claim that a zero count makes arbitrary blocking safe.

## 9. IRQ-safety model

Preserved direct per-class IRQ-use / acquired-with-IRQs-on checks. Usage
loads/stores are atomic and the report decision uses the before/after
values from one serialized update. Successful trylock IRQ classification
and transitive safe→unsafe graph relationships remain gaps.

## 10. Preemption interaction

Spin stacks remain per CPU; mutex stacks remain per thread. Runqueue lock
handoff across a same-CPU context switch and thread migration are unchanged.
Existing positive-count assertion on preempt_enable remains. Both SMP
boots exercised scheduler/PI/migration tests without unexpected lockdep
reports; no broad preemption redesign was performed.

## 11. Lifetime/quiescence interaction

No changes to unlink, prevent-new-access, drain, grace-period or final-put
ordering. IRQ unregister, timer cancellation, module zombie handling and
device/network removal retain their existing implementations. Copying
graph names removes its dependency on freed module image storage. The
fixture exercises a class in module rodata through unload and reload.
Callback-wait dependencies such as timer_cancel_sync are still not graph
edges; their existing rule must be preserved explicitly.

## 12. Diagnostics

Held entries now print the lock object's address as well as its acquisition
address. Fatal reporting masks interrupts and enters existing console
panic mode before printing, avoiding re-acquisition of console.lock when
the violation originated in a sink. This ordering is code-reviewed;
normal boots use expected-report tests rather than inducing a fatal
console failure. NMI/#MC raw-lock reentrancy and sink-specific catastrophic
failure behavior are not proven by this change.

## 13. Tests added

- Host metadata-lifetime: changed/freed source storage, equal names in
  different storage, kind separation, maximum name length, overlong and
  NULL handling; exhaustion uses unique names rather than duplicate keys.
- Host path-bounds: 1280-node chain and the reporter's iteration under
  ASan/UBSan, empty output, self reachability and disconnected reverse.
- Kernel lockdep-order: independently observed ten-lock chain, closing
  cycle through real instrumentation, unheld release without performing
  a corrupting primitive unlock.
- Module-load: fixture lock named in module rodata, no new class on reload;
  end-of-suite graph dump dereferences the copied name after image teardown.

Negative reproducers compiled against HEAD's core failed with ASan
stack-buffer-overflow and heap-use-after-free. Identical reproducers
against the fixed core exited zero. Artifacts:
`out/lockdep-negative-repro/{path,metadata}{,-fixed}.log`.

## 14. Stress testing

Normal debug boots exercised all 416 selftests, userland, shell and host
network harness: scheduler/migration/PI, VFS rename/cache/put races,
filesystem replay and fault injection, TCP timers and teardown, device
removal and module zombie tests, lifetime/quiescence stress. No new random
stress hook or broad warning suppression was introduced. Separate chaos,
fuzz, guard and panic variants were not rerun in this increment.

## 15. Cross-architecture results

| Run | Result |
|---|---|
| Final x86-64 debug SMP | PASS, 416 tests, 118.8 s (`out/lockdep-final-x86.log`) |
| Final AArch64 debug SMP | PASS, 416 tests, 121.8 s (`out/lockdep-final-arm.log`) |
| x86-64 release | PASS, 17.3 s |
| AArch64 release | PASS, 20.4 s |
| Host ASan/UBSan + Python harness units | PASS (`out/lockdep-final-host.log`) |
| Fresh debug LOCKDEP=0 image + boot | Initial FAIL process-rlimit; rerun PASS, 416 tests, 116.4 s |
| Fresh release LOCKDEP=1 kernel | Builds successfully (`out/lockdep-release-enabled-build.log`); not booted |
| x86-64 UP | Kernel selftests pass; complete boot FAIL cwdtest `checked > 0`, shell marker absent |
| AArch64 UP | FAIL two cross-CPU clock tests and cwdtest progress assertions |
| Unchanged HEAD AArch64 UP | Reproduces both clock failures and both cwdtest failures; baseline defect |
| Unchanged HEAD x86-64 UP | Reproduces cwdtest `checked > 0` failure; all 416 kernel tests pass (`out/lockdep-baseline-x86-up.log`) |
| Analyzer, both architectures | Targets exit successfully; changed-source incremental runs report no warning. Full runs report existing warnings in unchanged sources, below |

All lockdep-specific UP tests pass. AArch64 clocktest's bracket helper
returns false when fewer than two CPUs are online; this is unchanged in
HEAD. cwdtest's progress assertions reject an unexercised comparison and
are preserved, not skipped. The complete UP suite is not green.

Full analyzer runs emitted twenty-seven x86 and nineteen AArch64 warnings in
unchanged sources (scheduler, list callers, epoll, syscall, device tests,
guest memory, VFS/CosmoFS tests, networking and NVMe). Logs:
`out/lockdep-analyze-{x86,arm}.log`. The make target prints "clean" even
after these warnings because analyzer diagnostics do not fail the current
rule; its successful exit must not be presented as a warning-free audit.

## 16. Findings against the existing kernel

Final debug SMP runs contain nine expected validator reports (including
the new cycle/unheld checks and runqueue-order test) and no unexpected
report. Their module-class edge prints after unloading the fixture.
The intermediate AArch64 SMP run recovered from the known QEMU reset
on back-connection attempt two; final rerun succeeded on attempt one.
The sighting and updated tally are in docs/testing/flakes.md.

## 17. Real bugs discovered

Confirmed/reproduced: unsafe long-cycle path consumption and stale class
name dereferences after storage reclamation. Confirmed by review: shared
non-atomic cache/usage/bitmap access and reporter re-acquisition of the
console lock before panic mode. No new subsystem ABBA was observed in the
passing debug boots.

## 18. Bugs fixed

Bounded path output, permanent copied class metadata with content identity,
atomic cache/usage/bitmap access, and fatal console-mode ordering. Existing
lock-order and lifetime rules were preserved. Public lock/thread layouts
and module ABI did not change.

## 19. False positives encountered

No new unexpected lockdep report or suppression. The same-name grouping
continues to match tested workloads. That evidence cannot establish that
every future identically named lock belongs to one logical class.

## 20. Performance/overhead

ELF symbol inspection: graph is 232968 bytes (200 KiB bitmap plus owned
class records); copying names adds 17920 bytes (17.5 KiB) to the enabled
graph, rather than retaining pointers. Scratch is 5280 bytes. This storage
and validator runtime functions are absent from the debug-disabled ELF.
Primitive/thread layouts remain stable, including the pre-existing class
fields and thread held array. Cached lookups avoid repeated string scans.
The initial pass did not run an acquisition microbenchmark. The warmed
spin-path increment below now records matched debug LOCKDEP=0/1 samples;
whole-boot timing differences are not lock-overhead measurements.

## 21. Known limitations

Eight-node printed cycle tail, bounded 320 classes/held stacks, no global
snapshot protocol for concurrent held-stack diagnostics/statistics, no sanitizer model
of kernel execution-context state, and no general callback-wait graph remain.
The host graph model described below covers publication and shared graph
access under TSan. Timer cancellation now
checks only the lock paths learned from active timer callbacks; it does not
model other callback classes or arbitrary wait dependencies. IRQ dependency
validation is transitive for modeled lock-class paths. `spin_unlock_irqrestore`
validates the restored enabled/disabled state against the matching held-lock
record, but raw `arch_irq_restore` ownership/pairing remains unchecked.
NMI/#MC instrumentation remains unsupported without proving raw-lock
reentrancy. Release-with-lockdep was built, not exercised.

## 22. Deferred work

Updated main inventory with the fixed defects and remaining model gaps.
UP test validity needs its own correction before claiming full cross-
configuration validation. The disabled boot's process-rlimit failure
was exit 19 from rlimit-unpriv: 80 console writes over roughly 1.4 s let
the 64-token bucket refill at 16/s, so its assumed instantaneous burst
observed no EAGAIN. Rerun passed without changing assertions; this is a
timing-sensitive test, not evidence that lockdep code executes when disabled.

## 23. Remaining concurrency risks

Wait-for-callback edges, IRQ-safe chains through another lock, interrupted
diagnostic readers, long preemption-disabled populate sections and formal
memory-order validation remain separate correctness questions. Runtime
coverage only detects executed relationships. Copied source names do not
make numeric acquisition addresses symbolize after a module is unmapped.

## 24. Recommended next subsystem

Finish validator context/dependency modeling: successful trylock IRQ usage
and transitive IRQ-safe→unsafe relationships, with deterministic failure
tests, then callback wait dependencies and fatal/NMI diagnostics. Before
expanding this implementation, correct the demonstrated UP harness
prerequisites and obtain a green full UP run without removing progress or
cross-CPU correctness assertions.


## PR #302 review follow-up

Corrected the runqueue rules throughout the lockdep and scheduler pages:
increasing-CPU pairs, address-space tag allocation (`asid`, observed on
AArch64), and logging during expected diagnostics explain the outgoing
edges. The old leaf/zero-successor claim was stale. Configuration guidance
now consistently distinguishes `LOCKDEP=0/1` from its debug/release defaults.

`lockdep_dump_graph` now captures class/edge counts under the raw lock,
reads bitmap words atomically, and restricts names/kinds to the published
class range. Printing remains outside the raw lock. Edges can grow during
the dump; it is not a consistent snapshot, and the general statistics
snapshot limitation remains deferred.

The reported double increment in `lockdep_core_add_edge` is not reachable
through the current kernel caller: `lockdep_acquire` holds `raw_lock`
across the duplicate recheck, cycle search and insertion. Host callers are
single-threaded. The helper now explicitly documents this serialization
requirement; atomic bitmap updates support unlocked readers, not concurrent
writers or lock-free cycle decisions.

Review validation: `gmake host-test` passes (ASan/UBSan and Python harness
units); both kernel builds pass; x86-64 and AArch64 debug SMP boot suites
pass all 416 self-tests in 172.6 s and 171.3 s respectively. Each prints
nine expected reports, no unexpected lockdep report, and the unloaded
module's copied class name. Logs: `out/lockdep-review-host.log`,
`out/lockdep-review-{x86,arm}-build.log`, `out/lockdep-review-{x86,arm}.log`,
and each architecture's `lockdep-review-smp.log`. These boots exercise the
dump but are not a proof of all possible concurrent interleavings; the
shared-access argument also relies on the lock/publication inspection.

## Active milestone continuation

The later milestone work extends the original IRQ model with forward and
reverse transitive-path checks. Successful trylocks from IRQ context add
neither IRQ-use labels nor blocking edges; IRQ-enabled thread trylocks remain
unsafe. The IRQ integration cases cover either endpoint arriving last,
transitive chains, subclasses, and trylocks. `spin_unlock_irqrestore` now
checks that its lock was acquired with the same saved interrupt state and
that enabling interrupts will not expose another spinlock acquired with
interrupts disabled.

Timer callback profiling records up to 16 distinct spinlocks per active
timer callback. A contended `timer_cancel_sync` wait checks
the caller-held spin and mutex stacks against that observed profile after
dropping the timer queue lock. Profiles learn only executed callback paths.
Full boots exposed profile exhaustion during TCP PCB churn: first, stale
profiles remained after cancellation; releasing them at synchronous cancel
still allowed many live timers to occupy every slot on UP. The active
implementation now allocates a profile only when a callback starts and
releases it after the timer queue clears `running`. The fixed table has 64
slots, one for every possible active CPU callback, and each profile tracks
16 distinct locks. `g_timer_profiles` occupies 0x4400 bytes (17 KiB) in the
x86-64 debug ELF, down from 0x22000 bytes in the first version. Lookups use
a bounded hash probe. The regression
holds a callback-needed lock while a real callback blocks on it, then checks
that `timer_cancel_sync` reports and returns. UP x86-64 and AArch64 now both
pass all 416 tests plus the full boot harness with active-callback profile
allocation (115.9 s and 112.5 s). Logs:
`out/milestone-callback-active-up-x86.log` and
`out/milestone-callback-active-up-arm.log`. Both report timer cancellation
success and `net-accept-race` completes in under 60 ms. Final SMP x86-64
and AArch64 debug boots also pass all 416 tests plus the full harness
(126.1 s and 139.5 s). Logs:
`out/milestone-callback-active-smp-x86.log` and
`out/milestone-callback-active-smp-arm.log`. All four UP/SMP debug
combinations pass with the active-callback profile lifetime. The host suite
passes after graph/IRQ work.

The earlier IRQ and irqrestore integration runs passed all 416 tests on
both SMP architectures before callback profiling was added. With the final
64-slot callback profile table, both SMP debug architectures pass all 416
tests and the full boot harness: x86-64 in 126.4 s and AArch64 in 129.0 s
(`out/milestone-callback-64slots-x86.log` and
`out/milestone-callback-64slots-arm.log`). The final 64-slot table also
passes UP x86-64 and AArch64 (110.8 s and 113.9 s;
`out/milestone-callback-64slots-up-x86.log` and
`out/milestone-callback-64slots-up-arm.log`), so all four final UP/SMP
debug configurations pass. `g_timer_profiles` is 0x4400 bytes (17 KiB) in
the x86-64 debug ELF. NMI/#MC reentrancy, raw architecture IRQ restore pairing,
global concurrent diagnostic snapshots, TSan modeling, generalized callback
wait dependencies, and isolated performance measurement remain unresolved.
The inventory now carries these limits forward.

## PR #303 review follow-up

The valid IRQ-restore control incorrectly armed `LOCKDEP_R_IRQ_STATE`
while expecting zero reports. Reading the hit counter does not disarm an
expectation, so a later real violation on that CPU could have been consumed.
Removed the expectation from the valid control; deliberately invalid probes
still arm and consume exactly one expected report.

The reported 64-slot timer-profile exhaustion is not reachable through
the current timer execution model. `run_expired` is called only from the
local timer interrupt and runs callbacks serially with interrupts masked.
It clears the profile before starting the next callback on that CPU, so
live profiles cannot exceed `CONFIG_MAX_CPUS` (64). The existing static
assertion requires at least that many slots. Pending timers consume none;
hash probing visits every slot and reuses tombstones. Documented this
bound at profile allocation rather than increasing the table without a
reachable exhaustion case.

Review validation: both four-CPU debug boots pass all 416 self-tests and
the full user-mode/network harness, including `lockdep-irq` and
`timer-cancel-sync`: x86-64 in 129.6 s and AArch64 in 137.9 s. Logs are
`out/pr303-review-x86_64.log` and `out/pr303-review-aarch64.log`.
`git diff --check` passes.

## Concurrent graph model increment

Added `tests/host/test_lockdep_threads.c`: four writers and two unlocked
diagnostic readers use the real `lockdep_core.h` helpers. The writer lock
uses the same acquire/release atomic-word protocol as the kernel raw lock.
Writers serialize class registration, usage validation, cycle detection,
and edge insertion. Readers take the lock to obtain a published class
range, then inspect immutable metadata and atomic usage/edge fields. A
final independent closure oracle verifies edge totals, acyclicity, and
that no IRQ-used class reaches an IRQ-enabled class. This supplements the
single-threaded graph oracle with actual concurrent memory accesses.

The model runs in the ordinary ASan/UBSan host suite and separately via
`gmake host-test-lockdep-tsan`. The TSan binary has its own output name,
so changing sanitizer modes cannot reuse an ASan build. On the macOS
AArch64 development host, both sanitizer modes pass. As a negative control,
a temporary copy of `lockdep_core_has_edge` used a plain bitmap read;
TSan reported its race with the atomic write in `lockdep_core_add_edge`
and exited unsuccessfully. The repository helper remains atomic. Logs:
`out/lockdep-threads-tsan.log`, `out/lockdep-threads-host.log`, and
`out/lockdep-tsan-negative.log`.

The complete `gmake host-test` suite, including its Python harness tests,
passes after integration; `git diff --check` also passes.

This increment changes no kernel runtime code. It does not cover CPU-local
held stacks, interrupt entry, migration, callback profile lifetime, or
NMI/#MC execution. Graph diagnostic reads remain a live view, not a
globally consistent snapshot. TSan execution on the Linux CI host remains
unverified; the new model is included in its existing ASan/UBSan host step.

## Consistent graph dump increment

`lockdep_dump_graph` now allocates a private bounded graph, copies the
source under the raw writer lock, and prints from that copy after unlocking.
The emitted header and edge list describe the same instant. Allocation,
logging, and freeing never run while holding the raw lock. The snapshot
owns its class metadata as well as its bitmap; module metadata lifetime
rules remain intact. Allocation failure emits an explicit unavailable
message. The 232968-byte copy uses a temporary 256 KiB heap allocation,
with no permanent second graph and no additional acquisition-path work.
The raw lock and masked interrupts do cover the bounded copy.

The host `snapshot` regression changes and frees the source graph before
checking the captured metadata, counts, and subclass edges. The threaded
model now also captures snapshots during concurrent writes and checks
their counts and endpoint ranges after releasing the writer lock. Both
the complete ASan/UBSan host suite and the separate TSan model pass; release
kernels build for x86-64 and AArch64. Logs are
`out/lockdep-snapshot-host.log`, `out/lockdep-snapshot-tsan.log`, and
`out/lockdep-snapshot-release-build.log`.

Four-CPU debug boots pass all 416 self-tests and the complete boot harness:
x86-64 in 121.5 s and AArch64 in 128.5 s. The captured logs contain 275
classes each; printed edge counts exactly match their headers (1254 on
x86-64, 1240 on AArch64). Logs: `out/lockdep-snapshot-x86_64.log` and
`out/lockdep-snapshot-aarch64.log`. `git diff --check` passes.

This normal diagnostic API requires a working allocator and raw lock.
Panic/NMI paths do not call it. Concurrent remote held-stack inspection,
global statistics consistency, and NMI/#MC reentrancy remain unresolved;
a consistent graph alone does not freeze those execution states.

## Panic execution-context increment

The AArch64 CI panic-path log exposed a stale diagnostic: it reported
`context: boot (no threads yet)` while its held-lock section named `kmain`.
`panic_common` printed that boot label unconditionally. It now captures
the local current thread, IRQ depth, and preemption count after masking
interrupts and before stopping peers or printing. The report distinguishes
thread, interrupt, and early-boot context, and prints thread ID and a bounded
name alongside nesting counts. No allocation or scheduler lock is added.
This improves fatal locking/atomic-context diagnostics without claiming
NMI/#MC reentrancy or safety for corrupt current-thread pointers.

Both deliberate fault harness modes now require the known `kmain` thread
context with zero IRQ depth and preemption count. The previous CI output
is rejected by this marker, closing the validation gap that let the stale
label pass. Synchronous exceptions are distinguished by their trap dump;
they do not imply execution in an interrupt handler.

Validation: four-CPU debug `test-crash` passes on x86-64 (106.7 s) and
AArch64 (113.2 s), including all 416 self-tests followed by the deliberate
page fault. The panic names thread 1 `kmain`, with both nesting counts
zero, on CPU 2 and CPU 1 respectively. AArch64 release `test-wxn` also
passes (8.7 s), verifying the context with lockdep disabled. Logs are
`out/panic-context-{x86_64,aarch64}-result.log`, their corresponding
`out/<arch>-debug-crash/boot-test-crash.log`, and
`out/aarch64-release-wxn/boot-test-wxn.log`. Early-boot and IRQ-context
branches were inspected but not exercised by these deliberately
thread-context faults. `git diff --check` passes.

## Remote held-stack snapshot increment

`lockdep_dump_held_cpu` previously read another CPU's mutable stack with
ordinary loads and printed it directly. A release shifting entries or an
irqsave metadata update could race with that traversal. The dump now uses
one bounded atomic snapshot attempt and prints only a successful private
copy; a busy, changed, or invalid stack produces an unavailable message.
It neither allocates nor acquires a target-owned lock, and an unresponsive
writer cannot make it spin.

CPU-local writers already mask interrupts for these updates. They now
bracket pushes, irqsave flag updates, and removals with a 64-bit sequence
and use atomic field/count stores. Snapshot readers accept only equal
even sequence values around atomic field reads. All protocol operations
are sequentially consistent, so a successful bounded copy cannot cross a
writer in the atomic order. This adds eight bytes per possible CPU and
atomic operations to debug held-stack updates; isolated overhead remains
unmeasured. Thread mutex tracking is unchanged.

The sanitizer model checks immediate failure with a writer stopped at an
odd sequence, rejects oversized counts, and races readers against complete
generation changes. The kernel `lockdep-order` case checks real push,
irqsave flags, out-of-order release shifting, and empty-stack publication.
The full ASan/UBSan host suite and TSan model pass; release builds pass on
both architectures. Four-CPU debug boots pass all 416 tests and the full
harness: x86-64 in 130.4 s and AArch64 in 133.6 s. Logs:
`out/lockdep-held-host.log`, `out/lockdep-held-tsan.log`,
`out/lockdep-held-release-build.log`, and
`out/lockdep-held-{x86_64,aarch64}.log`. Both debug kernels were rebuilt
after clarifying the unavailable-message wording. `git diff --check` passes.

The snapshot covers a single CPU's spinlock stack at one instant, not
simultaneous CPU/thread state. It does not make writer instrumentation
NMI-reentrant. Global statistics consistency, thread-stack snapshots,
NMI/#MC reentrancy, generalized callback dependencies, and raw IRQ-state
pairing retain their existing limitations.

## Warmed spin-path benchmark increment

Added `lockdep-bench`, compiled identically into debug builds with lockdep
enabled or disabled. It measures an empty loop/dispatch control, one plain
spin acquire/release pair, one irqsave pair, and an outer irqsave plus
inner plain pair with a warmed dependency edge. Each path has 64 warmup
iterations followed by nine samples of 1024 iterations; output reports
minimum, median, and maximum guest-clock nanoseconds per iteration.
The thread is pinned for CPU-local clock readings. IRQs, scheduling,
and other CPUs remain active, so their interference is included. Logging
and affinity changes are outside the measured intervals.

The benchmark is descriptive and has no timing pass threshold. Separate
debug output trees for `LOCKDEP=1` and `LOCKDEP=0` avoid comparing other
debug/release differences. QEMU/TCG timings are not native-hardware
performance claims. Cold graph searches, contended object locks, mutex
paths, and precise attribution of individual instrumentation costs remain
outside this benchmark.

Initial measurements: QEMU 11.1.1/TCG on an arm64 macOS host, Apple clang
21.0.0, four virtual CPUs, 256 MiB, default `qemu64,+nx,+svm,+npt` and
`cortex-a72` CPU models. Each architecture was booted once per configuration;
the architecture pair ran concurrently, with enabled and disabled pairs
run sequentially. Host load and emulator/code-layout differences are not
controlled tightly enough to attribute every timing difference to a
particular hook. Values below are min/median/max ns per iteration, with
the empty control left unsubtracted.

| Architecture | Path | LOCKDEP=0 | LOCKDEP=1 |
|---|---|---:|---:|
| x86-64 | empty | 42 / 42 / 57 | 29 / 30 / 78 |
| x86-64 | spin | 384 / 386 / 430 | 1605 / 1634 / 1698 |
| x86-64 | irqsave | 695 / 698 / 716 | 2004 / 2033 / 2295 |
| x86-64 | nested | 1058 / 1063 / 1127 | 3346 / 3862 / 4400 |
| AArch64 | empty | 13 / 13 / 25 | 13 / 13 / 24 |
| AArch64 | spin | 210 / 210 / 212 | 1062 / 1102 / 1114 |
| AArch64 | irqsave | 346 / 347 / 381 | 1527 / 1568 / 1705 |
| AArch64 | nested | 636 / 638 / 673 | 2574 / 2603 / 2666 |

The distributions expose measurable debug instrumentation cost in these
emulated workloads. They do not isolate the remote-snapshot atomics from
the existing class lookup, usage validation, graph checks, stack tracking,
and IRQ masking; a native run or a separate controlled comparison is
needed for that attribution.

All four debug SMP boots pass all 417 self-tests and the full boot harness:
x86-64 enabled 125.0 s / disabled 113.3 s; AArch64 enabled 129.1 s /
disabled 119.3 s. Each log contains all four expected paths, nine samples
of 1024 iterations, the matching configuration flag, and ordered
min/median/max output. Logs: `out/lockdep-bench-{on,off}-{x86_64,aarch64}.log`.
Release kernels also build on both architectures
(`out/lockdep-bench-release-build.log`). `git diff --check` passes.

## Hardware NMI held-stack snapshot increment

Extended x86 `trap-paranoid` with two local-APIC hardware NMIs while the
validator graph raw lock is held and local IRQs are masked. The first
handler must capture the private irqsave lock on the interrupted CPU's
held stack, with the expected per-CPU block, interrupt depth and IST stack.
The second interrupts an unfinished held-stack update (odd sequence) and
must return unavailable with zero entries. Handler completion is published
with a release/acquire hit counter; the delivery wait is bounded to 100 ms.
The graph-lock probe hook is compiled only with lockdep and self-tests.

Inspection found that current NMI lockup sampling and corrected machine-check
handlers avoid tracked lock acquisitions. This increment validates the
read-only snapshot boundary, rather than changing those handlers or claiming
general NMI/#MC writer reentrancy. Acquiring a tracked lock from such a
handler remains unsupported. AArch64 has no corresponding NMI test here.

Both four-CPU debug boots pass all 417 self-tests and the complete boot
harness: x86-64 in 144.0 s and AArch64 in 128.4 s. The x86 log explicitly
confirms successful hardware NMI capture and refusal of the busy writer;
the AArch64 boot provides cross-architecture regression coverage only.
Logs: `out/lockdep-nmi-{x86_64,aarch64}.log` and matching `-result.log`
files. Default x86 release and release with `LOCKDEP=1` kernels build
successfully (`out/lockdep-nmi-release-build.log`), covering the self-test
configuration boundary. The core snapshot algorithm is unchanged; the
prior host sanitizer model remains its concurrency evidence.

## PR #304 review follow-up

The late-NMI finding is valid: returning after the delivery deadline could
unregister the handler while a sent NMI remained pending. The test now
suppresses the second probe after a failed delivery, releases both test
locks, and fails stop with an explicit `SELFTEST: trap-paranoid` panic.
The handler remains registered and its stack argument remains live. A
hardware NMI cannot be safely cancelled, so a bounded timeout cannot also
promise safe normal return. The success path still unregisters normally.

The field-consistency finding is a false positive for the implemented
protocol. Every writer brackets its field/count stores with sequence
increments; all those accesses and snapshot loads are sequentially
consistent. An accepted snapshot's equal even sequence loads exclude an
intervening writer in that total order (assuming no full 64-bit wrap during
one copy). Separate field stores therefore cannot produce an accepted
mixed entry. Failed output is unusable. The header now explains this
explicitly; the existing host model checks every field against its
generation, stopped odd writers, and invalid counts.

CI run `37126012073`, AArch64 job `111211325103`, passed both ordinary and
GIC debug boots but faulted in the protection-capable CPU boot after all
417 self-tests. The `thrtest` spawn path was interrupted in
`random_get_bytes` beneath `build_initial_stack`; timer/scheduler lock
entry exhausted the kernel stack. This is distinct from the earlier
net-nicbench timing failure. Local AArch64 disassembly showed 5,328 bytes
for `build_initial_stack` alone, with large spawn and ELF-loading callers
still live. Its two temporary arrays now use private heap scratch, reducing
that frame to 192 bytes with the same local compiler. Scratch is freed on
success; allocation failure returns `-ENOMEM` through the existing process
cleanup path. The 16 KiB kernel stack and interrupt/lockdep checks remain
unchanged.

Validation of the review fixes:

- Four-CPU x86 debug boot: all 417 self-tests and full harness passed in
  134.0 s (`out/lockdep-review-x86_64.log`).
- Four-CPU AArch64 `test-guard` on cortex-a76: all 417 self-tests and full
  harness passed in 137.3 s (`out/aarch64-debug/boot-test-guard.log`). This
  includes the post-self-test `thrtest` workload that faulted in CI.
- Full host ASan/UBSan suite and lockdep TSan model passed
  (`out/lockdep-review-{host,tsan}.log`). Release kernels built on both
  architectures (`out/lockdep-review-release-{x86_64,aarch64}.log`).
- Temporary fault injections suppressed first and second hardware NMI
  sends independently, forcing the actual 100 ms timeout. Each injected
  a late software NMI entry after lock cleanup and before panic. Both
  reached the explicit timeout diagnostic without an unhandled/recursive
  exception, with zero IRQ/preemption nesting and empty held stacks.
  Logs: `out/lockdep-review-nmi-timeout-{first,second}.log`. The mutations
  were removed and the normal x86 image rebuilt successfully.
- `git diff --check` passed. Remote CI still needs to validate the pushed
  revision; one local passing guard boot is not a claim about all schedules.

The subsequent inventory review correctly identified a documentation
error: panic reports have held-lock diagnostics, not a best-effort graph
dump. Corrected the inventory and PR description, and made the distinction
explicit in the design document. Cross-checked the API, invariants,
diagnostics documentation, and audit descriptions against `panic_common`
and all `lockdep_dump_graph` call sites: panic calls `lockdep_dump_held`,
while the normal graph dump is called at the end of the self-test run.


## Post-#304 continuation: statistics and mutex measurement

Base: merged PR #304 (`aede0142`). This continuation proceeds in three
phases: serialize the statistics snapshot, extend the matched benchmark,
then validate both configurations on both architectures. The larger NMI
writer, callback-wait, raw IRQ pairing, and global held-state questions
remain separate work.

### Phase 1: consistent statistics

Previously, `lockdep_get_stats` held the graph lock while reading three
independently changing atomic counters. Class/edge totals were stable,
but acquisitions and reports could change between field reads. All
counter writes now share the graph raw lock with the complete copy.
Search counting uses its existing graph hold; acquisition and report
counting each add a short hold at the same counting point as before.
Every `g_stats` access and every report call site was audited: reports
are entered after any graph hold is released, so counting does not recurse
on the raw lock. Printing remains outside it.

This yields a snapshot of counter values at one instant, not a transaction
covering a whole acquisition. A class may already exist while its first
acquisition is still in progress; reports and acquisitions retain their
existing meanings. CPU/thread held stacks are not frozen. The API requires
normal diagnostic context and may wait for the raw lock; no NMI/panic
safety is claimed. The actual kernel `lockdep-order` case now also checks
acquisition/report deltas, alongside its existing search-reuse assertions.
The proof of cross-field consistency is common-lock coverage, not the
older host graph model (which does not run this statistics API).

### Phase 2: warmed mutex measurements

The existing identical debug LOCKDEP=0/1 workload now includes a private
mutex lock/unlock pair and a successful mutex trylock/unlock pair. The
trylock result is asserted so a refused acquisition cannot be counted as
a fast successful one. Initialization and 64 warmup iterations precede
nine batches of 1024 timed iterations for each of six paths. Mutex paths
include their internal spinlock and bookkeeping costs. Thread pinning,
interrupt/scheduling noise, and descriptive-only output remain unchanged.

The additional statistics serialization affects enabled-build timings.
These runs measure the full current hooks, not an isolated before/after
cost for that serialization. Cold graph searches, contended waits,
priority inheritance, and native hardware remain outside this experiment.

### Phase 3: cross-architecture validation

Measurements below are min/median/max guest nanoseconds per iteration.
Environment remains QEMU 11.1.1/TCG on arm64 macOS, Apple clang 21.0.0,
four virtual CPUs and 256 MiB, with default qemu64 and cortex-a72 models.
The architecture pair ran concurrently; the enabled pair ran before the
disabled pair, using separate matched debug output trees. Host load is
not controlled and no timing is a pass threshold. The empty control is
not subtracted.

| Architecture | Path | LOCKDEP=0 | LOCKDEP=1 |
|---|---|---:|---:|
| x86_64 | empty | 44 / 45 / 59 | 42 / 43 / 55 |
| x86_64 | spin | 391 / 393 / 446 | 2195 / 2219 / 2296 |
| x86_64 | irqsave | 707 / 1074 / 1639 | 2615 / 2658 / 2691 |
| x86_64 | nested | 1077 / 1084 / 1138 | 4280 / 4344 / 4463 |
| x86_64 | mutex | 3007 / 3033 / 3251 | 11477 / 11595 / 11757 |
| x86_64 | mutex-try | 2786 / 2817 / 2952 | 11011 / 11150 / 11372 |
| aarch64 | empty | 13 / 13 / 25 | 12 / 13 / 24 |
| aarch64 | spin | 305 / 305 / 669 | 1281 / 1321 / 1389 |
| aarch64 | irqsave | 540 / 583 / 863 | 1771 / 1854 / 2001 |
| aarch64 | nested | 917 / 980 / 1370 | 3054 / 3108 / 3159 |
| aarch64 | mutex | 2348 / 2376 / 2411 | 8072 / 8215 / 8372 |
| aarch64 | mutex-try | 2269 / 2351 / 2395 | 7956 / 7992 / 8131 |

All four debug SMP boots passed all 417 self-tests and the complete boot
harness: x86-64 enabled 131.1 s / disabled 111.4 s; AArch64 enabled
132.4 s / disabled 118.5 s. Each log has all six expected paths, nine
samples of 1024 iterations, matching configuration, and ordered
min/median/max values. Logs: `out/lockdep-stats-{on,off}-{x86_64,aarch64}.log`.
Both release kernels built (`out/lockdep-stats-release-{x86_64,aarch64}.log`).
`git diff --check` passed. The host core algorithms are unchanged; prior
host sanitizer results are retained rather than claimed as fresh evidence
for kernel statistics serialization.


## New-edge core benchmark continuation

Base: `64c492e6`. This increment addresses sparse new-edge search costs in
three phases: define reproducible private topologies, implement timed core
validation with checked results, then run both architectures and disabled
configuration builds. It does not alter live acquisition policy.

`lockdep-graph-bench` allocates a private graph and scratch, with one active
subclass-zero node per class at sizes 16, 64, and 256. For insertion and
IRQ rejection, two chains have a missing middle edge. The IRQ case marks
only the first class IRQ-used and the last IRQ-enabled, so both components
are individually valid and only the proposed bridge creates a conflict.
The cycle case has a full chain and proposes last-to-first. Each operation
uses the real reachability, IRQ-edge, and insertion helpers in the kernel
check order. Checks outside timing validate the outcome, edge count,
proposed-edge presence, truncated cycle endpoints, and IRQ endpoints.

The graph is rebuilt before every operation, including two warmup samples
and nine timed samples per case. Allocation, setup, validation, and output
are outside the measured interval; allocation and affinity are cleaned up
on failure as well as success. The private classes and edges never enter
the live validator. Disabled builds skip this benchmark.

These measurements characterize the core algorithm for a previously absent
edge, not cold CPU caches: rebuilding and warmup can populate caches.
They exclude raw-lock acquisition/contention, class registration, held-stack
scanning, statistics, and report output. Single-operation timing includes
clock overhead and is visibly quantized around a microsecond in these runs;
small differences at the smallest size are not meaningful. Dense graphs,
complete first-acquisition cost, and native hardware remain open.

Same QEMU 11.1.1/TCG arm64 macOS environment, Apple clang 21.0.0, four CPUs
and 256 MiB; both architecture boots ran concurrently. Values are
min/median/max guest nanoseconds per operation, with no pass threshold.

| Active nodes | Operation | x86-64 | AArch64 |
|---:|---|---:|---:|
| 16 | insert | 1002 / 2004 / 2005 | 992 / 2000 / 2000 |
| 16 | cycle | 2004 / 2005 / 3007 | 992 / 2000 / 2992 |
| 16 | irq-bridge | 4009 / 4010 / 8019 | 2992 / 3008 / 4000 |
| 64 | insert | 5012 / 5012 / 6015 | 4000 / 4000 / 5008 |
| 64 | cycle | 9021 / 9022 / 11026 | 6992 / 6992 / 7008 |
| 64 | irq-bridge | 13031 / 14034 / 15036 | 10992 / 12000 / 52000 |
| 256 | insert | 19045 / 21050 / 60145 | 14992 / 15008 / 18000 |
| 256 | cycle | 35084 / 36086 / 38092 | 26000 / 27008 / 51008 |
| 256 | irq-bridge | 54130 / 55132 / 60144 | 44992 / 46000 / 48000 |

Both four-CPU debug boots passed all 418 self-tests and the complete
harness: x86-64 reported 133.5 s and AArch64 143.8 s. The x86 harness
retried once because firmware did not hand over within 30 s on the first
attempt; the successful attempt ran all tests. The benchmark itself took
132 ms on x86-64 and 137 ms on AArch64, with all nine topology/operation
cases checked. Logs: `out/lockdep-graph-bench-{x86_64,aarch64}.log` and
matching `-result.log` files. Debug LOCKDEP=0 and release kernels built
on both architectures (`out/lockdep-graph-bench-config-{x86_64,aarch64}.log`).
The pure core algorithms are unchanged. `git diff --check` passed.


## Interrupt writer serialization continuation

Base: `b6351212`. The next phases moved from measurements to a confirmed
interrupt concurrency defect: table mutation masked local IRQs but did
not exclude a writer on another CPU. Competing registrations could both
observe an empty slot and race over the alternating record and publication.
The dispatch count also paired atomic increments with a plain load;
diagnostic name lookup could race a reused record's name store.

Phase 1 adds a raw writer lock per vector, preserving early-boot use and
avoiding any global dispatch lock. Registration/removal hold it with local
IRQs masked, and never allocate, dispatch a handler, or wait for a grace
period under it. Dispatch still acquires one published record pointer and
takes no writer lock. Count reads and diagnostic name stores/loads are now
atomic. Names remain immortal and are samples, not registration handles.
NMI/#MC table mutation is explicitly unsupported; read-only NMI dispatch
and name lookup still take no writer lock.

Phase 2 compiles the actual `kernel/interrupt/interrupt.c` into a pthread
host test. For 64 rounds, four registrations compete while four other
threads dispatch the same vector; exactly one writer must win and handled
calls must use its matching function/argument. Unhandled calls are counted
too. After dispatchers join, four removers compete and exactly one wins.
A diagnostic reader overlaps all rounds, including record reuse. The test
checks exact final dispatch counts, wrong-function removal, and invalid
arguments. It runs in the regular ASan/UBSan suite and separately via
`make host-test-interrupt-tsan`.

Host IRQ masking is a no-op, so exclusion depends on the real slot lock.
The host grace-period function is a stub: joined dispatchers provide the
lifetime boundary, not a simulation of kernel quiescence. The existing
caller requirement to coordinate unregister, grace period and subsequent
record reuse is unchanged. This increment does not prove generalized
callback wait dependencies, arbitrary hardware entry interleavings, or
NMI-safe mutation.

Negative controls verified that the test exposes the defects: the original
source fails TSan on `interrupt_count` versus atomic dispatch increments;
a second temporary copy with atomic diagnostics retained but the writer
lock removed fails TSan on competing slot publication. Neither temporary
source is in the repository. Logs: `out/interrupt-writers-baseline-tsan.log`
and `out/interrupt-writers-mutation-tsan.log`. Corrected code passes TSan
(`out/interrupt-writers-tsan.log`) and the full ASan/UBSan host suite
(`out/interrupt-writers-host.log`).

Phase 3: both four-CPU debug boots passed all 418 self-tests and the full
boot harness: x86-64 in 128.6 s and AArch64 in 137.4 s. Existing breakpoint,
IRQ synchronization and x86 NMI snapshot tests remained enabled. Logs:
`out/interrupt-writers-{x86_64,aarch64}.log`. Release kernels built on both
architectures (`out/interrupt-writers-release-{x86_64,aarch64}.log`). The
interrupt architecture, design, API, invariant and testing descriptions
were reconciled with the actual record publication/lifetime protocol.
`git diff --check` passed.

## Thread mutex-stack snapshot continuation

Base: `e886d6d7`. Phase 1 extends the bounded held-stack reader to a
referenced thread's mutex stack. The common helper now takes an explicit
capacity, rejecting oversized counts before reading either stack. The
owning thread brackets atomic entry/count writes with a sequence; readers
make one attempt and reject busy or changed state. Preemption or migration
of the writer cannot make a reader wait. The caller must retain the thread
object throughout the read; the API cannot acquire lifetime from an
arbitrary pointer.

Phase 2 changes panic held-state output to snapshot the current CPU and
thread separately, or print unavailable. It no longer walks a partially
updated local stack directly. This adds an eight-byte sequence at the end
of `struct thread`, preserving existing member offsets, plus atomic writes
on debug mutex tracking updates. No isolated overhead comparison was made
for this increment. It does not add reentrant NMI writers, freeze mutex
owner fields, or provide a simultaneous global held-state view.

Phase 3 extends the sanitizer generation test to both 24-entry CPU and
eight-entry mutex capacities, including exactly sized source/output arrays
for bounds checks and refusal of a stopped writer. The kernel mutex test
checks trylock metadata, shifted removal, busy refusal, and empty state.
It also snapshots a referenced live thread through 1024 acquisition/release
rounds and keeps an extra reference across join to check its exited stack.

The full ASan/UBSan host suite and lockdep TSan target passed:
`out/lockdep-thread-host.log` and `out/lockdep-thread-tsan.log`. Both final
four-CPU debug boots passed all 418 self-tests and the full harness, in
146.4 s on x86-64 and 145.8 s on AArch64. The remote mutex test accepted
7591 and 6404 concurrent samples respectively, as well as checking the
initial pair and exited empty stack. Logs:
`out/lockdep-thread-final-{x86_64,aarch64}.log`. Both release kernels built
successfully (`out/lockdep-thread-release-{x86_64,aarch64}.log`).
The deliberate panic regressions also passed on both architectures (112.9 s
and 111.0 s), with both current-CPU and `kmain` empty-stack snapshots printed
after the backtrace. Logs: `out/lockdep-thread-crash-{x86_64,aarch64}-result.log`
and `out/{x86_64,aarch64}-debug-crash/boot-test-crash.log`.
`git diff --check` passed. The inventory marks this bounded snapshot work
complete while retaining simultaneous global snapshots as deferred.

## Dense graph measurement continuation

Base: `fc64d2c2`. Phase 1 extends the private graph benchmark to dense
acyclic components and adds the 320-class limit to both chain and dense
cases. Only subclass zero is active. Dense components contain every
forward edge: the full 320-node cycle graph has 51,040 edges, while the
two-component insertion/IRQ graphs have 25,440 before the proposed bridge.
Graph setup remains outside each timed operation and checks these counts.

Phase 2 validates every warmup and measured operation: expected verdict,
edge count, proposed-edge presence, cycle path length/endpoints and every
path edge, or the IRQ-conflict endpoints. The dense cycle case finds a
direct first-to-last path, unlike the chain's long truncated path. The
bridge cases exhaust the second component in the unsuccessful reverse
reachability check. No classes or dependencies enter the live validator.

Phase 3 measures all 24 topology/size/operation combinations on both
architectures, with two warmups and nine samples per case. Representative
320-node results below are guest nanoseconds (min / median / max):

| Topology | Operation | x86-64 | AArch64 |
|---|---|---|---|
| chain | insert | 25103 / 25103 / 28115 | 18000 / 19008 / 20992 |
| chain | cycle | 44181 / 45185 / 106437 | 32000 / 33008 / 34000 |
| chain | irq-bridge | 68280 / 69284 / 74305 | 52000 / 52992 / 54000 |
| dense | insert | 24098 / 24099 / 26107 | 18000 / 18000 / 19008 |
| dense | cycle | 2009 / 3012 / 7029 | 2000 / 2992 / 4000 |
| dense | irq-bridge | 66271 / 67276 / 69285 | 48992 / 50000 / 58000 |

Density is not a worst-case latency bound: these bitmaps skip visited
neighbors, and dense cycle rejection terminates on a direct edge. Small
samples can also fall below guest clock resolution (including zero);
there is no timing pass threshold. These QEMU measurements exclude class
lookup, held-stack processing, the raw lock, statistics and reporting.
They neither measure native overhead nor cover all 1,280 subclass nodes.
Complete acquisition, contention and worst-case bounds remain deferred.

The expanded benchmark passed in 511 ms on x86-64 and 487 ms on AArch64.
Release kernels built on both architectures; logs:
`out/lockdep-dense-release-{x86_64,aarch64}.log`. The production core and
host algorithms are unchanged in this increment.
Both four-CPU debug boots passed all 418 self-tests and the complete boot
harness: x86-64 in 129.7 s and AArch64 in 129.6 s. Full measurements and
validation logs are `out/lockdep-dense-{x86_64,aarch64}.log` and matching
`-result.log` files. `git diff --check` passed. The inventory marks the
dense subclass-zero measurements complete, retaining broader performance
coverage as deferred work.

## Full-capacity graph continuation

Base: `ac166445`. Phase 1 extends the private chain/dense benchmark to
all 1,280 class/subclass nodes, retaining the previous subclass-zero cases.
The full dense cycle graph contains 818,560 edges and the two-component
bridge graphs 408,960. All 30 cases validate every warmup and timed result;
setup and validation remain outside the timed operation.

Phase 2 checks class-wide IRQ usage explicitly. In the full chain, the
multi-source predecessor search reaches the proposed bridge from subclass
3 of the first class; in the dense graph it reaches from subclass 0.
Both descendant searches stop at subclass 0 of the last class. The cycle
cases check the expected truncated chain or direct dense path and verify
each returned edge. The host `dense-capacity` test fills every bitmap word,
uses a two-entry output path, exhausts a full dense traversal without a
matching usage label, and seeds the BFS queue with all 1,280 nodes. This
exercises full queue capacity and duplicate suppression under ASan/UBSan.

The full host sanitizer suite passed (`out/lockdep-full-host.log`). Both
release kernels built (`out/lockdep-full-release-{x86_64,aarch64}.log`).
The production core is unchanged. These are bounded-capacity workloads,
not a proof of worst-case execution time or complete acquisition costs.

Phase 3 measured both architectures under four-CPU QEMU. Full-capacity
results from the first runs are guest nanoseconds (min / median / max):

| Topology | Operation | x86-64 | AArch64 |
|---|---|---|---|
| chain | insert | 90125 / 105800 / 281152 | 64000 / 64992 / 214992 |
| chain | cycle | 184170 / 217476 / 331113 | 130000 / 136000 / 218000 |
| chain | irq-bridge | 258621 / 368338 / 952195 | 186992 / 194992 / 632000 |
| dense | insert | 90125 / 91105 / 98942 | 64992 / 68000 / 107008 |
| dense | cycle | 11755 / 11756 / 12735 | 10000 / 10992 / 14000 |
| dense | irq-bridge | 250784 / 322296 / 706309 | 179008 / 191008 / 238992 |

The benchmark passed in 1597 ms on x86-64 and 1201 ms on AArch64, below
the existing self-test budget. AArch64 passed all 418 self-tests and the
full harness in 147.0 s. The initial x86 run completed 418 tests with one
failure: `quiesce-straggler-idle` observed a kick instead of zero (15 ms).
It ran before the graph benchmark; neither its code nor the quiescence
implementation changed. The test assumes idle publication beats the
two-tick kick threshold. Host scheduling delay is a possible explanation,
not an established root cause. The graph benchmark itself passed all 30
cases. Logs: `out/lockdep-full-{x86_64,aarch64}.log` and matching
`-result.log` files. No test assertion or timeout was relaxed.

An isolated x86 rerun passed all 418 self-tests and the complete harness
in 130.9 s (`out/lockdep-full-x86_64-retry.log` and matching `-result.log`).
The idle test completed in 4 ms with no kick and the graph benchmark in
1241 ms. The initial failure did not reproduce; its cause remains open.
`git diff --check` passed. The inventory strikes out all-subclass capacity
measurements while retaining worst-case latency bounds and contention work.

## Kernel interrupt writer/IPI continuation

Base: `52d14a30`. Phase 1 adds `irq-writers`, a kernel integration test
for the existing per-vector writer serialization. Two pinned threads
rendezvous and race to register the same allocated vector. Exactly one
must succeed and the other must return `-EBUSY`. A real IPI dispatches
the winner; distinct handlers validate their own argument identity, the
vector and a non-null trap frame. Two threads then race to remove that
handler: one success and one `-ENOENT` are required.

Phase 2 checks reuse over 16 rounds. After joining removal threads, the
caller waits through `synchronize_irq`, checks exact handler/dispatch
counts and absent publication, then permits stack probe and alternating
record reuse. SMP writers run on distinct CPUs; UP uses the same yielding
rendezvous and self-IPI, without claiming simultaneous contention.
Allocation/rendezvous failure joins any created workers and cleans up
publication through a grace period. IPI timeout explicitly fails stop
while retaining the vector, handler and live probes, because delayed
delivery cannot safely return to freed storage.

Dispatch starts after the registration race, so this test does not claim
publication/dispatch overlap; the actual-source host test covers that.
The existing `irq-sync` test separately holds a handler active during
unregister. Arbitrary entry interleavings, NMI/#MC mutation and generalized
callback dependencies remain deferred. The production interrupt path is
unchanged in this increment.

Phase 3: both four-CPU debug boots passed all 419 self-tests and the full
harness (x86-64 142.1 s, AArch64 138.2 s). The new test passed in 64 ms
and 66 ms respectively, with writers on CPUs 2 and 3. Logs:
`out/irq-writers-kernel-{x86_64,aarch64}.log` and matching `-result.log`.
Both release kernels built successfully:
`out/irq-writers-kernel-release-{x86_64,aarch64}.log`.
The single-CPU branch also completed all 16 rounds on each architecture
(x86-64 13 ms, AArch64 23 ms). Failure cleanup and timeout retention were
reviewed, not fault-injected in this increment. Host sanitizer tests were
not repeated: neither the production interrupt source nor its host model
changed; their earlier evidence remains in the writer-serialization section.
Both single-CPU boots passed all 419 self-tests and the complete harness:
x86-64 in 118.8 s and AArch64 in 124.4 s. Logs:
`out/irq-writers-kernel-up-{x86_64,aarch64}.log` and matching `-result.log`.
`git diff --check` passed. Interrupt invariant/testing and quiescence test
documentation now state the complementary host/kernel coverage, and the
inventory strikes out this integration regression with its scope limits.

## PR #305 review follow-up

Both initial review findings were valid. The interrupt testing guide now
states that real/self-IPIs already exist; the missing coverage is a test
that deliberately dispatches a bound vector without a registered handler.
The affected interrupt documentation was searched for repeated future-LAPIC
claims; none remain.

The mutex snapshot test previously fell through its two-second completion
deadline into an unbounded join. It now explicitly panics if the worker
has not acknowledged completion, retaining the creator reference and the
stack-owned mutex probe. It joins only after the acknowledgement. This
covers a worker stalled in its tested mutex operations; it does not make
the scheduler or thread-exit implementation universally timeout-safe.

A separate temporary clone replaced the worker's completion publication
with an endless yielding loop. Its x86-64 image reached the exact new
completion-timeout panic and the expected failure exit in 14.8 s total
boot time, rather than hanging in join. The boot harness used a targeted
panic marker for this injection, with its ordinary failure-exit check;
it did not require the normal deliberate page-fault marker. Logs:
`out/pr305-timeout-result.log` and `out/pr305-timeout-x86_64.log`.
The injected source and image are outside the PR; normal source/images
were never modified by this experiment.

The normal four-CPU AArch64 boot passed all 419 self-tests and the full
harness in 156.3 s (`out/pr305-review-aarch64.log`). The concurrent x86
run passed the changed mutex test but failed `smp-ticks` (tick count),
`lockup-sample` (response mask), and `lockup-hard` (answered mask), all
before the mutex test. These source paths are unchanged. The normal
boots overlapped the isolated injection build/run; host load is a possible
factor, not a proven cause. The failures are retained in
`out/pr305-review-x86_64.log`; no test threshold or assertion was changed.
The isolated x86 rerun completed in 137.1 s with one failure remaining:
`lockup-sample` did not receive the required CPU response (107 ms).
`lockdep-mutex` was among the 418 passing tests. This repeated failure
remains unresolved; the x86 full suite is not reported green for this
review follow-up (`out/pr305-review-x86_64-retry.log`).

The pre-fix AArch64 CI job 111238579811 in run 37135325697 likewise
passed all 419 self-tests, then exceeded the 180-second boot deadline
before the rc/interactive-shell completion markers. Its log is retained
at `out/pr305-ci-aarch64-job.log`; this is separate from either review
finding. `git diff --check` passed for the review fixes.

## Lockup sampling deadline investigation

The repeated x86 failure was the missing-spinner bit in the returned
mask, not the previously repaired interrupted-stack/leaf-PC check. In
`out/pr305-review-x86_64-retry.log` all remote CPUs were omitted, while
later output showed their ticks and subsequent NMI samples. Those logs
lack publication timestamps at the failing deadline, so they do not
prove whether every original omission was this race or truly late
interrupt delivery. An instrumented normal boot and 32 additional
unforced samples passed during investigation; that does not discharge
the reported failures.

Inspection found a concrete stale-observation race in both sampling
APIs. They checked response sequence(s) before reading the deadline
clock. If the reporter was interrupted or its host vCPU descheduled in
between, other CPUs could publish their answers before it resumed, but
the expired clock check returned the earlier incomplete mask. The
single-target form could likewise return false for an available answer.

Both loops now read deadline expiry first, then acquire-load response
publication before deciding to return. This preserves one original
five-millisecond deadline, adds no retry window or additional request,
and leaves truly absent targets absent. A final bounded response sweep
can accept publications made while the reporter was delayed; the mask
represents available samples, not a delivery-latency guarantee. The
`lockup-sample` test also copies fields only for an acknowledged response,
avoiding a read racing an unfinished responder on failure.

`tools/lockup-deadline-probe.py` builds a temporary clone, gates the first
sample's responders, then lets them finish during a simulated pause in
the reporter's deadline read. It resumes after the original deadline.
With `--old-order`, both x86-64 and AArch64 fail specifically at the same
missing-spinner mask assertion; with the corrected order, both pass the
existing mask, stack and timestamp assertions. Both modes use real
NMI/IPI response publication, and guard expiry is rejected rather than
counted as reproduction. The probe stops at a named panic and checks
that exact outcome and failure exit; this is targeted evidence, not a
full-suite pass. Its all-CPU case exercises the observed failure; the
single-target loop received the same ordering correction by inspection.

Probe logs: `out/lockup-deadline-{x86_64,aarch64}-{old,fixed}/boot.log`
and corresponding top-level `-result.log` files. The old/fixed x86 probes
completed in 9.6/8.3 s; both AArch64 probes completed in 11.4 s. No code
that gates responders is linked into normal kernels. The production
change does not guarantee that an unscheduled remote vCPU answers within
five milliseconds; genuine late delivery can still yield an absent bit.

Final normal four-CPU boots passed all 419 self-tests and complete
harnesses: x86-64 in 151.1 s and AArch64 in 160.3 s. The existing
`lockup-sample-busy` test still verified one deadline and genuinely
unanswered masked targets (observed waits 5035/5200 us). The x86 network
harness recovered on attempt 2 of 3, a recorded QEMU reset sighting.
Logs: `out/lockup-deadline-final-{x86_64,aarch64}.log` and corresponding
`-result.log` files. Both release kernels built; logs:
`out/lockup-deadline-release-{x86_64,aarch64}.log`. Probe syntax checking
and `git diff --check` passed.

## Unhandled IPI continuation after PR #305

Base: merged PR #305 (`9e7c2274`). Phase 1 closes the ordinary
unregistered-vector integration gap: `irq-unhandled` allocates and binds
a vector without a handler, sends two rounds of IPIs to every online CPU
(including itself), and requires exact dispatch counts. A grace period
after each observed round finishes entry/dispatch/EOI before the next
round or registration change. A handler is then installed on the same
vector and its self-IPI identity/count checked before synchronous removal.
The test fails stop on missing delivery or unsafe registration/cleanup
state, preserving the allocated vector and any live stack probe.

Phase 2 fixes a related confirmed data race found by inspection: both
architectures incremented their shared unhandled-interrupt warning total
with a plain read/modify/write. Concurrent CPUs could lose increments.
They now use atomic fetch-add and log its returned total. This is a
diagnostic count, so relaxed ordering is sufficient. The log order itself
is not necessarily numeric: CPUs serialize their warning output after
receiving their distinct counts. No dispatch lock or new allocation was
introduced.

This is the generic unregistered non-exception path, not the LAPIC's
hardware-spurious vector, GIC spurious INTIDs, or fatal exception policy.
The test's ordinary IPI delivery and reuse do not establish NMI mutation
safety or arbitrary nested-handler behavior. The inventory was reconciled
with PR #305's merge and marks this continuation separately.

Phase 3: final four-CPU debug boots passed all 420 self-tests and the
full harness (x86-64 133.1 s, AArch64 138.2 s). `irq-unhandled` took
24/22 ms. Each controlled test segment had eight unhandled warnings on
one allocated vector and eight unique consecutive diagnostic totals;
the log order was allowed to differ from increment order. Logs:
`out/irq-unhandled-final-{x86_64,aarch64}.log` and matching `-result.log`.
Both release kernels built successfully:
`out/irq-unhandled-release-{x86_64,aarch64}.log`.
Single-CPU boots also passed all 420 self-tests and the full harness:
x86-64 in 131.7 s and AArch64 in 136.7 s. Each controlled log segment
contains exactly two unhandled warnings on the test vector, followed by
successful handled reuse. Logs: `out/irq-unhandled-up-{x86_64,aarch64}.log`
and matching `-result.log` files. The UP test took 12/1 ms respectively.

A temporary x86 UP negative control skipped `arch_irqc_eoi` only for
unregistered vectors. The first self-IPI produced one unhandled warning;
the second remained pending and the exact `irq-unhandled` delivery-timeout
panic fired. The targeted harness required that panic and the failure
exit, completing in 8.1 s; it was not a full-suite pass. Logs:
`out/irq-unhandled-noeoi.log`, `out/irq-unhandled-noeoi-result.log` and
`out/irq-unhandled-noeoi-build.log`. The clone was separate from normal
source/images and its mutation is not committed. This tests the missing
EOI failure and retention path; other cleanup failures were reviewed,
not injected. `git diff --check` passed.

## Interrupt boundary continuation after PR #305

This increment follows the local unhandled-IPI work. Phase 1 closes the
explicit out-of-range dispatcher testing gap using the actual
`kernel/interrupt/interrupt.c` under the existing host shims. Tests use
both a 16-vector architecture and the full 1344-slot capacity, dispatch
the last valid slot, and require `panic_frame` for the first invalid
vector and `UINT_MAX`, preserving the supplied frame pointer and exact
diagnostic. A NULL frame is checked separately. Oversized architecture
counts of 1345 and `UINT_MAX` must call the initialization panic. Panic
interception is confined to the single-threaded setup phase; the table
is reinitialized after each intercepted initialization failure.

Phase 2 checks synchronous-removal wrapper behavior: invalid vectors,
NULL/wrong functions, and absent registrations fail without a grace
period; failed mutations preserve a live handler. Both successful sync
variants unpublish before the stub grace period and count synchronization
exactly once afterwards. Dispatch counts remain cumulative through
unregistration and record reuse. These assertions supplement the existing
64-round competing-writer/dispatcher/diagnostic-reader test.

Phase 3 validation passed `gmake host-test` (all host ASan/UBSan tests and
boot-harness Python checks) and `gmake host-test-interrupt-tsan`. Logs:
`out/interrupt-boundaries-host.log` and
`out/interrupt-boundaries-tsan.log`. Three temporary ASan/UBSan source
mutations each failed at the intended assertion: dispatch `>=` changed
to `>`, disabled oversized-init guard, and synchronization even after
failed removal. Logs: `out/interrupt-boundaries-negative-dispatch-off-by-one.log`,
`out/interrupt-boundaries-negative-oversized-init.log`, and
`out/interrupt-boundaries-negative-failed-removal-sync.log`. Mutated
sources and binaries were isolated in a temporary directory and removed.

Production kernel code is unchanged in this increment; no additional
QEMU boots or architecture builds were run. The host panic shim validates
the dispatch/init decision and diagnostic, not panic rendering or CPU
shutdown. Stub synchronization checks wrapper ordering, not real epoch
completion. The previous increment's cross-architecture integration
evidence and the remaining NMI/callback concurrency gaps still apply.
The inventory and interrupt test/invariant documents now reflect this
bounded coverage. `git diff --check` passed.

## Deterministic graph-search bounds continuation

Phase 1 adds host-only work accounting to the existing shared graph
helpers, enabled only by `tests/host/test_lockdep.c`. It counts visited
clears, seed checks, enqueue/dequeue operations, adjacency-word loads and
parent reconstruction. Counts accumulate across composite IRQ checks.
Kernel scratch layout and generated operations exclude all of this
instrumentation. The search algorithm and its decisions are unchanged.

Phase 2 adds full-capacity `search-work` regressions. The visited-on-enqueue
rule limits each search to 1280 queue entries and at most 25,600 adjacency
word loads. Dense and cyclic complete traversals attain the read bound;
a 1280-node chain attains the 2,559-step reconstruction bound, including
when output capacity is zero. Tests also cover an isolated target,
zero/all predecessor sources, self reachability, fresh and warmed usage,
usage conflicts, and one/two-search IRQ-edge checks. Documentation derives
the bound from the implementation, with a conservative 100-search ceiling
for acquisition decision paths (four usage searches plus up to three for
each of 32 held entries). This excludes class/profile scans, statistics,
diagnostics and lock contention; it is not a latency guarantee or a claim
that every individual maximum is jointly attainable.

Phase 3 validation passed the full `gmake host-test` ASan/UBSan and Python
harness suite, `gmake host-test-lockdep-tsan`, and debug kernel builds for
x86-64 and AArch64. Logs: `out/lockdep-search-bounds-host.log`,
`out/lockdep-search-bounds-tsan.log`, and
`out/lockdep-search-bounds-{x86_64,aarch64}-build.log`.

A temporary mutation repeated each usage-search adjacency-word scan.
Graph answers and the existing closure-oracle test remained correct,
while the new general read bound and exact-count assertions failed.
Log: `out/lockdep-search-bounds-negative.log`. Optimized freestanding
wrappers for reachability, usage search, IRQ-edge checking, usage marking
and scratch size produced identical before/after assembly on both targets
with instrumentation disabled. The comparison result is recorded in
`out/lockdep-search-bounds-controls.log`; this comparison covers those
wrappers, not whole kernel binaries. Temporary sources/binaries were
removed. No additional QEMU boots or release builds were run for this
host-instrumentation increment. `git diff --check` passed.

The inventory marks deterministic search work as covered and retains
wall-clock maxima, first-acquisition timings, contention, priority
inheritance and native measurements as open. The design's stale
depth-first-search description now correctly says breadth-first search.

## First-acquisition measurement continuation

Phase 1 adds `lockdep-first-bench`, using the real public spinlock,
irqsave, nested-spin and mutex acquisition paths against the live graph.
Each path has three fresh-name samples and a subsequent reuse sample on
each object. The clock stops after acquisition, while ownership is still
held; release, validation, statistics snapshots and output are excluded.
The nested case times two acquisitions together. An empty branch/clock
control is reported separately, without subtraction. Class caches start
empty, become populated with lockdep enabled and remain stable on reuse.
Ownership/release checks apply with either setting. Class-count growth
and a nested new-edge search are checked outside timing; global deltas
are lower bounds because other CPUs keep running.

The test adds 18 classes, using static names and leaving all graph state
intact. Three samples deliberately limit consumption of the 320-class
pool; no graph reset or capacity increase was introduced. Final graph
dumps reached 302 classes on x86-64 and 301 on AArch64. This is a
first-use measurement, not a CPU-cache-cold guarantee. The graph grows
across samples, and each path reports its before/after class range.

Phase 2 ran matched debug LOCKDEP=1/0 four-CPU boots on both architectures,
using separate output trees and the same QEMU settings. All four passed
421 self-tests and the complete boot harness. Enabled x86-64/AArch64 runs
completed in 146.9/150.3 s; disabled runs in 121.1/127.8 s. Logs:
`out/lockdep-first-{x86_64,aarch64}-{on,off}.log` and corresponding
`-result.log` files. The enabled benchmark itself took 16/38 ms.
These total boot durations are validation outcomes, not overhead estimates.

Observed median guest nanoseconds per acquisition interval (nested is a
pair), each from only three samples:

| Architecture | Path | Enabled first | Enabled reuse | Disabled first | Disabled reuse |
|---|---|---:|---:|---:|---:|
| x86_64 | spin | 66105 | 1002 | 1006 | 0 |
| x86_64 | irqsave | 5008 | 2003 | 1007 | 1006 |
| x86_64 | nested | 25040 | 3004 | 1007 | 1007 |
| x86_64 | mutex | 10016 | 3005 | 1007 | 1006 |
| aarch64 | spin | 60992 | 992 | 1008 | 0 |
| aarch64 | irqsave | 4992 | 1008 | 0 | 0 |
| aarch64 | nested | 25008 | 2992 | 1008 | 0 |
| aarch64 | mutex | 9008 | 3008 | 992 | 992 |

Full min/median/max and empty controls are in the logs. Short intervals
frequently meet guest-clock granularity and can read zero; do not derive
ratios from these values. QEMU translation, host scheduling, interrupts
and other CPUs affect the observations. Some builds/boots overlapped on
the host; this was not an isolated performance experiment. No timing
threshold determines success. These data do not establish native cost,
worst-case latency, contention/PI overhead or a graph-size sweep.

Phase 3 used a temporary clone with sample indices removed from class
names. The second spin sample then reused an existing class and the fresh
class-growth assertion failed. The targeted harness required the exact
`FIRSTPROBE: result=0 reason=check failed: ok` panic and failure exit;
it passed that negative-control check in 15.3 s, not a full suite.
Logs: `out/lockdep-first-negative/{build,boot}.log` and
`out/lockdep-first-negative-result.log`. The mutation is not committed.
Both release kernels also built successfully; logs:
`out/lockdep-first-{x86_64,aarch64}-release-build.log`. Log parsing checked
all ten rows per boot, ordered min/median/max and disabled class counts.
`git diff --check` passed. Host tests were not repeated for this kernel
benchmark-only increment. The inventory now distinguishes this completed
live-graph baseline from remaining graph-size, contention and native work.

## Contention-test failure cleanup after PR #306

Base: merged PR #306 (`6201baff`). Phase 1, while investigating remaining
contention coverage, found a concrete cleanup bug in the existing
`lockdep-contention` self-test. Its callback-window `CHECK` ran immediately
after acquiring the contended spinlock. If the timer had not fired, the
macro returned without unlocking, cancelling its stack timer or joining
the holder. This leaked preemption disable and left a published timer in
expired stack storage. The readiness assertion also returned without
draining the created worker. The successful path had an unbounded join.

Phase 2 preserves the original test outcome while making cleanup precede
any returning failure. IRQ-state validation happens before worker/timer
publication. Atomic handshake flags are reset before creation and read
atomically. After acquisition the timer observation is saved, the lock is
released, the timer synchronously cancelled, and holder exit completion
awaited with a one-second guard before joining. Only then is the saved
observation checked. A late callback cannot retroactively satisfy the
test. Readiness or exit timeout fails stop with the creator reference
retained, rather than returning or entering an unbounded join. Selection
of another CPU now defaults to self so the skip also covers no other
online CPU. This does not bound a spin primitive waiting on a stopped
owner or guarantee the callback window under arbitrary host delays.

Phase 3 adds `tools/lockdep-contention-probe.py`, which builds temporary
clones and checks exact fatal diagnostics and emulator failure exit.
The missed-timer mode delays the callback to five seconds; `--old-order`
restores the early assertion before cleanup. Both architectures reproduce
the old failure state (`held=1 cancelled=0 joined=0 irq=1 preempt=1`) and
validate the fixed failure state (`held=0 cancelled=1 joined=1 irq=1
preempt=0`). The fixed test still reports its missed callback. Targeted
old/fixed probe times were 16.8/12.5 s on x86-64 and 15.6/19.0 s on AArch64.
Separate x86 probes withhold readiness or exit and require the named
timeout panic; they passed in 13.2/13.6 s. No probe is a full-suite pass.
No delayed timer, stalled worker or probe panic enters normal builds.

Probe artifacts: `out/lockdep-cont-probe-{x86_64,aarch64}-missed-timer-{old,fixed}/`
and `out/lockdep-cont-probe-x86_64-{readiness,exit}-fixed/`, each with
`build.log` and `boot.log`; top-level `-result.log` files record outcomes.
Both release kernels built, with logs at
`out/lockdep-cont-cleanup-{x86_64,aarch64}-release.log`. Python syntax
compilation and `git diff --check` passed. Host tests were not repeated
because the change is confined to the kernel self-test and probe tool.

The x86-64 four-CPU full boot passed all 421 self-tests and the complete
harness in 139.7 s (`out/lockdep-cont-cleanup-x86_64{,-result}.log`). The
initial AArch64 run passed the modified contention test in 21 ms but
failed the harness: `syscall-fuzz` took 13,055 ms against its 8,000 ms
budget and emitted the no-progress watchdog diagnostic at 8,005 ms.
The watchdog showed the fuzzing process running and logging syscalls;
the fuzzer later reported success and the kernel shut down normally.
Probe builds/boots overlapped on the host. This is a recorded validation
failure, not evidence establishing host load as its cause. Its artifacts
are `out/lockdep-cont-cleanup-aarch64{,-result}.log`; the budget and
watchdog checks were not changed.

An isolated rerun of the unchanged AArch64 image passed all 421 self-tests
and the full harness in 130.5 s. `lockdep-contention` remained 21 ms and
`syscall-fuzz` took 3,840 ms without a watchdog marker. Logs:
`out/lockdep-cont-cleanup-aarch64-retry{,-result}.log`. The initial
budget failure remains recorded; one successful rerun does not establish
its cause. The inventory now reflects PR #306's merged status and this
local cleanup continuation separately.

## Queued mutex acquisition continuation

Phase 1 adds `lockdep-mutex-bench`, an equal-workload debug LOCKDEP=0/1
measurement of the public mutex acquisition path under verified queue
contention. The owner holds a private mutex before releasing a worker's
start gate, yields until the real wait queue is nonempty, holds for another
1 ms, then releases. The waiter measures `mutex_lock` on its pinned CPU
through ownership, checks protected-data publication and ownership, then
unlocks. The owner drains exit completion before joining or checking any
failure. Queue and exit guards are one second. An exit timeout retains
the worker reference and stack probe and fails stop. Two warmups precede
nine samples; the same mutex and two lock classes are reused. Thread
creation, validation, unlock and join are outside the timed acquisition.
The UP case yields to a waiter on the same CPU and uses the same queue
protocol. This proves the slow path was entered without assuming that a
fixed delay schedules the waiter; it does not prescribe context switches.

The result includes controlled holding time, scheduling and wakeup, not
only lockdep instructions. The hold is never subtracted and no performance
threshold or overhead ratio determines success. Waiters use the default
priority; no priority donation workload is deliberately induced. Broader
mutex workloads, contended spin, graph-size sweeps, PI and native costs
remain open.

Phase 2 exercised failure cleanup in temporary x86 clones. Withholding
the start gate until the queue guard expires must return failure only
after an unlocked mutex, empty queue, joined worker and normal IRQ and
preemption state are observed. Withholding exit after worker unlock must
produce the explicit waiter-exit retention panic. Both targeted harnesses
passed in 14.7/13.9 s, requiring exact diagnostics and failure exit.
Artifacts: `out/lockdep-mutex-bench-probe-{queue,exit}/{build,boot}.log`
and corresponding top-level `-result.log` files. Neither is a full-suite
pass, and the injections are not committed.

Full validation also exposed a separate pre-existing fixture lifetime
bug. `elf-text-ro` and `elf-data-private` still ran `init --block`, despite
the shared-text fixture documentation requiring `--spin`. That program
reads one console byte and exits with status 5; the process reference
does not retain its address space past exit. The initial AArch64 run
reported a keyboard-input mismatch, then the later text fixture exited
with status 5 before `vm_user_protect(p->space, ...)`, causing a kernel
NULL-space fault at offset 0xd0. Both fixtures now use the existing
`--spin` mode and retain their explicit kill/wait/put cleanup. Protection,
zero-tail and private-data assertions are unchanged. The keyboard mismatch
preceded creation of the text fixture; this fix prevents residual console
input from ending the fixture, and does not claim to explain that mismatch.

Initial validation failures are retained in
`out/lockdep-mutex-bench-{x86_64,aarch64}-on{,-result}.log`: x86's
`process-user` exceeded its internal 15-second bound (19,054 ms); AArch64
had the keyboard mismatch and NULL-space panic. After the fixture fix,
an isolated AArch64 boot passed the benchmark and both ELF inspections,
but `syscall-fuzz` took 10,135 ms against its 8,000 ms budget and emitted
the watchdog marker (`out/lockdep-mutex-bench-aarch64-on-fixed{,-result}.log`).
This budget failure was also seen before this increment (recorded above);
budgets and watchdog checks were not relaxed. Later matrix runs are
serialized, and their results are recorded separately below.

Phase 3's final serialized matrix passed all 422 self-tests and the full
harness in all six configurations. Guest nanoseconds for the nine queued
acquisitions, including the controlled 1 ms hold:

| Architecture | Lockdep | CPUs | Min | Median | Max | Full boot seconds |
|---|---|---:|---:|---:|---:|---:|
| x86-64 | on | 4 | 1066412 | 1070426 | 1078451 | 133.5 |
| x86-64 | off | 4 | 1038246 | 1040249 | 1048258 | 110.9 |
| AArch64 | on | 4 | 1060000 | 1064992 | 1156992 | 142.7 |
| AArch64 | off | 4 | 1030000 | 1032000 | 1034992 | 122.2 |
| x86-64 | on | 1 | 1046628 | 1047628 | 1056634 | 115.8 |
| AArch64 | on | 1 | 1046992 | 1049008 | 1055008 | 117.5 |

Logs: `out/lockdep-mutex-bench-{x86_64,aarch64}-{on,off}-final.log`,
`out/lockdep-mutex-bench-{x86_64,aarch64}-on-up.log`, and corresponding
`-result.log` files. A parser verified one measurement row per boot, nine
samples, configured hold, enabled state, placement, ordered min/median/max,
422-test success and full harness success. Both release kernels built
(`out/lockdep-mutex-bench-{x86_64,aarch64}-release-result.log`). No host
suite was repeated for these kernel-test-only changes. Final green boots
do not erase the earlier failures or establish their timing-related causes.

A targeted AArch64 console-input comparison also reproduced the fixture
cause independently of the initial keyboard mismatch. Temporary clones
run the text-inspection fixture first, inject `x\n` into the console, and
wait 100 ms for child exit. The old `--block` mode exits with status 5 and
a NULL address space; the fixed `--spin` mode stays alive and completes
the original protection and zero-tail checks before normal kill/wait/put
cleanup. The harness requires the exact old-state panic or the successful
fixture marker plus final result panic, so a skipped test cannot pass the
fixed control. These expected-failure probes passed in 9.3/9.4 s; they are
not full-suite boots. Logs: `out/elf-fixture-probe-final-{old,fixed}/{build,boot}.log`
and `out/elf-fixture-{old,fixed}-final-result.log`. The injected input,
early test placement and probe panics are not committed. Temporary probe
setup/build failures were corrected before these successful runs; they
provided no regression evidence. `git diff --check` passed.

## Continuation: same-CPU raw-lock re-entry and panic output

Phase 1 replaces the validator's exchange-and-spin word with an owner
word: zero means free, otherwise CPU plus one. The successful acquire CAS
publishes ownership in the same operation; a separate owner store would
leave an NMI window. Local IRQ masking prevents migration until the
release store. A failed CAS observing this CPU as owner invokes the exact
`lockdep: graph raw lock re-entry on CPU ...` panic without releasing or
stealing the interrupted owner's lock. Other CPUs keep the usual waiting
behavior. This adds no table, class, lock-object field or release-build
validator cost. It does add CPU identification and a failed-CAS ownership
comparison to enabled builds; existing measurements are not a quantified
performance claim for this new implementation.

Phase 2 found and corrected a prerequisite in fatal diagnostics. The
initial x86 NMI and direct-statistics probes timed out before printing the
diagnostic. Panic already bypassed the console lock, but `kprintf` still
acquired the tracked log-ring lock. Bypassing the ring alone still timed
out: a debugger captured the first panic at the intended raw-lock re-entry
and the second panic inside `vcon_write`'s tracked `virtio-console` lock.
Repeated recursion eventually exhausted the IST stack and corrupted the
raw word. The captured stacks are in `out/lockdep-reentry-lldb{,2}.log`;
initial failures are in `out/lockdep-reentry-{direct,nmi-acquire-busy,
nmi-acquire-busy-fixed}-result.log`. Diagnostic boot runs with debugger
stops are investigation artifacts, not test passes.

The console's irreversible panic-mode flag now uses atomic access and an
additive `console_in_panic_mode` module export. In that mode logging skips
ring writes and the VirtIO console returns before touching device/queue
locks. Serial and framebuffer output remains available, with neither a
new sink layout nor an ABI version change. Fatal output intentionally no
longer appends to the ring or VirtIO transport; the ring is not a frozen
failure-time snapshot. This fixes actual tracked acquisitions on the
panic output path instead of suppressing raw-lock reports or disabling
validation globally. General sink faults, sink-list lifetime during
catastrophic failure and a VirtIO panic transport remain separate concerns.

Phase 3 adds `tools/lockdep-reentry-probe.py`. Isolated builds use the
working sources and a fresh retained output directory, avoiding dependency
files that refer to deleted temporary clones. Direct mode exercises normal
statistics and public spin acquisition under the held raw lock on both
architectures. x86 NMI mode first completes two software interrupt checks,
then injects the operation on real APIC delivery. Busy mode interrupts an
unfinished held-stack update and requires the panic's explicit unavailable
snapshot. Ring mode injects panic while holding the actual log-ring lock,
with lockdep enabled or disabled. Probes require the exact diagnostic,
correct context, completed panic output and failure exit; an unrelated
panic or timeout cannot pass. Old-lock and old-ring controls restore the
unsafe implementations and deliberately fail that same harness. One ring
probe setup initially omitted the panic declaration; its compile failure
was corrected before the runtime comparisons and is not regression evidence.

This increment detects and terminates same-CPU raw-lock recursion. It does
not make arbitrary NMI/#MC tracked acquisitions supported, make held-stack
writers reentrant, detect cross-CPU raw-lock cycles, bound waits on stopped
owners, or add AArch64 NMI delivery. The source header and concurrency
section no longer claim unrestricted reentrancy safety. Read-only NMI
snapshot tests remain part of normal boots.

The ten corrected expected-panic probes passed:

| Probe | x86-64 seconds | AArch64 seconds |
|---|---:|---:|
| Direct statistics re-entry | 12.0 | 15.8 |
| Direct spin acquisition, busy held stack | 11.9 | 15.4 |
| Real NMI statistics re-entry | 5.2 | unsupported |
| Real NMI spin acquisition, busy held stack | 5.2 | unsupported |
| Held-ring panic, LOCKDEP=1 | 5.5 | 8.8 |
| Held-ring panic, LOCKDEP=0 | 5.2 | 8.6 |

The x86 old-lock NMI control timed out and failed the harness in 34.1 s;
the old-ring LOCKDEP=0 control likewise failed in 34.6 s. Both reached
their explicit probe-armed markers but lacked the required completed panic
report. These are successful negative-control observations, not passing
boots. Final probe result logs are
`out/lockdep-reentry-{x86_64,aarch64}-{direct-stats,direct-acquire,ring-ld1,ring-ld0}-result.log`,
`out/lockdep-reentry-nmi-{stats,acquire-busy-final,old-lock}-result.log`,
and `out/lockdep-reentry-ring-old-ld0-result.log`. Each names the retained
`run-*` directory with its image, build and boot logs. No injected panic or
handler change is present in the production kernel source.

The final serialized normal-boot matrix passed all 422 self-tests and the
full harness in every configuration. No normal boot required a retry or
relaxed budget in this increment:

| Architecture | Lockdep | CPUs | Full boot seconds |
|---|---|---:|---:|
| x86_64 | on | 4 | 132.7 |
| x86_64 | on | 1 | 114.4 |
| x86_64 | off | 4 | 107.4 |
| aarch64 | on | 4 | 133.7 |
| aarch64 | on | 1 | 119.1 |
| aarch64 | off | 4 | 122.0 |

Logs are `out/lockdep-reentry-{x86_64,aarch64}-{on-smp,on-up,off-smp}.log`
and corresponding `-result.log` files. A parser checked the exact 422-test
success marker, full harness pass and absence of kernel panic in all six
logs. Both release kernels built successfully, and `gmake host-test`
passed the existing ASan/UBSan suites and boot-harness unit tests. These
host suites do not execute the new kernel raw-lock implementation; that
boundary is exercised by the kernel probes and normal boots. Release and
host results are `out/lockdep-reentry-{x86_64-release,aarch64-release,host}-result.log`.
`out/lockdep-reentry-validation-progress.log` records the serialized run,
including the corrected ring-probe setup failure. Python syntax validation
and `git diff --check` passed. The pre-fix panic-path failures remain
recorded above; final green results do not erase them.

## PR #307 follow-up: NAT expiry-test completion race

The x86-64 harness-retry job in CI run `37150697619` failed
`net-nat` at `ns1.entries == 0 && ns1.expired > ns0.expired`, after
1,323 ms. The failure is the same assertion recorded in the September 20
flake history. This run did not print the counter values at failure;
the mechanism below is established by source inspection and a controlled
reproduction of that assertion, rather than inferred from duration alone.

`nat_flush()` clears entries but leaves lifetime statistics intact. The
flood wait incorrectly compared absolute `out_new + out_drop_share`
against its 264 injections. Earlier UDP, TCP, ICMP and ICMP-error setup
had already created four mappings. Thus 260 flood outcomes could satisfy
the wait while four packets were still pending. After the test aged the
existing entries using a future timestamp, those packets could create
fresh entries before the statistics check. Periodic aging only removes
entries and cannot create a mapping. This is a test completion race; the
production aging implementation requires no change.

The wait now subtracts the pre-flood baseline, checks each injection, and
asserts exactly 264 outcomes before aging. Its existing 200 × 10 ms polling
bound and quota/expiry assertions remain. The completion sum accounts for
NAT state changes, not for all downstream transmission work. The success
message now describes the actual per-guest quota being tested.

`tools/nat-expiry-probe.py` builds isolated clones and pauses the final four
real receive packets before entering NAT translation, without holding a
NAT lock or receive-hook quiescence section. It establishes the state
baseline=4 / completed=260 / pending=4 before evaluating the wait. The old
predicate exits immediately; after aging, releasing the tail creates four
entries and fails the original expiry assertion. The fixed predicate
refuses that state, releases the tail and accounts for all 264 outcomes;
the original expiry assertion then passes with zero entries. A stalled
control never releases the tail and requires the new completion assertion
to fail before aging. Exact diagnostics and panic exit are required;
unrelated panics or gate timeouts cannot pass. Production sources contain
none of these injected gates or early test placement.

Both old/fixed comparisons passed on x86-64 and AArch64; the stalled
control passed on x86-64. Logs and retained image directories are named by
`out/nat-expiry-{x86_64,aarch64}-{old,fixed}-result.log` and
`out/nat-expiry-x86_64-stalled-result.log`. The first x86 fixed probe
reported successful test completion and zero entries, but the harness
incorrectly expected `reason=none` rather than the runner's empty success
reason. The probe now normalizes successful reasons; the corrected probe
passed. The initial harness failure remains in
`out/nat-expiry-x86_64-fixed-initial-result.log` and was not counted as a
passing run. The historical flake entries and network test documentation
now record the established cause and correction.

Full-boot follow-up validation:

- x86-64 `gmake -j4 ARCH=x86_64 test-harness-retry` passed all 422
  self-tests and the complete harness (134.7 s). The network harness
  recovered on attempt two after the deliberately broken first attempt.
  Log: `out/nat-expiry-x86_64-hbreak-result.log`.
- AArch64 `gmake -j4 ARCH=aarch64 test-chaos` completed with one failure:
  `irq-route`'s existing `hits >= 5` fixed-interval interrupt count
  (`schedtest.c:282`, 71 ms). `net-nat` passed in 1,358 ms and
  `net-nicbench` in 2,127 ms. This is a failed full run, preserved in
  `out/nat-expiry-aarch64-chaos-result.log`; it does not establish a fix
  for the earlier CI `net-nicbench` timing failure. The IRQ sighting is
  also recorded in the flake history. No IRQ or benchmark checks changed.
- AArch64 standard `gmake -j4 ARCH=aarch64 test` passed all 422
  self-tests and the complete harness (136.9 s); `net-nat` passed in
  1,426 ms. Logs: `out/nat-expiry-aarch64-normal-result.log` and
  `out/nat-expiry-aarch64-normal.log`. This pass does not replace the
  failed chaos result above.

Python syntax compilation and `git diff --check` passed. The production
NAT implementation, timeouts and test budgets are unchanged.

## PR #307 follow-up: IRQ delivery-count window (2026-10-04)

The AArch64 chaos run during NAT validation failed `irq-route` at
`hits >= 5` in 71 ms. That log provides neither the observed hit count
nor a trace of timer-source assertions, so it cannot identify the exact
host scheduling gap. Source inspection establishes that the test assumed
a minimum delivery rate: it requested 200 Hz, waited 50 ms, then demanded
five hits. The earlier sightings span both architectures.

On AArch64, `arch_test_periodic_irq_start` uses a kernel timer to raise a
spare GIC SPI. Its callback rearms relative to the callback's execution,
and expiry is tick-grained. With a 4 ms tick, a callback rearming for
5 ms normally needs two more ticks even without host delay, already
leaving less slack than the old comment's ten expected hits. A late
callback pushes subsequent assertions back; the tick also skips missed periods rather than delivering every
elapsed tick. Consequently the nominal source frequency cannot guarantee
five delivered interrupts in that window. x86 uses the PIT but likewise
cannot require a descheduled vCPU to observe every nominal period.

`irq-route` now waits for the same five hits, sleeping between checks,
with `clock_deadline_ns` / `clock_deadline_passed` enforcing a 1 s bound.
A broken route reports the observed count and returns failure; the
runner's existing deferred cleanup releases the line and source. Counter
accesses are atomic because chaos can migrate the test away from the
IRQ's target CPU. Relaxed ordering suffices: the counter publishes no
other data. The existing two mask-observation windows, duplicate-request,
vector and release assertions remain unchanged. No controller, periodic
source implementation or whole-test watchdog budget changed.

`tools/irq-route-probe.py` runs in isolated clones. Old/fixed modes slow
the real source to 20 Hz, making the former rate assumption fail while
still delivering five interrupts within the new bound. This is a
controlled slower-source comparison, not a replay of the failed host's
scheduling. The silent mode leaves the line masked and requires the
zero-hit deadline diagnostic. The broken-mask mode keeps delivery enabled
at 200 Hz and requires the mask assertion to fail. Each negative requires
two deferred releases, and every mode then runs `irq-affinity` before
the expected panic shutdown. AArch64 must deliver to all four CPUs;
x86 has no manually raisable spare line and explicitly skips affinity.
A skipped routing test cannot
satisfy the fixed mode's required delivery-count marker.

All eight controlled probes passed: old, fixed, silent and broken-mask
on x86-64 and AArch64. Here a negative probe passes only when its expected
failure, cleanup count and architecture-specific affinity outcome are
all observed.
Both fixed probes observed exactly five deliveries. Logs are
`out/irq-route-{x86_64,aarch64}-{old,fixed,silent,broken-mask}-result.log`,
with retained images and boot logs in the corresponding `run-*` directory.

The complete AArch64 `test-chaos` boot passed all 422 self-tests and the
full harness in 138.2 s; `irq-route` passed in 85 ms. Logs:
`out/irq-route-aarch64-chaos-result.log` and
`out/irq-route-aarch64-chaos.log`. The earlier failed chaos run remains
in `out/nat-expiry-aarch64-chaos-result.log`, including its captured boot
output. This new pass does not erase that observation.

The complete x86-64 standard boot also passed all 422 self-tests and the
full harness in 138.7 s; `irq-route` passed in 75 ms. Logs:
`out/irq-route-x86_64-normal-result.log` and
`out/irq-route-x86_64-normal.log`. Python syntax compilation and
`git diff --check` passed. After making the probe's affinity marker
architecture-specific, all eight retained boot logs were checked against
that additional requirement: real four-CPU delivery on AArch64 and the
explicit unsupported-source skip on x86.

## PR #307 follow-up: AArch64 CI timing limits (2026-10-04)

[CI run 37154202680, AArch64 job 111294098641](https://github.com/vkuttyp/CosmoOS/actions/runs/37154202680/job/111294098641)
failed the chaos boot at commit `a2fbf47f`. The ordinary, alternate-GIC
and protection-capable boots passed in 165.4, 163.6 and 167.2 s.
Chaos returned success from all 422 self-tests, including `irq-route`
(85 ms) and `net-nat`, but failed two time limits:

- `net-nicbench` completed in 8,360 ms against the default 8,000 ms.
  The watchdog captured the second interface's UDP send loop, and the
  benchmark finished shortly afterward. Its reported UDP rates were
  6,272 and 2,082 sends/s, with two 10,000-attempt rounds. The workload
  also includes 2,000 ARP requests per interface and receive drains.
- Kernel self-tests totaled 137.459 s versus 124–127 s in the passing
  boots. The shell harness exhausted its 170 s whole-run deadline before
  advancing to command 15 (`echo after-fg-ok`); QEMU then hit its 180 s
  limit. The captured log contains exactly 15 newline-prefixed prompts,
  ends with that fifteenth prompt, and records the foreground sleep's
  exit with status 130. Thus the final prompt is recognizable and the
  signal did terminate the job. The log has no per-line host timestamps,
  so its precise arrival time is not established; a late prompt after
  deadline exhaustion fits the retained log and the harness's exit path.

The benchmark now has a named 20 s entry in the kernel budget table,
which drives both the watchdog and harness. Sample counts, ARP assertions,
receive-drain checks and packet paths are unchanged. This follows the
existing policy for composite tests, rather than reducing measurement
work to fit a single-test default. `make test-chaos` now passes an explicit
240 s total timeout, giving the shell 230 s; ordinary boots remain at
180 s. Signal-response latency checks and per-test watchdogs remain active.
No IRQ, NAT, controller or scheduler implementation changed in this fix.

The original artifact is retained under
`out/ci-37154202680-aarch64/aarch64-debug-chaos/boot-test-chaos.log`.
An offline replay (`out/ci-budget-replay-result.log`) verifies that the
archived 8,360 ms duration fails the old budget and fits the explicit one,
while 20,001 ms still fails. With a controlled clock, the archived
fifteenth prompt is accepted when time remains and rejected after the
old deadline. That replay establishes harness behavior, not the original
prompt's timestamp. The existing budget-parser suite passed all 10 checks.

The updated `gmake -j4 ARCH=aarch64 test-chaos` passed all 422 self-tests
and the complete harness in 136.7 s. Its launch reports 240 s total,
the network harness reports 210 s, and the kernel budget line includes
`net-nicbench=20000`; no watchdog fired. The benchmark completed in
2,464 ms locally. Logs: `out/ci-budget-aarch64-chaos-result.log` and
`out/ci-budget-aarch64-chaos.log`. This verifies the configuration and
full boot locally; it does not reproduce the CI host's slowdown.
`git diff --check` passed. The prior GitHub x86 job completed successfully;
the updated AArch64 limits still need the next CI run's validation.
