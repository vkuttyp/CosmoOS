# Lock discipline audit — 2026-10-03

This is a renewed audit against the current tree, before changes to the
validator. The September milestone already shipped; Next-Milestone.md is
partly stale. Existing source, rather than its example API names, governs
this pass. Investigation covered README, the governing prompt, the deferred
inventory, lifetime report and quiesce documents, lockdep documents, build
and CI, scheduler, SMP, interrupts, VFS, networking and module contracts.

## Baseline and environment

The supported hosts are Linux with LLVM/GNU make and macOS with Homebrew.
This run uses macOS arm64, GNU make (`gmake`), LLVM cross compilation and
QEMU TCG. Targets are x86-64 and AArch64. `gmake test` defaults to debug,
four CPUs; `QEMU_SMP=1` covers UP; `ARCH=aarch64` selects the other target;
`BUILD=release` selects release. `host-test` uses ASan/UBSan; `analyze`
uses clang's analyzer. CI runs both targets in Debian trixie and also
tests release, panic paths, guards, GIC variants, chaos, fuzzing and
reproducibility. Logs for this baseline are under
`out/lockdep-baseline-{host,x86,arm}.log`.

Approved baseline reruns passed: host suites and 416 kernel selftests on
each SMP architecture. Final validation details and later unchanged-HEAD
UP comparisons are in `2026-10-03-lockdep-report.md`. The observed graph
snapshot is `2026-10-03-lock-order.tsv` (1288 union edges); initialization
and helper sites are `2026-10-03-lock-sites.tsv` (232 sites). These runtime
artifacts supplement this pre-implementation audit rather than imply that
all possible lock relationships were observed.

Initial sandbox runs passed the C host suites and built the x86 image but
failed at local socket binding (`PermissionError`) in the Python harness.
These are environment restrictions, not kernel regressions. Approved
reruns of host and both boot suites were started before implementation;
their final results will be recorded in the hardening report. User edits
to reviewer.yaml and the supplied prompt are outside this work.

## Synchronization primitives actually present

| Mechanism | Implementation and rules |
|---|---|
| Spinlock, irqsave, nested, trylock | `kernel/core/spinlock.c`; acquire/release atomics, CPU ownership, preemption disabled while held; irqsave additionally masks IRQs |
| Mutex, nested, trylock | `kernel/scheduler/mutex.c`; nonrecursive thread ownership, sleeping acquisition, priority inheritance via `mutex-pi` spinlock |
| Wait queue | `kernel/scheduler/wait.c`, `kernel/wait.h`; irqsave spinlock, prepare/recheck/block protocol |
| Counting semaphore | `kernel/scheduler/semaphore.c`; internal irqsave spinlock and wait queue; down may sleep |
| Completion | `kernel/scheduler/completion.c`; internal irqsave spinlock and wait queue; wait performs the completion lifetime handshake |
| Futex | `kernel/ipc/futex.c`; hashed bucket spinlocks, wake/queue sequences, user-copy outside the bucket lock |
| Preemption control | `kernel/percpu.h`, `kernel/core/percpu.c`; nested per-CPU count, positive-count assertion on enable, scheduling at zero |
| IRQ masking and nesting | `arch/irq.h`, architecture trap tails; masking is separate from preemption count; irq_depth identifies handler context |
| Quiescence | `kernel/core/quiesce.c`; preemption-disabled readers, epoch publication, sleeping grace-period wait, worker callbacks after a grace period |
| Atomics and references | `__atomic_*`, kobjects, thread/mbuf/pcb references; no general lock ownership implied by a reference |
| Raw validator lock | `kernel/core/lockdep.c`; uninstrumented static atomic word with IRQ masking, no allocation |

No kernel rwlock primitive was found. The native userland condition
variable is futex-backed; it is not an additional kernel lock primitive.

## Important lock families and observed relationships

Acquisition and release sites for the families below are in their named
implementations. Dynamic instances share logical classes rather than
receiving a class per allocation. Every listed spinlock forbids sleep and
faulting user-memory access. `kmalloc` currently does not sleep; allocation
under a spinlock does not by itself violate that rule, but introduces
allocator edges. Every mutex permits sleep in thread context.

| Family / owner | Protection and order | IRQ / callbacks / lifetime |
|---|---|---|
| `runqueueN`, per CPU (`sched.c`) | queue/state; two queues in increasing CPU-id order | irqsave; handed across a context switch on the same CPU; release before final thread put |
| Wait/sleep/completion/semaphore locks, per primitive | state → wait queue → runqueue | IRQ wakers; wait entries belong to blocked threads; completion handshake before free |
| `mutex-pi`, global (`mutex.c`) | PI lists → mutex internal/wait locks and runqueues | irqsave; donation chain traversed without sleeping |
| `futex`, 64 buckets (`futex.c`) | address-ordered bucket pair, second subclass 1 → runqueue | copy/reclassification and vnode puts outside buckets; timeout callback wakes a thread |
| `mounts`, global (`vfs.c`) | registry → vnode/filesystem; mount operations serialized | mutex; filesystem callbacks can sleep; sync takes a reference and drops registry lock before commit |
| `rename`, per mount | stable ancestry → ancestor parent #0 → second parent #1 → child #2 | mutex; serializes topology changes; filesystem callback under parent locks |
| `vnode`, per vnode | directory/size/links → pagecache → filesystem → block | mutex; character device I/O deliberately dispatched without vnode mutex; references pin objects |
| `mount-hash`, per mount | hash membership and nr_vnodes; leaf below VFS mutexes | irqsave; final drop and unhash in one locked operation; release outside lock |
| `file`, `pagecache`, `mount-sync` | file → vnode/cache; cache → filesystem; sync → filesystem | mutexes; I/O may block; mount-sync excludes filesystem teardown |
| `cosmofs`, per filesystem | transaction/inodes → block sync and driver/virtqueue locks | mutex; commit waits for I/O; writeback count published under same lock |
| `socket`, per socket | socket mutex → protocol/queue/timer/driver spinlocks | protocol wakeups outside protocol locks with socket references |
| TCP table/pcb, UDP, ARP, ND | per-connection/table state; timer scheduling below pcb locks | timer callbacks defer work; cancellation must not hold a callback's lock; ARP/ND retries pin interfaces before unlock |
| netif registry/interface, RX queues | registration and per-CPU packet queues → worker wait/runqueue | IRQ RX producers; unregister stops new traffic, drains workers and quiesces before last put |
| device/block/driver/virtqueue | registries → device state → queue; synchronous teardown outside callback locks | IRQ completions; gone/submitting handshake and synchronous IRQ/timer teardown before reclaim |
| `modules`, global (`module.c`) | loader → init/shutdown → device/VMM/allocator locks | mutex; callbacks must not re-enter loader; grace period and live objects before image free |
| VM/PMM/slab | space/table and allocator locks; user-copy boundary outside caller spinlocks | faulting user copies annotated might_sleep; long populate regions remain a latency risk |
| `console` | console serialization → sink callback | irqsave; fatal diagnostics must avoid waiting on locks held by the failure |

The established graph is recorded automatically by `lockdep_dump_graph`
at the end of debug selftests; `docs/kernel/lockdep/testing.md` lists prior
observations. Current boot graphs will be summarized separately, including
runqueue-pair edges that supersede the old single-runqueue leaf wording.
Static edges are supported by the source above. Runtime coverage remains
necessary for error paths, module shutdown and driver callbacks.

## Known findings rechecked

| Prompt risk | Current classification / source evidence |
|---|---|
| Vnode cache check-then-get | Stale finding: `vnode_lookup_cached` gets under hash spinlock; `vnode_put` uses `kobject_put_and_lock`, unhashes before final release |
| Futex user copy under bucket | Stale finding: `wake_seq` snapshot, unlocked copy, key reclassification and sequence recheck before enqueue |
| Global mount lock across sync | Stale finding: `vfs_sync` references one mount under registry mutex and syncs after dropping it |
| Rename versus rmdir ABBA | Stale finding: rename mutex stabilizes ancestry; ancestor-first locking with subclass annotations |
| Scheduler leaf / AArch64 GIC | Partially stale documentation: IPI binding is lock-free; migration legitimately nests two ordered runqueues (S24) |
| Network global locking | Stale assumption: per-connection locking and per-CPU RX workers already exist; teardown/timer combinations still need runtime coverage |

## Confirmed validator bugs and unresolved risks

1. **Long-cycle diagnostic overread:** `lockdep_core_reaches` returns the
   complete length but stores at most eight nodes in the caller's array;
   `report` iterates the complete length. Any cycle with a recorded path
   longer than eight nodes can read outside that array, then index the
   class table with a garbage node. Fix the path-output contract and test
   a maximum-length path under sanitizers, not just short cycles.
2. **Module metadata lifetime:** class records keep name pointers forever.
   Module driver names such as `virtio-blk` reside in unloadable rodata;
   `module_unload` frees that image after quiescence, but the graph still
   dereferences its name when dumped or reported. Pointer reuse also
   aliases unrelated future classes. Use bounded kernel-owned name copies
   and content identity (the existing documented logical naming model),
   without module loader calls or allocation from validator hooks.
3. **Concurrent validator reads:** class cache, usage and graph hot-path
   reads occur outside the raw lock while other CPUs write them. Further
   synchronization review is needed; sanitizer tests of the pure graph
   are not a concurrent validator test.
4. **IRQ model limits:** only direct per-class usage is checked; transitive
   IRQ-safe → IRQ-unsafe chains and successful trylock usage need further
   validation. No claim that direct classification proves all IRQ safety.
5. **Diagnostics/context limits:** NMI/#MC cannot safely take the raw
   validator lock; current fatal paths require separate review. Printing
   before entering console panic mode can deadlock if a sink reports a
   violation. IRQ masking alone is not tested by might_sleep.
6. **Timer wait dependencies:** timer_cancel_sync spins rather than sleeps;
   ordinary lock edges do not model waiting for a callback while holding a
   callback lock. The quiescence invariant remains mandatory.

## Assessment and proposed incremental implementation

Extend the existing core, hooks, hybrid held stacks and graph; no parallel
lockdep framework or scheduler/quiescence redesign. The prompt correctly
requires lock classes, arbitrary cycles, IRQ awareness, atomic checks,
bounded resources and subsystem validation; these mostly exist already.
Its assumption that the mechanism and historical VFS/futex fixes are
missing is stale.

First make diagnostics memory-safe and class names independent of module
storage. Preserve cached class indices, kinds, subclass ordering and
public lock layouts. Names too long for bounded storage must fail loudly,
never silently truncate and merge classes. Add an explicit LOCKDEP build
knob using existing make configuration, defaulting to the current behavior.
Then rerun synthetic tests and both normal kernels with the validator on.
Investigate every unexpected report rather than suppressing it.

Affected files: lockdep_core.h, lockdep.c as necessary, host graph tests,
kernel lockdep selftests, build configuration/toolchain and canonical
locking documents. Module lifecycle stays intact; name copying removes
the graph's dependency on freed module memory. Proof: short and long
cycles, class storage freed/reused, exhaustion and overlong metadata,
kernel long-cycle and unheld-release checks, existing IRQ/sleep/recursion
tests, host ASan/UBSan, x86/AArch64 SMP and UP, disabled debug and release
builds, analyzer. Residual risks above must remain explicit even when the
first increment passes.

## 2026-10-04 increment: observed spin contention

Baseline: commit `2ee2d783`, CI run `37155497752`, passed both architecture
jobs, including chaos, release, host sanitizers, fuzzing, analysis,
reproducibility and panic tests. PR #307 remains open; this increment is
on `milestone/lockdep-spin-contention`, based on that reviewed branch.

The inventory still defers contended spin measurements. Source inspection
also confirms `lockdep-contention` holds its remote lock for only 20 ms
and starts a local callback for 5 ms later: it can miss the intended
interleaving on a slow host, and a callback before the wait does not prove
that an awaited lock is absent from the held stack. Cleanup was fixed in
PR #307; the timing premise was deliberately left open.

Planned phases:

1. Add a self-test-only per-CPU observation of an actual failed spin-lock
   exchange. Publish once on entering the slow path, restore any interrupted
   wait when acquisition completes, and expose a read-only identity query.
   No lock layout/module ABI changes, allocation or tracked locks; compile
   out with SELFTEST=0. The observation is not a general deadlock detector.
2. Make `lockdep-contention` keep its holder until a callback observes the
   real wait, with a bounded holder deadline and existing lifetime cleanup.
   A premature callback rearms without taking its test lock.
3. Measure plain and irqsave contended acquisitions on distinct CPUs,
   with two warmups and nine samples, a controlled 1 ms hold after the
   observed failed exchange, and ownership/exclusion/protected-data checks.
   UP explicitly skips; timings include the observer and controlled hold.
4. Validate enabled/disabled lockdep SMP boots on both architectures,
   UP skip behavior, release compile-out, and controlled failure paths.
   Update the inventory only for evidence-backed completed scope.

The established per-CPU spin ownership, per-thread mutex ownership,
preemption migration barrier, timer cancellation and thread lifetime
protocols remain the design boundaries. General callback wait modeling,
raw IRQ pairing, NMI writer support, global snapshots and worst-case
latency bounds remain separate work.
