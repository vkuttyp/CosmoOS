# Lockdep raw interrupt-state pairing

Date: 2026-10-06. Branch `lockdep-irq-pairing` from `main` at `2c1a1d98`.
Scope: `docs/plan.md` §1 "Implementation/validation — raw IRQ pairing", the
second half of the plan's third suggested increment. The first half, timer
callback classes, merged as PR #312.

## Summary

Lockdep checked interrupt state only inside the tracked spinlock wrappers
(L14). Raw `arch_irq_save`/`arch_irq_restore` had no checks, though the
scheduler, the interrupt table, the FPU paths, both hypervisor backends,
the IPI layer and lockdep itself use them directly: 29 call sites outside
the tests, eight of them in the spinlock wrappers and lockdep.
With `LOCKDEP=1` they are now lockdep wrappers around the architectures'
`arch_irq_save_hw`/`arch_irq_restore_hw`. Each context keeps a stack of its
outstanding saves, and each restore is checked. A full debug boot of the
tree on both architectures reported **no** violation, so every existing
raw pair is balanced, in order, and keeps interrupts masked inside.

## What is checked

| Violation | Reported at |
|---|---|
| a restore with no outstanding save in its context | the restore |
| a restore of a state other than the innermost outstanding save's | the restore (one slot consumed per restore) |
| interrupts enabled inside a saved region | the restore that ends it |
| a thread exiting with a save outstanding | `lockdep_thread_exit` |
| saves nested deeper than 16 | the save (`LOCKDEP_R_OVERFLOW`) |

The third row is the portable form of an architecture divergence found
while reading the primitives. AArch64's `arch_irq_restore` writes DAIF, so
restoring a masked state masks interrupts again. x86-64's restores only
an enabled state (`sti`) and does nothing for a masked one. Code that wrongly
enables interrupts inside a saved region is therefore silently repaired on
AArch64 and left running with interrupts on x86-64. The check fails it on
both.

## Design

- **Per-thread stacks.** A saved state can be outstanding across a context
  switch: `schedule_internal` saves on the outgoing thread and restores in
  the incoming thread's own earlier frame, with `current` changed before
  the switch. A per-thread stack in `struct thread` pairs them correctly; a
  per-CPU stack would pair one thread's restore with another thread's save.
  Contexts before a CPU has a current thread use a per-CPU stack. Interrupt
  handlers share the interrupted thread's stack, since their pairs are
  balanced and nest above it.
- **New threads** start in `thread_trampoline` with `arch_irq_enable()`, not
  a restore, so they never pop a save they did not make.
- **Thread exit** is checked before `thread_exit` masks interrupts and enters
  the scheduler, whose own save the exiting thread never undoes.
- **NMI safety.** The bookkeeping runs with interrupts masked, but an NMI
  can land anywhere. A push reserves `depth` before it writes the entry,
  and a pop reads before it releases, so a balanced NMI in between uses the
  slots above.
- **No cost without lockdep.** The wrappers are `static inline`
  pass-throughs when `CONFIG_LOCKDEP` is off. Release kernels contain only
  `arch_irq_save_hw`/`arch_irq_restore_hw` (checked with `llvm-nm`). The
  per-thread stack (264 bytes) is in every build, to keep the thread layout
  stable; the per-CPU stacks exist only with lockdep.
- **Not tracked.** Plain `arch_irq_enable`/`arch_irq_disable` belong to boot
  code and the scheduler's own transitions. Two saves with equal states
  restored in swapped order cannot be told apart.

## A latent scheduler dependency, found by this change

The first full validation ran 425 self-tests at every CPU count. Then the
one-CPU x86-64 boot panicked with a **double fault** while userland ran
`pkg`, on the idle thread, in interrupt context, with a stack trace of one
return address repeated until the guard page: `arch_irq_restore`.

`schedule()` ends by restoring interrupts. A restore that enables them
calls `preempt_point()` (the wake-preempt unit), and with a reschedule
pending that enters `schedule()` again: "one more trip", as the comment in
`kernel/core/percpu.c` says. Each resumption of a busy thread can add a
level. On `main` this costs no stack, but only by accident of code
generation: every link is a tail call (`schedule_internal → jmp
arch_irq_restore_hw → sti; jmp preempt_point → jmp sched_preempt → jmp
schedule_internal` on x86-64, `b` on AArch64), so each frame is gone before
the next is made. The first version of this change ran its checks inline
in `arch_irq_restore`, with a 160-byte message buffer whose address
escapes. The compiler then *called* the hardware restore and returned,
keeping a frame per level, and a one-CPU machine under load resumed its
idle thread often enough to exhaust the stack. The fault needed sustained
one-CPU load: the same tree passed the four- and two-CPU and AArch64
boots.

Two changes:

- the checks move to `irq_restore_track` (`noinline`), and the hardware
  restore is the wrapper's last action, which compiles to a tail jump on
  both architectures;
- `scripts/check-kernel-elf.sh`, run after every kernel link, now fails a
  kernel in which any link of that chain is a call. Built with the
  pre-fix wrapper, it fails on both architectures with "arch_irq_restore
  calls arch_irq_restore_hw instead of tail-calling it".

The recursion itself remains the scheduler's design; the guard makes its
dependency explicit instead of incidental. Replacing it with a loop would
remove the dependency, but that is a scheduler change, recorded in the
plan rather than made here.

## Tests

`lockdep-irq-pairing` (pinned; one report per case, through the real
wrappers):

1. a second restore after a matched pair;
2. the outer save's state restored while the inner save is innermost. The
   slot is consumed and interrupts are enabled; the test masks again and
   restores the outer state, which now matches, so only the out-of-order
   restore reports;
3. `arch_irq_enable()` inside a saved region;
4. a pinned worker that returns with a save outstanding.

**Negative controls** (`tools/lockdep-irq-pairing-probe.py`). Each mode
removes one check in a throwaway worktree. The test must then fail at that
check's own case: the case's `lockdep_expected_hits() == 1` line, located
in the source at probe time.

| Mode | Removes | Fails at |
|---|---|---|
| `no-tracking` | all bookkeeping (both wrappers skip it) | case 1, both architectures |
| `no-order` | the innermost-save comparison | case 2, both architectures |
| `no-enabled` | the enabled-inside check | case 3, both architectures |
| `no-exit` | the outstanding-at-exit check | case 4, both architectures |

Eight of eight, each at its own case's line (758, 771, 778, 786), on the
final tree. The tail-call guard has its own negative control: the pre-fix
wrapper fails `check-kernel-elf.sh` on both architectures.

## Validation

On `29166295` (the tail-call fix and guard), one chain, every row run
(`out/irqpfinal2/*.log`):

| Step | Result |
|---|---|
| `make host-test`, `make host-test-lockdep-tsan` | PASS |
| x86-64 debug, 1 CPU, three boots (the configuration that double faulted) | PASS 135.1 / 131.3 / 136.3 s |
| x86-64 debug, 2 / 4 CPUs | PASS 142.8 / 133.0 s |
| AArch64 debug, 1 / 2 / 4 CPUs | PASS 124.2 / 133.5 / 135.0 s |
| `test-chaos`, both | PASS 139.4 s, 145.3 s |
| release build + boot, both | PASS 16.8 s, 20.1 s (wrappers inline; `check-kernel-elf.sh` passes) |
| `make analyze`, both | "static analysis: clean" |
| `tools/lockdep-irq-pairing-probe.py`, 4 modes × 2 architectures | 8 of 8 at the required case |

425 self-tests per debug boot; `lockdep-irq-pairing` passed in every one.

**Overhead.** x86-64, four CPUs, `main` and the branch booted back to back,
alternating, twice per round. Total self-test time:

| Round | `main` | branch |
|---|---|---|
| pre-fix wrapper (checks inline) | 108.3 s, 108.5 s | 110.5 s, 112.0 s |
| final (checks out of line, tail-called restore) | 102.6 s, 107.0 s | 104.6 s, 105.5 s |

The first round put the branch about 3% slower. The final round is within
`main`'s own spread (4.4 s). Four boots per round cannot resolve a cost
this small, so the honest bound is "at most a few percent of a debug
self-test run". Release kernels are unchanged.

## CI timing margin

On CI, the branch's AArch64 `test-gic` step failed once by **time**, not by
a test: its first boot passed in 176.3 s and its second exceeded the
harness's 180 s whole-boot timeout in the shell stage (run 37388584414).
The previous run of the same branch passed with boots near 150 s. `main`'s
own recent AArch64 CI boots range from 120 s to **173.1 s**, so the 180 s
budget has under 4% headroom on a slow runner before this change. A
debug overhead of a few percent narrows it further. The job was re-run,
not the budget raised: one run is not evidence for a new number, and the
margin is `main`'s problem as much as this branch's. It was recorded in
the plan (§12) for a measured decision, and a second timeout followed on
the next run. The measurement then settled it (PR #314): over 257 AArch64
CI boots in 40 runs the median was 154 s and p95 173 s, and four boots
exceeded 180 s -- two of them on branches before this one. The AArch64
whole-boot timeout (`BOOT_TIMEOUT`) is now 240 s, the same as
`test-chaos`; x86-64 stays at 180 s. Locally, back to back, this branch
booted AArch64 in 132.3/132.5 s against `main`'s 135.8/134.3 s, so the
branch itself adds no boot-level time there.

## Remaining

None in this item's scope. Out of scope: plain `arch_irq_enable`/
`arch_irq_disable` pairing, and the swapped-equal-states blind spot above.
