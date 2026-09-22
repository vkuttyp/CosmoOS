# Scheduler and Threads: Testing

## Self-tests (`kernel/scheduler/schedtest.c`)

Run from thread 0 in every `CONFIG_SELFTEST=1` boot, after
`sched_init` and `arch_irq_enable`, with the tick at 250 Hz. Every test
records `thread_count()` first and requires it back at the end, so a
leaked `struct thread` fails the test that leaked it. Timing bounds are
loose on purpose: QEMU TCG on a laptop is not a real-time platform.

### `thread`
| Step | Proves |
|---|---|
| `thread_create(basic_entry)` then `thread_join == 7` | first-run path: `arch_context_init` frame → `x86_context_start` → `thread_trampoline` → entry; `thread_exit(7)` → completion → join |
| `st.ran == 1` | the entry ran on its own stack |
| thread whose entry returns → `thread_join == 0` | return from entry is `thread_exit(0)` |
| `thread_count()` unchanged after each join | S13: both references released, struct and stack freed |
| `thread_current()->flags & THREAD_FLAG_BOOT`, state RUNNING | thread 0 adoption in `sched_init` |
| `sched_switch_count(cpu) >= 2` | real context switches happened |

### `yield`
Two threads each loop 200 times incrementing a counter and calling
`sched_yield()`. Both reach 200: same-priority FIFO rotation works and
`sched_yield` forfeits the slice (S8, tail re-queue).

### `preempt`
A thread spins on an atomic flag without ever yielding. Thread 0 sleeps
30 ms and checks it slept at least that, then sets the flag and joins. Thread 0 can only wake if the spinner is preempted:
the timer callback marks it READY, and either the spinner's 10 ms slice
expires (`rr_tick` sets `need_resched` because an equal-priority thread
is ready) or the wake itself requests one. `s->switches >= 1` confirms
the spinner was switched in at least once. There is no upper bound on
the sleep: a spinner that is never preempted does not return late, it
never returns, and the self-test watchdog reports that with a scheduler
dump and every CPU's program counter (`docs/kernel/diagnostics/`). A `< 200 ms` used to sit here; nothing a kernel does wrong lands
between "one slice late" and "never", so it could fail only on a loaded
host (`docs/testing/flakes.md`).

### `preempt-wake`, `preempt-wake-direct`, `preempt-wake-locked`
A same-CPU wake of a higher-priority thread runs **before the waker's
next statement** (the wake-preempt unit). A priority-16 waiter pinned to
CPU 0 blocks; thread 0, on CPU 0, wakes it and as its very next
statement stores `after = 1`; the waiter's first statement on waking
reads `after` into `saw`, which starts at 2. `saw == 0` is the claim.
The three differ only in the wake: a semaphore post (the wait-queue
shape), a direct `sched_wake` on a thread parked as a futex waiter
(`wait_event` on a private queue, the futex's, `poll`'s and signal
delivery's shape), and a post made inside a bare `arch_irq_save`/
`arch_irq_restore` region, where the post's own unlock restores
interrupts to *off* and the caller's restore is the first enable -- the
case that puts the point in the restore rather than in
`spin_unlock_irqrestore`. Each posts only once the waiter reads
`THREAD_BLOCKED`, so a waiter that never blocked cannot pass. Proved:
with the point removed all three read 1 (the waiter ran at the tick);
with the point placed before the enable, the same; with the point in
`spin_unlock_irqrestore` only, exactly `-locked` fails. Measured: the
waiter runs 20-77 µs after the wake, both architectures.

### `irqrestore-bench`
A million `arch_irq_save`/`arch_irq_restore` pairs with `need_resched`
clear: the cost of the point's predicate on the hot path, printed with
the CPU's count of restore-point preemptions so far.

### `debug.preempt_probe` (user mode, `init --selftest`)
The read of this sysctl is the system call under test: it creates a
priority-16 thread pinned to the caller's CPU, waits for it to block,
posts, stores `after = 1` as its next statement and returns `saw=N`;
init asserts `saw=0`, so the wake made inside the call ran before the
call returned. Debug builds only (a release kernel answers `ENOENT`,
which init also asserts); privileged; no wake path gains a hook.

### `sleep`
`thread_sleep_ms(20)` must take at least 20 ms and less than
20 ms + 3 ticks + 100 ms; `thread_sleep_ns(1 ms)` must not return early.
The upper bound is genuinely temporal -- it says the wake is the first
tick past the deadline and not a coarser mechanism -- and nothing
observable replaces it, so the test is on the load-sensitive list
(`docs/testing/flakes.md`): the slack was 10 ms and failed on a correct
kernel when the host held the vCPU (2026-09-13). The
lower bounds are exact because `timer_start` computes
`expires = now + delay` and `run_expired` fires only when
`expires <= now`.

### `mutex`
`mutex_trylock` twice (second fails), `mutex_is_locked`, unlock. Then
four threads each perform 100 lock / read / `sched_yield` / write /
unlock cycles with an `inside` counter checked under the lock. The yield
inside the critical section forces every other worker to block on the
wait queue, so this exercises `mutex_lock`'s `wait_event` loop under
contention. `counter == 400` and `violated == false` prove mutual
exclusion; the lock is free afterwards.

### `semaphore`
Consumer thread does five `semaphore_down` on a semaphore initialised
to 0; thread 0 verifies nothing was consumed after 5 ms, then does five
`semaphore_up` 2 ms apart. `consumed == 5` and `semaphore_count == 0`.
Then two consumers and ten posts with no sleep between them: every post
must reach a distinct blocked waiter even though the first woken one is
still linked. The joins are the check -- a post that wakes the
already-woken consumer again leaves the other blocked for ever, which
the watchdog reports -- then `consumed == 10` and a count of 0. A
`< 500 ms` on the joins used to follow and named nothing the joins do
not.

### `completion`
A thread sleeps 10 ms then `complete`s. `wait_for_completion` returns
after at least 10 ms, `completion_done` is true, and a second wait
returns immediately.

### `waitqueue`
Two threads `wait_event` on `go != 0`. After 5 ms the queue is non-empty
and nobody has woken. `waitqueue_wake_all` with `go` still 0 returns 2
and both re-block (S10, Mesa re-check). With `go = 1`,
`waitqueue_wake_one` returns 1 and exactly one thread proceeds
(`woke == 1` after 5 ms); `waitqueue_wake_all` releases the other; both
join; the queue is empty.

## Bugs the tests caught during bring-up

1. **GS base reset (S16).** `percpu_init_boot` originally ran before
   `gdt_init`; the GDT reload zeroed the GS base, so `this_cpu()`
   dereferenced address 0. The loader's identity map made that a silent
   write into physical page 0; after `vmm_init` switched page tables the
   next `spin_lock` faulted, the fault handler faulted again on
   `this_cpu()`, and the machine triple-faulted with no output. The
   symptom was a boot log ending at `vmm: kernel page tables active`.
2. **Wait-entry double push (S9).** Under the `mutex` test a worker woken
   with the owner still set re-entered `waitqueue_prepare`, which pushed
   its already-linked entry a second time. The list became cyclic with a
   stale node; `waitqueue_wake_one` handed `sched_wake` a garbage thread
   pointer, which faulted in `spin_lock_irqsave` on `g_rqs[t->cpu]`. The
   symbolised backtrace pointed straight at `wake → sched_wake`.

## Measured results (2026-09-04, QEMU TCG, `-m 256M`)

- 19/19 self-tests pass in debug and release builds; the debug boot
  including all tests takes about 2.8 s.
- Calibration: TSC 996–1060 MHz, LAPIC timer 62–67 MHz after the
  divide-by-16, over a 10 ms PIT window.
- `make host-test`, `make analyze`, `make reproducible`, and
  `make test-crash` unchanged and passing.

## Gaps and planned tests

- Single CPU only. SMP variants (threads pinned per CPU, cross-CPU wake
  with IPI, contended mutex across CPUs, migration at creation) arrive
  with the SMP PR.
- No priority-inheritance test because there is no priority inheritance.
- No host tests yet for the run-queue bitmap logic or the wait-queue
  protocol; both separate cleanly (`policy_rr.c` and `wait.c` depend on
  little) and are the next host-test candidates.
- Timing bounds are loose under TCG; a KVM/HVF run
  (`QEMU_ACCEL=kvm`) would allow tighter ones.
- Stack overflow into the guard page is not exercised; it would need a
  crash-test variant (`CRASH_TEST=2`) that recurses on a thread stack.
- Stress: thousands of short-lived threads, thread creation from inside
  threads, and randomised sleep/wake interleavings.

## Migration (`kernel/scheduler/smptest.c`)

| test | asserts |
|---|---|
| `percpu-claim` | with the expectation armed, an unpinned preemptible `arch_cpu_id()` and `this_cpu()` are each reported once; the same reads under `preempt_disable`, under `arch_irq_save`, under a self-pin, and from a thread created on one CPU are not, and the test names which quiet form spoke if one does |
| `lockdep-rq-order` | after a migration attempt from 0 to 1 recorded `runqueue0 -> runqueue1`, holding 1 and asking the checker about 0 (`spin_lock_check_order`, never taking it) is reported as an inversion (S24 is a checked order). It asks rather than takes because under the chaos migrator a tick on another CPU holds the pair in the right order at the same moment, and the test's spin for the second lock was that deadlock |
| `sched-migrate` | a worker held READY on CPU A behind a higher-priority pinned spinner, its mask widened to {A, B}, is moved to B by `sched_migrate` and its *first* recorded CPU is B; the migration count rose. The test thread pins itself, so A and B are two other CPUs (three online needed, else skipped) |
| `sched-migrate-refuses` | each refusal by name: a worker pinned to A -> `affinity`; to A itself -> `same-cpu`; to `cpu_count()` -> `offline`; the running spinner -> `not-ready`; a thread that has run `waitqueue_prepare` (BLOCKED, still running) -> `not-ready`; the same thread woken (READY, queued, still `rq->current`, held there by `preempt_disable`) -> `current`; a worker that has run on A and is then displaced by a higher-priority spinner pinned to A (READY, queued, not current) -> `preempted` |
| `sched-migrate-stress` | 200 ms: eight spinners, eight 1 ms sleepers, four completion ping-pong pairs, two mutex contenders, and a migrator calling `sched_migrate_from` on random CPU pairs; every worker progressed, none ever found itself outside its mask (checked under `preempt_disable` each round), at least 100 migrations happened (about 3,500 on four CPUs) |

Every test that reads "my CPU" and keeps it across a sleep -- the
preempt-wake trio, the lockup tests, the NMI entry test -- runs pinned
(`thread_pin_self` in a wrapper), and every worker that records the CPU
it ran on reads it under `preempt_disable`; the rest read raw with a
reason. A test that switches address spaces from thread context (the
ASID tests, `uaccess-guard`) reads the CPU's current space and switches
with interrupts off, as the switch path does.

**The suite under migration: `make test-chaos`.** A `SCHED_CHAOS=1`
debug image whose tick moves one ready thread to another CPU every
fourth tick, on every CPU, booted through the whole suite and the
user-mode sections; the boot test requires the tally line
`sched: chaos migrated N threads from the tick` with `N > 0`. CI runs
it on both architectures. Thread 0, the self-test thread, is not
pinned: it moves too.

## Waits and time bounds

A test waits for the property, never for an interval: a bounded deadline
loop on an observable (`threads_settle` here, `wait_until` in
`nettest.c`) whose expiry is `CHECK`ed. An upper bound on elapsed time is
one of three things, decided at the site and written there: the property
itself, or a guard so generous only a broken host fails it (kept); a
proxy for something observable (restated as that); or genuinely temporal
with nothing to replace it (widened, labelled `LOAD-SENSITIVE`, and
listed in `docs/testing/flakes.md`, which the boot harness reads to name
a failing test against). `sleep` is this suite's one listed test.

## Running

```sh
make test                       # SELFTEST: thread ... waitqueue
make BUILD=release test
```

Look for `SELFTEST: PASS (19 tests)` in `out/x86_64-debug/boot-test.log`.
A failure prints the failing `CHECK` expression and its source line.
