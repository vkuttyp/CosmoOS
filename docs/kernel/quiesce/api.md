# Kernel object lifetime and quiescence: API

All interfaces are kernel-internal unless marked *(exported)*, in which
case they are part of the module ABI (`docs/kernel/module/api.md`). Each
entry follows constitution section 52.

## Grace periods (`kernel/include/kernel/quiesce.h`, `kernel/core/quiesce.c`)

### `void quiesce_init(void)`
- Purpose: create the `quiesce` callback worker thread. Called by
  `kernel_main` right after `sched_init`. Before it runs,
  `synchronize_quiesce` returns after publishing its own CPU (no other
  CPU can hold a reference yet).

### `void quiesce_read_lock(void)` / `void quiesce_read_unlock(void)` *(debug halves exported)*
- Purpose: delimit a read-side critical section for lock-free readers.
  `quiesce_read_lock` is `preempt_disable()` plus, in debug builds, a
  per-CPU depth counter; the unlock reverses both.
- Rules: nestable; must not block (every sleeping primitive already
  panics with `preempt_count != 0`); every spinlock and every interrupt
  handler is implicitly such a section, so a reader that already holds a
  spinlock needs no explicit call.
- Concurrency: any context. `quiesce_read_unlock` is a preemption point
  (it is `preempt_enable`): a woken higher-priority thread runs at once.

### `void quiesce_note_quiescent(void)`
- Purpose: the calling CPU publishes the current epoch. Called only by
  the scheduler (`schedule_internal`), the idle loop, the arch
  interrupt-return tails (under `irq_depth == 0 && preempt_count == 0`
  and interrupts enabled in the interrupted frame), `sched_start_cpu`
  and `synchronize_quiesce`. Not for users.
- Memory ordering: acquire load of the epoch, release store of the
  per-CPU record (Q1/Q2 in `design.md`).

### `void synchronize_quiesce(void)` *(exported)*
- Purpose: wait for one grace period over the CPUs online at the call:
  each has passed a quiescent state after the call began. On return no
  CPU can still hold a reference obtained inside a read-side section
  before the caller's unlink.
- Preconditions: thread context, `preempt_count == 0` (both asserted:
  panic otherwise). Sleeps in `TICK_NS / 2` steps; after two ticks it
  sends `IPI_RESCHEDULE` to the CPUs still pending (at most eight
  rounds); warns after 1 s; in debug builds panics after 10 s.
- Cost: a few microseconds with one CPU (the caller is quiescent by
  construction); otherwise until every other CPU's next tick return or
  scheduler entry, typically 4–6 ms on QEMU (`testing.md`).
- Ordering: `seq_cst` RMW on the epoch (W1), acquire loads of the
  per-CPU records (W2); see `design.md`.

### `void call_quiesce(struct quiesce_head *h, void (*fn)(struct quiesce_head *h))` *(exported)*
- Purpose: run `fn(h)` in thread context after a grace period that
  begins after the call; the deferred form of the above.
- Inputs: `h` embedded in the object being freed (zero-initialised or
  previously run); the callback recovers the object with `container_of`.
  `h` belongs to the subsystem from the call until `fn` runs; submitting
  it again before then panics (`pending` flag), because the object it is
  embedded in is about to be freed by the first callback.
- Concurrency: any context (a spinlock, irqsave). Callbacks are batched:
  the worker takes the whole list, waits one grace period, runs them in
  submission order.

### `struct quiesce_stats`, `void quiesce_get_stats(struct quiesce_stats *out)`
Epoch, completed `synchronize_quiesce` calls, callbacks run, the longest
grace period observed, straggler IPIs sent, `synchronize_irq` calls and
`timer_cancel_sync` calls that had to wait. For tests and diagnostics.

### `uint32_t quiesce_cpu_depth(unsigned cpu)`, `uint64_t quiesce_cpu_transitions(unsigned cpu)`
Debug: the CPU's open read-section depth (debug builds; 0 otherwise) and
quiescent points passed.

## Interrupt handlers (`kernel/include/kernel/interrupt.h`)

### `void synchronize_irq(unsigned vector)` *(exported)*
Wait until no CPU is inside the handler that was registered on `vector`
before the preceding `interrupt_unregister`. Thread context, no spinlock.
Implemented as one grace period (a handler is a read-side section).

### `int interrupt_unregister_sync(unsigned vector, interrupt_handler_fn fn)` *(exported)*, `int interrupt_unregister_vector_sync(unsigned vector)`
Unregister, then `synchronize_irq`. On return the handler's `arg` and
code may be freed. Same results as the plain variants. `irq_release` and
`irq_release_msi` use this ordering internally (unpublish under the IRQ
lock, wait with the lock dropped, then free the vector), so a driver that
releases its interrupts before freeing its state needs nothing more.

## Timers (`kernel/include/kernel/timer.h`)

### `bool timer_cancel_sync(struct timer *t)` *(exported)*
Cancel and wait until the callback is not running anywhere: on return
the timer may be freed. Spins (no sleep) while the callback runs on
another CPU, re-cancelling if the callback re-armed. Any context except
the timer's own callback (panic). On the timer's own CPU the wait is
free (callbacks run in interrupt context, which cannot interleave with a
lock holder that has interrupts masked). Returns what `timer_cancel`
would have.

## Objects (`kernel/include/kernel/object.h`)

See `docs/kernel/object/api.md`: `kobject_tryget`, `kobject_track_code`,
the mandatory `release`, and the owner-module record that the module
unload protocol consumes.

## Modules (`kernel/include/kernel/module.h`)

### `struct module *module_owner_of(uintptr_t addr)`
The live module whose text, rodata or data contains `addr`, with its
`live_objects` count raised inside the lookup's read-side section, or
NULL for a kernel address. Balanced by `module_object_released`. Any
context.

### `void module_object_released(struct module *m)`
Drop one live object (panics on underflow).

### `void module_set_unload_timeout_ms(unsigned ms)`
How long `module_unload` waits for live objects before it leaves the
module as a zombie (default 5000 ms; the self-test uses 50). A zombie
keeps its dependencies pinned until it is freed: its outstanding release
code may call into them.

## Registration failures

`device_register`, `blk_register` and `netif_register` record the owner
module (and, for block devices and interfaces, initialise the kobject)
only after the registry has accepted the object. A refused registration
(`-EEXIST`, `-ENOSPC`) therefore leaves nothing to balance: the caller's
failure path frees its storage directly, as every virtio driver's `fail`
label does, and no live-object count is left raised.

## Test hooks

None architecture-specific: the `irq-sync` test raises a real interrupt
on another CPU with `arch_vector_alloc` + `interrupt_register` +
`arch_ipi_send` (`kernel/include/arch/irqc.h`), which both architectures
already provide.
