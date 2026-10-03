# Kernel locking rules

This is the canonical entry point for choosing a synchronization primitive.
Subsystem invariants still specify the detailed orders: scheduler S24/S25,
VFS V7/V32, networking N-L1 through N-L4 and N22, quiescence Q1–Q19,
module M18/M22–M24. Source and the runtime graph govern stale examples.

| Primitive / context | May sleep? | IRQ use | Preemption | User memory / allocation |
|---|---|---|---|---|
| `spin_lock`, nested, successful trylock | No | Only with an IRQ-safe calling convention; plain lock preserves IRQ state | Disabled until unlock | No faulting user copy; only proven nonblocking allocation |
| `spin_lock_irqsave` | No | Yes; use for locks shared with handlers | Disabled; IRQs masked until matching restore | Same as spinlock |
| `mutex_lock`, nested | Yes, even if presently uncontended | No | May block/migrate while held | User copies may fault; allocation may block if other held locks allow |
| `mutex_trylock` | Does not wait | Ownership is still thread-based; not an IRQ synchronization API | Internal spinlock briefly disables preemption | Successful ownership follows mutex rules; other atomic-context rules still apply |
| `semaphore_down`, completion wait, wait queue wait, thread sleep | Yes | No; up/complete/wake may run in IRQ | Blocking boundary requires zero preemption count | No caller spinlock or quiesce reader section |
| `quiesce_read_lock` | No | Nestable; IRQ/spinlock contexts already protect readers implicitly | Disabled until read unlock | No faulting user copies or blocking allocation |
| `synchronize_quiesce`, synchronous IRQ unregister | Yes | No | Thread context, no spinlock/read section | Drop registration lock before grace-period wait |
| `call_quiesce` | Enqueue does not sleep; callback runs in a worker | May enqueue from IRQ | Brief irqsave protection at enqueue | Callback gets thread context after grace period; preserve ownership until callback |
| `timer_cancel_sync` | Spins rather than sleeps | Excludes own callback | Queue lock used briefly; wait may span CPUs | Must hold no lock needed by the callback or its completion path |
| IRQ masking alone | Does not make blocking safe | Separate from irq_depth | Does not by itself increment preempt_count | Do not infer permission to sleep from a zero count alone |
| Atomics / reference count | No ownership exclusion implied | Depends on operation | No implicit migration barrier | A held reference pins lifetime; it does not serialize mutable data |

No kernel rwlock exists. Do not add a fictitious rwlock annotation to
callers of these APIs.

## Ownership, nesting and release

Spinlocks are nonrecursive and owned by a CPU; mutexes are nonrecursive
and owned by a thread. A scheduler runqueue lock may be handed from the
outgoing to the incoming thread on that same CPU. That is why lockdep's
spin stack is per CPU and its mutex stack is per thread. Preemption
disabling is the migration barrier; do not retain a per-CPU pointer across
a sleep without a one-CPU affinity guarantee.

Unlock the object actually acquired. Non-LIFO release is legal and tracked
by object identity. Restore the IRQ state saved for the corresponding
irqsave region; never restore an unrelated state. Preemption disables and
enables must balance; `preempt_enable` asserts a positive count.

Same-class multi-object nesting needs a documented subclass invariant.
Vnode parent/second-parent/child use subclasses 0/1/2. Two futex buckets
use stable address order, with the second annotated subclass 1. Runqueue
pairs use increasing CPU-id order and separate `runqueueN` classes.
An annotation does not permit recursively acquiring the same object.

## Names and metadata

Lock classes use equal name contents plus kind; allocate one logical name
per family. Do not merge unrelated families by giving them the same name.
Separate names for runqueues represent their explicit total order.
Names must be NUL-terminated, at most 63 characters, and valid/unchanged
throughout the lock's lifetime. The validator copies names so historical
edges survive object destruction and module unload. It never allocates
from a hook. Exhaustion and overlong metadata report rather than silently
dropping instrumentation. Public lock layouts do not depend on LOCKDEP.

## Callbacks, allocation and user access

A callback under a lock inherits its caller's context and held locks.
Include its acquisitions in the order graph. IRQ and timer callbacks
must not sleep. A filesystem callback under a vnode or filesystem mutex
may sleep, but must preserve the subsystem's order. Character-device I/O
runs outside the vnode mutex (V32). Driver completion callbacks and console
sinks must obey spinlock/IRQ rules. Module init/shutdown cannot re-enter
the module loader while its mutex is held (M18).

`kmalloc` currently does not sleep. That does not justify invoking arbitrary
allocation APIs under spinlocks: `vm_kernel_alloc` and faulting user access
have `might_sleep` boundaries. Never call `copy_from_user`, `copy_to_user`
or `strncpy_from_user` while holding a spinlock or read section. Copy into
a kernel buffer before protocol/bucket locking where needed.

Unregister removes new access first, then drains readers/callbacks before
dropping the final reference. Never replace this lifetime protocol with a
lockdep suppression. Lockdep does not yet model all callback waits or all
transitive IRQ conflicts; a silent graph is not proof that these waits
are safe.

## Configuration and diagnostics

`LOCKDEP=1` enables validation and defaults on in debug; `LOCKDEP=0`
defaults in release. Use separate `OUT` trees when changing the option.
The always-on sleep and primitive ownership checks remain. Debug reports
include CPU/thread/context, lock classes, addresses and held stacks;
long cycle paths print at most their last eight nodes. No user-controlled
metadata or userspace mappings expose the graph.
