# NEXT SUBSYSTEM — the vGIC queue test still asserts an order the hypervisor does not promise

> Constitution §68 report. Takes up the fourth sighting of
> `el2-guest-irq-queue` (`docs/testing/flakes.md`), on aarch64 CI's GIC
> boot (run 36112785285, 2026-09-25, a branch that changed documentation
> and a probe script only). The entry records the test as fixed: the
> third sighting had been diagnosed as a timing accident and the first
> half of the test rewritten. The fourth was in the second half, and it
> was the same accident. This report measures the class across every
> guest that takes an interrupt and makes the remaining site order-free.
> It also proposes running the test in both orders on every boot, so the
> class cannot come back unseen.

## Problem

```
[ERROR] selftest: hv: line 1239: expected hypercall 2, got exit kind 4 hypercall nr 5 a0 0
SELFTEST: el2-guest-irq-queue ... FAIL: unexpected vm exit at line 1239 (8 ms)
```

The test's own instrument (`CHECK_HC`, built after the second sighting)
names what came back. The guest was expected to reach its heartbeat
(hypercall 2) after completing INTID 42. Instead it took INTID 5, which
was pending behind 42, first.

### The mechanism

- **The list register is refilled only at entry.** There is one list
  register and no maintenance interrupt (`ICH_HCR_EL2` has only `EN`).
  So the host places a pending interrupt only when the vCPU enters:
  the owner's `vintr_take_lowest` and `el2_vdist_offer`
  (`kernel/arch/aarch64/hv_el2.c`).
- **`vcpu_run` re-enters without returning after some exits**
  (`kernel-services/virtualization/vcpu.c`): a host interrupt
  (`HV_EXIT_INTR`) and a guest access to its own GIC (`HV_EXIT_EMULATED`).
  Each loops back to the offer.
- **So the order depends on a host interrupt.** Say the guest completes
  42 (its `msr icc_eoir1_el1`, which frees the register) while INTID 5
  is pending. If the next exit is the guest's heartbeat, 5 comes after
  it. If a host interrupt lands between the EOI and the heartbeat, the
  re-entry places 5 and the guest takes it before the heartbeat.
- **Both orders are correct.** Once the resident interrupt is complete,
  the next one is pending and unmasked, and taking it at once is right.

The window is the handful of guest instructions between the EOI and the
heartbeat's `hvc`, against the host's tick. That is rare and
load-dependent, which is how it survived three sightings: the first half
of the same test had the same assertion, and the fix on 2026-09-22
replaced that one assertion rather than the pattern.

### Measured

**CI.** Five sightings in the logs, all aarch64, all the GIC boot, none on
a revision that touched the hypervisor:

| run | date | line | what came back |
| --- | --- | --- | --- |
| 35205713978 (`main`) | 2026-09-17 | 1157 | (before the instrument) — not in `flakes.md` |
| (the mprotect unit, `60ccfd7`) | 2026-09-20 | 1157 | (before the instrument) |
| 35726616478 | 2026-09-22 | 1159 | (before the instrument) |
| 35728477825 | 2026-09-22 | 1185 | hypercall 42: the first half's order, since fixed |
| 36112785285 | 2026-09-25 | 1239 | hypercall 5: the second half's order |

**Deterministic, from the mechanism.** `tools/irq-order-probe.py` puts
one exit into the window every time. It adds two instructions after the
EOI in the IRQ handler of every guest that ends its handler with
EOI-then-`eret`: a read of `GICD_CTLR`, which EL2 emulates and hands
back as `HV_EXIT_EMULATED`. That covers nine guests under
`tests/hv/aarch64/` and thirteen tests. The read uses `x28`, which none
of the nine touches, so no hypercall argument changes. With the probe
applied, every assertion that needs the heartbeat to come first fails
at its own line, and every assertion that does not passes.

`make ARCH=aarch64 test-gic`, each boot confirmed booted and its guests
confirmed rebuilt:

| boot | result |
| --- | --- |
| probe, `guest_irq` only | FAIL, **1 of 403**: `el2-guest-irq-queue`, line 1239, hypercall 5 |
| probe, `guest_irq` only, again | the same, the same line |
| probe, all nine guests | FAIL, **1 of 403**: the same line, `a0 0` as on CI; the other twelve tests pass |
| no probe (the control) | PASS |

So across the thirteen tests there is **one** remaining assertion that
depends on the order: line 1239. The queue test stops at its first
failure, so the lines after 1239 were read by hand. After 5 is taken,
nothing is pending behind it, so nothing after it depends on the order.
The other site of the class is line 1185, the first half, which the
2026-09-22 fix already made order-free with a loop.

### Why it matters

- **It is the same defect a fix already claimed to remove.** The
  `flakes.md` entry closes with "Three sightings, one instrument, one
  diagnosis, one fix", and the fourth arrived three days later, one
  screen further down the same test.
- **A flake in the vGIC tests reads as a vGIC bug.** Each sighting has
  cost a diagnosis, and the class is invisible to a re-run.
- **Nothing runs the other order on purpose.** Every boot runs the likely
  order; the other happens only when the host's tick lines up. So a new
  assertion of the same kind would pass every local run and land.

## Current implementation

- **`el2-guest-irq-queue`** (`kernel-services/virtualization/hvtest.c`)
  has two halves:
  - *The same INTID twice:* 42 is injected again while the first is
    Active. Since 2026-09-22 this half runs up to eight times, accepting
    heartbeats while 42 reads as pending, until 42 arrives, then requires
    a heartbeat with nothing pending.
  - *A resident interrupt taken while a lower number is offered:* 42 is
    resident while masked, 5 is offered, then the guest is unmasked. This
    half requires hypercall 42, then **the heartbeat (line 1239)**, then
    hypercall 5.
- **`guest_irq.S`**: the handler acknowledges (`ICC_IAR1_EL1`), reports
  the INTID with `hvc`, completes (`ICC_EOIR1_EL1`) and returns to the
  heartbeat loop.
- **No test** runs any guest with a guaranteed exit after the EOI.

## Design

### 1. The second half asserts what it claims, in either order

Lines 1238-1241 claim that 42, once taken, is cleared, and that 5 is then
delivered exactly once. The replacement runs until 5 arrives, accepting
heartbeats on the way and requiring 5 to read as pending at each. Then
it requires the heartbeat with nothing pending, exactly as the first
half does now.

**One helper for both halves**, so the pattern exists once:

```c
/* Run until the guest reports INTID `want`, accepting heartbeats while
 * `want` still reads as pending (the guest may reach its heartbeat before
 * the pending interrupt is placed, or not -- a host interrupt between its
 * EOI and the heartbeat decides, and both orders are correct). Anything
 * else fails, named. Returns the heartbeats seen, or -1. */
static int run_until_irq(struct vcpu *v, struct cosmo_vm_exit *x, unsigned want, ...);
```

The first half's hand-written loop becomes a call to it. The kinfo line
keeps reporting how many heartbeats came before each delivery, so both
orders stay visible in the log.

### 2. Both orders on every boot

The probe's change becomes a permanent guest: `guest_irq_exit.S`, which
is `guest_irq.S` with the `GICD_CTLR` read after the EOI. It is built by
the same `hv.mk` rule. `el2-guest-irq-queue` runs its body twice, once on
`guest_irq.bin` (usually the heartbeat first) and once on
`guest_irq_exit.bin` (always the interrupt first), and must pass both. A
future assertion that depends on the order then fails on every boot, not
once a week on CI.

Only this test gets the second guest. It is the one test whose claim is
about an interrupt pending behind another. The measurement above shows
the other twelve do not depend on the order, and doubling their run time
buys nothing.

### 3. The record

`docs/testing/flakes.md`'s entry gets the fourth sighting, the 2026-09-17
one it never had, and the class. The virtualisation testing doc describes
the two-guest run and why.

### 4. The §70 gate

**Correctness.** One assertion that depended on the host's tick removed;
the claim it stood for is kept.

**Concurrency.** None in the kernel. The test's two orders are made by
the guest, not by timing.

**Ownership and lifetime.** None.

**Security.** None.

**Failure.** The helper names every unexpected exit, as `CHECK_HC` does,
and bounds its runs at eight.

**Performance.** One more guest run in one test: about 10 ms per GIC
boot.

## Affected files

| file | change |
| --- | --- |
| kernel-services/virtualization/hvtest.c | `run_until_irq`; both halves of `el2-guest-irq-queue` use it; the test runs on both guests |
| tests/hv/aarch64/guest_irq_exit.S (new), tests/hv/hv.mk | the guest with an exit after every EOI |
| tools/irq-order-probe.py | unchanged, kept as the class's measurement for the other guests |
| docs | `docs/testing/flakes.md` (the fourth and the unrecorded sighting, the class); `docs/kernel-services/virtualization/testing.md`; README Status |

## APIs

None.

## Migration plan

One PR: the helper, the second guest, the test on both, the documents.

## Tests

| test | checks | mutation it must catch |
| --- | --- | --- |
| `el2-guest-irq-queue` (on `guest_irq`) | both claims, in the order the host's timing gives | a 42 delivered twice, or 5 never |
| `el2-guest-irq-queue` (on `guest_irq_exit`) | the same claims, with the interrupt always first | line 1239's heartbeat-first assertion restored: fails on every boot |

## Benchmarks

None.

## Risks

- **The second guest changes what `x28` holds.** No guest or test reads
  it, as checked for the nine guests above. The new guest is a copy of
  `guest_irq.S`, whose handler does not touch `x28` either.
- **The emulated read's value is unused**, so a distributor that changes
  what `GICD_CTLR` reads cannot change the test.

## Alternatives considered

- **Replace line 1239 alone**, as the first half's fix did. That is what
  let the fourth sighting through. The helper removes the pattern, and
  the second guest removes the chance of it coming back unseen.
- **A maintenance interrupt, so the host refills the register at the
  EOI.** That changes the hypervisor to make a test deterministic, and
  it makes only one of the two orders impossible, not the test
  order-free.
- **Run the probe across all thirteen tests on every boot.** The
  measurement shows one test has the class. The probe stays a tool for
  the next guest that takes an interrupt.
