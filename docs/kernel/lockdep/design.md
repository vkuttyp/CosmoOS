# Lock discipline and lockdep: design

## Data structures (`kernel/include/kernel/lockdep_core.h`, `kernel/core/lockdep.c`)

```c
#define LOCKDEP_MAX_CLASSES   160       /* classes in the tree today: ~95 (a mutex and its spinlock are two) */
#define LOCKDEP_SUBCLASSES    4         /* nesting levels per class */
#define LOCKDEP_MAX_NODES     (LOCKDEP_MAX_CLASSES * LOCKDEP_SUBCLASSES)
#define LOCKDEP_MAX_HELD      24        /* per CPU: spinlocks, interrupt context included */
#define LOCKDEP_MAX_HELD_MUTEX 8        /* per thread */

struct lock_class {
    const char *name;                   /* the init-site literal; the class key */
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
so the host test drives them under the sanitizers. The bitmap is 640 nodes ×
80 bytes = 50 KiB.

## Classes and nodes

A class is a lock's initialisation name: the string literal passed to
`SPINLOCK_INIT`, `spinlock_init` or `mutex_init`. Locks initialised from one
site share the name pointer and are one class (every `vnode->lock`, every
`socket->lock`, every run queue); the linker merges identical literals from
different sites, which is what the rule wants (two locks named `"tcp"` are
the same class). A dynamically built name is not a valid class key and the
tree has none.

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
   B → … → Aᵢ.
3. Otherwise set `before[Aᵢ] |= B` for every Aᵢ (edges from every held lock,
   not only the innermost, so a chain seen once in pieces is still caught).

Edges are recorded and checked under the checker's raw spinlock, taken with
interrupts disabled, so the graph is consistent; the lock is not itself
tracked. The search is bounded by the node count (640) and runs only when
the edge set changes or a cycle exists: a repeated acquisition whose edges
are already recorded short-circuits after the recursion check with a
bitmap test per held lock.

## Interrupt safety

A class acquired in interrupt context (`irq_depth > 0`) is **IRQ-safe**; a
class acquired by `spin_lock` with interrupts enabled at the time is
**held-with-IRQs-on**. A class that is both is a report: an interrupt
arriving while the lock is held on this CPU would take the same lock and
spin forever. `spin_lock_irqsave` never sets the second bit;
`spin_lock` with interrupts already off (inside another irqsave section)
does not either. Each bit records the first instruction that set it so the
report names both sites. Mutexes are never interrupt-safe and the existing
panic on `mutex_lock` in interrupt context stays.

Because of this rule, the graph does not need edges between interrupt
context and the interrupted thread: the only cross-context deadlock (thread
holds L with interrupts on, interrupt takes L) is exactly the two-bit
conflict, and every other cross-CPU ordering is an ordinary ABBA the graph
sees.

## `might_sleep()`

The one annotation for "this call may block": a report if
`preempt_count != 0` (a spinlock or `quiesce_read_lock` is held) or
`irq_depth != 0`, with the held stacks. It is placed at the entry of
`mutex_lock`, `wait_event` (`waitqueue_prepare`), `semaphore_down`,
`wait_for_completion`, `thread_sleep_ns`, `synchronize_quiesce`,
`copy_from_user`, `copy_to_user`, `strncpy_from_user` (a demand fault
allocates), and `vm_kernel_alloc`. In release builds it is the
`preempt_count`/`irq_depth` panic those primitives already had (the sleep
check is correctness, not diagnostics, so it stays on); in debug builds it
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

Debug builds only, apart from the `class` field and `might_sleep`'s
always-on half. Per acquisition: a per-CPU stack push, a class lookup (cached
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

### Scheduler: the run-queue lock is a leaf (S2/S4)

S2 says nothing is acquired under `runqueue.lock`; on AArch64
`request_resched` → `ipi_send` → `arch_ipi_send` → `sgi_for_vector` took
the GIC lock under it. The SGI for an IPI vector is now bound once, at
`ipi_init`, through `arch_ipi_bind(vector)` (a no-op on x86-64), and
`arch_ipi_send` reads the binding without a lock. S4 ("`waitqueue.lock` →
`runqueue.lock`") is generalised to "the run-queue lock is a leaf that any
lock may precede" (`process.lock`, the futex bucket, `tty.lock` and
`waitqueue.lock` all do), which the checker verifies as the absence of
edges out of the run-queue class.

### Network

The documented order (`sock->lock` → protocol spinlock → `arp`/`nd` →
`netif->lock` → driver → mbuf) is what the checker records on the boot
tests; `testing.md` lists the recorded edges for the network classes so the
document and the graph can be compared.

## Ownership and lifetime

All checker state is static. A lock's class index lives in the lock and is
never reused; a class is never freed (a freed lock's name literal outlives
it). Held entries are removed at release; a thread that exits with mutexes
held is a report.

## Concurrency

The checker's raw lock (`g_lockdep_lock`, irqsave, untracked) covers the
class table and the graph. Held stacks are per CPU (written only by that
CPU) and per thread (written only by that thread). Class `usage` bits are
set under the raw lock. The checker is re-entrancy safe: it takes no
tracked lock and allocates nothing.

## Memory

50 KiB graph, 160 classes × 40 bytes, 24 × 24 bytes per CPU, 8 × 24 bytes per
thread. Debug builds only for all but the `class` field.

## Error handling

Every violation is a panic in debug builds with the full report; there is
no "warn once" mode, because a lock-order bug that fires once is a deadlock
that will fire later. Table exhaustion (classes, held entries) is itself a
report.

## Performance

Not measured for release builds (no code). Debug-build acquisition cost is a
few dozen instructions on the hot path (cached class, recursion scan of a
short stack, bitmap tests); a new edge takes the raw lock and a bounded
search once. `testing.md` records the debug boot-test time before and after.

## Security

The checker runs only in debug builds and has no user-facing surface. The
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
