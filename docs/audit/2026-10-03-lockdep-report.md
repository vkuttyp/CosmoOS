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
