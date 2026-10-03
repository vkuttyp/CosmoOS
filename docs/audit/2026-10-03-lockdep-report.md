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
No isolated acquisition microbenchmark was run; boot timing differences
under concurrent host load are not presented as lock-overhead measurements.

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
