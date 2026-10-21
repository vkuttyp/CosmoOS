# sched-restore-loop one trip short: the seam spent a trap's resumption

Date: 2026-10-21. Base: `main` at `cceff596` (#425's merge), whose CI run
39505882310 failed `build, boot, analyze (x86_64)` at the two-CPU debug
boot: `SELFTEST: sched-restore-loop ... FAIL: check failed: runs ==
RESTORE_RUNS && trips >= (uint64_t)RESTORE_RUNS * RESTORE_RUN && longest
>= RESTORE_RUN`, `100 runs of 32, 3199 trips`. Main red on a merge whose
change does not touch the scheduler: the fifth sighting of this line
(`docs/testing/flakes.md`, "`sched-restore-loop`: one trip short"; four
before it, 2026-10-13 to 2026-10-20, both architectures, all 3199).

## Scope and baseline

`sched-restore-loop` (`kernel/scheduler/schedtest.c`) arms a seam,
`test_resume_resched`, that `schedule_internal` spends at each of the
test thread's next 32 resumptions -- leaving a reschedule pending and the
slice spent -- and then yields, so that the restore loop takes 32 trips
(S31). The seam ran at every resumption, whatever state `schedule()` was
about to restore.

## Mechanism

A tick between the test's arming and its `sched_yield()` preempts it from
the trap's return: `sched_preempt()` with interrupts off. Its pass resumes
the test thread, the seam spends one and sets the reschedule, and
`preempt_point_due(s)` is false -- interrupts are off in `s` -- so no trip
is taken there. The run then makes 31. The record kept at the failure
fits it exactly: 3199 trips, every armed resumption spent (`unspent` 0),
32 the longest. Forced -- `arch_irq_save(); sched_preempt();
arch_irq_restore()` between the arming and the yield of the first run --
and instrumented on x86-64: `left 31 need_resched 1 trips 0` after the
preemption, 31 trips in the run, `3199 trips` and the check failed.

A test defect: the seam spent a resumption that the loop it tests cannot
take. The scheduler's restore loop is not at fault.

## Change

- `kernel/scheduler/sched.c`: the seam spends only when
  `arch_irq_state_enabled(s)`, a resumption whose restore the loop can
  take.
- `kernel/scheduler/schedtest.c`: the first run is preempted that way
  every boot, and `trap_left` -- the count still armed, read before the
  restore with interrupts off -- must be all 32 (`CHECK(trap_left ==
  RESTORE_RUN)`); the measurement line prints it.
- `tools/sched-restore-trap-probe.py`: `--old` restores the seam that
  spends at every resumption.

The trips check alone does not judge the old seam deterministically: the
forced run on x86-64 once passed it with 3200, a stray reschedule
elsewhere in the run adding the missing trip (it asks for at least
3200). Hence the direct check, which the probe requires.

## Validation

`tools/sched-restore-trap-probe.py`, both architectures:

| | x86-64 | AArch64 |
|---|---|---|
| `--old` | 3199 trips, `31 of 32 armed left`, FAIL `trap_left == RESTORE_RUN at line 996` | 3199 trips, `31 of 32`, FAIL at line 996 |
| fixed | 3201 trips, `32 of 32`, ok | 3200 trips, `32 of 32`, ok |

The validation matrix on `93bfd56d`, every step PASS: x86-64 `host-test`
84 s, `fuzz` 63 s, `analyze` 239 s, debug boots at `QEMU_SMP=1/2/4` 450 /
154 / 156 s, `test-smp2` 164 s, `test-chaos` 202 s, `test-harness-retry`
200 s, release 53 s; AArch64 `host-test` 83 s, `analyze` 242 s, debug boots
399 / 170 / 161 s, `test-smp2` 167 s, `test-chaos` 195 s,
`test-harness-retry` 192 s, release 55 s.
