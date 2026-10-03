# Lock discipline and lockdep: design

## Data structures (`kernel/include/kernel/lockdep_core.h`, `kernel/core/lockdep.c`)

```c
#define LOCKDEP_MAX_CLASSES   320       /* includes separate runqueue classes */
#define LOCKDEP_SUBCLASSES    4         /* nesting levels per class */
#define LOCKDEP_MAX_NODES     (LOCKDEP_MAX_CLASSES * LOCKDEP_SUBCLASSES)
#define LOCKDEP_MAX_HELD      24        /* per CPU: spinlocks, interrupt context included */
#define LOCKDEP_MAX_HELD_MUTEX 8        /* per thread */

struct lock_class {
    char name[64];                      /* owned copy; contents plus kind are the key */
    unsigned usage;                     /* LOCKDEP_USED_IN_IRQ | LOCKDEP_HELD_IRQS_ON */
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
so the host test drives them under the sanitizers. The bitmap is 1280 nodes
(320 classes × 4 subclasses) × 160 bytes = 200 KiB when `LOCKDEP=1`.

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
2. If B reaches any Aᵢ in the graph (`before[B] ⊇* Aᵢ`, a depth-first search
   over the bitmaps with a visited set): **lock-order inversion**. The
   report shows the held stack, B's acquisition, and the recorded chain
   B → … → Aᵢ. Long diagnostic paths keep their last eight nodes and are
   labeled accordingly; the output length is the stored count, so printing
   cannot overrun the buffer. Detection still searches all 1280 nodes.
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
Normal graph dumps allocate private storage before taking the raw lock,
copy the complete bounded graph under it, and print from that snapshot
after releasing it. Counts, metadata, and edges therefore describe one
instant even if logging or another CPU adds dependencies during output.
The copy is 232968 bytes (about 228 KiB); the heap temporarily reserves
a 256 KiB page allocation and frees it after printing. There is no extra
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
on debug held-stack updates. Local-only reads and thread mutex stacks
retain their existing ownership rules. This does not make NMI writers
reentrant or provide simultaneous snapshots of all CPUs.

The x86 `trap-paranoid` regression verifies the read-only NMI boundary
with real local-APIC delivery while the graph raw lock is held, including
an interrupted held-stack update. Current lockup NMI sampling and corrected
machine-check handlers avoid tracked locking. General lockdep writer
instrumentation from NMI/#MC remains unsupported; passing the snapshot
test does not make the graph raw lock reentrant.

The search is bounded by the node count (1280) and runs only when
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
completion waits use a separate bounded lock profile (below); this is not a
general callback graph. NMI/#MC reentrancy remains unsupported. Trylock edges are not
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
Profiles cover only callback executions already observed, so unexecuted
callback paths are not proved safe. This does not cover IRQ unregister,
quiescence, module teardown, or arbitrary completion waits.

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
headroom (`invariants.md` L9).

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

The checker's raw lock (`g_lockdep_lock`, irqsave, untracked) covers the
class table and the graph. Held stacks are per CPU (written only by that
CPU) and per thread (written only by that thread). Class `usage` bits are
set under the raw lock. The checker is re-entrancy safe: it takes no
tracked lock and allocates nothing.

## Memory

200 KiB graph, 320 classes × 88 bytes, 24 × 24 bytes per CPU, 8 × 24 bytes per
thread. The graph exists only with `LOCKDEP=1`; lock/thread layouts stay
stable when disabled.

## Error handling

With `LOCKDEP=1`, every validator violation panics with the full report;
there is
no "warn once" mode, because a lock-order bug that fires once is a deadlock
that will fire later. Table exhaustion (classes, held entries) is itself a
report.

## Performance

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
