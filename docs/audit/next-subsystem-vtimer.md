# NEXT SUBSYSTEM — the virtual timer: a guest that can be woken by time

## Problem

A guest can now be interrupted, and there is nothing to interrupt it
*with*. It has no clock it may trust and no timer of its own, so it
cannot preempt a thread, cannot sleep, cannot time out a device, cannot
run a scheduler. The vGIC unit gave the hypervisor a way to deliver an
interrupt; this is the unit that gives a guest a reason to receive one.

And there is a second problem, which is a defect rather than a gap:
**the virtual timer a guest can already reach is the host's own, and
nothing separates them.** `struct hv_sysregs` carries twenty EL1
registers across the world switch and `CNTV_CTL_EL0`, `CNTV_CVAL_EL0`
and `CNTVOFF_EL2` are none of them, so a guest's writes stay in the
hardware after it exits.

Measured, by running a guest that arms `CNTV_TVAL_EL0` and immediately
calls out, and reading `CNTV_CTL_EL0` in the host after every guest
exit:

```text
EXPERIMENT: back in the host, CNTV_CTL_EL0 = 0x2   (42 other guest exits: the host's own IMASK)
EXPERIMENT: back in the host, CNTV_CTL_EL0 = 0x1   (after the guest that armed it: ENABLE)
EXPERIMENT: the guest armed its virtual timer and exited (kind 4 nr 7)
```

`0x2` is the host's leftover — `arch_test_periodic_irq_stop` masks the
source it uses for `irq-route` — and it is what every other guest exit
shows. `0x1` is the guest's `ENABLE`, live in the host's context, with
the guest's compare value still loaded and counting.

## Current implementation

**The host's tick is the physical timer.** `timer.c` reads `CNTPCT_EL0`
and arms `CNTP_CVAL_EL0`; the tick arrives on PPI 30
(`VIRT_TIMER_EL1_PHYS_INTID`, or whatever the GTDT says). So the
*virtual* timer is not the host's clock, which is the fact that makes
this unit tractable at all.

**But the host does use the virtual timer, for one thing.**
`arch_test_periodic_irq_start` — the hook behind the `irq-route`
self-test, the only periodic interrupt source this architecture offers a
test — arms `CNTV_TVAL_EL0`/`CNTV_CTL_EL0` and routes PPI 27
(`VIRT_TIMER_EL1_VIRT_INTID`). It is stopped by masking, not by
disabling, which is where the `0x2` above comes from. So the virtual
timer has one host user already, and a guest would be a second.

**Nothing about time crosses the world switch.** `struct hv_sysregs` has
`cntkctl` (EL1's counter access control) and no timer registers;
`hv_el2_switch.S` mentions neither `CNTV_*` nor `CNTVOFF_EL2`. The
loader sets `CNTVOFF_EL2 = 0` once, for the host, and
`CNTHCTL_EL2 = 0x3` (`EL1PCTEN | EL1PCEN`: EL1 may read the physical
counter and program the physical timer).

Three consequences follow from those two facts, and all three are
already true:

- A guest sees **the host's uptime** in `CNTVCT_EL0`, because
  `CNTVOFF_EL2` is zero and no one sets it per VM.
- A guest may read `CNTPCT_EL0` and program `CNTP_*` — the *host's*
  tick timer — because `CNTHCTL_EL2` permits EL1 to, and a guest is at
  EL1. Nothing traps it.
- A guest's `CNTV_*` writes outlive it, as measured above.

**And the hypervisor has no timer of its own.** `vcpu_run` loops until
an exit; `vcpu_run_limited(n)` bounds the host interrupts it will
absorb. There is no per-vCPU deadline, nothing that says "enter this
guest again at time T", which is what a guest whose timer expires while
it is *not* running would need.

## Why it matters

- **A guest that cannot be woken by time cannot run an operating
  system.** Not "runs one slowly": a kernel with no timer interrupt
  cannot preempt, cannot expire a sleep, cannot time out a device
  probe. Every guest this tree can host is still a program that
  computes and traps, only now it can also be interrupted by its owner.
- **The isolation defect is a defect now, not after this unit.** A
  guest can arm the virtual timer and leave it armed in the host. It is
  benign today only because the host does not enable that PPI except
  during one self-test — which is a coincidence of configuration, not a
  property anyone chose, and the sort of coincidence that stops being
  true when someone gives the host a use for `CNTV`.
- **A guest can read the host's uptime and program the host's tick.**
  Neither is a sandbox escape, and both are the kind of thing a
  virtualization layer is supposed to be exact about. `CNTHCTL_EL2`
  exists precisely to stop them and is set to permit them.
- **It is the last piece before a guest can be given real work.** With
  interrupts, a timebase and a timer, the remaining gap to running a
  small guest kernel is the virtual distributor — a much larger unit
  that this one does not need and that needs this one.

## Proposed design

### 1. The guest gets the virtual timer; the host keeps the physical one

The division is already almost true and this unit makes it exact. The
host's tick stays `CNTP`; a guest's timer is `CNTV`, which is what a
guest kernel expects to find at EL1 anyway. `CNTVOFF_EL2` becomes
per-VM, so a guest's `CNTVCT_EL0` starts near zero at creation rather
than reporting how long the host has been up.

**Per-VM means one source, and the source is the VM.** `struct
arch_hv_vm` gains `cntvoff`, set once in `el2_vm_create` to the host's
`CNTPCT_EL0` at that moment. Every vCPU's `ctx->cntvoff` is a *copy* of
it, taken in `el2_vcpu_create`, and the switch reads the copy because
EL2 assembly reads the context page and nothing else. A vCPU created
later than its siblings gets the same value, because the value is the
VM's and not the clock's at creation time. Two vCPUs of one VM therefore
read the same `CNTVCT_EL0` (to within the time between the reads), and
two VMs read different ones — both of which are tests below, because a
guest that compares time across its CPUs would find the per-vCPU-source
version out by however long apart its vCPUs were created.

The host's one virtual-timer user, `arch_test_periodic_irq_start`, has
to go somewhere. Two honest options, and the report proposes the first:
**move the test hook to the physical timer's spare compare** — there is
no second compare, so in practice this means giving the hook its own
periodic source derived from the existing tick — or **declare the hook
and guests mutually exclusive**, which is true today by accident and
would become a rule with an assertion behind it. The second is less
work and less honest; the first is a small change to one test hook.

### 2. Per-vCPU timer state, saved and restored like everything else

`struct hv_ctx` gains `cntv_ctl`, `cntv_cval` and `cntvoff`, and the
switch moves them exactly as it now moves the vGIC's registers:

```text
entry:  CNTVOFF_EL2 = ctx->cntvoff
        CNTV_CVAL_EL0 = ctx->cntv_cval
        CNTV_CTL_EL0  = ctx->cntv_ctl        (the guest's timer becomes live)
exit:   ctx->cntv_ctl  = CNTV_CTL_EL0        (what the guest left)
        ctx->cntv_cval = CNTV_CVAL_EL0
        CNTV_CTL_EL0 = 0                     (disarmed: the host's context is its own)
        CNTVOFF_EL2  = 0                     (the host's view of time is unshifted)
```

Disarming on exit is the fix for the measured defect, and it is one
instruction. Restoring on entry is what makes the guest's timer
*continue* rather than restart.

### 3. When it fires while the guest is running

Three separate facts have to be arranged for, and a first draft of this
section arranged for none of them.

**Enablement.** The virtual timer's PPI (27 on `virt`; the GTDT's
value in general) is *not enabled* in the host's redistributor today
except while `arch_test_periodic_irq_start` is running. A disabled PPI
raises nothing, so a guest's expiry would cause no exit and would be
noticed only at the next unrelated one. The backend therefore owns that
line: it binds it at probe (`gic_bind_ppi`, the way the tick is bound)
and enables it on each CPU when that CPU's switch is installed
(`el2_ready_here`, where the per-CPU EL2 stack is set up), so that an
expiry during a guest run becomes a physical interrupt, which
`HCR_EL2.IMO` takes to EL2, which is an exit.

**Identification is from the timer, not from the exit.**
`HV_EXIT_INTR` carries no INTID — it says only that a physical interrupt
arrived, and it arrives for the host's tick just as readily. The exit
path in the switch reads `CNTV_CTL_EL0` into `ctx->cntv_ctl` *before*
disarming it, so the saved value carries `ISTATUS`: the timer's own
statement that its condition was met. After any run, the backend checks
`ctx->cntv_ctl` for `ENABLE && !IMASK && ISTATUS` and, if so, marks the
guest's timer INTID pending for the next entry — through the vGIC, the
way `vcpu_inject` does, so that PPI 27 is an INTID in a list register
like any other. This is independent of which exit occurred and of
whether the host ever saw the physical interrupt at all.

**Acknowledgement belongs to the host, and is usually moot.** The
generic timer interrupt is level-sensitive: asserted while the condition
holds. The switch disarms `CNTV_CTL_EL0` on exit, so by the time the
host is back at EL1 with interrupts enabled the line is down and a
level PPI that is no longer asserted is no longer pending. If the
redistributor does still hold it, the host's ordinary interrupt path
takes it, and the backend's handler for PPI 27 acknowledges it and does
nothing else — the timer is already disarmed, and the decision to
inject was made from the saved `ISTATUS`, not from the handler running.
So the physical interrupt is never "consumed and reobserved": the host
consumes it (or finds nothing to consume), and the guest is given a
*virtual* one from state captured before either could happen.

This is the piece the vGIC unit was the prerequisite for, and it needs
nothing new from it. It does need one thing from `timer.c`: the test
hook and the backend cannot both bind PPI 27, which is the reason §1
moves the hook rather than merely asking it to share.

### 4. When it would fire while the guest is *not* running

The hard half, and the one to be explicit about. A guest whose timer
expires while its vCPU thread is elsewhere must not lose the expiry.
Three answers:

**(a) Nothing.** The guest's timer is armed only while the guest runs;
an expiry that would have happened in between is noticed on the next
entry, because `CNTV_CTL.ISTATUS` is computed from the compare and the
counter, not latched. The interrupt is late by however long the vCPU
was descheduled. Simple, correct in the sense that no interrupt is
lost, and wrong for any guest that cares when its timer fires.

**(b) A host timer per vCPU.** On exit, if the guest's timer is armed
and unexpired, arm a host `struct timer` for that deadline whose
callback wakes the vCPU's thread. The guest is then entered at
approximately the right time and takes the interrupt. This is what KVM
does and it is the right answer.

**(c) Trap `CNTV_*` and model the timer entirely in software.** Most
control, most cost, and it makes every guest timer read an exit.

Proposed: **(a) for the first step, (b) as the step that follows**, so
that "the guest's timer state is isolated and its interrupt is
delivered" lands and is tested before "and it is delivered on time"
adds a host timer per vCPU to the picture. The distinction is
measurable — the lateness is the measurement — and (a) is not a design
that has to be undone to get to (b).

### 5. Closing the two windows the guest already has

- `CNTHCTL_EL2` for a running guest becomes `0` rather than `0x3`:
  `EL1PCTEN` and `EL1PCEN` clear, so a guest's reads of `CNTPCT_EL0`
  and its accesses to `CNTP_*` trap to EL2. The host's own value is
  restored on exit — it needs `0x3` for itself, since the host is at
  EL1 and its tick *is* the physical timer.
- Trapped `CNTP_*` accesses become an exit the backend can answer. The
  smallest honest answer for now is to report them the way an
  unhandled system register is reported today (`HV_EXIT_SYSREG`), so a
  guest that insists on the physical timer gets an owner-visible exit
  rather than the host's timer.

## Affected files

| file | change |
|---|---|
| `kernel/arch/aarch64/include/aarch64/hv_ctx.h` | `cntv_ctl`, `cntv_cval`, `cntvoff`, offsets, static asserts |
| `kernel/arch/aarch64/hv_el2_switch.S` | save/restore and disarm; `CNTHCTL_EL2` for the guest and back |
| `kernel/arch/aarch64/hv_el2.c` | the expiry → INTID offer from the saved `ISTATUS`; trapped `CNTP_*` |
| `kernel/arch/aarch64/timer.c` | the test hook stops using `CNTV` and stops binding PPI 27; the guest timer PPI is named |
| `kernel/arch/aarch64/hv_el2.c` | binds PPI 27 at probe and enables it per CPU in `el2_ready_here`; a handler that acknowledges and nothing else; `struct arch_hv_vm.cntvoff` |
| `kernel/arch/aarch64/hv.c`, `kernel/include/arch/hv.h` | the guest's timer INTID, for the test to inject against |
| `tests/hv/aarch64/guest_timer.S` | **new**: arms its timer, takes the interrupt, reports |
| `kernel-services/virtualization/hvtest.c` | the tests below |
| `docs/kernel/arch/aarch64/design.md`, `invariants.md`, `docs/kernel-services/virtualization/` | the timer, the isolation rule, what a guest may and may not read |

## New APIs

Nothing new in `arch/hv.h`'s vcpu operations: a timer interrupt is an
INTID and the vGIC delivers it. What is new is internal —
`struct hv_ctx` fields and a per-VM `cntvoff` — plus one query for the
tests:

```c
/* The INTID a guest's virtual timer raises (AArch64: the GTDT's virtual
 * timer PPI, 27 on QEMU's virt). 0 where the architecture has no such
 * thing. */
unsigned arch_hv_guest_timer_intid(void);
```

## Migration plan

1. **Isolation first, alone.** Save, restore and disarm `CNTV_*` across
   the switch; `CNTVOFF_EL2` from the VM's one value, copied into each
   vCPU. No delivery yet. The measurement above is the test: after a
   guest that arms its timer, the host's `CNTV_CTL_EL0` reads what it
   read before that guest ran — and two vCPUs of one VM read the same
   `CNTVCT_EL0`.
2. **Close the physical-timer windows.** `CNTHCTL_EL2 = 0` for a
   running guest, restored on exit; trapped `CNTP_*` reported as a
   system-register exit. A guest can no longer read host uptime or
   touch the host's tick.
3. **Deliver the expiry.** The backend takes over PPI 27 from the test
   hook, enables it per CPU, and after each run reads the saved
   `CNTV_CTL` for `ISTATUS` and offers the guest's timer INTID to the
   vGIC. The guest fixture takes it. This step and the test-hook move
   are one commit, because neither is correct without the other.
4. **On time.** The per-vCPU host timer of §4(b), and the lateness
   measured before and after.
5. Docs, and the decision about `arch_test_periodic_irq_start`.

Steps 1 and 3 are separate commits: "the guest's timer stopped leaking"
and "the guest's timer fires" are different claims and a bisect should
land on one.

## Tests

- **`el2-guest-timer-isolated`** — the measurement that opened this
  report, as a test: a guest arms `CNTV`, exits, and the host's
  `CNTV_CTL_EL0` is unchanged from before the run. Fails today with
  `0x1` where `0x2` was.
- **`el2-guest-timer`** — the guest arms its timer, the host runs it,
  and the guest's own handler reports through a hypercall that the
  timer INTID arrived. The interrupt must be the *timer's*, not one the
  owner injected, which the fixture distinguishes by the INTID it
  acknowledges. It also asserts the exit that preceded delivery was a
  timer expiry and not the host's tick — the saved `CNTV_CTL` read
  `ISTATUS` — so a backend that injected on every `HV_EXIT_INTR` would
  fail it by delivering on the tick.
- **`el2-guest-timer-offset`** — two VMs created at different times
  both see `CNTVCT_EL0` start near zero, and neither sees the host's
  uptime; and **two vCPUs of one VM, created at different times, read
  the same `CNTVCT_EL0`** to within the gap between the reads. The
  second half is what a per-vCPU offset source would fail — a guest
  that compares time across its CPUs would see them disagree by however
  long apart the vCPUs were created. Fails today, where every vCPU sees
  exactly the host's counter.
- **`el2-guest-phys-timer`** — a guest reading `CNTPCT_EL0` or writing
  `CNTP_CTL_EL0` gets an exit rather than the host's timer. Fails
  today, where it silently succeeds — and the second half of that is
  the one worth having, because a guest that *arms the host's tick* is
  a fault the host would feel.
- **`irq-route` unchanged**, which is the constraint on whatever is
  done about the test hook.

Each with the bug-proof this project expects: reintroduce, watch the
named test fail for the stated reason, restore, verify the tree clean.

## Benchmarks

Time under TCG is not a number worth printing, with one exception this
unit creates: **lateness is a difference between two guest-visible
counter reads**, and a guest can measure it itself without trusting the
host's clock. So:

- **Interrupts a guest can be woken by: none today, its own timer
  after.** The headline, and it needs no clock.
- **Lateness, in guest counter ticks**: the guest records `CNTVCT_EL0`
  at the deadline it asked for and again in its handler. Step 3 will
  show a large and variable number (the vCPU is entered when the owner
  next runs it); step 4 should show a much smaller one. That is the
  measurement that says whether the host timer of §4(b) was worth
  adding, and it is the honest way to compare them.
- **Exits per guest second**, counted: a timer that traps every read
  would show here, which is the argument against design (c).

## Risks

- **This is the EL2 assembly again.** Every one of this project's worst
  bugs has been in the world switch — a stale `SP_EL2` in a freed page,
  and the active-priority registers that leaked between guests one unit
  ago. Both were found by instrumenting rather than reasoning; page
  poisoning stays on, and the same discipline applies.
- **The timer registers are shared state like the active priorities
  were.** That is the exact shape of the bug the vGIC unit shipped and
  had to fix: an EL2-visible register that belongs to whichever guest
  last touched it. The lesson is fresh enough to apply on purpose this
  time — every register the switch touches gets an invariant and a test
  that a second guest sees a clean one.
- **`CNTHCTL_EL2` gates the host too.** The host runs at EL1 and its
  tick *is* the physical timer, so the value that is right for a guest
  is fatal for the host. Getting the restore wrong stops the host's
  clock, which presents as a hang rather than as a wrong number.
- **QEMU is not hardware, again.** Counter behaviour under TCG is not
  the architecture's: `CNTVCT` advances with emulated time, and
  lateness measured there says more about the emulator's scheduling
  than about a machine. The step-4 comparison is therefore a shape, not
  a figure.
- **A guest that spins on its own timer can consume its vCPU.** Nothing
  new — a guest can already spin — but a guest that arms a very short
  period turns every entry into an immediate exit. `vcpu_run_limited`
  already bounds host interrupts absorbed per call and should be
  checked to cover this, rather than assumed to.

## Alternatives considered

- **Emulate the timer entirely in software** (design (c) above), so no
  guest touches `CNTV` at all. Most control and most exits; it also
  makes the guest's clock the host's arithmetic, which is a bigger
  promise than this unit should make. Named as what to do if hardware
  ever disagrees with the switch-based approach.
- **Give the guest the physical timer and move the host to the
  virtual one.** Symmetrical, and wrong: a guest kernel expects `CNTV`
  at EL1 and would need modifying, which defeats the purpose of the
  virtual distributor unit that follows.
- **Do the virtual distributor first.** It is the bigger prize — it is
  what lets a stock guest kernel probe a GIC — but it does not need a
  timer and a timer does not need it, and this one is a tenth of the
  work while fixing a defect that exists today. Order chosen for that
  reason rather than by size.
- **Fix only the isolation leak and stop.** Tempting, since that is the
  actual bug and it is one instruction on the exit path. Rejected
  because the leak exists precisely *because* no one had decided who
  owns the virtual timer, and a fix that disarms it without giving the
  guest a timer leaves the guest with a register it may write and
  nothing that happens — which is the same shape of lie the vGIC unit
  was written to remove.
