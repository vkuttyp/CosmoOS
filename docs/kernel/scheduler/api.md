# Scheduler and Threads: API

Headers: `kernel/include/kernel/thread.h`, `sched.h`, `wait.h`,
`mutex.h`, `semaphore.h`, `completion.h`, `percpu.h`, and the arch
interfaces `arch/context.h`, `arch/percpu.h`. All of it is internal
kernel ABI; nothing here is visible to user space.

`kernel/scheduler/sched_internal.h` (`thread_init_subsystem`,
`thread_alloc`, `thread_prepare`, `sched_finish_switch`,
`sched_enqueue_new`, `sched_set_running_current`) is private to
`thread.c`, `sched.c`, and `wait.c` and is not an API.

## Shared contracts

- **Lock order** (outermost first): `waitqueue.lock` →
  `runqueue.lock`. The primitives' own locks (`mutex.lock`,
  `semaphore.lock`, `completion.lock`) are taken before their wait
  queue's lock. `runqueue.lock` is a leaf: nothing is acquired while it is
  held except that `schedule()` calls `arch_context_switch` with it held.
- **Interrupt context**: anything that can block (`schedule`,
  `sched_yield`, `sched_block_current`, `wait_event`, `mutex_lock`,
  `semaphore_down`, `wait_for_completion`, `thread_sleep_ns`,
  `thread_join`, `thread_exit`) panics if `this_cpu()->irq_depth != 0` or
  `preempt_count != 0`. Wakers (`sched_wake`, `waitqueue_wake_*`,
  `semaphore_up`, `complete`) and `mutex_trylock`/`semaphore_trydown` are
  interrupt-safe.
- **Preemption**: holding any spinlock disables preemption
  (`spin_lock` → `preempt_disable`). A thread is preempted only at
  interrupt return with `RFLAGS.IF` set, `preempt_count == 0`, and
  `irq_depth == 0` (`x86_trap_dispatch` tail), or when `preempt_enable`
  drops the count to zero with `need_resched` set and interrupts enabled.
- **Slice**: `sched_yield()` sets `slice_left_ns = 0` and re-queues at
  the tail of its priority. A thread preempted with slice remaining is
  re-queued at the head (`policy.enqueue(rq, prev, at_head = true)`).
- **Allocation**: `thread_create`/`thread_prepare` allocate a
  `struct thread` (slab cache `"thread"`) and a 16 KiB stack
  (`vm_kernel_alloc` with guards, populated); nothing else allocates.

---

## thread.h

### `struct thread *thread_create(void (*entry)(void *), void *arg, const char *name, int priority)`
- **Purpose**: create a kernel thread and make it READY on the least
  loaded online CPU in its affinity (`CPUMASK_ALL` by default).
- **Inputs**: `entry` runs on the new stack; returning from it is
  `thread_exit(0)`. `name` is copied (truncated to `THREAD_NAME_MAX - 1`).
  `priority` is clamped to `[0, SCHED_PRIO_LOWEST]`; 0 is highest,
  `SCHED_PRIO_DEFAULT` is 32.
- **Outputs**: the thread, or NULL if the struct or stack allocation
  failed.
- **Ownership/lifetime**: refcount starts at 2: one held by the thread
  itself (dropped by the CPU that switches away from it after exit), one
  by the creator, who must call `thread_join` or `thread_put` exactly
  once. Stack and struct are freed at zero, never by the thread itself.
- **Concurrency**: takes `thread_list_lock`, `kernel_space.lock`, slab
  and zone locks, then the target `runqueue.lock`. May set that CPU's
  `need_resched` if the new thread has higher priority than its current.
- **Blocking**: never; may be called from any thread context, not from
  interrupt context (it allocates).

### `void thread_exit(int code)` (noreturn)
- **Purpose**: terminate the calling thread with `code`.
- **Behaviour**: signals `t->exited` (a completion), sets
  `THREAD_EXITED` with interrupts disabled, and calls `schedule()`. The
  next thread's `sched_finish_switch` drops the self reference.
- **Failure modes**: panics if called by the idle thread or thread 0
  (`THREAD_FLAG_IDLE`/`THREAD_FLAG_BOOT`), with preemption disabled, or
  if the exited thread is ever scheduled again.

### `int thread_join(struct thread *t)`
- **Purpose**: wait for `t` to exit, drop the creator's reference, return
  its exit code.
- **Blocking**: `wait_for_completion(&t->exited)`; returns immediately if
  already exited.
- **Failure modes**: panics on self-join. Joining twice is a refcount
  underflow (`KASSERT` in `thread_put`).

### `void thread_get(struct thread *t)` / `void thread_put(struct thread *t)`
- Atomic reference count. `thread_put` at zero requires
  `THREAD_EXITED` and `t != thread_current()`; it unregisters the thread,
  frees the stack (unless `THREAD_FLAG_BOOT`), and returns the struct to
  the cache. Interrupt-safe except for the final free, which takes
  `kernel_space.lock` (irqsave, so still callable with interrupts off).

### `cpumask_t thread_pin_self(void)`, `void thread_set_affinity_self(cpumask_t)`, `void thread_set_affinity(struct thread *t, cpumask_t)`
Pin the calling thread to the CPU it is on (returning the mask it had),
and put a mask back; a one-CPU affinity is a declared per-CPU claim (S25)
that outlives a sleep, and a migrator never moves a pinned thread. The
third widens another thread's mask (tests); it must admit the CPU the
thread is on, since nothing here moves it.

### `struct thread *thread_current(void)`
- `this_cpu()->current`. NULL before `sched_init`.

### `bool thread_stack_contains(const struct thread *t, uintptr_t addr)`
- True if `addr` lies in `[stack_base, stack_base + stack_size)`. Used by
  `arch_backtrace` to validate frames on thread stacks.

### `unsigned thread_count(void)` / `void thread_dump_all(void)`
- Diagnostics over the global list (`thread_list_lock`). Self-tests use
  `thread_count` to detect leaks.

---

## sched.h

### `void sched_init(void)`
- **Purpose**: adopt the boot context as thread 0 (`"kmain"`, stack from
  `arch_boot_stack`, `THREAD_FLAG_BOOT`, refcount 1), create CPU 0's run
  queue and idle thread, register `sched_tick` with `timer_set_tick_hook`.
- **Lifetime**: once, after `timer_init`, before `arch_irq_enable`.
- **Failure modes**: panics on allocation failure.

### `void sched_start_cpu(void)` (noreturn)
- AP entry into the scheduler (SMP PR): builds the CPU's run queue and
  idle thread, marks the CPU online, switches into idle from a context
  that is never resumed.

### `void schedule(void)`
- **Purpose**: the single switch point. Re-queues a RUNNING current
  (`READY`, head if slice remains), leaves a BLOCKED or already-READY
  current alone, records an EXITED current for reaping, picks the next
  thread through the policy or idle, switches.
- **Concurrency**: takes `runqueue.lock` with `spin_lock_irqsave` and
  holds it across `arch_context_switch`; the thread that runs next
  releases it (`sched_finish_switch`, or `thread_trampoline` for a first
  run). Interrupt state is restored by the resumed thread from its own
  saved value.
- **Failure modes**: panics if `irq_depth != 0` or `preempt_count != 0`
  on entry.

### `void sched_yield(void)`
- Forfeits the remaining slice (`slice_left_ns = 0`) then `schedule()`.
  Same-priority threads run before the caller returns.

### `void sched_preempt(void)`
- Called by `preempt_enable` and the interrupt-return path. Asserts
  `irq_depth == 0 && preempt_count == 0`, then `schedule()`.

### `bool sched_wake(struct thread *t)`
- **Purpose**: BLOCKED → READY on the run queue of `t->cpu`; sets that
  CPU's `need_resched` if `t` outranks its current thread or the CPU is
  idle. **No-op** for any other state, which is what makes the
  `wait_event` protocol lost-wakeup free.
- **Concurrency**: `runqueue.lock` irqsave; interrupt-safe.

### `void sched_block_current(void)`
- Caller has already set `current->state = THREAD_BLOCKED` under a
  wait-queue lock (via `waitqueue_prepare`). Panics in interrupt context
  or with preemption disabled; otherwise `schedule()`.

### `void sched_tick(uint64_t now_ns, struct arch_trap_frame *frame)`
- Tick hook: the self-test watchdog check on CPU 0, `lockup_tick(frame,
  now)` (the soft- and hard-lockup detectors, `docs/kernel/diagnostics/`),
  then `policy.tick(rq, current, TICK_NS)` for a non-idle current;
  sets `need_resched` when idle is running and the bitmap is non-empty.
  Runs in interrupt context; the policy part under `runqueue.lock`.

### `enum sched_migrate_result sched_migrate(struct thread *t, unsigned cpu)`
Move `t` to `cpu`'s run queue. Only a READY thread that is not its queue's
`current` moves (S26); both run-queue locks are taken inside in increasing
CPU-id order (S24) and released before the return. The result names the
check that refused: `SCHED_MIGRATED`, `SCHED_MIGRATE_SAME_CPU`,
`SCHED_MIGRATE_NOT_READY` (RUNNING, BLOCKED, EXITED), `SCHED_MIGRATE_CURRENT`
(READY and queued but still its CPU's current: the woken-before-blocked
window), `SCHED_MIGRATE_AFFINITY`, `SCHED_MIGRATE_OFFLINE` (not online, or
not a CPU). `sched_migrate_result_name` spells it. Re-reads `t->cpu` under
the first lock and retries if the thread moved meanwhile. **Must not be
called with a run-queue lock held**; callable with interrupts off and from
a tick.

### `enum sched_migrate_result sched_migrate_from(unsigned from, unsigned to, struct thread **moved)`
The same move for a thread the policy chooses: both locks, then
`policy->pick_migratable(&rq[from], CPUMASK_OF(to))` under them, then the
move. `*moved` is the thread on `SCHED_MIGRATED`, NULL otherwise;
`SCHED_MIGRATE_NOT_READY` means the queue offered nothing. The entry for
a caller that only knows the queue -- the chaos migrator, and the
balancer of the next unit -- since there is no selection to hand over.

### `uint64_t sched_migration_count(void)`, `void sched_chaos_stats(uint64_t *migrated, uint64_t *refused)`
Moves made since boot, every entry counted (printed by `sched_dump`); and,
in a `SCHED_CHAOS=1` build, the tick migrator's tally.

### `struct runqueue *sched_runqueue(unsigned cpu)`, `uint64_t sched_switch_count(unsigned cpu)`, `void sched_dump(void)`
- Diagnostics; `sched_dump` prints per CPU the queue state, `ticks`, and
  the tick sample (`last tick N ms ago pc 0x...`), then `thread_dump_all`,
  whose `run_ms` is live for the running thread, then every hook
  registered with `sched_dump_register(name, fn)` (a subsystem's
  counters: the network workers' queues and work runs). The self-test
  watchdog follows it with every CPU's sample (`lockup_sample_all`) and
  eight more of every busy CPU (`lockup_profile`).

### `void sched_dump_register(const char *name, void (*fn)(void))`
- A subsystem's contribution to `sched_dump`, printed after the thread
  table on every dump. Runs in interrupt context with interrupts off and
  must take no lock another CPU may hold: print counters. Eight slots.

### `struct sched_policy` / `sched_policy_rr`
`pick_migratable(rq, allowed)`: a ready thread on `rq` that is not
`rq->current` and whose affinity admits a CPU in `allowed`, or NULL;
called with `rq->lock` held. Round-robin offers the thread it would run
last (the lowest priority level's tail).
- Function table `{enqueue, dequeue, pick_next, tick, slice_new}`. All
  entries run under `runqueue.lock`. `pick_next` returning NULL selects
  idle. The only policy is `sched_policy_rr` (`policy_rr.c`): 64 FIFO
  lists with a 64-bit occupancy bitmap, `SCHED_SLICE_NS` = 10 ms; a thread
  whose slice expires yields only if the highest ready priority is ≤ its
  own, otherwise the slice refills.

---

## wait.h

### `struct waitqueue`, `void waitqueue_init(struct waitqueue *wq, const char *name)`, `WAITQUEUE_INIT(name)`
- A spinlock and a list of `struct wait_entry` (link + thread). Entries
  live on the waiting thread's stack.

### `wait_event(wq, cond)` (macro)
- **Protocol**: `wait_entry_init(&e)` once; loop { `waitqueue_prepare(wq,
  &e)`; if `cond` break; `sched_block_current()` }; `waitqueue_finish(wq,
  &e)`. `cond` is evaluated with the thread already linked and BLOCKED,
  so a wake that lands between the evaluation and the switch flips the
  state to READY and `schedule()` returns at once; `cond` is re-checked
  after every wake (Mesa semantics).
- **Restrictions**: not in interrupt context, not with preemption
  disabled (panics in `waitqueue_prepare`). `cond` must not sleep.

### `void waitqueue_prepare(struct waitqueue *wq, struct wait_entry *e)`
- Links `e` if `list_empty(&e->link)` (a re-prepare after a false
  condition must not push twice), sets `current->waiting_on`, sets
  `THREAD_BLOCKED`. Under `wq->lock` irqsave.

### `void waitqueue_finish(struct waitqueue *wq, struct wait_entry *e)`
- Unlinks `e` under `wq->lock`, then `sched_set_running_current()`, which
  dequeues the thread from its run queue if an early wake already queued
  it and marks it RUNNING.

### `unsigned waitqueue_wake_one(wq)` / `waitqueue_wake_all(wq)`
- `sched_wake` on the first / every linked entry under `wq->lock`;
  returns the number of entries touched (not necessarily state changes).
  Interrupt-safe.

### `bool waitqueue_empty(wq)`
- Snapshot under the lock.

### `wait_event_killable(wq, cond)` (macro, Phase 9)
- The `wait_event` loop that also ends when the calling process is
  being killed: after `waitqueue_prepare` and a false `cond` it checks
  `process_kill_pending()` (declared in `wait.h`, defined in
  `kernel/process/process.c`: the current process's `kill_sig`) and
  breaks with `-EINTR` instead of blocking; otherwise identical. The
  check sits after the thread is queued and BLOCKED, so `process_kill`'s
  `sched_wake(t)` (a direct wake of the thread, not of the queue) cannot
  be lost: it either finds the thread BLOCKED and makes it READY, after
  which the loop re-checks the flag, or lands before the check, which
  then sees the flag. Evaluates to 0 or `-EINTR`. Same restrictions as
  `wait_event`; kernel threads (no process) never see `-EINTR`. Used by
  the tty, pipes, `process_wait_child`, the killable sleep and the socket
  layer's blocking paths.

### `void thread_sleep_ns(uint64_t ns)`, `thread_sleep_ms(ms)`
- Arms a stack `struct timer` whose callback sets a flag and
  `waitqueue_wake_all`s a stack wait queue, then `wait_event`s on the
  flag. Granularity is one tick (`TICK_NS` = 4 ms); the sleep is never
  shorter than `ns`. Not usable in interrupt context.

### `int thread_sleep_ns_killable(uint64_t ns)` (Phase 9)
- The same sleep with `wait_event_killable`; returns 0, or `-EINTR`
  early when the calling process is being killed, in which case the
  stack timer is cancelled before the frame goes away. `sys_sleep_ns`
  uses it.

---

## mutex.h

`struct mutex { spinlock_t lock; struct thread *owner; struct waitqueue wq; const char *name; uint16_t class; }`

- `mutex_init(m, name)`; the name is the lock class for the debug-build
  checker (`docs/kernel/lockdep/`).
- `mutex_lock(m)`: `might_sleep()`, then a loop of the internal try and
  `wait_event(&m->wq, owner == NULL)`. Panics in interrupt context, under a
  spinlock (`might_sleep`) and on recursive acquisition by the owner. No
  priority inheritance. The order check runs before the wait.
- `mutex_lock_nested(m, subclass)`: the same, annotated for nesting inside
  another mutex of the same class (`VNODE_NESTED_*`).
- `mutex_trylock(m)`: interrupt-safe; true if acquired.
- `mutex_unlock(m)`: panics unless the caller owns it; clears the owner
  under `m->lock`, then `waitqueue_wake_one`.
- `mutex_is_locked(m)`: unsynchronised read.

## semaphore.h

`struct semaphore { spinlock_t lock; int count; struct waitqueue wq; }`

- `semaphore_init(s, count, name)`, `semaphore_down(s)` (blocks until
  `count > 0`; panics in interrupt context), `semaphore_trydown(s)`,
  `semaphore_up(s)` (interrupt-safe; increments and wakes one),
  `semaphore_count(s)`.

## completion.h

`struct completion { spinlock_t lock; bool done; struct waitqueue wq; }`

- `completion_init(c, name)`, `complete(c)` (sets `done` and wakes all
  under the lock; interrupt-safe; idempotent), `wait_for_completion(c)`
  (returns at once if done; panics in interrupt context; when it returns,
  `complete` has finished touching `c`, so the caller may free it),
  `completion_done(c)` (a lock-free read: true does *not* mean `complete`
  has returned — a poller that saw it must still call
  `wait_for_completion` before freeing `c`).

---

## percpu.h

### `struct percpu`
`self` (must stay at offset 0), `cpu_id`, `current`, `idle`,
`preempt_count`, `need_resched`, `online`, `irq_depth`, `rq`, `timers`,
`ticks`, `irq_count`. Architecture-specific per-CPU data lives in arch
arrays indexed by `cpu_id` (for example `x86_cpu_apic_id`).

- `void percpu_init_boot(void)`: zero the static boot instance, register
  it as CPU 0, `arch_percpu_install`. Must run after `gdt_init` on x86-64
  (loading the GS selector resets the GS base) and before any spinlock.
- `void percpu_register(struct percpu *pc, unsigned cpu_id)`: SMP.
- `struct percpu *this_cpu(void)`: `arch_percpu_get()`, checked in debug
  builds: a call where the answer could not be kept (S25 -- preemption on,
  interrupts on, thread context, an affinity of more than one CPU, more
  than one CPU) panics naming the call site. `unsigned arch_cpu_id(void)`
  (`arch/cpu.h`, implemented in `kernel/core/percpu.c` over the
  architecture's `arch_cpu_id_raw`) is checked the same way, and is the
  module export.
- `struct percpu *raw_this_cpu(void)` / `unsigned raw_cpu_id(void)`: the
  unchecked reads, for an answer that is the same on any CPU that runs
  the thread or for a diagnostic; every use carries a comment saying
  which.
- `void percpu_claim_expect(void)` / `unsigned percpu_claim_expected_hits(void)`
  (debug): the next violation is counted instead of fatal; one-shot, for
  the `percpu-claim` self-test.
- Build knob `PERCPU_WARN=1`: the check warns once per site
  (`PERCPU-CLAIM ip=... thread=...`) instead of panicking; the sweep's
  form, never shipped on.
- `struct percpu *percpu_get(unsigned cpu)`, `unsigned cpu_count(void)`,
  `bool cpu_online(unsigned cpu)`, `cpumask_t cpu_online_mask(void)`.
- `void preempt_disable(void)` / `void preempt_enable(void)`: nestable
  counter on the raw block. `preempt_enable` calls `sched_preempt()` when
  the count reaches zero with `need_resched` set, `irq_depth == 0`, and
  interrupts enabled; it asserts the count was positive. Preemption
  disabled is the migration barrier: the thread cannot become READY and
  so cannot be moved (S25).
- `bool preemptible(void)`: `preempt_count == 0 && irq_depth == 0`.
- `CONFIG_MAX_CPUS` = 64, `cpumask_t` = `uint64_t`, `CPUMASK_ALL`,
  `CPUMASK_OF(c)`.

---

## arch/context.h

`struct arch_context { uintptr_t sp; }`

- `void arch_context_init(ctx, stack_top, entry)`: `stack_top` must be
  16-byte aligned; lays out the first-run frame (`context.c`) so the
  first switch "returns" into `x86_context_start`, which pops `entry` and
  jumps to it with a null frame pointer and a zero return address.
- `void arch_context_switch(from, to)`: saves rbp/rbx/r12–r15 and the
  stack pointer into `from`, loads `to`, restores, `ret` (`switch.S`).
  Interrupts must be disabled by the caller; RFLAGS is not saved.
- `void arch_boot_stack(uintptr_t *base, size_t *size)`: bounds of the
  64 KiB boot stack from `entry.S`.

## arch/percpu.h

- `void arch_percpu_install(struct percpu *pc)`: sets `pc->self = pc`
  and writes `MSR_GS_BASE`.
- `struct percpu *arch_percpu_get(void)`: `mov %gs:0, %rax`.
- `arch_cpu_id()` (declared in `arch/cpu.h`) reads `%gs:offsetof(struct
  percpu, cpu_id)`.
