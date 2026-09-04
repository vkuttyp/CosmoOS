# Scheduler and Threads: Architecture

Phase 3 of the roadmap. This document is the specification; `design.md`
holds the data structures and algorithms, `api.md` the contracts,
`invariants.md` the rules, `testing.md` the proof.

## 1. Purpose

Give the kernel concurrent execution: kernel threads with their own
stacks, a preemptive per-CPU scheduler with priorities and time slices,
blocking primitives (wait queues, mutexes, semaphores, completions),
timed sleep, and per-CPU state. Processes and user threads (Phase 4)
build on the same thread object.

## 2. Where it sits

```text
     drivers, services, self-tests
             │ thread_create / wait_event / mutex / sleep
             ▼
   ┌──────────────────────────────────────────────────┐
   │ kernel/scheduler/   threads, run queues, policy, │
   │                     wait queues, sync primitives │
   │       │ timers, clock            ▲ tick, wakeups │
   │ kernel/timer/       clock source, timer queue,   │
   │                     per-CPU tick                 │
   │       │ arch_timer_*             ▲ interrupt     │
   │ kernel/interrupt/   vectors, IRQ routing, IPIs   │
   │       │ arch_irqc_*, arch_ipi_*                  │
   └───────┼──────────────────────────────────────────┘
           ▼
   kernel/arch/x86_64/   context switch, per-CPU (GS), LAPIC,
                         IOAPIC, LAPIC timer, TSC, PIT calibration
   drivers/acpi/         MADT (LAPIC/IOAPIC/CPU enumeration)
```

Dependencies point downward. The scheduler depends on memory (stacks
from `vm_kernel_alloc`, objects from slab caches) and on the timer for
slices and sleeps; the timer depends on the interrupt subsystem; nothing
below depends on the scheduler. Invariant 3 (scheduler independent of
filesystems and networking) holds by construction: neither exists yet
and the scheduler's includes are limited to memory, timer, interrupt,
and arch headers.

## 3. Mechanism versus policy

Mechanism (`kernel/scheduler/sched.c`, `thread.c`, arch context switch):
enqueue, dequeue, pick the next thread through the policy, switch, wake,
block, per-CPU run queues, preemption requests, migration at creation.

Policy (`kernel/scheduler/policy_rr.c`, behind `struct sched_policy`):
which ready thread runs next, how long, what a tick does. The first
policy is fixed priority with round-robin inside a priority and a fixed
time slice. Fairness, deadlines, and interactivity are later policies
behind the same interface.

## 4. Responsibilities

**Threads** (`thread.c`)
- `struct thread`: identity, state, arch context, stack (with guard
  pages), priority, affinity, slice accounting, wait linkage, exit
  status, reference count.
- Creation of kernel threads with an entry function and argument; exit;
  join; detached lifetime.
- The boot context becomes thread 0 (`kmain`), so `schedule()` is legal
  from the moment the scheduler is initialised.

**Scheduler** (`sched.c`)
- One run queue per CPU with a priority bitmap and per-priority lists.
- `schedule()`: the single switch point; `sched_yield()`; wake and block
  with correct state transitions under the run-queue lock.
- Preemption: `need_resched` per CPU set by the tick or by a wake of a
  higher-priority thread; honoured at interrupt return when the
  interrupted context is preemptible (preempt count zero, interrupts
  were enabled), and at `preempt_enable()`.
- Idle thread per CPU: halts with interrupts enabled until work arrives.
- CPU selection at creation: the least-loaded CPU in the affinity mask.
  No periodic rebalancing yet.

**Synchronisation** (`wait.c`, `mutex.c`, `semaphore.c`, `completion.c`)
- Wait queues with a lost-wakeup-free `wait_event()` protocol.
- Mutex (sleeping lock, owner tracked, not recursive), counting
  semaphore, completion (one-shot broadcast), all built on wait queues.
- Timed sleep through the timer subsystem.

**Per-CPU data** (`kernel/include/kernel/percpu.h`, arch GS base)
- `struct percpu`: CPU index, current thread, preempt count,
  need-resched flag, interrupt nesting depth, run queue, idle thread,
  arch fields (LAPIC id, TSS). Reached via the GS segment base on x86-64.

## 5. Non-responsibilities (later)

- User threads, signals, process ownership of threads (Phase 4).
- Load balancing across CPUs after creation; CPU hotplug.
- Real-time and deadline policies, priority inheritance.
- Reader-writer locks, futexes, RCU (Phase 3 SMP follow-up and later).
- Thread-local storage (the field exists; nothing uses it).

## 6. Interfaces (contracts in api.md)

| Header | Provides |
|---|---|
| `kernel/thread.h` | `struct thread`, `thread_create`, `thread_exit`, `thread_join`, `thread_current`, refs |
| `kernel/sched.h` | `sched_init`, `schedule`, `sched_yield`, `sched_wake`, `sched_block_current`, preempt control |
| `kernel/wait.h` | `struct waitqueue`, `wait_event`, `waitqueue_wake_one/all`, `thread_sleep_ns` |
| `kernel/mutex.h`, `semaphore.h`, `completion.h` | sleeping primitives |
| `kernel/percpu.h` | `this_cpu()`, `cpu_count()`, `cpu_online()` |
| `arch/context.h` | `arch_context_init`, `arch_context_switch` |
| `arch/percpu.h` | `arch_percpu_setup`, `arch_percpu_get` |

## 7. Data structures (detail in design.md)

`struct thread`, `struct runqueue` (per CPU: lock, bitmap, 64 lists,
counts, current, idle), `struct sched_policy` (function table),
`struct waitqueue` (lock + list of `struct wait_entry`), `struct mutex`,
`struct semaphore`, `struct completion`, `struct percpu`.

## 8. Concurrency model

Locks, outermost first:

```text
waitqueue.lock → runqueue.lock (one CPU's)
mutex/semaphore/completion internal lock → waitqueue.lock
runqueue.lock is a leaf: nothing is taken under it.
```

Every lock is a spinlock taken with interrupts disabled; run-queue and
wait-queue operations happen from interrupt context (tick, wake from a
timer callback). `schedule()` takes the local run-queue lock, switches
with it held, and the resumed thread releases it (a new thread's
trampoline releases it on its first run). Preemption is impossible while
any spinlock is held because `spin_lock` raises the per-CPU preempt
count.

Thread lifetime is reference counted. A thread cannot free its own stack
while running on it; the CPU that switches away from an exited thread
drops the exited thread's self-reference after the switch, outside the
run-queue lock.

## 9. Memory ownership

- A thread owns its stack (`vm_kernel_alloc` with guards) and its
  `struct thread` (slab) until the last reference drops.
- Run queues own no threads; they hold READY threads by intrusive link.
- Wait entries live on the waiting thread's stack; the waker only
  touches them under the wait-queue lock while they are enqueued.

## 10. Error handling

Creation returns NULL on allocation failure. Misuse (scheduling with a
spinlock held, blocking in interrupt context, unlocking a mutex one does
not own, double join) panics with a precise message: these are bugs, not
conditions to recover from.

## 11. Performance considerations

O(1) pick via the priority bitmap; O(1) enqueue/dequeue; context switch
saves only callee-saved registers. Tick work is bounded (slice
accounting, timer queue head check). Nothing is measured yet.

## 12. Security considerations

Kernel threads only; no user input reaches this subsystem yet. Stacks
have guard pages; a stack overflow reaches the double-fault IST stack and
produces a report instead of silent corruption. Preempt and interrupt
state are asserted at every blocking entry point.

## 13. Testing strategy

Kernel self-tests for creation/exit/join, yield ordering, preemption of a
spinning thread, timed sleep bounds, wait-queue wake, mutex exclusion,
semaphore counting, completion, timers. SMP variants of each in the SMP
PR. Host tests for the run-queue bitmap logic and the wait-queue
protocol where they separate cleanly. See `testing.md`.

## 14. Future extensibility

Policies plug in via `struct sched_policy`. Per-CPU run queues and
affinity masks are the base for load balancing. The thread object gains
a process pointer, signal state, and user context in Phase 4 without
changing the scheduler. AArch64 implements `arch/context.h` (x19–x29,
sp, lr) and `arch/percpu.h` (TPIDR_EL1).
