# Lock discipline and lockdep: design

## Data structures (`kernel/include/kernel/lockdep_core.h`, `kernel/core/lockdep.c`)

```c
#define LOCKDEP_MAX_CLASSES   512       /* includes runqueue, callback and completion classes */
#define LOCKDEP_SUBCLASSES    5         /* nesting levels per class: a chain of five epoll sets */
#define LOCKDEP_MAX_NODES     (LOCKDEP_MAX_CLASSES * LOCKDEP_SUBCLASSES)
#define LOCKDEP_MAX_HELD      24        /* per CPU: spinlocks, interrupt context included */
#define LOCKDEP_MAX_HELD_MUTEX 8        /* per thread */

struct lock_class {
    char name[64];                      /* owned copy; contents plus kind are the key */
    unsigned usage;                     /* LOCKDEP_USED_IN_IRQ | LOCKDEP_HELD_IRQS_ON */
    unsigned cover;                     /* completion classes: waited for, signalled (diagnostics) */
    uintptr_t irq_ip, irqs_on_ip;       /* where each usage was first seen */
};

struct held_lock {
    uint16_t node;                      /* class * LOCKDEP_SUBCLASSES + subclass */
    uint8_t  flags;                     /* HELD_TRYLOCK | HELD_IN_IRQ | HELD_IRQS_ON */
    uintptr_t ip;                       /* acquiring instruction */
    const void *lock;                   /* for release matching and assertions */
};

struct lockdep_state {
    struct lock_class classes[LOCKDEP_MAX_CLASSES];
    unsigned nr_classes;
    uint64_t before[LOCKDEP_MAX_NODES][LOCKDEP_MAX_NODES / 64];   /* bit b in before[a]: b was taken while a held */
};

/* per CPU */  struct held_lock held[LOCKDEP_MAX_HELD]; unsigned nr_held;
/* per thread (struct thread) */ struct held_lock held_mutex[LOCKDEP_MAX_HELD_MUTEX]; unsigned nr_held_mutex;
```

`spinlock_t` and `struct mutex` gain a `uint16_t class` (0 = not yet
classified; the class index plus one otherwise), filled on the first
acquisition by a linear search of the class table under the checker's own
raw lock. The field exists in every build so the module ABI has one layout:
**module ABI v3**.

The graph and the class table are one `struct lockdep_state` behind pure
inline functions in `lockdep_core.h` (class lookup, edge add, reachability),
so the host test drives them under the sanitizers. The bitmap is 2048 nodes
(512 classes × 5 subclasses) × 320 bytes = 800 KiB when `LOCKDEP=1` (four
subclasses and 512 KiB until the wake_one follow-up to the epoll-callback
unit, which needed a fifth for Linux's nesting depth of five sets).

## Classes and nodes

A class is a lock's initialisation name: the contents passed to
`SPINLOCK_INIT`, `spinlock_init` or `mutex_init`. Equal names of one kind
share a class (every `vnode->lock`, every `socket->lock`), independently
of literal merging or the mapping chosen for a reloaded module. The graph
copies at most 63 characters plus NUL into permanent kernel-owned storage.
Overlong names report metadata overflow; they are never truncated keys.
NULL names normalize to `"?"`; meaningful names distinguish unrelated
families. The original name must stay valid and unchanged during the
lock's lifetime because primitives also use it directly. After its last
use, graph diagnostics need no reference to that storage. The run queues
use a static table of `"runqueue0"`, `"runqueue1"`, ... in `sched.c`, so
each queue has its own class and increasing CPU-id order (S24) is checked.

A node is (class, subclass). Subclass 0 is the default. A lock taken while
another lock of the same class is held is a **recursive acquisition** report
unless the inner one is annotated with a higher subclass
(`mutex_lock_nested(&child->lock, VNODE_NESTED_CHILD)`). Annotations are
the only way to express parent → child of one class, and each is listed in
`invariants.md` with its justification.

## Held-lock stacks

Spinlocks are held with preemption disabled, so the holder never leaves the
CPU: the spinlock stack is **per CPU**. This also covers the run-queue lock,
which `schedule()` hands from the outgoing thread to the incoming one on
the same CPU, and interrupt context, whose acquisitions push on top of the
interrupted context's and pop before the interrupt returns. Mutexes are
held across sleeps, so their stack is **per thread**. A release that is not
the top entry is legal (nested critical sections end in any order); the
entry is removed from the middle.

The held set an acquisition is checked against is: the CPU's spinlock stack
(all of it in interrupt context; only entries below the interrupt boundary
are not distinguished because they are, in fact, held by this CPU), plus,
in thread context, the current thread's mutex stack. In interrupt context
the thread's mutexes are not part of the held set: the handler does not hold
them, and mutexes are never taken in interrupt context, so no edge into them
can be needed.

## The dependency graph and the order check

On acquiring node B with held set {A₁ … Aₙ} (trylocks excluded from the
check, included in the stacks):

1. If any Aᵢ has B's node: recursive acquisition report (unless B is a
   subclass annotation, in which case the node differs by construction).
2. If B reaches any Aᵢ in the graph (`before[B] ⊇* Aᵢ`, a breadth-first search
   over the bitmaps with a visited set): **lock-order inversion**. The
   report shows the held stack, B's acquisition, and the recorded chain
   B → … → Aᵢ. Long diagnostic paths keep their last eight nodes and are
   labeled accordingly; the output length is the stored count, so printing
   cannot overrun the buffer. Detection still searches all 2048 nodes.
3. Otherwise set `before[Aᵢ] |= B` for every Aᵢ (edges from every held lock,
   not only the innermost, so a chain seen once in pieces is still caught).

The check runs before the acquisition waits, so a deadlocking order is
reported rather than hung on, and it pushes nothing: the push onto the held
stack happens only once the lock is owned (`lockdep_acquired`). A lock
still being contended is not held, and with a plain `spin_lock` and
interrupts enabled a handler can run on this CPU during the wait; if the
lock were already on the stack the handler's acquisitions would record
phantom edges from it (found in review of PR #18; test
`lockdep-contention`). Edges therefore mean "attempted while held", which
is the relation the order check wants whether or not the attempt has yet
completed. The transitions themselves are atomic with respect to
interrupts: with the checker on, `spin_lock` wins the lock and pushes it,
and `spin_unlock` pops it and releases it, with interrupts masked, so a
handler never runs between an ownership change and the stack that records
it (it would otherwise see an owned lock as not held and miss a real
dependency; the second review finding on PR #18). `LOCKDEP=0` compiles
the masking out with the checker.

Edges are recorded and checked under the checker's raw spinlock, taken with
interrupts disabled, so the graph is consistent; the lock is not itself
tracked. `lockdep_core_add_edge` requires that caller serialization; its
atomic bitmap write supports unlocked readers, not concurrent writers.
The raw word is zero when free and otherwise contains the owning CPU plus
one. An acquire CAS publishes ownership atomically; no separate owner
store leaves an NMI window. IRQ masking prevents migration until release.
An unsuccessful CAS that observes this CPU as owner panics with
`lockdep: graph raw lock re-entry on CPU ...` instead of spinning on the
interrupted owner. Other CPUs still wait normally. The panic path uses
bounded held-stack snapshots and does not acquire this raw lock.
Normal graph dumps allocate private storage before taking the raw lock,
copy the complete bounded graph under it, and print from that snapshot
after releasing it. Counts, metadata, and edges therefore describe one
instant even if logging or another CPU adds dependencies during output.
The copy is 573,448 bytes (about 560 KiB); the heap temporarily reserves
a page allocation of that order and frees it after printing. There is no extra
permanent graph or acquisition-path cost. The raw lock and disabled IRQs
cover only the copy, never allocation, printing, or freeing. Allocation
failure prints an explicit unavailable message. This API requires a
working allocator and raw lock and is not used by panic/NMI diagnostics.
Panic calls `lockdep_dump_held()` to print the current CPU's spinlock stack
and the current thread's mutex stack; it does not dump the dependency graph.
Statistics use the same raw lock for all counter updates and the complete
`lockdep_get_stats` copy, so classes, edges, acquisitions, searches and
reports describe one instant. Acquisition and report counting each add a
short raw-lock hold; searches are counted inside the existing graph hold.
The copy can include operations in progress: a whole acquisition is not
one transaction. Neither this counter snapshot nor the graph snapshot
freezes CPU/thread held state. Statistics are a normal diagnostic API,
not a panic/NMI API.

### Search work bounds

Let N = `LOCKDEP_MAX_NODES` (2048), W = `LOCKDEP_NODE_WORDS` (32),
and C = the registered class count. Under caller serialization and valid
node/class indices, each search has the following conservative bounds:

| Work per search | Bound |
|---|---|
| Clear visited bitmap | W writes |
| Usage-search seed candidates | 4C ≤ N checks; none for reachability |
| Queue insertions and removals | at most N each |
| Adjacency bitmap loads | at most NW = 65,536 words |
| Newly discovered neighbor iterations | at most N minus the initial sources |
| Reachability path reconstruction | at most 2N − 1 = 4,095 parent steps |
| Usage-search predecessor reconstruction | at most N − 1 parent steps |

Nodes are marked visited when enqueued, including every initial source.
Consequently each node can enter the queue only once. Each dequeued node
scans at most W words, regardless of edge density. Parent pointers lead
to earlier discoveries, so reconstruction cannot loop, even if the input
contains a cycle. A missing label on a fully reachable graph attains NW
adjacency reads; a full chain attains 2N − 1 reconstruction steps. A
zero-capacity output path still performs reconstruction in the current
implementation. These are algorithmic work bounds, not instruction counts
or elapsed-time guarantees.

An IRQ edge check runs at most two searches. Publishing one new usage bit
runs at most four, one per subclass; a direct mixed-usage conflict returns
before searching. An acquisition can check at most 32 held entries (24
spinlocks plus eight mutexes), with one reachability and up to two IRQ
searches per missing edge. Including usage validation gives a conservative
100-search ceiling, or 6,553,600 adjacency-word loads, for those decision
paths. This is a loose bound, not a claim that one graph attains every
maximum together. It excludes class lookup, held-stack/profile scans,
statistics, diagnostic searches/printing and raw-lock contention. It does
not bound interrupt-disabled wall time or lock acquisition latency.

Host `search-work` tests instrument the shared helpers under
`LOCKDEP_CORE_TEST_WORK`, accumulating counts across composite checks.
The extra scratch fields and increments compile out unless that host-test
macro is defined. Full-capacity chain, dense, disconnected and cyclic
cases validate the bounds and selected exact maxima; see `testing.md`.

### Remote held-stack snapshots

Remote CPU spinlock-stack dumps use a separate bounded snapshot protocol.
The CPU-local writer already has IRQs masked for pushes, releases, and
irqsave flag updates. It increments a 64-bit sequence before and after
each update and writes each shared field atomically. Remote readers load
an even sequence, copy up to 24 entries using atomic reads, and accept
only if the sequence is unchanged. These operations are sequentially
consistent, so an accepted copy cannot span a writer in the atomic total
order (assuming no sequence wrap during the bounded attempt). An odd or
changed sequence yields an unavailable message, with no retry, allocation,
or target-owned lock. The cost is eight bytes per CPU plus atomic writes
on debug held-stack updates. Owner-local reads retain their existing rules.

Thread mutex stacks use the same atomic protocol with an explicit capacity
of eight entries and an additional 64-bit sequence at the end of `thread`.
Only the owning thread writes; preemption or migration during an update
leaves an odd sequence until that thread resumes. Remote readers must keep
the thread object alive with a reference; current-thread diagnostics already
have that lifetime guarantee. Pushes, shifted removals and counts are atomic.
Panic diagnostics now snapshot each local stack independently and report
unavailable on an interrupted update. This does not make NMI writers
reentrant or provide a simultaneous CPU/thread view or mutex-owner snapshot.

The x86 `trap-paranoid` regression verifies the read-only NMI boundary
with real local-APIC delivery while the graph raw lock is held, including
an interrupted held-stack update. Current lockup NMI sampling and corrected
machine-check handlers avoid tracked locking. General lockdep writer
instrumentation from NMI/#MC remains unsupported; passing the snapshot
test does not make the graph raw lock reentrant. Same-CPU raw-lock re-entry
now fails stop, including from NMI, but this is not permission to take
tracked locks in NMI/#MC handlers. Re-entry outside a raw critical section,
held-stack writer nesting, and cross-CPU wait cycles remain unsupported.

The search is bounded by the node count (2048) and runs only when
the edge set changes or a cycle exists: a repeated acquisition whose edges
are already recorded short-circuits after the recursion check with a
bitmap test per held lock.

## Interrupt safety

A blocking acquisition in interrupt context (`irq_depth > 0`) marks the
class IRQ-used. A successful acquisition while interrupts are enabled,
including a successful thread trylock, marks it IRQ-enabled. Failed
trylocks record nothing. A successful IRQ trylock does not make a class
IRQ-used because it will not wait on the interrupted holder. Mutexes are
never interrupt-safe and the existing panic on `mutex_lock` in interrupt
context stays.

The validator rejects a new edge when an IRQ-used node can reach its
source and its destination can reach an IRQ-enabled node. It also checks
existing edges when either endpoint first receives a usage label. Labels
apply to every subclass of a class; traversal preserves the observed node
edges. The search is breadth-first with one preallocated scratch area under
the graph raw lock; conflict reports run after releasing that lock. A
rejected edge or usage label is not published, including when a self-test
consumes the report.

This represents blocking lock cycles through IRQ paths. Timer callback
completion waits are in the graph through callback classes (below), with
the bounded per-object profile kept beside them. NMI/#MC reentrancy remains unsupported. Trylock edges are not
recorded at the attempted acquisition, since a failed try cannot wait;
locks held after a successful trylock participate in subsequent blocking
acquisition edges.

## Timer cancellation waits

When a timer callback starts, it reserves a profile keyed by the timer
object's address. While its callback is active, blocking spinlock acquisitions add the exact
lock object and class node to that profile. Before `timer_cancel_sync`
re-enters a wait for a running callback, it compares the caller's held
spinlock and mutex objects with the observed profile. A match reports
`LOCKDEP_R_CALLBACK` rather than waiting while holding a callback-needed
lock. The queue lock is released before this check.

Profiles have fixed storage (one active callback address per possible CPU,
64 total, and 16 distinct locks each). Completion releases a profile after
the timer queue clears `running`; a later callback execution reserves a
fresh profile. Exhaustion is a validator report. Interrupt masking permits
at most one active timer callback on each CPU, so this covers the maximum
configured CPU count.
Profiles cover only the callback execution in progress on the timer being
cancelled. Callback classes, below, cover every execution of the same
function the graph has seen.

## Callback classes

A synchronous wait for a callback deadlocks when the waiter holds a lock
the callback needs, directly or through a chain. The graph expresses this
by giving each timer callback **function** a pseudo-class, kind
`LOCKDEP_KIND_CALLBACK`, named `callback <address>` and cached in
`struct timer`'s `lockdep_class` (reset by `timer_setup`):

- `run_expired` brackets each callback with `lockdep_callback_enter` and
  `lockdep_callback_exit`. Enter acquires the class and holds it on the
  CPU's stack, so every lock the callback takes records the edge
  `callback → lock`. Exit names only the function, because the timer may
  be freed by the time the callback returns.
- `timer_cancel_sync` calls `lockdep_callback_wait` on **every** call,
  before it looks at the queue. The wait acquires the class without
  holding it, recording `held lock → callback` for each held spinlock and
  mutex: "this was waited for while those were held". A cancel may wait
  whether or not the callback is running at that moment, so the
  dependency is recorded unconditionally.

The ordinary cycle check then answers the question in both orders and
transitively. A wait that holds a lock any callback of the function took,
or a lock such a lock reaches, is reported as `LOCKDEP_R_CALLBACK`, and
`timer_cancel_sync` still cancels a pending timer and returns whether it was
pending, but does not wait for a running callback. A callback that first takes
a lock already recorded before its class, by an earlier wait, is reported
as an inversion whose chain names the class. A callback that waits for a
callback of its own function is reported as `LOCKDEP_R_CALLBACK`
(recursion). Two timers sharing a function share the class. That is the
point: what one timer's callback did answers a wait on another. It is
also the usual lockdep conservatism, since the class says "a callback of
this function", not "this timer's callback".

The class has no interrupt-usage labels of its own (`check_usage` labels
spinlock classes only). The locks taken inside the callback carry the
callback's interrupt context as before. Callback classes share the class
table (`LOCKDEP_MAX_CLASSES`, raised to 384 for them in 2026-10-05 and to
512 when the completion classes arrived: a full debug boot creates 20
callback classes and 38 completion classes), and a cached class reuses the
graph's copy of its name, so a callback that runs every tick formats
nothing.

**What is not covered.** `synchronize_irq` and `interrupt_unregister_sync`
wait through `synchronize_quiesce`, which calls `might_sleep`: the waiter
can hold no spinlock, and an interrupt handler can take no mutex, so no
lock cycle through that wait exists to model. Module teardown waits by the
same route. Completion waits are the next section.

## Completion waits

A completion is a one-shot signal between contexts (`docs/kernel/scheduler/design.md`,
"Mutex, semaphore, completion"): a waiter calls `wait_for_completion`,
some other context calls `complete`. The deadlock a lock checker can see
is a waiter that holds a lock L across the wait while the signaller must
acquire L, directly or through a chain, before it can call `complete`.
Until 2026-10-07 nothing modelled it: a completion had no class, and the
callback-class report said there was "no function to key a class on".
The key is the completion's **name**, which `completion_init` already
requires.

### What the kernel's rules make possible

Three facts fix the shape of every such deadlock in this kernel, and the
design follows from them:

1. `wait_for_completion` and `wait_for_completion_timeout` begin with
   `might_sleep()`. The waiter therefore holds no spinlock and is in no
   `quiesce_read_lock` section; what it can hold across the wait is
   **mutexes**, on its per-thread stack.
2. A mutex is never acquired under a spinlock (`mutex_lock` is a sleeping
   call) and never in interrupt context. In the recorded graph, every
   edge into a mutex node therefore starts at a mutex node; no spinlock
   node reaches a mutex node.
3. A cycle through a completion wait has the form L → C → … → L with L a
   mutex. By 2, every node on the C → … → L side is a mutex (or another
   completion). The locks a **signaller** can contribute to such a cycle
   are mutexes, and only a thread holds mutexes.

So an interrupt handler or a timer callback that signals a completion
cannot be party to a lock cycle through the wait: it takes spinlocks
only, and a spinlock reaches no mutex. The deadlocks those signallers can
cause are of another kind (the callback never scheduled, the interrupt
never delivered) and are not lock-order facts; the timer half of that is
what the callback-coverage listing below bounds. The thread signaller is
the one the graph must see.

### The model

- **Class.** Each completion name is a class of kind
  `LOCKDEP_KIND_COMPLETION`, created at `completion_init`. Like a callback
  class it is never held: both sides *acquire* it. `struct completion`
  gains no field (module ABI v4 is unchanged and the `LOCKDEP=0` layout is
  identical): `completion_init` classifies the completion's own spinlock
  at init, as its first acquisition would, and a table indexed by that
  spinlock's class gives the completion class, so each hook below reads
  one cached slot and one table entry and searches the class table only
  the first time a name is seen.
- **The wait side.** `wait_for_completion` and `wait_for_completion_timeout`
  acquire the class without holding it, before they wait and on every
  call, done or not (a wait that finds the completion done could have
  waited): the edge `M → C` for each mutex M the thread holds, and a
  `LOCKDEP_R_COMPLETION` report if C already reaches a held M. This is
  `lockdep_callback_wait`'s primitive with a different report kind. A
  timed wait records the same edges: a timed wait that must always time
  out because its signaller needs a lock the waiter holds is the same
  defect, bounded.
- **The signal side.** `complete`, in thread context, records for each
  mutex M the thread holds **at the call** the edge `C → M` ("to signal
  C, M was held"), and reports `LOCKDEP_R_COMPLETION` if M already
  reaches C. A signaller that holds M at `complete` cannot have got there
  without taking M and still holds it, so a waiter holding M would block
  it: the edge is a certainty, not a history. In interrupt context, or
  with no mutex held, `complete` records nothing (fact 3; the common case
  -- `thread_exit`, every interrupt-driven I/O completion -- costs one
  load). Spinlocks held at `complete` are not recorded: no path from a
  spinlock node reaches a mutex or a completion node, so the edge could
  never be on a cycle, and it would only grow the graph. The edges are
  not written at the call but kept **pending in the thread**
  (`thread.completion_pending`: the object, its node, the held mutex
  nodes) and committed, with the cycle check, at the thread's next
  completion event, mutex release or exit. The one event that discards
  them instead is the next paragraph's.
- **Both orders.** The cycle check runs at whichever side closes it: a
  wait after a signal that held L (`C → L` then `L → C`), or a signal
  after a wait that held L (`L → C` then `C → L`). The report names the
  completion, the lock and the chain. After a report the operation
  proceeds (the tests use private objects), as every lockdep report does.
- **Self-signalling.** A completion is sometimes signalled **inside the
  waiter's own call chain**, with the waiter's locks merely inherited.
  The RAM block device completes a bio synchronously in `submit`, so
  `sync_io` under a VFS mutex completes its own `blk-sync` holding that
  mutex and then waits; `usb_sync_msg` and the AHCI `cmd_sync`, when a
  timed wait returns false, cancel the transfer or restart the port, which
  completes the object, and then wait plainly for the handshake. A thread
  cannot deadlock with itself, but the class cannot tell its own waiter
  from another thread's: the thread holds M, completes C holding M
  (`C → M`), then waits on C holding M (`M → C`): a cycle in the graph
  and no deadlock in the machine -- and the `C → M` edge, once written,
  would make every later wait of the class under M a false report. This
  is why the signal side is pending rather than written: when the
  thread's next completion event is **its own wait for the same object**,
  the pending `complete` is discarded, and the wait itself records
  nothing new (a timed wait that gave up has already recorded `M → C`,
  which is true). Any other next event -- a wait for another object,
  another `complete`, the release of a mutex, the thread's exit --
  commits the edges, so a remover completing a transfer somebody else
  waits for, under its own mutex, is in the graph by the time it drops
  that mutex: the cross-thread case the model is for. The pending record
  is 40 bytes in `struct thread`, present in every build so the layout is
  stable. What this does not recognise: a self-signal followed by a
  **poll** rather than a wait (the NVMe admin fallback without a vector,
  which drives the queue itself and polls `completion_done`) commits at
  the next mutex release; that path does not run under QEMU, and the
  survey records it.
- **Re-initialised completions.** `xhci_cmd` re-initialises one
  completion per command; the memory hold seams and the reaper hold do so
  per arming. The class is the name's, so each init finds the same class,
  and re-init costs the spinlock classification the first acquisition
  would have cost anyway.
- **`completion_done`** is a query, not a wait, and records nothing. A
  poll loop (the xHCI first-scan wait under `g_controllers_lock`, the
  NVMe admin fallback without a vector) is therefore outside the graph;
  the coverage listing shows such a class as waited by nobody.
- **Interrupt-usage labels.** A completion class has none (`check_usage`
  labels spinlock classes only), and nothing IRQ-used reaches it: its
  only predecessors are mutexes. The new edge `C → M` still goes through
  the ordinary IRQ-edge validation, for one code path.

### Against crossrelease and DEPT

Linux's **crossrelease** (4.14, reverted in 4.15) treated a completion
wait as the acquisition of a "crosslock" and, at the release, added a
dependency from the crosslock to every lock the *releasing context had
acquired since the crosslock was taken*, read from a per-task lock
history. The history is what produced the false positives: a kworker's
history spans unrelated work items; a lock taken and dropped on the way
to the release, for a reason that had nothing to do with it, became a
prerequisite; the window closed only for crosslocks currently held, so a
signal that happened to precede its wait recorded nothing. Hand
annotation could not keep up and the feature was removed. **DEPT**
(Byungchul Park's dependency tracker, RFCs 2022-2024, not merged)
generalises the same idea to every wait/event pair -- wait queues,
completions, page locks -- with per-context histories reset at context
boundaries and classes split per site; the review objections were the
same false positives on real workloads.

This design records no history. The signal side records only what is
**held at the signal**, which is a prerequisite by construction; the wait
side records only what is **held at the wait**, as every lockdep edge
does. Both orders are covered because both sides record persistently,
not only while a wait is in flight. The price is the shape crossrelease
caught and this does not: a signaller that takes and *releases* a mutex
before `complete` ("lock, unlock, complete") records no edge for it,
although a waiter holding that mutex would block it. That shape is the
survey's business (`docs/audit/2026-10-07-lockdep-completion-waits-report.md`):
every signaller in the tree is enumerated with the mutexes it takes on
its way to `complete`. One class of signaller deserves its own note: a
**worker thread** completing a barrier item depends on every item ahead
of it in its queue, so a waiter must hold nothing any item may take. The
network worker's items (`input_one` and the receive path, `age_work`,
`pcb_work`, `barrier_fn`) take spinlocks only -- the only `mutex_lock`
calls in `kernel-services/network` are `netif_unregister`'s own and the
`ksock_*` system-call paths -- so no mutex-holding waiter can be blocked
by it today. If an item ever takes a mutex, the mechanism is the callback
class applied to the worker: the worker holds a pseudo-class while it
runs any item and the barrier wait acquires it.

A second shape the model does not record follows from the classes being
acquired and never held: **a signaller that is itself blocked in another
wait** -- a completion, a `thread_join`, a `timer_cancel_sync` -- before
its `complete()` contributes no edge for that wait. A waiter holds M and
waits for C1; C1's signaller waits for C2 before completing C1; C2's
signaller needs M: a deadlock the graph cannot close, because the wait
for C2 records only what its waiter *holds* (mutexes), and a wait is not
held. Closing it would mean a thread holding the class of the completion
it waits for while it waits, which is what crossrelease did and where its
nesting problems began (a wait inside a wait, orders between completion
classes, every long-lived wait in the graph); the model stops at one
level on purpose. The survey of 2026-10-07 named the signallers that wait
before they complete: the xHCI port worker completes `xhci-first-scan`
after a scan that waits for `xhci-cmd` and `usb-sync`; a USB removal
completes `usb-sync` (through `ring_flush`) after `xhci_disable_device`'s
waits for `xhci-cmd`; and threads that join children before exiting
complete `thread-exit` after waits for other threads' `thread-exit`. None
of their second-level signallers takes a mutex (the xHCI event handler
and the exiting threads hold nothing), so no such chain exists today.

### Coverage: callback and completion classes never exercised

A class the boot never exercises contributes no edges, and the graph is
silent about it. Debug builds now record what was *registered* and what
*ran*, and the self-test runner prints the difference after the suite:

- **Timer callbacks.** `timer_setup` records the callback function (and
  the first setup site) in a bounded table in `timer.c`; `run_expired`
  marks it run. `timer_dump_callbacks` lists the functions set up and
  never run, with their setup counts.
- **Completion classes.** The class records whether any `complete` and
  any `wait_for_completion*` has named it; `lockdep_dump_completion_coverage`
  lists classes never signalled or never waited.

The listing is the evidence the plan item asks for: which paths the
suite does not drive (error and teardown paths, mostly), judged one by
one in the report, so that the gap is a named list rather than an
unknown.

## Raw interrupt-state pairing

The spinlock wrappers check their own `irqsave`/`irqrestore` (L14). Raw
`arch_irq_save`/`arch_irq_restore`, used directly by the scheduler, the
interrupt table, the FPU, the hypervisor backends and lockdep itself, had
no check. With `LOCKDEP=1` the two are lockdep's wrappers around the
architectures' `arch_irq_save_hw`/`arch_irq_restore_hw`, and
`arch_irq_restore_nopoint` -- `schedule()`'s restore, which has no
preemption point (`docs/kernel/scheduler/invariants.md` S31) -- is the same
check around `arch_irq_write_hw`; without it they are inline
pass-throughs, and release kernels contain only the hardware functions.

Each context keeps a stack of its outstanding saves (`struct
lockdep_irq_saves`, 16 deep): the current thread's, in `struct thread`, or
the CPU's before the CPU has a current thread. An interrupt handler shares
the interrupted thread's stack, because its own pairs are balanced before
it returns and so nest above the thread's. A context switch needs nothing:
`schedule_internal` saves on the outgoing thread, sets `current` before the
switch, and the incoming thread restores the save in its own earlier
frame. A new thread starts in `thread_trampoline` with
`arch_irq_enable()`, not a restore, so it pops nothing it did not push.

A restore reports `LOCKDEP_R_IRQ_STATE` when:

- the context has no outstanding save;
- the state differs from the innermost save's (out of order). The slot is
  consumed anyway, one restore per save;
- interrupts are enabled when it runs. The save masked them, and something
  in the region enabled them. The architectures diverge here: AArch64's
  restore of a masked state writes DAIF and masks them again, while
  x86-64's restore of a masked state does nothing and leaves them on. The
  check makes the bug fail on both.

`lockdep_thread_exit` reports a thread that exits with a save outstanding.
It runs before `thread_exit` disables interrupts and enters the scheduler,
whose own save is never undone by an exiting thread.

The bookkeeping runs with interrupts masked, after the hardware save and
before the hardware restore, so the context cannot change under it. An
NMI can still land anywhere, so a push reserves its slot (`depth`) before
filling it, and a pop reads before releasing. A balanced NMI between the
two uses the slots above. During a fatal report (`g_off`) nothing is
tracked. Plain `arch_irq_enable`/`arch_irq_disable` are not tracked: they
belong to boot code and the scheduler's own transitions. The
enabled-inside check catches the case where one of them breaks a saved
region.

## `might_sleep()`

The one annotation for "this call may block": a report if
`preempt_count != 0` (a spinlock or `quiesce_read_lock` is held) or
`irq_depth != 0`, with the held stacks. It is placed at the entry of
`mutex_lock`, `wait_event` (`waitqueue_prepare`), `semaphore_down`,
`wait_for_completion`, `thread_sleep_ns`, `synchronize_quiesce`,
`copy_from_user`, `copy_to_user`, `strncpy_from_user` (a demand fault
allocates), and `vm_kernel_alloc`. With `LOCKDEP=0` it is the
`preempt_count`/`irq_depth` panic those primitives already had (the sleep
check is correctness, not diagnostics, so it stays on); with `LOCKDEP=1` it
also prints the stacks.

## Reports

A report prints the kind, the offending lock and acquisition address, the
CPU's and the thread's held stacks (class name, subclass, address, in-IRQ
and IRQs-on flags) and, for an inversion, the chain of nodes that makes the
cycle; then it panics. Reports are also emitted by the panic path itself
(`lockdep_dump_held` from `panic`) so any crash shows what was held.

Self-tests cannot survive a panic, so `lockdep_expect(kind)` arms a
one-shot expectation: the next report of that kind increments a counter and
returns instead of panicking, and the acquisition proceeds (the tests use
private locks, so proceeding cannot deadlock). Any other kind still panics.
The expectation is cleared by the report; a test that armed one and saw no
report fails on the counter.

## Cost and scope

Enabled by `LOCKDEP=1` (the debug default; release defaults to 0).
Lock/thread layouts and `might_sleep`'s always-on half remain with it off.
Per acquisition: a per-CPU stack push, a class lookup (cached
after the first), a recursion scan of the held set (≤ 24 entries), a bitmap
test per held lock, and, for a new edge, the raw lock and a bounded search.
No allocation anywhere: every table is static, sized for the tree with
headroom (`invariants.md` L9): 512 classes, of which a full debug boot
uses 382.

## The fixes this milestone makes

Each is a place the audit or the checker found the documented order and the
code disagreeing, or a lock held where it must not be.

### VFS: the mount lock and the vnode cache (V7, audit #21)

`mount->lock` was a mutex taken by `vnode_release`, which runs from
`vnode_put` under whatever the caller holds, including a parent
`vnode->lock`: mount lock under vnode lock, the reverse of V7. And
`vnode_lookup_cached` skipped vnodes at refcount 0 (a release in progress,
waiting for the mount lock the lookup holds), instantiating a second vnode
for the same inode.

Now `mount->lock` is a spinlock protecting only the hash and `nr_vnodes`,
a leaf below every mutex. The unhash moves from the release into the put:
`vnode_put` reads the count; if it is 1 it takes the hash lock, re-reads
the count (a lookup that raised it since must have held the hash lock, so
after we hold it the count is stable), unhashes when still 1, drops the lock
and puts. A hashed vnode therefore always has a reference, and
`vnode_lookup_cached` takes one plainly under the hash lock. The release no
longer touches the mount.

### VFS: rename (audit #20)

The two parents were locked in address order while every other path locks
parent before child: `rename("/a/x", "/a/b/y")` with `&b < &a` against
`rmdir("/a/b")` was an ABBA, and the "directory under itself" walk read
`..` without the ancestors' locks.

Now each mount has a `rename_lock` mutex taken for the whole of
`vfs_rename` after the two parents are resolved. Under it the parent
relationship is stable (only rename changes a directory's parent), so the
walk from `ndir` to the root is sound without per-directory locks, and it
also decides the lock order: if `odir` is an ancestor of `ndir` lock `odir`
first, if `ndir` is an ancestor of `odir` lock `ndir` first, otherwise the
two directories are unrelated and no other path can lock them in the
opposite order (parent-then-child needs an ancestry; another rename is
excluded by the mutex), so address order is safe there. The second parent is
annotated `VNODE_NESTED_PARENT2`; children found under them
`VNODE_NESTED_CHILD`. The mutex serialises renames per mount, which is the
price of a stable ancestry check; renames are rare.

### VFS: `vfs_sync` (audit, medium)

It held `g_mounts_lock` across every filesystem's `sync`, blocking mount and
unmount behind a cosmofs commit. It now snapshots the mount list with a
reference on each mount under the lock, drops the lock, syncs each, and puts.
A mount unmounted meanwhile is synced once more harmlessly (`fs->sync` on an
unmounted cosmofs is a no-op on a committed tree) or skipped when its
`unmounting` flag is set.

### futex: the copy under the bucket lock (audit #14)

`futex_wait` read the user word with `copy_from_user` while holding the
bucket spinlock with interrupts off; a demand fault would allocate under a
raw spinlock, and a fatal fault killed the process with the lock held. The
compare and the enqueue must still be atomic with respect to a waker, or a
wake between them is lost.

Now each bucket carries a `wake_seq` counter, incremented by `futex_wake`
under the bucket lock whenever it wakes at least one waiter or finds none
(a store-then-wake protocol's wake always bumps it). `futex_wait` reads
`wake_seq` under the lock, drops the lock, copies the word (may fault, may
sleep), compares, re-takes the lock and enqueues only if `wake_seq` is
unchanged; otherwise it returns 0 as a spurious wake, which the futex
contract permits and every user (musl) retries. Lost wake: impossible, since
a wake that ran between the read and the enqueue bumped the counter. The
copy now runs with no spinlock held, which `might_sleep` in
`copy_from_user` enforces.

### Scheduler: remove GIC nesting and specify runqueue successors (S2/S4)

The original audit found `request_resched` → `ipi_send` → `arch_ipi_send`
→ `sgi_for_vector` taking the GIC lock under a runqueue on AArch64.
Binding the SGI once at `ipi_init` through `arch_ipi_bind` makes sending
lock-free. Current runqueues are not leaves: migration takes ordered
runqueue pairs (S24), and address-space switching can take `asid`.
Expected self-test reports also record logging locks under a runqueue.
See L6 and `testing.md` for the observed edges; the old zero-successor
rule no longer describes the kernel.

### Network

The documented order (`sock->lock` → protocol spinlock → `arp`/`nd` →
`netif->lock` → driver → mbuf) is what the checker records on the boot
tests; `testing.md` lists the recorded edges for the network classes so the
document and the graph can be compared.

## Ownership and lifetime

All checker state is static. A lock's cached class index lives in the lock;
class records and their copied names are never freed. Original name storage
may disappear after the lock's last use, including on module unload. Held entries are removed at release; a thread that exits with mutexes
held is a report.

## Concurrency

The checker's raw lock (`g_raw`, irqsave, untracked) covers the
class table and the graph. Held stacks are per CPU (written only by that
CPU) and per thread (written only by that thread). Class `usage` bits are
set under the raw lock. Acquisition tracking takes no tracked lock and
allocates nothing. Same-CPU raw-lock recursion fails stop; arbitrary
NMI/#MC writer reentrancy is unsupported. Normal graph dumps allocate
private snapshot storage before acquiring the raw lock.

## Memory

800 KiB graph (2048 nodes squared, as a bitmap), 512 classes × 96 bytes, 24 × 24 bytes per CPU, 8 × 24 bytes per
thread, 264 bytes per thread (and per CPU) of raw interrupt saves, 1 KiB
for the spinlock-class to completion-class table, and 40 bytes per thread
for the pending `complete()` record. The
graph and the per-CPU save stacks exist only with `LOCKDEP=1`; lock/thread
layouts stay stable when disabled (the per-thread save stack is in every
build, unused without lockdep).

## Error handling

With `LOCKDEP=1`, every validator violation panics with the full report;
there is
no "warn once" mode, because a lock-order bug that fires once is a deadlock
that will fire later. Table exhaustion (classes, held entries) is itself a
report.

## Performance

Self-test builds keep an atomic per-CPU pointer to the innermost observed
spin wait. `lock_common` publishes only after an exchange fails, saving
any interrupted wait, and restores that pointer on acquisition before
returning or restoring IRQs in LOCKDEP=1. An uncontended nested acquisition
does not touch it. The single CPU writer cannot migrate while spinning;
remote tests read the pointer atomically and retain the referenced lock's
lifetime themselves. No diagnostic dereferences it. This test aid takes
no locks, changes no spinlock layout or module ABI, and compiles out with
SELFTEST=0. It is not an ownership API or a global deadlock detector.
Contended spin measurements include its slow-path stores and owner polls.

With `LOCKDEP=0` the validator hooks compile out. Enabled acquisition cost is a
few dozen instructions on the hot path (cached class, recursion scan of a
short stack, bitmap tests); a new edge takes the raw lock and a bounded
search once. `testing.md` records the debug boot-test time before and after.

## Security

The checker runs with `LOCKDEP=1` and has no user-facing surface. The
fixes close a local denial of service (a futex fault leaving a bucket locked
for every process), a local corruption (duplicate vnodes for one inode) and
a two-process deadlock (rename versus rmdir).

## Future extensibility

- Class keys by address of a static key object instead of the name literal
  if two independent lock sites ever need one name.
- Read-write locks: two nodes per class with the reader/writer rules.
- A "chain" cache (Linux's `lock_chain`) if the recursion scan becomes
  measurable.
- Lock statistics (contention, hold time) on the same hooks.

## Configuration

`LOCKDEP=0/1` maps to `CONFIG_LOCKDEP` through the normal make configuration.
The default is 1 in debug and 0 in release. Use a distinct `OUT` for each
configuration: the build rules do not track command-line flag changes.
Lock layouts and module ABI are stable across this option; the thread held
array also remains in the layout. The graph and runtime hooks compile out
when disabled. Atomic class-cache publication and graph bitmap hot-path
accesses do not rely on unsynchronized shared loads/stores.
