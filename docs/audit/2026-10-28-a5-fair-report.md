# A5, third PR: a weighted fair share at the default level, in Nova, 2026-10-28

Roadmap 1.2 A5 absorbs LX-13. The second PR (#476) gave processes a nice
value with Linux's rules. This PR makes the scheduler weigh it, as the
owner chose on 2026-10-26: a weighted fair share at the default priority
level by Linux's weight table (nice 0 = 1024), the other 63 levels
unchanged.

## Scope and baseline

Branch `a5-fair` from main at `1d98fc72` (#476).

## Audit (main at `1d98fc72`, read, not copied)

| Item | Where | What it means |
|---|---|---|
| 64 levels, FIFO round-robin with a 10 ms slice at each | `policy_rr.c` | the policy |
| Every user thread runs at `SCHED_PRIO_DEFAULT` (32), as do most kernel workers | `process.c` `thread_prepare` calls; drivers, network | the level to weigh |
| Run time is exact, charged at switch-out | `schedule_pass` (`run_time_ns`) | the measure to weigh |
| The tick calls the policy under `rq->lock`, a leaf (S2) | `sched_tick` | no process lock there: the weight must be read without it |
| A preempted thread is queued at the head, a yielding one at the tail (`slice_left_ns` 0) | `schedule_pass`, `sched_yield` | a yield must still go behind its equals: 82 call sites spin with it |
| Migration takes the tail of the lowest level (`pick_migratable`) and moves under both locks | `migrate_locked` | a vruntime is relative to its queue |
| Priority inheritance moves a ready thread between levels | `sched_reprioritize` | a thread can leave and rejoin the level |
| Linux's `sched_setaffinity` is accepted and ignored | `compat/linux/syscalls.c` | no guest test can pin two processes to one CPU: the kernel tests do |

## What changed

- **The arithmetic in Nova** (`ef8e41f6`; `kernel/scheduler/fair.nova`,
  101 lines, generated `fair.c`; API `kernel/include/kernel/fair.h`):
  Linux's weight table; the weighted charge, exact (q·1024 + ⌊r·1024/w⌋)
  and saturating; placement; rebasing between queues; the order; the
  preemption rule; the queue's minimum. It runs under a run-queue lock,
  in the tick too. The generated checks (an index, a divide and a
  remainder by the weight, one multiply) cannot fail: the index is
  clamped, the weight is at least 1, and `r·1024 < 2^42`.
- **The level, C** (`policy_rr.c`, `sched.c`): the list at
  `SCHED_PRIO_DEFAULT` in vruntime order; two policy hooks, `charge` (at
  the tick and the switch-out, from the later of the switch-in and the
  last charge) and `moved` (migration); `thread.yield_pending`, so a
  yield queues behind every equal; the tick's rule at that level (a better
  level waiting, or the running thread a granularity ahead of the
  leftmost). The design is `docs/kernel/scheduler/design.md`, "The fair
  level"; the invariant is S35.
- **The weight's source.** `thread.nice`, a copy of the process's value,
  set when a thread joins its process and whenever `setpriority` changes
  the process's (under the process's lock), read by the scheduler
  atomically without it. Kernel threads stay at 0.

Choices, with the reason for each:

- **The granularity is a tick (4 ms).** With a slice-sized granularity,
  two equal threads alternated every 24 ms (twice it and a tick: the
  host simulation showed it). At 4 ms they take 12 to 16 ms turns, about
  the 10 ms slice they had.
- **The wake bonus is 5 ms.** Half a slice, as Linux's `place_entity`
  gives half its latency.
- **No wake preemption at the level.** A wake still preempts only from a
  better level, as before. The tick bounds the wait.

Not done: the value is per process, not per thread (Linux's is per
thread); the balancer counts threads, not weights, so a nice 19 thread
counts as much as a nice 0 one when CPUs are balanced; no groups.

## Tests

Written first (`4a3eeb3c`) and run against the unchanged policy
(2026-10-28, x86-64 debug): `sched-fair` failed, nice 0 and nice 5
sharing 306 and 297 ms. No other test failed.

| Test | Where | Checks |
|---|---|---|
| `sched-fair` (kernel, both arches) | `kernel/scheduler/schedtest.c` | two spinners pinned to one CPU at the default level: equal values within 2:3; 0 against 5 and -5 against 0 between 2.0 and 4.5 to 1; 19 runs, at most a fifteenth of 0's |
| `process-nice-threads` (kernel, both arches) | `kernel/process/proctest.c` | a new process's thread starts at the process's value; `process_nice_set` reaches it; a refused change does not |
| `test_fair` (host) | `tests/host/test_fair.c` | the weights against Linux's table; the charge against 128-bit arithmetic (a grid and 200,000 random cases); placement, rebasing, order, preemption, minimum; one CPU simulated for 400 s in six nice mixes, shares within 2 % of the weights; two equal threads' turns at most 16 ms; a sleeper's lead bounded |

Two failures on the way:

- **In the test.** The first `process-nice-threads` held a reference on
  the probe's thread, which kept the process from finishing its exit,
  and the boot hung in `process_wait_exit` until the watchdog fired. It
  now reads the threads under the process's lock.
- **A slowdown, in the code** (fixed in `5cc0a639`). aarch64 debug boots
  ran their self-tests in 168-180 s against main's 143-154 s, back to
  back in both orders; x86-64 did not slow. An experiment build with the
  fair order turned off was as slow (170 s), and the failing-first test
  commit alone was not (153 s): the cost was in the new code paths, not
  the policy. `sched_yield` marked the thread with a `flags` bit under
  interrupts off, since the preemption path writes `flags` too, and
  kernel spin-waits yield in loops: under emulation an interrupt restore
  is costly (`irqrestore-bench` exists for it). The mark is now a field
  only the thread writes, `yield_pending`, with no masking, and the
  aarch64 self-tests take 155 s, the new ones included. That the
  restore was the cost is inferred; that this one change removed the
  slowdown is measured.

### Measured shares (`sched-fair`, debug, 2026-10-28, before the yield fix; the final run's agree within 3 ms)

| Pair (ms each) | x86_64 | aarch64 | Linux's weights |
|---|---|---|---|
| 0 and 0 | 203 / 199 | 198 / 204 | 1 |
| 0 and 5 | 454 / 149 (3.05) | 453 / 149 (3.04) | 3.06 |
| -5 and 0 | 449 / 153 (2.93) | 447 / 153 (2.92) | 3.05 |
| 0 and 19 | 793 / 11 (72) | 791 / 12 (66) | 68 |

## Validation (2026-10-28, macOS host, this branch's code at `5cc0a639`)

| Check | x86_64 | aarch64 |
|---|---|---|
| debug `make test` (lockdep) | PASS (201.2 s): 522 self-tests, `sched-fair` and `process-nice-threads` ok, `LINUXTEST: PASS`, `SHTEST: PASS`; no lockdep report beyond the self-test's expected ones; no stall warning | PASS (203.1 s), the same |
| `make test-busybox` | PASS (439 s) | PASS (464 s) |
| `BUILD=release make test` | PASS (19.2 s) | PASS (21.7 s) |
| `make analyze` | clean: 7 baselined, 0 unexpected | clean, the same |
| `make host-test` (`test_fair` included) | PASS | (host) |
| `make nova-check` with novac 0.1.1 | match (6 files) | |

Self-test time against main, debug, run back to back: x86-64 150.1 s
(main 147.9 s), aarch64 155.4 s (main 143.1 and 153.8 s in two runs);
the branch's two new tests take about 3 s.

## Backlog

LX-13 is done. What is left of it is recorded there: per-thread values,
and a balancer that weighs.
