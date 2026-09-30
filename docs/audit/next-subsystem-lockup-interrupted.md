# NEXT SUBSYSTEM — lockup-sample asserts a leaf PC on a spinner that can be interrupted

## Problem

`docs/testing/flakes.md` ("`lockup-sample`, and a failure that was not a
flake at all") records **three sightings**, all x86-64, all on commits
that change no kernel code (two documentation-only, the third a report
plus a probe):

```
SELFTEST: lockup-sample ... FAIL: check failed: in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND)
```

`lockup-sample` starts a spinner on another CPU **with interrupts
enabled**, samples every CPU, and asserts that the spinner's sampled
program counter lies inside `spin_here` (`lockuptest.c:187`). On x86-64
the sample is an NMI. An NMI can arrive while an ordinary interrupt is
being handled on the spinner's CPU, and then the sampled PC is in that
handler, not in `spin_here`.

The second sighting kept the symbolized trace, and it showed exactly
that: the sampled PC was in a trap tail (`lock_common` under
`quiesce_note_quiescent_preemptible`, reached from `x86_trap_dispatch`
and `isr.S`), with `spinner_main` at frame #6. The spinner had been
interrupted, and the leaf PC was the interrupt handler's. The assertion
has no allowance for it, and a spinner with interrupts enabled will be
interrupted.

`flakes.md` already named this as a known-brittle check with a repair in
hand: assert on the whole trace, not the leaf PC. This unit measures it
and makes the repair.

### Measured

`tools/lockup-interrupted-probe.py` classifies the sample and each trace
entry, and can force the interrupted case. `spin_here` records its own
return address into `spinner_main` (`__builtin_return_address(0)`); that
one address marks the spinner's frame exactly, without a coarse
function-size bound. The classes are **P** (the probe's parked handler),
**R** (that exact return address), **S** (`spin_here`), **?** (anything
else).

`--force` parks a cross call on the spinner's CPU (a helper on a third
CPU sends `smp_call_function_single`, whose handler spins bounded at
50 ms) and samples while it is parked.

| run | arch | sample | `lockup-sample` |
|---|---|---|---|
| plain | x86-64 | `pc S, trace SR?` (uninterrupted) | ok |
| plain | aarch64 | `pc S, trace SR?` (ordinary IRQ, no NMI) | ok |
| `--force` | x86-64 | `pc P, trace P???R?` — the NMI in the handler; `spin_here` absent, its return deeper | **FAIL** `in_fn(pc, spin_here)` (the sighting) |
| `--force --fix` | x86-64 | `pc P, trace P???R?` | ok |
| `--force` | aarch64 | `pc S`, **unanswered** | FAIL `m & CPUMASK_OF(k)` (a different failure) |
| `--force --fix` | aarch64 | as above, unanswered | FAIL `m & CPUMASK_OF(k)` |

Each row is one debug boot, run one at a time.

- **x86-64 forced reproduces the sighting exactly**: PC in the handler,
  `spin_here` not on the trace, and its return address (`R`) below the
  interrupt frames. The candidate accepts it.
- **aarch64 cannot land the sample inside a handler**, because it has no
  NMI: the sample is an ordinary interrupt, which the parked handler
  (interrupts masked) holds off, so the sample times out unanswered. The
  leaf-PC check is not what fails there, and the candidate does not
  change that outcome. The interrupted-leaf hazard is x86-64's alone,
  which is why every sighting is.

### Why it matters

- **A correct sample fails the test.** The check asserts the spinner's
  PC is in `spin_here`, but the object under test — the lockup sampler —
  faithfully reports a PC in whatever the CPU was running, and an NMI
  can catch it in an interrupt handler. The test conflates "the sample
  named the spinner's CPU and reached its stack" with "the leaf was in
  `spin_here`".
- **It is x86-64 CI's recurring flake.** Three sightings, each a re-run
  away from green, each costing a reader the question of whether the
  commit broke something.

## The implementation before this unit

| piece | where | what it asserts |
|---|---|---|
| the sample test | `lockuptest.c:187-189` | `pc` in `spin_here`; `depth >= 2`; `trace[1]` in `spinner_main` |
| the spinner | `lockuptest.c`, `spin_here` | sets `running`, then a bare loop |
| `spinner_main` | `lockuptest.c`, `spinner_main` | calls `spin_here`, then a store (so its frame stays) |

The other three `in_fn(pc, spin_here)` checks are **not** subject to
this hazard and are left alone:
- `lockup-sample-irqoff` (`:364`) and `lockup-sample` hard variant
  (`:647`) spin with **interrupts masked**, so no ordinary interrupt can
  preempt the spinner; the NMI lands in `spin_here`.
- `lockup-sample-busy` (`:583`) reads `soft_pc`, sampled from the
  **tick** on the stalled CPU — the timer interrupt records the PC it
  interrupted, which is the spinner's.

## Design

### 1. `spin_here` records its own return address

`spin_here` writes `__builtin_return_address(0)` into the spinner struct
before its loop. That is the single address in `spinner_main` that the
call returns to — the spinner's frame, identified exactly, with no
reliance on a function-size bound.

### 2. The check accepts the spinner, in its loop or interrupted

The sample names the spinner if either:
- **uninterrupted:** `pc` is in `spin_here` and `trace[1]` is that exact
  return address; or
- **interrupted:** `pc` is elsewhere and that return address appears
  anywhere below the leaf in the trace.

Both say the same thing — the spinner's frame is on the sampled stack —
and neither cares what the leaf is. A sample that named the wrong CPU,
or reached a stack without the spinner's frame, still fails.

The `MAIN_FN_BOUND` bound on `trace[1]` goes: the exact return address
replaces it. `SPIN_FN_BOUND` stays, for the uninterrupted leaf and for
the three masked/tick checks that keep their present form.

## Affected files

| file | change |
|---|---|
| `kernel/core/lockuptest.c` | `spin_here` records its return; `lockup-sample`'s check accepts the interrupted spinner |
| `docs/kernel/diagnostics/testing.md` | the check names the spinner's frame, not the leaf |
| `docs/testing/flakes.md` | the sighting explained and fixed |
| `README.md` | Status entry |

## APIs

None. The spinner struct gains one field, private to the test.

## Tests

| test | proves |
|---|---|
| `lockup-sample` | the sample names the spinner whether or not an interrupt was in flight; the spinner's frame is on the sampled stack |

**Planned mutations** (each alone, both architectures, boot confirmed):
- the check back to `in_fn(pc, spin_here)` alone, with the probe's
  `--force`: fails on x86-64 as the sighting did (the mutation and the
  forcing are the probe's two halves).
- the sample recording a stack without the spinner's frame (a wrong CPU,
  or a truncated trace): the interrupted branch finds no return address
  and fails.
- `spin_here` not recording its return: the check has no address to look
  for; the uninterrupted branch still passes on the leaf, so this must
  fail on the forced interrupted case, where the return address is the
  only evidence.

## Benchmarks

None.

## Risks

- **The interrupted branch scans the trace for one address.** A trace
  that happened to contain that address for another reason would pass a
  bad sample. The address is a specific return site inside a test-only
  function that only the spinner calls, so nothing else on any stack
  returns there.
- **The masked and tick checks keep the leaf-PC form.** They are correct
  there (no preemption, or a tick-sampled PC), and widening them would
  lose the precision they legitimately have. This is recorded so a
  future reader does not "fix" them to match.

## Alternatives considered

- **Sample only while the spinner has interrupts masked.** That removes
  the hazard by testing a different thing: the point of `lockup-sample`
  is that the sampler works against a *running* CPU, interrupts and all.
- **Widen `SPIN_FN_BOUND` to cover the interrupt path.** The interrupt
  handler is nowhere near `spin_here` in the image, and its address is
  not bounded relative to it; there is no bound that includes it and
  excludes a genuinely wrong sample.
- **Retry the sample until the leaf is in `spin_here`.** A retry that
  hides the interrupted case is the flake in a different hat, and it
  asserts less: that the sampler *can* catch the leaf, not that it names
  the spinner every time.
