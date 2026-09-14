# NEXT SUBSYSTEM — a wake that preempts

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
**nothing in it is implemented**.

**Subsystem: the scheduler's fourth preemption point, and the network
worker's priority decided by measurement.** The scheduler documents that
a thread which wakes a higher-priority thread is preempted "at
`preempt_enable` reaching zero" rather than at the next tick. That is
true only when interrupts are enabled at that moment, and **every wake
in this kernel happens under an interrupt-disabling spinlock**, whose
unlock re-enables preemption *before* it re-enables interrupts. So the
promise is void at all forty wake sites: a thread woken from another
thread's context runs at the next tick, up to 4 ms later, or when the
waker blocks. The inventory names it twice (`docs/audit/2026-09-deferred-work-inventory.md`
§4: "woken-thread latency", "the network worker runs below default
priority"); the lifetime report named it in 2026-09-05 as architectural
debt and assigned it to the lockdep milestone, which did not do it; the
audit filed it as 4.2 MEDIUM. Closing it also unblocks the one decision
that has been waiting on it: whether the network worker should run above
the threads that feed it.

## Problem

`kernel/core/spinlock.c:134`:

```c
void spin_unlock_irqrestore(spinlock_t *lock, arch_irq_state_t state)
{
    spin_unlock(lock);          /* ... preempt_enable(): IRQs are still off, so no switch */
    arch_irq_restore(state);    /* IRQs back on; nobody looks at need_resched */
}
```

and `kernel/core/percpu.c:69`:

```c
if (pc->preempt_count == 0 && pc->need_resched && pc->irq_depth == 0 && arch_irq_enabled())
    sched_preempt();
```

`sched_wake` (`kernel/scheduler/sched.c:308-323`) runs under the run
queue's `irqsave` lock and, when the woken thread outranks the current
one on its CPU, calls `request_resched`, which for the *local* CPU only
sets `need_resched` (`sched.c:163-170`; a remote CPU gets an IPI and
preempts on its interrupt return). Wakes reach `sched_wake` in two
shapes. **Forty sites** go through `waitqueue_wake_one`/`_all`
(`kernel/scheduler/wait.c:61-75`), which holds the wait queue's
`irqsave` lock around it: semaphores, mutexes, completions, pipes, the
tty, socket readiness, the network workers' queues, the reaper.
**Thirteen sites** call `sched_wake` directly on a thread they have
found by other means: the futex (`kernel/ipc/futex.c:55,150,213`),
`poll` and the AIO ring (`kernel/io/poll.c:32`, `aio.c:335`), process
kill (`kernel/process/process.c:819,1183`) and signal delivery
(`kernel/process/signal.c:120-307`), most of them under a lock of their
own. Either way the sequence on the waking CPU is the same, because
`sched_wake` brings its own `irqsave` lock: lock (IRQs off, preemption
off) → `need_resched = true` → `spin_unlock` → `preempt_enable` sees
IRQs off and returns → `arch_irq_restore` turns IRQs on (or restores
them still off, when the caller holds an outer `irqsave` region whose
own restore comes later) → the waker keeps running. None of the
fifty-three preempts. The woken thread runs at the next tick (250 Hz: up to
4 ms), at the next `preempt_enable` that happens to run with interrupts
enabled (the lifetime pass added one at the end of `netif_transmit`
precisely because the network stack, which uses `irqsave` locks
throughout, had none), or when the waker blocks.

**The system-call return has no preemption point either.** `x86_syscall_c`
(`kernel/arch/x86_64/user.c:97`) and `handle_syscall`
(`kernel/arch/aarch64/trap.c:74`) run the dispatcher with interrupts on
and return to user without consulting `need_resched`; only the kill and
signal checks live there. A system call that woke a higher-priority
thread returns to its user code, which runs until the tick.

**Who is affected today.** Every kernel thread but one is created at
`SCHED_PRIO_DEFAULT` (32), and native user threads have no other
priority, so no thread in the tree is starved by this *yet*: an
equal-priority wake is not supposed to preempt (`t->priority <
rq->current->priority`, `sched.c:318`). The exception runs the other
way: the network workers are created at **40**, below default
(`kernel-services/network/netif.c:725`), so a default-priority thread
sending on the worker's own CPU is never displaced by it. That is
measured on every boot: `net-bench` with steering off delivers **512 of
10 000** UDP sends -- the receive queue's depth, `NET_RXQ_MAX` --
because the sender on CPU 0 runs its whole burst before CPU 0's worker
gets a slice, and the suite-waits unit's proofs met the same thing from
the other side (a 300-echo flood is processed only after the sending
thread first sleeps, `docs/audit/next-subsystem-suite-waits.md`, as-run).
The lifetime report saw both halves in 2026-09-05 and named them
together: "The `netrx` worker runs below default priority; with prompt
preemption a busy user thread can hold packets in the receive queue for
a slice. Per-connection locking and a priority decision belong to
milestone 8." Milestone 8 did the locking. The priority decision needs
the preemption point first, because raising the worker without it
changes nothing: the enqueue's wake would set `need_resched` under the
same `irqsave` lock and the sender would still finish its burst.

## Current implementation

- Three preemption points, documented as exhaustive (invariant **S8**,
  `docs/kernel/scheduler/invariants.md:74`; `docs/kernel/scheduler/design.md`
  "Preemption points"): the interrupt-return tail when the interrupted
  frame had interrupts enabled (`kernel/arch/x86_64/trap.c:91-95`,
  `kernel/arch/aarch64/trap.c:114-117`), `preempt_enable` reaching zero
  with interrupts enabled (`percpu.c:63-71`), and an explicit
  `sched_yield`/block.
- `arch_irq_restore` is two lines on each architecture
  (`kernel/arch/x86_64/cpu.c:183-187`: `sti` if the saved flags had IF;
  `kernel/arch/aarch64/irq.c:13-16`: write DAIF back) and is called from
  `spin_unlock_irqrestore` and from 26 direct `arch_irq_save`/`restore`
  pairs outside the spinlock code.
- `need_resched` is per CPU (`percpu.h`), set by `request_resched`, by
  the tick when a slice ends (`sched.c:393`), consumed and cleared in
  `schedule_internal` (`sched.c:252`).
- Cross-CPU wakes are prompt: `request_resched` sends `IPI_RESCHEDULE`
  to another CPU and the target preempts on interrupt return;
  `smp-wake` counts that IPI (suite-waits unit). The same-CPU case has
  no equivalent.
- The system-call return checks a pending kill and pending signals
  (`process_return_to_user`, `x86_syscall_return_check`) and nothing
  about scheduling.
- The network workers: one per CPU at priority 40, pinned, woken by
  `netif_rx` (`netif.c:527`) and by deferred work (`netif.c:633`) under
  the queue's `irqsave` lock. `net-bench` reports UDP delivery per boot;
  with steering off it has read 512 of 10 000 in every logged run.

## Why it matters

1. **Correctness of a stated invariant.** S8 and the design document say
   a wake preempts at `preempt_enable`; the code says that only when the
   waker happened to have interrupts on, which at every wake site it does
   not. An invariant that is documented and untrue is the kind the
   constitution's §74 is about, and the audit (4.2), the lifetime report
   (§8) and the inventory (§4) have all named it; it has waited through
   thirty units.
2. **It is the prerequisite for a measured decision the stack has needed
   since milestone 8.** Whether the network worker should outrank the
   threads that feed it cannot be answered until a wake can preempt;
   until then the answer is "no" by mechanism rather than by choice. The
   cost of "no" is visible on every boot (512 of 10 000) and was the
   direct cause of two misdrawn proofs in the last unit.
3. **Small and mechanical.** One generic function, two call sites in the
   architecture layer, a test that is deterministic on one CPU, and a
   benchmark that already exists. No ABI, no new lock, no new state.
4. **It removes the latency floor a family of future units would
   otherwise inherit**: a high-priority thread of any kind -- an
   interrupt bottom half, a real-time policy (constitution §20), a
   console reader -- would today wake at tick granularity when woken by a
   thread.

## Design

### The fourth preemption point: restoring interrupts

The condition the scheduler needs is "this CPU is about to run with
interrupts enabled, holds no spinlock, is not in an interrupt, and a
higher-priority thread is runnable". `preempt_enable` tests it when the
count reaches zero; the missing test is when *interrupts* become
enabled while the count is already zero. That is exactly
`arch_irq_restore` with a state that enables them:

```c
/* kernel/core/percpu.c -- generic, called from the arch layer */
void preempt_point(void)
{
    struct percpu *pc = this_cpu();
    if (pc->preempt_count == 0 && pc->need_resched && pc->irq_depth == 0 && arch_irq_enabled())
        sched_preempt();
}
```

```c
/* kernel/arch/x86_64/cpu.c */
void arch_irq_restore(arch_irq_state_t state)
{
    if (state & RFLAGS_IF) {
        __asm__ volatile("sti" ::: "memory");
        preempt_point();
    }
}
/* kernel/arch/aarch64/irq.c: the same after the DAIF write, when the
 * restored state has I clear */
```

`preempt_enable` keeps its own test (the case where interrupts were
already on); the two together make the predicate complete: preemption
happens at the first instant all four conditions hold, whichever of the
two became true last. `spin_unlock_irqrestore` needs no change -- its
`arch_irq_restore` is the point -- and neither do the 26 direct
`arch_irq_save`/`restore` pairs, which is the reason to put the point in
the restore rather than in the unlock: a wake inside any
interrupts-off region, locked or not, is caught at that region's exit.

**Why this is safe where it runs.** `sched_preempt` asserts `irq_depth
== 0` and `preempt_count == 0` and then calls `schedule_internal(true)`.
The four conditions exclude every context in which a switch would be
wrong: inside an interrupt (`irq_depth`), holding any spinlock
(`preempt_count`, invariant S7), and the two internal callers of
`arch_irq_restore` that the check must pass through unharmed -- the
lockdep bracket inside `spin_unlock` (`spinlock.c:108-116`, which
restores while the lock's `preempt_disable` is still in force, so the
count is non-zero and the point does nothing) and the end of `schedule()`
itself (invariant S5: the resumed thread restores its caller's state with
`arch_irq_restore` after `sched_finish_switch`). At that last site the
count is zero and interrupts come back on; if `need_resched` was set
again meanwhile -- a wake during the switch -- the CPU takes one more
trip through `schedule()`, which is the same thing a tick arriving at
that instant would do, and which `schedule_internal` already handles
(it clears `need_resched` at `sched.c:252` before picking). The idle
loop's `sti; hlt` sequence is not an `arch_irq_restore` and is
unchanged.

**What it does not change.** Equal-priority wakes still do not preempt;
the slice is still the unit of fairness; the interrupt-return point is
untouched; cross-CPU wakes are already prompt. A thread that wakes a
higher-priority thread and is preempted keeps its slice remainder and is
re-queued at the head, as the policy already does for a preempted thread
(`policy_rr.c`).

**The system-call return** gets no separate point, and the report says
why rather than adding one for symmetry: the dispatcher runs with
interrupts enabled, and every wake it can perform -- wait-queue or
direct -- ends in `sched_wake`'s own `irqsave` unlock or the caller's
outer restore, so the point above fires inside the call, before the
return. The user-mode test in step 2 asserts exactly that, so if a
wake path ever appears that runs with interrupts on and no lock, the
test says so and the return point becomes the next change.

### The worker's priority, decided by the numbers

With the point in place, the enqueue in `netif_rx` (`netif.c:527`)
becomes a wake that can preempt the enqueuing thread -- if the worker
outranks it. Whether it should is a trade the constitution (§20,
mechanism versus policy; §73, "batching") says to measure rather than
assert:

- **above default (e.g. 31)**: a packet handed to the local worker is
  processed at once. Latency for the sender's own CPU falls from "after
  the burst or the tick" to one switch; the steer-off queue no longer
  overflows on a burst, so `net-bench`'s 512 of 10 000 becomes 10 000.
  The cost is a switch per enqueue when the sender and the worker share
  a CPU: a bulk sender ping-pongs with its worker on every packet, which
  is what the 64-packet budget in `worker_main` (`netif.c:699-711`) was
  written to avoid.
- **at 40, as today**: bursts overflow, latency is the tick, throughput
  of a bulk sender is undisturbed.
- **at 32, equal**: no preemption by priority; the tick and the
  sender's blocking are the only switches, as today, but a worker that
  is runnable when the sender's slice ends is picked before a later
  equal-priority thread. Cheapest change; may be enough.

The unit builds the point, then runs `net-bench` (both steering modes;
TCP one and two flows, UDP sends per second and delivered of 10 000)
and the wake-to-run latency test below at 40, 32 and 31, on both
architectures, five runs each so the run-to-run spread is known, and
chooses by this rule, in order -- the report commits to the rule, not
the number, because the number is the measurement's to give:

1. **Throughput is the veto.** A setting whose TCP one-flow or two-flow
   figure is below 40's by more than the measured spread, on either
   architecture, is out.
2. **Among the survivors, the highest UDP delivered count wins**
   (10 000 of 10 000 beats 512 of 10 000); this is the metric the
   priority exists to move, and it is deterministic.
3. **On a tie, the lower wake-to-run latency wins**; on a tie there, the
   setting closest to today's (40, then 32, then 31), so that a change is
   made only when a number asks for it.

Every outcome names one setting. If 31 is vetoed and 32 survives with
the same delivered count as 40, the rule keeps 40, and the batching a
preempting worker would need (wake once per burst, not per packet -- an
`avail` bit the enqueue sets and the wake consumes) becomes the
follow-up rather than something smuggled into this unit.

### The §70 gate

**Correctness.** The predicate for a preemption point is stated once
(four conditions) and tested at the two moments either of its last two
terms can become true. Before, one of those moments was untested; after,
both are. Nothing about *which* thread runs changes; only *when*.

**Concurrency.** `need_resched` is per CPU and consulted on its own CPU;
`preempt_point` reads it with interrupts enabled, so a tick between the
read and `sched_preempt` can only set it again, and `schedule_internal`
consumes it under the run-queue lock. No new sharing. The point runs on
the waker's stack in thread context, as `preempt_enable`'s does today.

**Ownership / Lifetime.** None: no object, no state beyond the flag that
exists.

**Failure.** A `sched_preempt` from a context that violates its
assertions panics in debug builds, which is the right failure; the four
conditions are what keep it from happening, and the lockdep and
`schedule()` sites are argued above and tested below.

**Security.** None: kernel-internal, no user-visible interface. A user
thread cannot raise its priority, so it cannot use the point to starve
another.

**Performance.** One predicted-not-taken branch per `arch_irq_restore`
that enables interrupts: a load of `preempt_count`, a load of
`need_resched`. On the hot paths (every `irqsave` unlock in the network
stack, the scheduler, the timer) that is measurable in principle and is
measured in the benchmarks section. The worker priority is the
performance decision, and it is made by measurement.

**Scalability.** Per-CPU state only; nothing crosses CPUs. At 1 CPU the
point is the only way a same-CPU wake can be prompt; at 64 nothing
changes.

**Portability.** Two arch call sites, one line each, after the
architecture's own enable; the predicate is generic.

**Testing.** Deterministic on one CPU: the waker's very next statement
is the observation.

## Affected files

| file | change |
| --- | --- |
| `kernel/core/percpu.c`, `kernel/include/kernel/percpu.h` | `preempt_point()`; `preempt_enable` unchanged; a per-CPU debug counter of preemptions taken at the restore point, incremented in `preempt_point` and printed in the scheduler dump beside `switches` (`sched.c`, the `cpu %u:` line) |
| `kernel/arch/x86_64/cpu.c` | `arch_irq_restore` calls it after `sti` |
| `kernel/arch/aarch64/irq.c` | the same after the DAIF write when I is cleared |
| `kernel-services/network/netif.c` | the worker's priority, as measured (one constant, with the measurement in the comment) |
| `kernel/scheduler/schedtest.c` | `preempt-wake` (a wait-queue wake on the same CPU preempts), `preempt-wake-direct` (a direct `sched_wake` preempts), `preempt-wake-locked` (a wake inside a plain `arch_irq_save`/`restore` region preempts at the restore); the existing `preempt` test unchanged; **the debug probe** behind `preempt-wake-syscall` (a priority-16 thread and a completion, created and completed by the sysctl's own write); **`irqrestore-bench`**, the micro-benchmark of the Benchmarks section, a self-test that prints and asserts nothing, in the shape of `fpu-bench` |
| `kernel/syscall/native.c` | the `debug.preempt_probe` sysctl entry in the registry (debug builds; writable; privileged, like `debug.faultinject`), dispatching to the probe in `schedtest.c` |
| `userland/init/init.c` | `preempt-wake-syscall`: a wake made inside a system call preempts before the call returns, observed through the probe -- one step of `init --selftest` |
| `kernel-services/network/nettest.c` | `net-bench` unchanged; its UDP delivered count becomes the worker decision's evidence and the report quotes it |
| `docs/kernel/scheduler/design.md`, `invariants.md` (S8 → four points), `testing.md` | the rule and its check |
| `docs/kernel-services/network/design.md`, `testing.md` | the worker's priority and why |
| `docs/audit/2026-09-deferred-work-inventory.md` | strike "woken-thread latency" and "the network worker runs below default priority" (§4), the audit's 4.2 MEDIUM row's syscall-return half |
| `README.md` | Status entry |

**No kernel change outside these** -- in particular no hook in any wake
path: the probe's sysctl write is itself the system call that wakes,
so `SYS_futex_wake` and its kin are untouched -- no UAPI, and no module
ABI change (`arch_irq_restore` keeps its signature; modules that call
it get the point for free).

## New APIs

```c
/* kernel/include/kernel/percpu.h */
void preempt_point(void);   /* switch now if a higher-priority thread is runnable and it is safe */
```

Internal. No syscall, no header a program sees.

## Migration plan

1. **The point, and the test that shows it firing.** `preempt_point`,
   the two arch call sites, `preempt-wake`. Small enough to review as a
   pattern.
2. **The other shapes**: `preempt-wake-direct` (a direct `sched_wake`,
   the futex's and the signal path's shape), `preempt-wake-locked` (a
   wake under a bare `arch_irq_save`/`restore`), and the user-mode
   observation that a wake inside a system call preempts before the
   return, through the debug probe. Both arches.
3. **The worker's priority by measurement**: `net-bench` and the latency
   test at 40, 32, 31 on both architectures; the constant set by the
   rule above; the numbers in the design document.
4. **The documents**: S8 becomes four points; the inventory's entries
   struck; the as-built banner.

## Tests

Every test is proved by reintroducing its bug and reading the failure's
reason, then restoring the source byte-identically.

- **`preempt-wake`** (kernel, one CPU by construction). A thread at
  priority 16 pinned to CPU 0 blocks on a semaphore. The test thread --
  thread 0, on CPU 0, priority 32 -- posts and, as its **very next
  statement**, stores `after = 1`. The waiter's first statement on waking
  reads `after` into `saw`. Assert `saw == 0`: the waiter ran between
  the post and the waker's next instruction, which only a preemption
  inside `semaphore_up` can produce. **Bug-proof**: remove the
  `preempt_point` call from `arch_irq_restore`; `saw` reads 1, because
  the waker continued to the store and beyond, and the waiter ran at the
  tick (its wake-to-run latency, printed, jumps from microseconds to
  milliseconds). A second proof: keep the call but make it test
  `arch_irq_enabled()` *before* the `sti` -- the ordering mistake a
  reviewer would make -- and the test fails the same way, which is what
  pins the call after the enable.
- **`preempt-wake-direct`**: the same observation for the other shape.
  The waiter parks itself the way a futex waiter does -- `waitqueue_prepare`
  on a private queue, then `sched_block_current` -- and the test wakes it
  with `sched_wake(t)` directly, no wait-queue wake, then stores
  `after = 1`. Assert `saw == 0`. **Bug-proof**: the same removal; `saw`
  reads 1. This is the futex's, `poll`'s and the signal path's shape,
  and it is what shows the point belongs to `sched_wake`'s own unlock
  and not to the wait queue's.
- **`preempt-wake-locked`**: the same, with the post made inside a
  `arch_irq_save()`/`arch_irq_restore()` pair with no lock, so the wake's
  own unlock happens with interrupts already off and the *outer* restore
  is the first enable. Assert `saw == 0`. **Bug-proof**: move the point
  into `spin_unlock_irqrestore` instead of `arch_irq_restore` (the
  narrower design this report rejected): the test fails, because the
  outer restore is not an unlock.
- **`preempt-wake-syscall`** (user mode, `init --selftest`). A
  debug-only, privileged sysctl `debug.preempt_probe`, registered in
  `kernel/syscall/native.c` beside `debug.faultinject` and handled in
  `schedtest.c`: **its own write is the system call under test**. The
  handler creates a priority-16 thread pinned to the caller's CPU,
  waits for it to be blocked on a completion (the `THREAD_BLOCKED`
  discipline), completes it -- a wait-queue wake made inside a system
  call -- and then, as its next statement, records a per-CPU sequence
  number; the probe thread records the same counter as its first
  statement. The sysctl's read returns both numbers, and the test asserts
  the probe's is lower: the probe ran before the system call that woke
  it reached its own next line, let alone returned to user mode. No wake
  path gains a hook. **Bug-proof**: the same removal as above; the probe
  runs after the return (at the tick), the numbers invert. This is the
  test that justifies not adding a system-call-return point, and would
  name the day one is needed.
- **`preempt`** (existing, a spinner displaced by a woken sleeper):
  unchanged, and it must stay passing -- its wake comes from a timer
  callback in interrupt context, the point that already existed.
- **The lockdep and `schedule()` sites**: a debug assertion in
  `preempt_point` that `preempt_count == 0` is *already* the guard; the
  bug-proof is to call the point unconditionally from `spin_unlock`'s
  lockdep bracket and watch `sched_preempt`'s `KASSERT(preempt_count ==
  0)` fire on the first `spin_unlock` -- which shows the guard is load-
  bearing, not decorative.
- **Both architectures**, because the arch call sites are separate
  lines: a proof run on one is not a control for the other.

**Vacuity, named in advance.** `saw == 0` can pass for the wrong reason
if the waiter never blocked (a post that finds no waiter wakes nobody and
`saw` keeps its initial 0). The test therefore initialises `saw` to a
sentinel (2), waits for the waiter to read `THREAD_BLOCKED` before
posting (the discipline `smp-wake` adopted), and asserts `saw` is
exactly 0, not "not 1".

## Benchmarks

1. **The cost of the point**: `fpu-bench`'s switch measurement and
   `irqrestore-bench` (`schedtest.c`, in the affected-files table: a
   million `arch_irq_save`/`restore` pairs with `need_resched` clear,
   printed per boot like `fpu-bench`) before and after, both arches.
   Expected: two loads and a not-taken branch, under 1 ns on TCG's scale
   of things; reported either way.
2. **Wake-to-run latency, same CPU**: printed by `preempt-wake` (from
   the post to the waiter's first instruction). Expected: from up to
   4 000 µs to the cost of one switch (about 2-3 µs on this host's TCG).
3. **`net-bench`** at 40, 32 and 31: TCP one flow and two flows, UDP
   sends per second and delivered of 10 000, steering off and on, both
   arches. This is the decision, and its table goes in the design
   document whatever it says.

## Risks

- **A preemption where none was expected.** Code that wakes a
  higher-priority thread under an `irqsave` lock and then relies on
  running a few more instructions before it is displaced -- a pattern
  the kernel forbids but nothing enforced. The switch is the same one a
  tick could have made at that instant, so anything that breaks was
  already broken under a tick; the difference is that it now breaks
  deterministically, in the first test that runs the path. The full
  suite on both arches is the search for such sites.
- **Ping-pong when the worker outranks its feeder.** Named above;
  decided by the benchmark; the batching that would fix it is a named
  follow-up, not part of this unit.
- **The `schedule()` tail**: a wake during the switch now triggers a
  second `schedule()` immediately instead of at the tick. Argued safe
  above (it is what a tick would do); `preempt` and every blocking test
  exercise it, and the per-CPU debug counter of preemptions taken at the
  restore point (`percpu.c`, printed in the scheduler dump; in the
  affected-files table) makes the frequency visible.
- **A user-visible timing change.** A process that woke a higher-priority
  kernel thread ran on to the tick before; it does not now. No user
  thread can observe a *different* result, only a different
  interleaving, and the test suite's own waits are already written for
  the property rather than the interleaving (the last unit).

## Alternatives considered

- **A point in `spin_unlock_irqrestore` only.** Covers the forty wake
  sites and misses the 26 bare `arch_irq_save`/`restore` regions; a wake
  inside one of those would silently keep the old latency. The restore
  is the right place because it is the only place interrupts come back.
- **A deferred-preemption flag** (the lifetime report's phrasing).
  `need_resched` already is that flag; what was missing was a consumer,
  not a second flag.
- **A system-call-return point as well.** Not needed while every wake
  passes through an interrupts-off region; adding it "for symmetry"
  would hide the day that stops being true. The user-mode test is the
  guard instead.
- **Raising the worker's priority without the point.** Changes nothing:
  the enqueue's wake sets `need_resched` under the same lock and the
  sender finishes its burst regardless. This is why the two are one unit
  and in this order.
- **Running the network worker in interrupt context** (softirq-shaped).
  Rewrites a stack the audit and three units have made correct as a
  thread; out of scope and not what a priority question needs.

---

Named and deferred by this unit: a per-burst wake for the workers (an
`avail` bit, so a preempting worker is woken once per burst rather than
once per packet) if the measurement shows ping-pong; priority
inheritance for mutexes (audit 6.2 LOW, unrelated mechanism); the
scheduler policies of constitution §20.

Closes, in `docs/audit/2026-09-deferred-work-inventory.md`: §4 "woken-thread
latency" and "the network worker runs below default priority"; §3's
syscall-return half of audit 4.2 MEDIUM.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01FtzXcfogMnEqCnyAVzZYFj
