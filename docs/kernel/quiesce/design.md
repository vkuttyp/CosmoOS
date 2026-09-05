# Kernel object lifetime and quiescence: design

## Data structures (`kernel/core/quiesce.c`, `kernel/include/kernel/quiesce.h`)

```c
struct quiesce_cpu {                            /* one per CPU, its own cache line */
    uint64_t seen_epoch;                        /* last epoch this CPU published at a quiescent point */
    uint32_t depth;                             /* debug: nested read sections */
    uint64_t transitions;                       /* debug: quiescent points passed */
} __attribute__((aligned(64)));

struct quiesce_state {                          /* kernel/include/kernel/quiesce_core.h */
    uint64_t epoch __attribute__((aligned(64)));   /* the current grace-period epoch; advanced by waiters */
    struct quiesce_cpu cpus[QUIESCE_MAX_CPUS];
};

struct quiesce_head { struct quiesce_head *next; void (*fn)(struct quiesce_head *); };
```

The epoch algorithm lives in `quiesce_core.h` as inline functions over a
`struct quiesce_state`, with no kernel dependency, so the host test
(`tests/host/test_quiesce.c`) drives the same code under ASan with real
threads; `kernel/core/quiesce.c` wraps it with the CPU registry, the
sleeping wait, the callback worker and the statistics.

The callback list is a spinlocked (irqsave) singly linked list drained by
one kernel thread, `quiesce`, at priority `SCHED_PRIO_DEFAULT - 4`.

## Read-side sections

A read-side section is a preemption-disabled region. `quiesce_read_lock()`
is `preempt_disable()` plus, in debug builds, `g_cpus[cpu].depth++`;
`quiesce_read_unlock()` reverses both and asserts the depth was positive.
Interrupt handlers are implicitly read-side (interrupts off, `irq_depth > 0`).
Every spinlock is therefore also a read-side section, which is what makes
the quiescent-state definition below correct without any new bookkeeping
in the spinlock. A read-side section must not block: `sched_block_current`,
`waitqueue_prepare`, `mutex_lock`, `semaphore_down` and
`wait_for_completion` already panic when `preempt_count != 0`.

## The quiescent state

A CPU is quiescent when it is provably outside every read-side section.
That is true at exactly these points, and nowhere else:

1. **Interrupt return** to a context with `irq_depth == 0` **and**
   `preempt_count == 0` **and** interrupts enabled in the interrupted
   frame. This is the predicate the arch tails already evaluate for
   preemption (`x86_trap_dispatch`, `aarch64 handle_irq`);
   `quiesce_note_quiescent()` is called right there. An interrupt that
   lands inside a read section, a spinlock or any preempt-disabled region
   records nothing, because `preempt_count` of the interrupted context is
   non-zero; a nested interrupt records nothing because `irq_depth` is
   still positive on the inner return. The x86-64 paranoid path
   (`x86_trap_paranoid`) never records: it may have interrupted the
   scheduler.
2. **`schedule()`** (`schedule_internal`), which cannot be entered with
   `preempt_count > 0`.
3. **The idle loop**, before `hlt`/`wfi` and with `preempt_count == 0`:
   it publishes the epoch. There is no "idle" shortcut that would let a
   waiter skip a halted CPU: the interrupt that wakes it runs a handler,
   and a handler is a read-side section, so the halted CPU must publish
   again at that interrupt's return (point 1), which it does within one
   tick. An earlier draft kept an `idle` flag for this; it was unsound and
   was dropped.
4. **`synchronize_quiesce()` itself**: the calling CPU is in thread
   context with `preempt_count == 0` (asserted), so it records its own
   epoch before waiting.
5. **A CPU coming online** (`sched_start_cpu`), before it can run a reader.

## The epoch algorithm and its memory ordering

The reclamation protocol is: unlink the object → `synchronize_quiesce()`
→ free. The waiter:

```c
uint64_t target = __atomic_add_fetch(&g_epoch, 1, __ATOMIC_SEQ_CST);      /* (W1) */
publish own epoch;
for each online CPU c (snapshot of cpu_online_mask at W1):
    wait until __atomic_load_n(&g_cpus[c].seen_epoch, __ATOMIC_ACQUIRE) >= target;   /* (W2) */
```

The quiescent point:

```c
uint64_t e = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);                   /* (Q1) */
__atomic_store_n(&g_cpus[cpu].seen_epoch, e, __ATOMIC_RELEASE);            /* (Q2) */
```

Why each ordering exists:

- **W1 is `SEQ_CST`** so the unlink the caller performed before it (a
  release store of a pointer, or a list removal under a lock) is ordered
  before the new epoch becomes visible. A reader that starts a section
  after observing the new epoch therefore cannot find the object: it is
  gone from the structures the reader walks.
- **Q1 is an acquire load** for the same reason from the reader's side:
  a reader that publishes epoch `target` at Q2 observed `g_epoch == target`
  at Q1, and every read section it enters afterwards sees the unlink that
  preceded W1.
- **Q2 is a release store**: it orders every load and store the CPU made
  in the read-side sections that preceded the quiescent point before the
  epoch value. Nothing about the reader's earlier accesses is reordered
  past it.
- **W2 is an acquire load**: paired with Q2 it gives release/acquire
  synchronisation per CPU. Once the waiter has observed `seen_epoch >=
  target` from every online CPU, every access those CPUs made to the
  object in read sections before their quiescent points happens-before
  the free that follows. Transitively (through the waiter) the readers'
  last accesses happen-before the reclamation, on x86-64 (where release
  and acquire are plain loads and stores) and on AArch64 (`LDAR`/`STLR`).
- The `>=` comparison, not `==`, because two waiters may advance the
  epoch twice before a CPU passes one quiescent point; the CPU publishes
  the latest epoch, which satisfies both.
- Only the CPUs online at W1 are waited for. A CPU that comes online
  later publishes the current epoch before running anything, so it can
  hold no reference from before W1.

The waiter sleeps between polls (`thread_sleep_ns(TICK_NS / 2)`), never
spins. After two ticks without progress it sends `IPI_RESCHEDULE` to the
straggling CPUs so their interrupt-return path re-evaluates the predicate
(a CPU running a long preempt-enabled loop with interrupts on will pass
through the tick anyway; the IPI shortens the wait for a CPU whose tick
happened to land inside a spinlock). A reader that stays in a
preempt-disabled section indefinitely delays every grace period, which is
the correct semantics and a bug in that reader; after one second the
waiter logs the CPU and keeps waiting (debug builds panic after ten
seconds: `quiesce: CPU %u has not reached a quiescent state`).

## `call_quiesce()`

Deferred reclamation from contexts that cannot sleep. The head is pushed
on a global list under an irqsave spinlock and the worker is woken. The
worker takes the whole list, calls `synchronize_quiesce()` once for the
batch, then runs every callback in thread context. Callbacks may free
memory and take mutexes. Ordering: a callback runs after one full grace
period that began after `call_quiesce` returned, which is what the
caller's unlink-before-call gives.

## `synchronize_irq()` and `interrupt_unregister_sync()`

Interrupt handlers run with interrupts disabled inside an interrupt, so
a handler that was mid-flight when its slot was cleared finishes before
that CPU's next interrupt-return quiescent point. `synchronize_irq(vector)`
is `synchronize_quiesce()` plus statistics; `interrupt_unregister_sync`
and `interrupt_unregister_vector_sync` clear the slot then synchronise.
`irq_release()` and `irq_release_msi()` use the synchronous forms, so
after they return no handler holds the `arg` the driver is about to free.
They sleep and are called from thread context (driver remove) only.

## `timer_cancel_sync()`

Timers run on the CPU that armed them, in interrupt context, and
`struct timer_queue` now records `running`, the timer whose callback is
executing. `timer_cancel_sync(t)`:

1. `timer_cancel(t)` (removes a pending timer under the queue lock).
2. If `q->running == t` and the caller is on another CPU: spin with
   `arch_cpu_relax()` until it is not (the callback is short interrupt
   work), then cancel again in case the callback re-armed the timer, and
   repeat. The wait is bounded by the callback's own duration.
3. If the caller is on the timer's CPU: in thread context the callback
   cannot be running concurrently, so the cancel is already synchronous;
   from inside the timer's own callback it is a programming error and
   panics.

Callers may hold spinlocks (the wait does not sleep) but not the lock the
callback takes, which the network code satisfies: TCP timer callbacks
take only `g_work_lock`.

## The kobject rule and the converted objects

**Rule.** Memory reachable from an interrupt handler, a timer callback or
another CPU without a lock is freed only from a kobject release or a
`call_quiesce` callback, never directly by the code that unlinked it.

- `struct device` gains `void (*release)(struct device *)`, required by
  `device_register` (debug builds panic on NULL). `device_release` (the
  kobject release) calls it; `pci_device` frees itself there; `vpci`
  frees its container from `virtio_device`'s release, so `vpci_remove`
  puts references instead of `kfree`.
- `struct blkdev` gains `release`; `blkdev_release` calls it; `vblk`
  frees its container there. `blk_unregister` marks the device `gone`
  and `blk_submit` fails with `-ENODEV` afterwards, so a holder
  (`pool_open`, the VFS) that still has the pointer gets an error, not a
  freed structure.
- `struct netif` becomes a kobject: `netif_register` takes the
  registry's reference, `netif_default`/`netif_find`/`netif_loopback`
  return a referenced interface (callers put), `ipv4_route`/`ipv6_route`
  return referenced interfaces and every output path puts after
  transmit. `netif_unregister` unlinks, purges the receive queue of
  packets from that interface, waits for the worker to pass a barrier
  work item (so an `input_one` that already held the pointer has
  finished), and drops the registry reference. `vnet` frees its container
  in the netif release.
- `tcp_pcb` keeps its state-machine lifetime but `pcb_free_locked` uses
  `timer_cancel_sync` for the three timers, closing the callback-versus-free
  race; `tcp_accept(listener, owner)` attaches the socket under `g_lock`
  so an RST cannot free a half-accepted child; `udp_input` takes a socket
  reference under the lock before waking.
- `vq` freeing (`virtq_free`) happens after `pci_msix_release`, which now
  synchronises with any handler still holding the queue pointer.

## Module unload protocol

```text
module_unload(name):
  1. mutex g_lock; find LIVE module; refs (dependants) == 0 else -EBUSY
  2. state = GOING                       stop new users: module_find ignores GOING
  3. info->shutdown()                    the module unregisters drivers, devices, netifs, timers
  4. synchronize_quiesce()               no CPU is still executing module text from a handler
                                         or a lock-free reader entered before step 2
  5. wait until m->live_objects == 0     kobjects whose type lives in the module (counted by
                                         kobject_init/put through module_owner_of), bounded:
                                         5 s, then -EBUSY and the module stays GOING (zombie,
                                         listed by module_dump; a later unload retries from 4)
  6. unlink from the list; drop dependency refs; free text/rodata/data
```

`module_owner_of(addr)` walks the module list under `quiesce_read_lock`
(the list is published with release stores and unlinked before a grace
period, step 4 to 6), so `kobject_init` can attribute a type to its
module from any non-sleeping context.

## Ownership and lifetime

`quiesce_head`s are owned by the caller until the callback runs; the
worker never allocates. `struct quiesce_cpu` is static. The worker thread
is created in `quiesce_init` (after `sched_init`, before any user).

## Concurrency

`g_epoch` is written only by `synchronize_quiesce` (RMW) and read at
quiescent points. `seen_epoch` is written only by its own CPU. The
callback list lock is a leaf irqsave spinlock. `synchronize_quiesce` may
be called with mutexes held (they are sleeping locks) but never with a
spinlock held (asserted: `preempt_count == 0`), and never from the
quiesce worker's callbacks in a way that waits on itself. Lock order:
callers' mutexes → (sleep) → nothing; `timer_cancel_sync` spins under the
caller's spinlocks but takes only `timer_queue.lock`.

## Memory

No allocation on any path. Per-CPU state is `CONFIG_MAX_CPUS` × 64 bytes.

## Error handling

`synchronize_quiesce` cannot fail; it can be delayed by a misbehaving
reader, which it logs. `timer_cancel_sync` cannot fail. `module_unload`
returns `-EBUSY` with the module left GOING when objects do not drain.

## Performance

Read-side entry/exit is a preemption count increment/decrement (plus one
debug counter). A quiescent point is one acquire load and one release
store, only on the paths that already evaluate the preemption predicate.
`synchronize_quiesce` costs one to two ticks of latency in the common
case and no IPIs; `call_quiesce` amortises one grace period over a batch.
Measured numbers: `testing.md`.

## Security

Use-after-free of kernel objects is the primary exploitation primitive
class; this subsystem removes the class structurally for the objects it
converts, and the rule plus the debug assertions keep new code inside it.

## Future extensibility

CPU offline: mark the CPU's `seen_epoch` as "gone" after it has stopped
running readers (its last `schedule()`), and exclude it from the wait;
online reverses it (already done). A per-CPU callback list and a
per-node worker if the single worker ever contends. Expedited grace
periods (IPI every CPU immediately) for latency-sensitive callers.
