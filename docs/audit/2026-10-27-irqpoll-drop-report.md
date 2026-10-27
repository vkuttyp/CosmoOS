# irqpoll: the worker's drop and its raise as one step, 2026-10-27

`irqpoll-boost` failed eleven times between 2026-10-10 and 2026-10-27
(`docs/testing/flakes.md`): nine on CI (seven x86-64, two AArch64) and
two in local x86-64 boots, every one with `longest gap 290 ms (bound
100)`. A host stall leaves a gap of any length; one length every time is
a mechanism. This PR finds it and fixes it.

## Scope and baseline

Branch `irqpoll-lower-race` from main at `6779e0ff`. A test commit that
fails on main, then the fix. `tools/irqpoll-drop-probe.py --old` reverts
the fix in a throwaway worktree, keeping the test.

## The mechanism

`worker_main` (`kernel/core/irqpoll.c`) runs a backlog at the highest
priority for `IRQ_POLL_HOLD_NS` (10 ms), drops to the default, and arms
`boost`, a per-CPU timer that raises it again 10 ms later
(`docs/kernel/interrupt/design.md`, "Bounded completion handling"). The
drop came first:

```c
sched_reprioritize(self, SCHED_PRIO_DEFAULT);
w->lowered = true;
timer_start(&w->boost, IRQ_POLL_HOLD_NS);
```

- `sched_reprioritize` holds the run-queue lock with interrupts off. Its
  `spin_unlock_irqrestore` ends in `arch_irq_restore`, which calls
  `preempt_point`. A tick that arrived while the lock was held is taken
  there.
- That tick's `rr_tick` sees the worker at the default with its slice
  spent: the worker yields after every batch, and when picked again at
  once (`next == prev` in `schedule_pass`) it keeps the spent slice. With
  the test thread ready at the same level, `rr_tick` asks for a switch,
  and the worker is switched out before `timer_start`.
- In `irqpoll-boost` the test thread then starts the spinner, at the
  default less 4. Nothing raises the worker until the spinner ends: 300 ms
  less the 10 ms hold, 290 ms.

The debug build's lockdep makes the interrupts-off section long, so a
tick lands in it at a rate that shows up only over many boots.

## How it was found

An uncommitted build ran `irqpoll-boost` 1,000 times in one boot and
logged each drop and each `boost` firing, on QEMU TCG on the development
Mac, debug, both architectures booting at once (each loads the other).

| Kernel | AArch64 | x86-64 |
|---|---|---|
| main | 2 of 1,000 failed, both 290 ms | 0 of 1,000 |
| the fix | 0 of 2,000; longest gap 70 ms | 0 of 2,000; longest gap 55 ms |

- In both failures the only drop logged is at +300 ms, after the
  spinner, and `boost` never fired. The worker had not lowered itself at
  +10 ms: it was switched out partway through.
- In every passing run `boost` fired 10-14 ms after its drop, with the
  worker queued at the default and the spinner running, and raised it.
- At AArch64's rate on main, 2,000 clean runs by chance is about 2 %.
- The gaps of 50-70 ms that remain are `boost` firing late on a loaded
  host, and are under the bound.

## The test

`irqpoll-drop-preempt` runs `irqpoll-boost`'s scenario with the switch
made at the drop. A one-shot seam, `irq_poll_test_on_drop(cpu, fn)`
(`CONFIG_SELFTEST`; one load when unarmed), calls `fn` on that CPU's
worker between the drop and the timer. The test's `fn` sets
`need_resched` and calls `preempt_point`. It asserts no time bound: only
that the switch was made at the first drop, and that the poll ran while
the spinner held the CPU. On main the poll never ran in the spinner's
300 ms, on both architectures; with the fix it ran thousands of times.

## The fix

The drop, the `lowered` mark and `timer_start` run under
`preempt_disable`. The seam's `preempt_point` then cannot switch, and the
reschedule it asked for is taken at `preempt_enable`, after the timer is
armed. `boost_fn` already handles a worker that is running, ready or
queued when it fires.

## Validation (2026-10-27, macOS host)

| Check | x86_64 | aarch64 |
|---|---|---|
| `tools/irqpoll-drop-probe.py --old` | PASS: `irqpoll-drop-preempt` fails, `0 while it spun, longest gap 290 ms`; `irqpoll-boost` passes (14 ms) | PASS, the same (290 ms; `irqpoll-boost` 15 ms) |
| `tools/irqpoll-drop-probe.py` | PASS: 4,174 calls while it spun, longest gap 13 ms | PASS: 4,796 while it spun, 14 ms |
| debug `make test` (4 CPUs) | PASS (179.4 s): `irqpoll-drop-preempt` 3,875 calls while it spun, 15 ms; only lockdep's expected reports | PASS (201.7 s): 2,837 while it spun, 14 ms; only expected reports, and `class table at 499 of 512`, which #470's CI boots also logged (497-499) |
| `BUILD=release make test` | PASS (18.9 s) | PASS (22.1 s) |
| release `llvm-nm` | no self-test symbols; `irq_poll_test_on_drop` is the empty stub; `g_workers` 0x3000 bytes (debug 0x3200: the seam's field is `CONFIG_SELFTEST` only) | the same |
