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

**S24. Two run-queue locks are taken in increasing CPU-id order,
always.** Each run queue's lock is its own lockdep class (`runqueue0`,
`runqueue1`, ..., a static name table in `sched.c`), so a reversed pair is
a cycle the checker reports; the only holders of two are `sched_migrate`
and `sched_migrate_from` (`rq_lock_pair`). The first attempt at migration
(`docs/audit/next-subsystem-thread-migration.md`) found that with every
queue initialised from the one literal `"runqueue"`, lockdep saw the
second acquisition as recursion and, once annotated, could not check the
order at all; a class per instance is what makes this an invariant it
enforces. Check: `lockdep-rq-order` takes 1 then 0 against a recorded
0 -> 1 and expects the inversion report.

**S25. A per-CPU answer is kept only while the thread cannot move:
preemption disabled, interrupts off, interrupt context, or an affinity of
one CPU. `preempt_disable()` is the migration barrier.** A thread with
preemption disabled is never `THREAD_READY` (S1's transitions run through
`schedule()`, which panics with `preempt_count != 0`), and only READY
threads move (S26), so an answer read under `preempt_disable` is the CPU
it is used on. `this_cpu()` and `arch_cpu_id()` check the rule in debug
builds and panic naming the site; `raw_this_cpu()` and `raw_cpu_id()`
are for the two reads the rule does not govern -- the current thread and
an asserted-zero count (the same on any CPU the thread runs on), and a
diagnostic or statistic (a stale answer is a wrong number, never a wrong
action) -- and every raw use says which in a comment. The sweep that
introduced the rule found seventeen sites on x86-64 and the EL2 hand-back
on AArch64 that had kept a per-CPU answer across a point where a
migration could move the thread, `schedule_internal` itself among them
(`docs/audit/next-subsystem-percpu-migration.md`). Check: `percpu-claim`
(an unpinned preemptible read is reported; the four quiet forms are not);
every debug boot, since a new site panics.

**S26. Only a READY thread that is not its queue's `current` and was
not preempted is migrated, and a migration holds both run-queue locks.**
A RUNNING thread is on its CPU's stack; a BLOCKED thread is on no queue
and `sched_wake` re-enqueues it on its own `t->cpu`; a thread woken
between blocking and stopping (S22's window) is READY, queued, and still
`rq->current` until `sched_set_running_current`, and is refused by
identity. **A thread switched out by preemption stopped at a point it
did not choose**: it may hold the pointer to its CPU's block and be
about to read or write a field of it -- every per-CPU access is two
instructions, `preempt_disable` itself included, so the barrier S25
rests on is atomic only if such a thread resumes where it stopped.
`schedule_internal` marks it `THREAD_FLAG_PREEMPTED` and clears the mark
when it is switched in; `sched_migrate` refuses it (`preempted`) and
`pick_migratable` never offers it. A thread that yielded, blocked and
was woken, or never ran stopped at a call of its own with nothing in
flight, and may move. Found by the chaos boot: a worker migrated between
the two loads read another CPU's `irq_depth` and panicked as "in
interrupt context". Under both locks
the thread is dequeued, `t->cpu` rewritten, and enqueued at the
destination, so at every instant no run-queue lock is held a thread is on
exactly one queue and `t->cpu` names it. `sched_wake`'s unlocked read of
`t->cpu` stays correct because the thread it can wake is BLOCKED. Check:
`sched-migrate` (the moved worker's first run is on the destination),
`sched-migrate-refuses` (each refusal by name, the window built with
`waitqueue_prepare` and a wake under preemption off), `sched-migrate-stress`
(3,500 or so moves in 200 ms with every worker inside its mask), and the
whole suite under `make test-chaos`.

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

**S8. Preemption happens only at four points**: the interrupt-return
tail in `x86_trap_dispatch` when `irq_depth == 0`, `need_resched`,
`preempt_count == 0`, and the interrupted frame had `RFLAGS.IF` set;
`preempt_enable` reaching zero under the same conditions with interrupts
enabled; `arch_irq_restore` enabling interrupts under the same
conditions (`preempt_point`, the wake-preempt unit -- the point a wake
made under an `irqsave` lock reaches, since its unlock re-enables
preemption before interrupts); or an explicit `schedule`/`sched_yield`/
block. Check: review; test `preempt` (a spinning thread is displaced by
a woken sleeper, from interrupt context); tests `preempt-wake`,
`preempt-wake-direct` and `preempt-wake-locked` (a same-CPU wake of a
higher-priority thread runs before the waker's next statement, through
a wait queue, through a direct `sched_wake`, and from inside a bare
interrupts-off region); `init --selftest`'s `debug.preempt_probe` step
(a wake made inside a system call runs before the call returns).

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

**S27. Balancing is a pull: a CPU moves work to its own queue and never
away from it.** The CPU that decides is the CPU that receives, so the
scanning is paid for by the CPU with time to spare and the enqueue lands
on the queue that is about to consume it. An overloaded CPU never hands
work out. Check: review; `sched-balance-pull` (idle CPUs take the work
creation order left elsewhere); the `balance_tick` call site takes only
`sched_migrate_from(busiest, self)`.

**S28. A pull needs the busiest CPU to be at least two ahead.** A
difference of one is the steady state of an odd thread count, and moving
on it costs a cold cache to produce the mirror image of the same
imbalance. Two is also the smallest difference that means a thread is
*waiting* rather than running: a CPU with one thread and an empty queue
is at load 1, so its thread is never dragged to an idle CPU to arrive
cold and do the work it was already doing. One thread moves per look,
which shrinks the difference by two. Check: `sched-balance-hysteresis`
(two threads on one CPU against one on another, both movable, nothing
moves through hundreds of scans; sixteen moves when the threshold is
lowered to one).

**S29. Load counts the thread a CPU is running.** `sched_cpu_load(c)` is
`nr_running` plus the running thread unless it is that CPU's idle
thread. `nr_running` alone counts the ready list, and `schedule`
dequeues what it runs, so a CPU saturated by one compute-bound thread
and a CPU asleep in idle both report zero -- which is what `pick_cpu`
was reading. The load is read without the target's run-queue lock and is
a **hint**: it compares `rq->current` against `rq->idle` by identity and
never dereferences it, because that thread belongs to another CPU. Every
decision taken from it is re-made under both locks by
`sched_migrate_from`. Check: `sched-load` (a CPU running one thread
reports 1 while an idle one reports 0, and placement prefers the idle
one; both halves fail when the load is `nr_running` again).

**S30. A completion's waiter never returns while `complete` still holds
the completion.** `complete` publishes `done`, wakes, and releases the
completion's lock, all under that lock; a waiter that only reads `done`
(`completion_done`) can return -- and free `c`, which usually lives on
its stack -- while `complete` is still inside it, on another CPU. So
every wait that a caller frees after must do the handshake: take the
completion's lock once after `done` is seen, which cannot complete until
`complete` has let go. `wait_for_completion` and
`wait_for_completion_timeout` both do; `completion_done` does not, and a
poll of it followed by a bare return is the bug this rule forbids (the
NVMe admin path, `docs/audit/next-subsystem-nvme-admin.md`). Check:
`completion-timeout` (200 completions lingered from another CPU, the
completion's lock free on every return; it fails when the timeout wait
drops its handshake) and, for the driver, `tools/nvme-admin-probe.py`.

## Gaps (documented, not invariants)

- Cross-CPU `need_resched` is signalled by `IPI_RESCHEDULE` when the
  target is idle or running lower priority; equal-priority wakes wait
  for the target's slice to end (at most `SCHED_SLICE_NS`).
- **A thread time-slicing with another on the same CPU is never
  moved.** Two compute-bound threads sharing a CPU alternate by
  preemption, so whichever of them is in the queue always carries
  `THREAD_FLAG_PREEMPTED` and S26 forbids moving it. The balancer
  therefore corrects an imbalance *as work becomes runnable* -- a wake,
  a yield, a new thread -- and cannot correct one that has already
  settled into two threads alternating on one CPU while another is idle.
  Found by `sched-balance-hysteresis`, which passed under a deliberately
  broken threshold because the thread it was trying to have moved could
  never be moved at all. Measured by the balance-movable unit
  (`tools/balance-chaos-probe.py`): such a pair, widened to every CPU,
  is never separated while an idle CPU's pulls are refused some five
  hundred times a second (`NOT_READY`: a queue whose only spare thread is
  preempted offers none), and a yielding pair is separated in
  milliseconds. Checked by `sched-balance-pair`, which asserts both; with
  S26 removed entirely -- the policy's check and `migrate_locked`'s
  assertion -- its spinning half fails, and it alone. Lifting it means letting a preempted thread
  move, which is exactly what S25's barrier forbids, so it is a
  different unit and not a tuning change.
- No priority inheritance: a high-priority thread blocked on a mutex
  held by a low-priority thread waits for that thread's turn.
