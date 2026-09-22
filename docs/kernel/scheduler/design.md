# Scheduler and Threads: Design

## 1. Per-CPU data

```c
struct percpu {
    struct percpu   *self;           /* gs:0 on x86-64 */
    unsigned         cpu_id;
    struct thread   *current;
    struct thread   *idle;
    int              preempt_count;  /* >0 = preemption disabled */
    bool             need_resched;
    unsigned         irq_depth;      /* nesting of interrupt handlers */
    bool             online;
    struct runqueue *rq;
    uint64_t         ticks;          /* local timer ticks */
    struct arch_percpu arch;         /* LAPIC id, TSS, per-CPU GDT */
};
```

`this_cpu()` returns the pointer through the architecture (x86-64:
`mov %gs:0, %rax`). `arch_cpu_id()` reads `cpu_id` the same way. The BSP's
`struct percpu` is static and installed in `x86_start` before anything
that could call `arch_cpu_id`; APs get theirs from the heap in the SMP
PR. `preempt_disable()`/`preempt_enable()` are increment/decrement of
`preempt_count`; `preempt_enable` calls `schedule()` when the count
reaches zero with `need_resched` set and interrupts enabled.

**A per-CPU answer has a rule (S25).** The value either accessor returns
may be kept only while the thread cannot move: preemption disabled,
interrupts off, interrupt context, or an affinity of one CPU. Since
threads migrate (below), a thread that reads its CPU with preemption on
and uses the answer later may be using another CPU's. In debug builds
both accessors check the rule and panic naming the call site
(`kernel/core/percpu.c`, `claim_check`); `PERCPU_WARN=1` turns the panic
into a once-per-site warning, which is how the sweep that introduced
the rule listed its sites (113 on x86-64, 133 on AArch64 in one boot
each; `docs/audit/next-subsystem-percpu-migration.md`). Two reads are
not claims and use the raw forms `raw_this_cpu()` / `raw_cpu_id()`,
each with a comment saying which: the current thread (`thread_current`
is `raw_this_cpu()->current`, since a thread is the same on any CPU
that runs it) and a count asserted to be zero (`preempt_count`,
`irq_depth` in `might_sleep` and the sleeping primitives, zero on every
CPU a preemptible thread can be on); and a diagnostic or statistic,
where a stale answer is a wrong number and never a wrong action
(`clock_now_ns`'s per-CPU offset, whose error after a move is bounded
by the offsets' half-widths, the tick and shootdown counters, the
block layer's local/remote completion tally). A thread that must keep a
per-CPU answer across a sleep pins itself (`thread_pin_self`), which the
check honours as a one-CPU affinity; the tests that name "another CPU
than mine" do.

`spin_lock` now calls `preempt_disable()` first and `spin_unlock` calls
`preempt_enable()` last, so no spinlock holder is ever preempted.

## 2. Threads

```c
enum thread_state { THREAD_READY, THREAD_RUNNING, THREAD_BLOCKED, THREAD_EXITED };

struct thread {
    tid_t tid;
    char name[THREAD_NAME_MAX];
    enum thread_state state;
    struct arch_context ctx;      /* saved stack pointer */
    vaddr_t stack_base;           /* vm_kernel_alloc result, guards outside */
    size_t stack_size;
    void (*entry)(void *arg);
    void *arg;
    int priority;                 /* 0 (highest) .. SCHED_PRIO_COUNT-1 */
    cpumask_t affinity;
    int cpu;                      /* run queue it is on / last ran on */
    uint64_t slice_left_ns;
    uint64_t run_time_ns;
    uint64_t last_start_ns;
    struct list_node rq_link;     /* run queue */
    struct list_node all_link;    /* global thread list, for diagnostics */
    struct waitqueue *waiting_on; /* diagnostics only */
    struct completion exited;     /* signalled by exit, waited by join */
    int exit_code;
    uint32_t refcount;
    unsigned flags;               /* THREAD_IDLE, THREAD_DETACHED */
};
```

Stacks: `THREAD_STACK_SIZE` = 16 KiB, allocated with
`VM_KALLOC_GUARD | VM_KALLOC_POPULATE`. The boot stack (`entry.S`, 64 KiB
in `.bss`) belongs to thread 0 and is never freed.

Creation (`thread_create(entry, arg, name, priority)`):
1. Allocate `struct thread` from the thread cache, stack from the arena.
2. `arch_context_init(&t->ctx, stack_top, thread_trampoline)`: build a
   frame so the first `arch_context_switch` into it "returns" to
   `thread_trampoline`.
3. refcount = 2 (the thread itself, and the creator). The creator calls
   `thread_join` or `thread_put`.
4. Choose a CPU: lowest `nr_running` among online CPUs in `affinity`.
5. `sched_enqueue` on that CPU; if its current thread has lower priority,
   set that CPU's `need_resched` (and, in the SMP PR, IPI it).

`thread_trampoline()`: `sched_finish_switch()` (release the run-queue
lock inherited from `schedule`, reap the previous thread if it exited),
enable interrupts, call `entry(arg)`, then `thread_exit(0)`.

Exit: set `THREAD_EXITED`, signal `exited`, `schedule()`; never returns.
The next thread's `sched_finish_switch` sees `prev->state == EXITED` and
does `thread_put(prev)`, which frees stack and struct when the creator's
reference is also gone.

## 3. Run queues and policy

```c
#define SCHED_PRIO_COUNT 64
#define SCHED_PRIO_DEFAULT 32
#define SCHED_PRIO_IDLE (SCHED_PRIO_COUNT - 1)

struct runqueue {
    spinlock_t lock;
    uint64_t bitmap;                          /* bit p set = list p non-empty */
    struct list_node ready[SCHED_PRIO_COUNT];
    unsigned nr_running;                      /* READY threads queued */
    struct thread *current;
    struct thread *idle;
    uint64_t switches;
    unsigned cpu;
};

struct sched_policy {
    const char *name;
    void (*enqueue)(struct runqueue *rq, struct thread *t, bool at_head);
    void (*dequeue)(struct runqueue *rq, struct thread *t);
    struct thread *(*pick_next)(struct runqueue *rq);   /* NULL = idle */
    void (*tick)(struct runqueue *rq, struct thread *current, uint64_t ns);
    void (*slice_new)(struct thread *t);                /* refill on switch-in */
};
```

`policy_rr`: enqueue appends to `ready[prio]` (prepends when
`at_head`, used when a thread is preempted before consuming its slice),
sets the bitmap bit; `pick_next` takes `__builtin_ctzll(bitmap)`;
`tick` subtracts the tick period from `slice_left_ns` and requests
reschedule at zero; `slice_new` refills to `SCHED_SLICE_NS` (10 ms).

### schedule()

```text
KASSERT(preempt_count == 0 || called from preempt_enable path)
KASSERT(irq_depth == 0)            /* never from an interrupt handler */
s = spin_lock_irqsave(rq->lock)     /* raises preempt_count to 1 */
prev = rq->current
if prev->state == RUNNING:          /* yield or preemption */
    prev->state = READY
    if prev != idle: policy.enqueue(rq, prev, at_head = slice remaining)
next = policy.pick_next(rq) ?: rq->idle
if next is from a list: policy.dequeue
need_resched = false
if next != prev:
    next->state = RUNNING; rq->current = next; policy.slice_new(next)
    account prev run time
    arch_context_switch(&prev->ctx, &next->ctx)
    /* resumed here as `prev` later: rq->lock still held by us */
    sched_finish_switch()           /* reap, then unlock */
else:
    prev->state = RUNNING
    spin_unlock_irqrestore(rq->lock, s)
```

`sched_finish_switch()` reads `rq->prev_exited` (set before the switch
when `prev->state == EXITED`), releases the run-queue lock and restores
interrupts as they were for the resumed thread, then `thread_put`s the
exited thread outside the lock.

### Blocking and waking

```c
void sched_block_current(void);   /* caller already set state BLOCKED under a wq lock */
void sched_wake(struct thread *t);
```

`sched_wake`: lock the run queue of `t->cpu`; if `t->state == BLOCKED`,
set READY and enqueue at tail; if `t` has higher priority than that
CPU's current, set its `need_resched`; unlock. Waking an already READY or
RUNNING thread is a no-op (this is what makes the wait protocol safe).

### Preemption points

- Interrupt return (`x86_trap_dispatch` tail): if `irq_depth` is back
  to 0, `preempt_count == 0`, and the interrupted frame had IF set, the
  CPU first records a quiescent state (`quiesce_note_quiescent`,
  `docs/kernel/quiesce/`) and then, if `need_resched` is set, calls
  `sched_preempt()`. The switch happens inside the handler on the
  interrupted thread's stack; when the thread is switched back in it
  completes the `iretq`.
- `preempt_enable()` reaching zero with `need_resched` **and interrupts
  enabled**. Since the lifetime pass `quiesce_read_unlock()` is such a
  point too, so code that wakes a higher-priority thread and then leaves
  a read-side section is preempted there rather than at the next tick.
- **Interrupts being restored to enabled** with the preempt count already
  zero: `arch_irq_restore` calls `preempt_point()` after the enable, on
  both architectures. This is the point every wake needs, because every
  wake runs under an `irqsave` spinlock whose `spin_unlock_irqrestore`
  reaches `preempt_enable` with interrupts still off -- before the
  wake-preempt unit (`docs/audit/next-subsystem-wake-preempt.md`) a
  same-CPU wake of a higher-priority thread therefore ran at the next
  tick, up to 4 ms later. The predicate is the same four conditions as
  `preempt_enable`'s, tested at the other moment the last of them can
  become true. The two internal restores it passes through are harmless
  by the predicate alone: the lockdep bracket in `spin_unlock` restores
  with the count still non-zero, and the tail of `schedule()` restores
  with the count at zero, where a reschedule set during the switch means
  one more trip through `schedule()`, which a tick landing there would
  also cause. `preempt_point_count(cpu)` counts the switches taken here
  (the scheduler dump's `restore-preempts`).
- Explicit `sched_yield()` / blocking calls.

`schedule_internal`, the idle loop before `hlt`/`wfi`, and
`sched_start_cpu` also record a quiescent state: at each the CPU holds no
spinlock and is in no read-side section.

### Idle

`idle_thread_main`: loop `{ if need_resched: schedule(); else
arch_cpu_wait_for_interrupt(); }`. The idle thread never sits on a run
queue; `pick_next` returning NULL selects it.

## 3a. Migration

A ready thread can be moved from one CPU's run queue to another's
(`sched_migrate`, `sched_migrate_from`; `docs/audit/next-subsystem-percpu-
migration.md`). This is the mechanism only: nothing in the kernel moves
threads on its own, and the automatic balancer is the following unit.

**What moves.** Only a `THREAD_READY` thread that is not its queue's
`current` and was not preempted (S26). A thread switched out by
preemption -- an interrupt return or `preempt_enable` -- stopped at a
point it did not choose and may be between the two instructions of a
per-CPU access (the pointer to its CPU's block, then the field;
`thread_current` and `preempt_disable` are that shape), so it must resume
on the CPU it left: `schedule_internal` flags it `THREAD_FLAG_PREEMPTED`
until it runs again, and no migrator moves it. A thread that yielded,
blocked and was woken, or never ran has nothing in flight and may move. A running thread executes on its CPU's stack; a blocked
one is on no queue and wakes on its own `t->cpu` through `sched_wake`;
and a thread woken between blocking and stopping is both `rq->current`
and a queue entry until it runs `sched_set_running_current`, so "not in
a queue" is not the same as "not running" and the primitive refuses
`rq->current` by identity. The move is a policy `dequeue` from the source,
`t->cpu` rewritten, a policy `enqueue` at the destination's tail, and a
reschedule request there if the thread outranks its current; the
migration count rises.

**Two locks, one order.** Both run-queue locks are held for the whole
move, taken in increasing CPU-id order (S24) and released before the
call returns. Each run queue's lock is its own lockdep class
(`runqueue0`, `runqueue1`, ... from a static name table in `sched.c`),
so a reversed pair is a cycle lockdep reports; `lockdep-rq-order`
provokes one. Neither entry may be called with a run-queue lock held --
`sched_migrate_from` selects *under* both locks through the policy's
`pick_migratable`, so a caller that only knows the queue (the chaos
migrator, the balancer to come) has no selection to hand over and nothing
to revalidate.

**The result says why not.** `enum sched_migrate_result` names the check
that refused: same CPU, not ready, current (the window above), affinity,
offline. A caller asks which, never whether.

**`preempt_disable` is the migration barrier.** `schedule()` panics when
entered with `preempt_count != 0`, so a thread that has disabled
preemption can never become READY and can never be moved -- which is why
S25 needs no second counter: a per-CPU answer kept under `preempt_disable`
is kept on the CPU that gave it. The barrier is itself two instructions
(load the block's pointer, increment the count), which is why a thread
preempted between them is never moved (above): the chaos boot found one
that was, reading another CPU's `irq_depth` after the move. `sched_wake`'s unlocked read of `t->cpu`
is unchanged, because the thread it can wake is BLOCKED and a blocked
thread is never moved.

**Where the switch path reads its own block.** `schedule_internal` and
`sched_set_running_current` disable interrupts *before* reading
`this_cpu()`: they arrive preemptible, and a tick between the read and
the run-queue lock could move the caller, which would then switch on the
state of the CPU it left. The quiesce publish in the switch path is
inside that window too, and `quiesce_note_quiescent` disables interrupts
itself for its other callers (Q20).

**The chaos migrator** (`SCHED_CHAOS=1`, debug builds, `make test-chaos`)
is the adversary the mechanism is tested under: every fourth tick, after
`sched_tick` has released its own lock, each CPU calls
`sched_migrate_from(self, next)` for the next online CPU in a rotation,
moving one thread its queue can spare for no reason but to move it. The
whole self-test suite and the user-mode sections run with threads moving
underneath them; the run prints its tally after the self-tests
(`sched: chaos migrated N threads from the tick ...`) and the boot test
requires `N > 0`. CI runs it on both architectures.

## 3b. Balancing

A ready thread moves because a CPU decided to take it
(`docs/audit/next-subsystem-load-balancer.md`). The mechanism is §3a's;
this is the policy on top of it, and it is deliberately small.

**Load is what a CPU is carrying** (S29): `sched_cpu_load(c)` is that
queue's `nr_running` plus the thread it is running, unless that thread
is its idle thread. `nr_running` alone counts the ready list and
`schedule` dequeues what it runs, so a CPU saturated by one thread
reports the same zero as a CPU asleep -- which is the number `pick_cpu`
used to read. `pick_cpu` reads the load now, so placement can tell a
busy CPU from an idle one.

The load is read without the target queue's lock. It is a hint: it
compares `rq->current` with `rq->idle` by identity and never
dereferences it, and every decision taken from it is re-made under both
locks inside `sched_migrate_from`, which also chooses the thread. A
stale reading costs a scan.

**A pull, from two moments** (S27). The CPU that decides is the CPU that
receives. An idle CPU with an empty queue looks on every tick -- it has
nothing else to do, and this is the moment the imbalance this unit was
built for appears. A CPU that is running something looks every
`SCHED_BALANCE_TICKS` (16, so 64 ms at `CONFIG_HZ` 250), so two busy
CPUs still even out when no CPU is free. Both run from `sched_tick`
after it releases its own run-queue lock, the context the chaos migrator
already runs in.

**A difference of two, one thread at a time** (S28). One is the steady
state of an odd thread count; chasing it thrashes. Two is also the
smallest difference that means a thread is waiting: a CPU running one
thread with an empty queue is at 1, so its thread is never dragged to an
idle CPU to arrive cold. A pull shrinks the difference by two, so one
per look settles.

**What it cannot do.** It cannot move a thread that is time-slicing.
Two compute-bound threads on one CPU alternate by preemption, so the one
in the queue always carries `THREAD_FLAG_PREEMPTED` and S26 forbids
moving it -- it may have stopped between the two instructions of a
per-CPU access. So the balancer corrects an imbalance as work *becomes*
runnable, and not one that has already settled into alternation. The
benchmark works because its threads are woken: the idle CPUs take them
before they have ever been preempted.

`SCHED_BALANCE=0` compiles it out, which is how the tests prove it; the
boot prints what it did (`sched: balance pulled N threads in M scans`),
and `sched_dump`'s per-CPU line carries the load.

## 4. Wait queues

```c
struct wait_entry { struct list_node link; struct thread *thread; };
struct waitqueue { spinlock_t lock; struct list_node waiters; };

#define wait_event(wq, cond)                          \
    do {                                              \
        struct wait_entry __e;                        \
        for (;;) {                                    \
            waitqueue_prepare(wq, &__e);  /* enqueue + state = BLOCKED */ \
            if (cond) break;                          \
            sched_block_current();        /* schedule(); returns when woken */ \
        }                                             \
        waitqueue_finish(wq, &__e);       /* dequeue, state = RUNNING */ \
    } while (0)
```

Lost-wakeup freedom: the waiter is on the list and marked BLOCKED before
it evaluates `cond`; a waker that runs between the evaluation and
`schedule()` sets the state back to READY, and `schedule()` treats a
READY current thread as a yield, returning immediately. Wakers call
`waitqueue_wake_one/all`, which walk the list under `wq->lock` and call
`sched_wake` on each entry.

`thread_sleep_ns(ns)`: a stack `struct timer` whose callback wakes the
sleeping thread; `wait_event(&t->sleep_wq, timer fired)`.

## 5. Mutex, semaphore, completion

- `mutex`: `{ spinlock_t lock; struct thread *owner; struct waitqueue wq; }`.
  `mutex_lock`: loop { lock; if owner NULL: take, unlock, return; unlock;
  wait_event(wq, owner == NULL) }. `mutex_unlock` asserts ownership,
  clears owner, wakes one. Not usable in interrupt context (asserted).
- `semaphore`: `{ spinlock_t lock; int count; struct waitqueue wq; }`;
  `down` waits for `count > 0`, `up` increments and wakes one. `up` is
  interrupt-safe.
- `completion`: `{ spinlock_t lock; bool done; struct waitqueue wq; }`;
  `complete` sets done and wakes all *under one hold of the lock*, and
  `wait_for_completion`, having seen done, takes the lock once before
  returning — so a waiter cannot return, and free the completion (it
  usually lives on the waiter's stack), while `complete` is still inside
  it. The first version dropped the lock between setting done and
  waking; a waiter that arrived or polled in that window returned and
  its frame was reused under the wake (found by the AHCI unit's
  concurrent block benchmark). `completion-race` reproduces the shape —
  20 000 completions across two CPUs with the waiter's frame zeroed the
  moment it returns — and against the old code it panicked on aarch64
  with the completer's own lock found zeroed at its unlock, exactly the
  sequence above; on x86_64 the same rounds did not hit the window, which
  is a few instructions wide and needs the completing CPU stalled inside
  it. So the test is a reproducer on one architecture and a stress on the
  other; the fix follows from the sequence. One-shot.
  The wait queue's lock has its own class, `completion-wq`, because it is
  taken inside the completion's own lock.

## 6. Timer subsystem (summary; full text in docs/kernel/timer/)

Per-CPU sorted list of `struct timer { link, expires_ns, fn, arg, cpu,
state }`. The tick handler runs on every CPU at `CONFIG_HZ` (250):
`this_cpu()->ticks++`, expire timers whose `expires_ns <= now`, call
`policy.tick`. Callbacks run in interrupt context and may only call
interrupt-safe functions (`sched_wake`, `waitqueue_wake_*`,
`semaphore_up`, `complete`).

## 7. Failure modes

| Condition | Behaviour |
|---|---|
| out of memory in thread_create | NULL |
| schedule() from interrupt context | panic |
| schedule() with a spinlock held (preempt_count > 0 outside the rq lock) | panic |
| blocking primitive from interrupt context | panic |
| mutex_unlock by non-owner | panic |
| thread_join twice / join on detached | panic |
| stack overflow | #PF on guard page → #DF on IST stack → panic report |
| wake of a never-blocked thread | no-op |
