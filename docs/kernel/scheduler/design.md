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
  to 0, `need_resched` is set, `preempt_count == 0`, and the interrupted
  frame had IF set, call `schedule()`. The switch happens inside the
  handler on the interrupted thread's stack; when the thread is switched
  back in it completes the `iretq`.
- `preempt_enable()` reaching zero with `need_resched`.
- Explicit `sched_yield()` / blocking calls.

### Idle

`idle_thread_main`: loop `{ if need_resched: schedule(); else
arch_cpu_wait_for_interrupt(); }`. The idle thread never sits on a run
queue; `pick_next` returning NULL selects it.

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
  `complete` sets done and wakes all; `wait_for_completion` waits for
  done. One-shot.

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
