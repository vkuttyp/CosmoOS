# Interrupt Subsystem: Design

## The table

One `struct interrupt_slot` per vector, `INTERRUPT_MAX_VECTORS` = 1344
slots, statically allocated. `interrupt_init` asks the architecture how
many vectors it actually dispatches (`arch_trap_vector_count()`: 256 on
x86-64, 1312 on AArch64) and panics if that exceeds the constant, so an
architecture with a larger vector space fails loudly at boot rather than
indexing past the array. AArch64 folds the 1020 GIC INTIDs, a spurious
vector, the synchronous exception kinds and 256 dynamic software vectors
into this space in its arch layer (`docs/kernel/arch/aarch64/design.md`,
"Vector numbering").

## Registration

Registration and removal hold a raw per-vector writer lock with local
IRQs masked. Competing CPUs cannot both claim an empty slot or overwrite
its alternating record index. No allocation, tracked lock, handler call,
or grace-period wait occurs under this lock; it also works during early
boot. Mutations from NMI/#MC are unsupported because they could interrupt
a writer on the same CPU. Ordinary IRQ writers are safe because IRQs are
masked during each hold. Dispatch never takes the writer lock.

```
interrupt_register(v, fn, arg, name):
    validate v and fn
    lock slot.writer with local IRQs masked
    slot.cur != NULL → unlock and return -EBUSY
    fill next alternating record: fn, arg, immortal name
    atomic_store_release(&slot.cur, record)
    unlock and restore IRQ state
```

Unregistration checks the current record's function under the same lock,
returning `-ENOENT` on mismatch, then clears only `slot.cur`. It never
clears a record a dispatcher may still hold. A grace period remains
necessary before freeing handler state or reusing a registration record;
writer serialization does not replace the owner's lifecycle coordination.

## Dispatch

```
interrupt_dispatch(v, frame):
    v >= g_vector_count → panic_frame(...)        arch bug
    atomic_fetch_add_relaxed(&slot.count, 1)
    record = atomic_load_acquire(&slot.cur)
    record == NULL → arch_trap_unhandled(v, frame); return
    record.fn(v, frame, record.arg)
```

One acquire load selects the function and argument together. The count is
incremented before lookup, including unhandled dispatches. Architecture
unhandled-warning totals likewise use atomic increments and print the
returned value; concurrent warning lines need not appear in count order. No writer lock
is taken, including on the x86 paranoid/NMI entry path.

## Handler context

Handlers run:

- on the interrupted stack (no stack switch except `#DF` on IST1),
- with interrupts disabled (interrupt gates clear IF),
- before any EOI (the arch layer sends it after the handler returns),
- with a frame pointer that is valid only until they return.

Therefore handlers must not sleep, must not allocate (there is no
allocator, and when there is one its interrupt-safe variant will be a
distinct API), must not take a lock that can sleep, and must be short.
Anything longer is deferred (constitution section 53: minimal handler →
queue work → worker thread). For completion handlers that is the
irq_poll below.

## Bounded completion handling

A completion handler consumes what its device has finished until it
finds nothing more. That is not a bound when something refills the device
while the handler runs: a submitter on another CPU, or the handler
itself, since `bio_complete` hands the block layer's next waiting bio to
the driver and a USB callback submits the next transfer. Against a device
that completes at once, the handler then runs, interrupts off, for as
long as the refilling lasts. CI saw one CPU take no tick for 22 s inside
`vblk_done` (docs/testing/flakes.md, "held for 184 s"); `blk-irq-budget`
reproduces it at will.

`kernel/core/irqpoll.c` (`kernel/include/kernel/irqpoll.h`) bounds it,
with one design for every handler it applies to:

- **A budget per call.** The handler calls `irq_poll_sched`, which runs
  the driver's `poll(budget)`: at most `IRQ_POLL_BUDGET` (32)
  completions, returning how many it consumed.
- **The remainder on a worker.** A call that used its whole budget hands
  the rest to this CPU's `irqpoll/N` thread (pinned, highest priority),
  which calls `poll` a budget at a time and yields between batches,
  until a call returns less than the budget.
- **One consumer at a time.** A handler that finds `poll` running (on
  another CPU or in the worker) or queued only records `again`; whoever
  is consuming takes it before going idle. Completions are consumed in
  the device's order, as before, and ownership rules inside `poll` are
  the driver's own, unchanged (virtio-blk decides by pointer, under its
  lock, whether a popped bio is still its to complete; the timeout and
  removal walks clear the same slot table).

**The remainder always runs, promptly.** A deferral queues the irq_poll
on a started worker's list and wakes it; the worker is runnable from then
until the list is empty, at the highest priority, so no thread holds it
off -- as none held off the handler. At the default priority a busier
thread on its CPU (quiesce, the reaper, a pinned spinner) stalled the
queue's completions for as long as it ran (review, PR #334). **But for
one slice only.** A backlog that keeps the worker busy for longer than
`IRQ_POLL_HOLD_NS` (10 ms) continues at the default priority,
time-sliced with the CPU's other threads, until it ends; the worker
returns to the highest priority before it next sleeps. Without that
bound, a device that never ran dry kept that CPU's threads off it for as
long as it lasted. In `virtio-remove-inflight`'s held pass that
included the test thread that was to stop the submitter: a livelock, 36 s
on two CPUs and the boot's whole budget under chaos
(docs/audit/2026-10-08-irqpoll-lockdep-report.md §2a). `blk-irq-budget`
holds it with a default-priority bystander pinned to every CPU: it must
run within 250 ms throughout the storm (998 ms before the bound, 14 ms
after). Past the slice the remainder still always runs, because the
worker never leaves its run queue, as Linux's ksoftirqd does. Before
the workers start (boot, before any driver loads) a handler cannot
defer and polls to the end, as before.

**Teardown.** `irq_poll_disable` unqueues a deferral and waits for a
running `poll` to return; after it, none runs or starts until
`irq_poll_enable`. It is the deferred half of `synchronize_irq`, called
after the interrupt is released and before what `poll` reads is freed:
`virtq_free` calls it after the transport's teardown, so every virtio
driver's remove and reset path, which already freed the queue before
walking its slots, also stops the deferred work there
(`virtio-remove-inflight`'s held passes keep their meaning: the hold is
checked before every pop, in the handler and in the worker alike). A
path that only needs "whatever the handler was doing is done", like
xHCI's cancel, calls `irq_poll_synchronize`, which waits for a `poll`
running now to return. Both sleep on the irq_poll's idle queue. A finish,
or the worker dropping a disabled deferral, wakes the queue when a waiter
has registered under the irq_poll's lock. Both are lockdep callback waits
on the poll's class, and both call `might_sleep` (lockdep design.md,
"Callback classes"). Until 2026-10-08 they spun on `sched_yield`, which
lockdep could not see. The worker runs `poll` with preemption off. It
never sleeps, since it runs in interrupt context too.

Where it applies (the audit, docs/audit/2026-10-08-irq-budget-report.md):
virtio queues (block, network transmit and receive, entropy), NVMe
queues and the xHCI event ring. AHCI completes one snapshot of finished
slots per interrupt (at most 32) and e1000e is bounded by its ring under
the lock its transmit also takes; neither is changed.

## SMP publication and lifetime

The published record pointer uses release/acquire ordering. Per-vector
writer serialization protects slot ownership and record selection across
CPUs; different vectors do not share a writer lock. Dispatch counts use
atomic increments and loads.

A CPU may load a record just before unregistration clears it, then invoke
the handler afterwards. `interrupt_unregister_sync` and the IRQ release
paths call `synchronize_irq` (one `synchronize_quiesce`) before handler
state may be freed. Record storage alternates between two entries. The
existing caller requirement to coordinate unregister, grace period and
re-registration remains: the writer lock alone does not make rapid record
reuse safe against an older dispatch. See `docs/kernel/quiesce/design.md`.

## Relationship with the architecture layer

| Concern | Owner |
|---|---|
| Vector numbering, which vectors are exceptions | arch (`arch_trap_vector`, `arch_trap_is_exception`) |
| Register save/restore, stack, IRET | arch (`isr.S`) |
| EOI to the controller | arch (`x86_trap_dispatch` → `pic_eoi`) |
| Policy for unregistered vectors | arch (`arch_trap_unhandled`), because whether a vector is fatal is an architecture fact |
| Handler table, counts, names | generic (this subsystem) |

The arch layer calls exactly one function here, `interrupt_dispatch`.
This subsystem calls exactly four arch functions: `arch_trap_vector_count`,
`arch_irq_save`, `arch_irq_restore`, `arch_trap_unhandled`.

## Error handling

All failures are returned, never logged silently: `-EINVAL`, `-EBUSY`,
`-ENOENT`. The only panic is the out-of-range vector in dispatch, which
cannot be caused by a caller and indicates a corrupted frame or a
mismatch between the stub count and `arch_trap_vector_count`.

## Diagnostics

`interrupt_count(v)` uses an atomic relaxed load; it samples a counter
without promising the current handler's completion. Name lookup acquires
the published pointer and atomically samples its immortal name. It can
observe an old or reused record's name during replacement, but never a
non-atomic pointer race. This deliberately avoids a writer lock or grace
period in diagnostics, including NMI name lookup. Neither accessor pairs
its result with the other accessor's result or with a registration handle.

## Memory

Zero allocations. `sizeof(g_slots)` is 256 × 32 = 8 KiB of `.bss`.

## Security

No user-reachable surface. Handler pointers live in kernel `.bss`, which
is mapped `RW+NX`; a write primitive into it would be a full compromise
regardless, so no additional hardening (such as a read-only table after
boot) is planned until the module loader exists and needs it.
