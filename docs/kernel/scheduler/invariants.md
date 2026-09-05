# Scheduler and Threads: Invariants

Each invariant names how it is checked: **assert** (`KASSERT`/`panic`
in the code), **test** (a boot self-test, `docs/kernel/scheduler/testing.md`),
or **review** (no mechanical check yet).

## State and locking

**S1. Every thread state transition happens under the run-queue lock of
`t->cpu`.** `schedule()`, `sched_wake()`, `sched_enqueue_new()`,
`sched_set_running_current()`, and `sched_tick()` all take
`runqueue.lock` (irqsave) before touching `state`, the ready lists, the
bitmap, or `nr_running`. The one write outside it is
`waitqueue_prepare` setting `THREAD_BLOCKED` on the *current* thread
under `waitqueue.lock`; the only other writer of that field for a
running thread is `sched_wake`, which needs the wait-queue lock to find
the entry. Check: review; `policy_rr` asserts `list_empty` on enqueue
and non-empty on dequeue so a double transition corrupts nothing
silently.

**S2. `runqueue.lock` is a leaf.** Nothing is acquired while it is held.
`schedule()` calls `arch_context_switch` with it held and the resumed
thread releases it; `sched_finish_switch` unlocks *before* calling
`thread_put`, which takes `kernel_space.lock` and slab locks. The AArch64
IPI path used to take the GIC lock under it (`request_resched` →
`arch_ipi_send` → the SGI table); `arch_ipi_bind` at `ipi_init` now makes
that lookup lock-free. Check: the lock-order checker records no edge out
of the `runqueue` class on either architecture (`docs/kernel/lockdep/
testing.md`); the spinlock owner check would panic on a self-deadlock.

**S3. The run-queue lock is held across the context switch and
released by whoever runs next.** A resumed thread releases it in
`sched_finish_switch()` after `arch_context_switch` returns; a new thread
releases it in `thread_trampoline()` before enabling interrupts and
calling its entry. `rq->prev_exited` is the only state handed across the
switch. Check: assert (`spin_unlock` asserts the lock is held); test
`thread` (a first-run thread reaches its entry with interrupts enabled).

**S4. `runqueue.lock` may be preceded by any lock, and a primitive's own
lock precedes its wait queue's lock.** Wakers hold their own state lock
when they call `sched_wake`: every wait queue's lock, `process.lock`
(`process_kill`), the futex bucket, `tty.lock`, the SMP call and sleep
locks all precede `runqueue.lock` in the recorded graph; since S2 makes
the run-queue lock a leaf no order among them is implied by it. (The
previous text, "`waitqueue.lock` → `runqueue.lock`" alone, was
incomplete.) Check: the recorded graph, `docs/kernel/lockdep/testing.md`.

**S5. Interrupts are re-enabled by the resumed thread, not by the
switch.** `arch_context_switch` does not save RFLAGS; `schedule()`
restores the caller's saved state with `arch_irq_restore(s)` after
`sched_finish_switch`, and `thread_trampoline` does `arch_irq_enable()`.
Check: test `breakpoint-trap` and every blocking test (a thread that
returned with interrupts off would never take the next tick).

## Entry conditions

**S6. `schedule()` is never called from interrupt context or with
preemption disabled by the caller, and no sleeping primitive is entered
under a spinlock.** `schedule()` panics if `irq_depth != 0` or
`preempt_count != 0`; `sched_block_current` and `waitqueue_prepare`
repeat the check; `mutex_lock`, `semaphore_down` and
`wait_for_completion` check `preempt_count` on entry, on every call and
not only when they would block, so a sleeping lock taken under a
spinlock fails in the first test that runs the path rather than under
load (the Prompt #3 fix pass found `tcp.c` taking the netif registry
mutex under the TCP spinlock: uncontended it passed, contended it would
have panicked in `waitqueue_prepare`). Check: assert.

**S7. Holding a spinlock disables preemption.** `spin_lock` and
`spin_trylock` (on success) call `preempt_disable`; `spin_unlock` calls
`preempt_enable`. Check: assert (`preempt_enable` asserts the count is
positive); review.

**S8. Preemption happens only at three points**: the interrupt-return
tail in `x86_trap_dispatch` when `irq_depth == 0`, `need_resched`,
`preempt_count == 0`, and the interrupted frame had `RFLAGS.IF` set;
`preempt_enable` reaching zero under the same conditions with interrupts
enabled; or an explicit `schedule`/`sched_yield`/block. Check: review;
test `preempt` (a spinning thread is displaced by a woken sleeper).

## Wait queues

**S9. A `wait_entry` is linked into a wait queue at most once.**
`wait_event` initialises the entry once (`wait_entry_init`) and
`waitqueue_prepare` pushes only if `list_empty(&e->link)`. Found during
bring-up: a woken waiter whose condition was still false re-prepared and
pushed the same node twice, corrupting the list under mutex contention
(garbage `e->thread` in `sched_wake`). Check: test `mutex` (four threads,
400 contended acquisitions); review.

**S10. Waking a thread that is not BLOCKED is a no-op.** `sched_wake`
changes state only from `THREAD_BLOCKED`. This is what makes the prepare
→ evaluate → block protocol lost-wakeup free. Check: test `waitqueue`
(`waitqueue_wake_all` with a false condition leaves both waiters
blocked; a later wake with a true condition releases them); review.

**S11. A thread that was woken early is dequeued before it continues.**
`waitqueue_finish` → `sched_set_running_current` removes a READY current
thread from the run queue so it is never picked while running or,
later, while blocked. Check: review; test `waitqueue`.

## Lifetime

**S12. A thread never frees its own stack.** `thread_put` at zero asserts
`t != thread_current()` and `THREAD_EXITED`; the exiting thread's own
reference is dropped by the CPU that switched away from it
(`sched_finish_switch` on `rq->prev_exited`). Check: assert.

**S13. `thread_create` returns two references**; the creator releases
exactly one via `thread_join` or `thread_put`. A missed release leaks
the struct and stack (visible as `thread_count` not returning to
baseline); an extra release underflows (`KASSERT(old > 0)`). Check:
assert; every self-test compares `thread_count()` before and after.

**S14. The boot stack belongs to thread 0 and is never freed.** Thread 0
carries `THREAD_FLAG_BOOT`, refcount 1, `stack_base` from
`arch_boot_stack`; `thread_put` skips `vm_kernel_free` for it and
`thread_exit` panics for it. Check: assert.

**S15. The idle thread is never on a run queue.** It is created with
`thread_prepare` (not enqueued), `schedule()` skips enqueueing it, and
`pick_next` returning NULL selects it. Check: assert (`rr_enqueue`
asserts `list_empty`); review.

## Per-CPU data

**S16. The per-CPU pointer is installed after the GDT is loaded and the
GS selector is never reloaded afterwards.** `mov %ax, %gs` in `gdt_init`
replaces the GS base with the descriptor base (0). Found during bring-up:
with `percpu_init_boot` before `gdt_init`, every `this_cpu()` access
went to address 0, which the loader's identity map made silently
writable until the VMM's tables made it fault with no working panic
path (triple fault). `x86_start` now installs per-CPU data after
`gdt_init`, and nothing before that line may take a lock. Check: review;
any boot self-test would fail otherwise.

**S17. `struct percpu.self` is at offset 0** (`arch_percpu_get` reads
`%gs:0`). Check: review; a static assert is planned.

## Primitives

**S18. `mutex_unlock` by a non-owner panics; recursive `mutex_lock` by
the owner panics.** Check: assert.

**S19. Blocking primitives are never used in interrupt context.**
`mutex_lock`, `semaphore_down`, `wait_for_completion`,
`waitqueue_prepare` check `irq_depth`. `semaphore_up`, `complete`,
`waitqueue_wake_*`, and `sched_wake` are the interrupt-safe halves.
Check: assert.

**S20. A woken sleeper's timer has already fired.** `thread_sleep_ns`
waits on `done`, set by the callback before the wake, so the stack
`struct timer` is `TIMER_IDLE` when the function returns and may be
discarded. Check: review; test `sleep`.

**S21. `waitqueue_wake_one` wakes a waiter that actually needed waking.**
A woken waiter stays linked until it runs `waitqueue_finish`, so the
head of the list may already be READY. `wake()` uses `sched_wake`'s
return value to skip such entries and only stops after a real
BLOCKED→READY transition; otherwise two back-to-back `semaphore_up`
calls would both land on the same waiter and leave a second blocked
consumer waiting forever (found in review, PR #3). Check: test
`semaphore`, second half (two blocked consumers, ten posts with no
sleep between them, both consumers finish).

**S22. Preemption keeps the current thread runnable regardless of a
transient BLOCKED state.** `waitqueue_prepare` marks the caller BLOCKED
before the condition is evaluated. `schedule_internal(preempt = true)`
(interrupt return, `preempt_enable`) therefore re-queues the current
thread whatever its state, and only a voluntary `schedule()` with state
BLOCKED leaves it to its wait queue. Without this, a tick landing between
`waitqueue_prepare` and `waitqueue_finish` switched the thread out on no
run queue and no wait list; the hang watchdog showed thread 0 BLOCKED
with `waiting_on -` and every CPU idle, and about one in three four-CPU
boots stalled (found in bring-up of the SMP PR). A re-queued thread
resumes with state RUNNING, so its `sched_block_current` behaves as a
yield and the `wait_event` loop re-prepares and re-checks the condition.
Check: 24 consecutive stall-free four-CPU boots; `KASSERT(!preempt)`
for an EXITED thread (exit disables interrupts first).

**S23. Exited threads are reaped by the reaper thread.**
`sched_finish_switch` may run with interrupts disabled, and freeing a
stack requires a TLB shootdown that waits for other CPUs with interrupts
enabled, so it calls `thread_reap_later`; the reaper (`reaper_main`,
priority `SCHED_PRIO_DEFAULT - 8`) performs the final `thread_put`. The
`rq_link` is reused for the reap list. `thread_count()` therefore
settles after `thread_join`. Check: review; tests use `threads_settle`.

## Gaps (documented, not invariants)

- Cross-CPU `need_resched` is signalled by `IPI_RESCHEDULE` when the
  target is idle or running lower priority; equal-priority wakes wait
  for the target's slice to end (at most `SCHED_SLICE_NS`).
- Threads are placed at creation and never migrate.
- No priority inheritance: a high-priority thread blocked on a mutex
  held by a low-priority thread waits for that thread's turn.
