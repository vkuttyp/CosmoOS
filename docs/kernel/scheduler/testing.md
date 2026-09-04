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
30 ms and checks the elapsed time is in `[30 ms, 200 ms)`, then sets the
flag and joins. Thread 0 can only wake if the spinner is preempted:
the timer callback marks it READY, and either the spinner's 10 ms slice
expires (`rr_tick` sets `need_resched` because an equal-priority thread
is ready) or the wake itself requests one. `s->switches >= 1` confirms
the spinner was switched in at least once. The 200 ms bound is
30 ms sleep + one slice + TCG slack.

### `sleep`
`thread_sleep_ms(20)` must take at least 20 ms and less than
20 ms + 3 ticks + 10 ms (12 + 10 ms of slack for the tick phase and
scheduling latency); `thread_sleep_ns(1 ms)` must not return early. The
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

## Running

```sh
make test                       # SELFTEST: thread ... waitqueue
make BUILD=release test
```

Look for `SELFTEST: PASS (19 tests)` in `out/x86_64-debug/boot-test.log`.
A failure prints the failing `CHECK` expression and its source line.
