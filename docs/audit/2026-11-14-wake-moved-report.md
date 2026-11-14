# A wake whose thread moved under it, 2026-11-14

Main was red on the merge of the v1.1.1 notes (`24421af8`, documentation only; run 43523266637). One of its two
failed jobs was a kernel panic in the scheduler, which this PR fixes; the other was a self-test over its time bound
on a slow runner, recorded in `docs/testing/flakes.md` and not changed. A red merge run takes priority (AGENTS.md),
and the release candidate for v1.1.1 waits for this.

## Scope and baseline

Branch `wake-cpu-recheck` from main at `24421af8`. In scope: `sched_wake` (`kernel/scheduler/sched.c`), its
self-test and seam, the scheduler's documents. Not in scope: `cosmofs-replay`'s bound.

## The failure

Job `upgrade test (aarch64)`: `test-be: FAIL: be-crash-set: QEMU exited (code 35) waiting for the first prompt`.
The boot's log ends 9.0 s in, after `init: rc exited with status 0`, `pid 21 'sh' created` and `pid 22 'cat'
exited`:

```text
KERNEL PANIC (concurrent) on CPU 0: assertion failed: !list_empty(&t->rq_link) at /cosmo/kernel/scheduler/policy_rr.c:80 (rr_dequeue)
KERNEL PANIC: assertion failed: rq->nr_running > 0 at /cosmo/kernel/scheduler/policy_rr.c:84 (rr_dequeue)
CPU: 1  context: thread  thread: 38 'sh'  irq_depth: 0  preempt_count: 1
```

CPU 1's stack, against the run's release kernel: `el0_sync`, `handle_sync`, `syscall_dispatch`,
`syscall_handle_read`, `ceeF_objRead`, `ceeF_ringRead`, `wait_floor_until_killable`, `sched_set_running_current`.
The shell was reading the pipe of `$(cat /etc/console-shell)`, the first line of `/sbin/cosmo-console`.

## The mechanism

`sched_wake(t)` read `t->cpu`, locked that CPU's run queue, and queued the thread there if it was BLOCKED. It did
not read `t->cpu` again under the lock; `sched_reprioritize`, `sched_migrate` and `thread_set_affinity` do.

A pipe's reader has two wakers on two paths: the writer's close, through the pipe's wait queue, and its child's
exit, through signal delivery (the wait is killable). With the thread blocked after running on CPU A:

1. Waker 2 reads `t->cpu`: A.
2. Waker 1 wakes the thread: READY on A's queue.
3. The thread is moved to CPU B, runs there, finds its wait not over and arms it again: BLOCKED, and still B's
   current thread.
4. Waker 2 locks A's queue, sees BLOCKED, and queues the thread on A.
5. On B the thread ends its wait: `sched_set_running_current` locks B's queue and takes the thread off "its" queue,
   which is A's: B's count of ready threads is zero (`rq->nr_running > 0`). CPU A meanwhile picks the thread from
   its queue and finds it unlinked (`!list_empty(&t->rq_link)`).

The code dates from the first SMP scheduler. The console's session (#521) runs a command substitution at every
installed boot, which is the first sighting's pipeline.

## What changed

- **`sched_wake` reads `t->cpu` again under the queue's lock** and takes the lock again if it changed (S36). Under
  the lock of the queue `t->cpu` names the CPU cannot change: a migration holds that lock, and a pick writes the CPU
  of the queue the thread is on.
- **Self-test `sched-wake-moved`**, in CeeSafe (`kernel/scheduler/waketest.cee`): the five steps above, with the
  second waker held at a seam in `sched_wake` (`sched_test_wake_point`, `CONFIG_SELFTEST`) between its read and its
  lock, and the thread held on the other CPU, armed, with interrupts off, until the test has looked where it is
  queued. Two CPUs; skipped on one.
- **`tools/wake-moved-probe.py`**: both ways, at two CPUs.
- **No C added.** The seam's state is in the CeeSafe file; `sched_migrate_result_name`, which only tests call, moved
  there from `sched.c`. `scripts/cee-share.py --gate origin/main`: no tree has more C to convert than origin/main
  (`kernel/`'s C files: 20 lines added, 20 removed).
- **Documents**: scheduler invariants S36, testing, API, design; `docs/testing/flakes.md` (the panic, and
  `cosmofs-replay`'s bound in the same run).

A sweep of every place that takes a run queue from a thread's CPU (`sched.c`, `thread.c`): `sched_wake` was the
only one that did not read it again.

## Proof

`tools/wake-moved-probe.py`, on the developer's Mac, two CPUs:

| | `--old` (the fix reversed) | fixed |
|---|---|---|
| x86-64 | `PROBE: PASS`: `a wake that had read CPU 0 queued the thread on CPU 0 while it ran on CPU 1`; `sched-wake-moved ... FAIL: a wake queued a thread on the CPU it had left (S36)`; 1 of 528 failed, the boot otherwise whole | `PROBE: PASS`: `queued it on CPU 1, where it ran` |
| AArch64 | `PROBE: PASS`: the same lines | `PROBE: PASS`: the same line |

These four are the final commits' (2026-11-14, the Mac's one-minute load average 1.1 to 5.4 at their starts). An
earlier set on the same code but for one check of the self-test (below) gave the same four verdicts, after two of
its boots were run again: the first x86-64 fixed run and the first AArch64 old run started at load averages of 43
and 22 (other work on the machine), `cosmofs-replay` took 72.7 s and 42.9 s against 15.9 to 27.6 s, and the
watchdog fired; in both the self-test itself gave the line of its mode.

## Found before the PR

| Finding | Fix |
|---|---|
| The self-test required that its own `sched_migrate` had moved the thread; in the first matrix's two-CPU x86-64 boot the other CPU's balancer moved it first (`sched-wake-moved ... FAIL: the thread was not moved`) | who moves it does not matter: the test requires that the thread armed its next wait on the other CPU, and reports how many moves were its own |
| The first version was C: 189 lines more in `kernel/`, which CI's `cee-share` refuses | the test and the seam's state are CeeSafe; `sched_migrate_result_name` moved with them |
| The first version put the misqueued thread right with a helper in `sched.c` | the thread leaves its CPU as a ready thread does, and the test lets its own CPU run it once the other's switch is over: no helper |

## Validation

On the other machine (`docs/development.md`, "Running the matrix": the CI image, one container of 4 CPUs and 6
GiB), 2026-11-14, both architectures one after the other, on this PR's code. AArch64: `matrix: PASS`. x86-64:
`matrix: FAIL`: `test-chaos` and `test-harness-retry` out of time, in the slow mode of four-CPU x86-64 boots that
main has too (below).

| Step | x86-64 | AArch64 |
|---|---|---|
| debug, release, chaos and harness-retry builds, `make cee-check` | PASS | PASS |
| `make host-test`, `make fuzz` | PASS | PASS |
| `make analyze` | clean (15 baselined, 0 unexpected, 0 stale) | clean (15 baselined, 0 unexpected, 0 stale) |
| debug `make test`, `QEMU_SMP=1` | PASS in 235.5 s | PASS in 279.5 s |
| debug `make test`, `QEMU_SMP=2` | PASS in 239.1 s | PASS in 283.8 s |
| debug `make test`, `QEMU_SMP=4` | PASS in 252.6 s | PASS in 303.1 s |
| `make test-smp2` | PASS in 234.1 s | PASS in 283.0 s |
| `make test-chaos` | FAIL after 304.5 s (timed out); its image built again and booted twice: PASS in 249.1 s, PASS in 257.0 s | PASS in 304.9 s |
| `make test-harness-retry` | FAIL after 304.5 s (timed out); built again and booted twice: FAIL after 304.5 s (timed out), PASS in 254.6 s | PASS in 306.7 s |
| `make BUILD=release test` | PASS in 19.8 s | PASS in 24.1 s |

`sched-wake-moved` ran and passed in every debug boot of two or more CPUs, the timed-out ones included (`ok`, 5 to 7
ms); at one CPU it says so and is skipped.

**The timed-out boots.** Each is `timed out after 300s` with `self-test cosmofs-replay took 53 to 56 s (budget
40000 ms)` and the watchdog's line; no self-test failed. A four-CPU x86-64 debug boot on that machine runs in one
of two modes, `cosmofs-replay` at 31 to 36 s or at 53 to 56 s, and the slow one does not fit in 300 s. It is not
this PR's: main's own tree (`24421af8`) on the same machine, the same day, timed out in its four-CPU debug boot
(`cosmofs-replay` 53623 ms) and passed `test-chaos` and `test-harness-retry`; the same main images booted again
gave the four-CPU debug boot once in each mode. The machine was idle in the slow boots (a `vmstat` sample each
minute). The whole table is in `docs/testing/flakes.md` (2026-11-14); what the slow mode is, is not known (backlog
TEST-28). No budget was changed.

The first matrix of this PR, before the self-test's fix above, had the same two timeouts and that self-test's
failure at two CPUs; AArch64 passed.

Also: `python3 scripts/cee-share.py --gate origin/main` passes; the debug boot at two CPUs on the developer's Mac,
x86-64, PASS in 215.8 s.

## Limits

- Main's other failure in run 43523266637, `cosmofs-replay` over its bound in an x86-64 boot, is the slow mode and
  is not fixed here: main can go red on it again.
- The x86-64 matrix did not pass as a whole in any one run; each of its steps passed on this code.
- The self-test needs two CPUs and `CONFIG_SELFTEST`; a release boot does not run it.
- The sighting is one. The mechanism is from the code and the two assertions' stacks, and the self-test reproduces
  the wrong queue, not the panic.
