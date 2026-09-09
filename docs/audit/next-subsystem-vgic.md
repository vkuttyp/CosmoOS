# NEXT SUBSYSTEM — the vGIC: an AArch64 guest that can be interrupted

## Problem

**An AArch64 guest cannot receive an interrupt.** Not "receives them
slowly" or "receives only some" — the delivery path does not exist.

```c
static void el2_vcpu_set_irq(struct arch_hv_vcpu *v, int vector)
{
    v->offered = vector;   /* recorded; delivery needs the GIC list registers */
}
```

That is the whole of it (`kernel/arch/aarch64/hv_el2.c:440`). The
companion is worse, because it is silently wrong rather than honestly
absent:

```c
static bool el2_vcpu_irq_taken(struct arch_hv_vcpu *v) { return v->irq_taken; }
```

`v->irq_taken` is set to `false` on every entry (`hv_el2.c:515`) and is
never set anywhere. So the generic layer's

```c
int offered = vintr_take_lowest(v);
arch_hv_vcpu_set_irq(v->arch, offered);
...
if (offered >= 0 && arch_hv_vcpu_irq_taken(v->arch))
    vintr_clear(v, offered);
```

never clears a pending vector: an injected interrupt stays pending for
the life of the vCPU, is re-offered on every entry, and is delivered
never. `cosmo_vcpu_regs.pending_irq` reports it forever.

x86-64 delivers. Both backends there do real event injection — SVM's
`EVENTINJ`, VMX's VM-entry interruption-information field — and
`hv-guest-irq` exercises the whole of it: inject, the interrupt-window
case, the shadow after `sti`, a second vector. That test opens with

```c
#if !defined(ARCH_X86_64)
    /* This guest image and these expectations are x86's; the AArch64
     * guests are covered by the el2-* tests. */
    kinfo("selftest: hv-irq: an x86 guest; skipping");
```

and the `el2-*` tests do not cover interrupts, because there is nothing
to cover.

So `vcpu_inject()` is a public API — reachable from userland through the
vCPU handle — that returns 0 on AArch64 and does nothing. A caller
cannot tell the difference between "delivered" and "dropped".

## Current implementation

**The host half is done and works.** `HCR_EL2` while a guest runs is

```c
c->hcr = HCR_VM | HCR_RW | HCR_IMO | HCR_FMO | HCR_AMO | HCR_TWI | HCR_TWE | HCR_TID3 | HCR_TSC;
```

`IMO`/`FMO`/`AMO` route physical IRQ, FIQ and SError to EL2, which is how
a host interrupt exits the guest (`HV_EXIT_INTR`, and `vcpu_run` loops).
That is the mechanism the host needs and it is not what is missing.

**Nothing else about the GIC exists in the hypervisor.** A grep for
`ICH_`, `GICH`, `GICV_` or `vgic` across `kernel/` and `drivers/` returns
exactly one line, and it is a comment in the ACPI parser:

```c
/* GICC: CPU interface number 4, ACPI UID 8, flags 12, parking version 16,
 * performance GSIV 20, parked address 24, physical base 32, GICV 40, GICH 48,
 * VGIC maintenance 56, GICR base 60, MPIDR 68 ... */
```

The parser reads offsets 12, 32, 60 and 68. GICV, GICH and the
maintenance GSIV are named in the comment and dropped — the same
situation the GICv3 unit found for GICR and the ITS, and it was fixed
there the same way.

**The world switch has room but no state.** `struct hv_ctx` is one page
and currently ends at offset `0x310`; the switch saves and restores 20
system registers each way, the guest's GPRs, `SP_EL1`, `PC`, `PSTATE`,
and the host's callee-saved set. No GIC state passes through it.

**The generic layer has an x86-shaped range check:**

```c
int vcpu_inject(struct vcpu *v, unsigned vector)
{
    if (vector < 32 || vector > 255)
        return -EINVAL;
```

On x86 that is right: 0..31 are exceptions. On AArch64 INTIDs 0..15 are
SGIs and 16..31 are PPIs — the private interrupts, which is exactly what
a guest wants (the virtual timer is PPI 27). The check as written
forbids the useful half of the space.

**The guest fixtures are five small assembly files** (`guest_wfi.S`,
`guest_hvc.S`, `guest_mmio.S`, `guest_sysreg.S`, `guest_spin.S`). None
installs a vector table; none could take an interrupt if one arrived.

## Why it matters

- **A guest with no interrupts is a guest that can only run
  straight-line code.** No timer, so nothing inside a guest can be
  preempted or can sleep; no device interrupt, so virtio is impossible;
  no console input. Everything the AArch64 hypervisor can host today is
  a program that computes and then traps.
- **The API lies, which is worse than a gap.** `vcpu_inject` returns 0.
  `pending_irq` reports the vector. A user-space VMM written against
  this interface on x86 and moved to AArch64 gets a guest that hangs,
  with every call reporting success. A subsystem may be incomplete; its
  interface should not claim otherwise.
- **It is the only op in the seam that AArch64 does not implement.**
  `arch/hv.h` declares twenty-nine entry points and the EL2 backend
  fills in all of them but this one. The seam was built so the two
  architectures could be reasoned about together, and one unimplemented
  op in the middle of it is what makes "the backends are equivalent"
  false.
- **The prerequisite just landed.** The virtual CPU interface is the
  same architecture as the physical one, in the same registers' idiom:
  after the GICv3 unit, this tree knows what `ICC_IAR1_EL1`,
  `ICC_EOIR1_EL1`, priorities, groups and `ICC_PMR_EL1` mean. The
  virtual side is `ICH_*_EL2` describing the same objects. The cost of
  this unit is far lower now than it was a week ago, and that is the
  argument for doing it next rather than later.

## Proposed design

### 1. Which virtual interface — and the constraint that shapes everything

A GICv3 virtual CPU interface is **system registers**: `ICH_HCR_EL2`
(enable, maintenance controls), `ICH_VTR_EL2` (how many list registers
this implementation has), `ICH_LR<n>_EL2` (one virtual interrupt each:
state, group, priority, vINTID), `ICH_VMCR_EL2` (the guest's own view of
`PMR`, `BPR` and the group enables), and `ICH_MISR_EL2` / `ICH_EISR_EL2`
/ `ICH_ELRSR_EL2` for maintenance.

**Every one of those is an EL2 register, and this kernel runs at EL1.**
That is the constraint the whole design turns on. The host cannot write
a list register; only the code running at EL2 can — which in this tree
is the world switch (`hv_el2_switch.S`) reached through the loader's EL2
stub. So the vGIC state travels the way everything else travels: as
fields in `struct hv_ctx`, written by the host before `el2_run` and
written back by the switch on the way out.

This is not a workaround; it is the same shape as `vttbr`, `hcr` and
`exit_esr`, and it has a property worth having — the host's view of a
guest's interrupt state is a plain structure it can read, test and log
without an HVC.

### 2. GICv3 only, and the reason

A GICv2 host virtualises through MMIO instead: a `GICH` frame for the
hypervisor and a `GICV` frame that must be stage-2 mapped at the
address where the guest expects its `GICC`. Both frames exist on QEMU's
`virt` with `gic-version=2,virtualization=on`, so it could be done.

Proposed: **do not**. Three reasons, in order of weight.

1. **The guest-visible interface is different, so one guest fixture
   cannot serve both.** Under GICv3 the guest acknowledges with
   `ICC_IAR1_EL1`; under GICv2 it reads a memory-mapped `GICC_IAR`. Every
   test in this unit would need two guest images and two sets of
   expectations, roughly doubling the unit for an interface that is
   pre-2013.
2. **The host GICv2 driver exists because QEMU's default is
   `gic-version=2`** — a fact about this project's chain, not about
   hardware. The GICv3 unit already made that argument about GICv2m
   frames and it applies with more force here: no GICv2 machine anyone
   is likely to run this on has a hypervisor use case.
3. **Declining is honest and testable.** On a GICv2 host,
   `vcpu_inject` should return `-ENOTSUP` rather than 0. That is a
   behaviour change, it is an improvement over today's silent success,
   and it is a one-line test.

The consequence must be stated plainly rather than discovered: **the new
tests run only under `QEMU_GIC=3`**, which the chain already has
eight of. On the default GICv2 machine the guest-interrupt tests report
themselves skipped and `el2-vgic-decline` runs instead, so the coverage
exists -- it is just not where the default boot looks.

### 3. Where the code lives

`gicv3.c` knows the GIC; `hv_el2.c` knows the guest. Propose a small
interface between them, `aarch64/vgic.h`, implemented in a new
`gicv3_vgic.c`:

```c
bool     aarch64_vgic_available(void);   /* false on a GICv2 host */
unsigned aarch64_vgic_lr_count(void);    /* from ICH_VTR_EL2, read at EL2 init */
```

and nothing else — the list registers themselves are filled in by
`hv_el2.c` into the context, because they are per-vCPU state and the GIC
driver has no business holding it.

**Not** four more entries in `struct aarch64_irqc_ops`. That table is the
host controller's interface, every entry of which both drivers must
implement (`ops_check` panics on a hole, by design). A vGIC group would
force `gic.c` to carry four stubs whose only content is "GICv2 does not
do this here", which is worse than one `available()` that answers the
question once.

### 4. What the switch does

On entry, for each list register the host wants live:

```text
ICH_HCR_EL2  = ctx->vgic_hcr        (En, and later UIE/LRENPIE)
ICH_VMCR_EL2 = ctx->vgic_vmcr       (the guest's PMR/BPR/group enables, preserved across runs)
ICH_LR<n>_EL2 = ctx->vgic_lr[n]
```

On exit, the reverse: the LRs, `ICH_VMCR_EL2`, `ICH_MISR_EL2` and
`ICH_ELRSR_EL2` are read back into the context, and `ICH_HCR_EL2.En` is
cleared so the host's own interrupts are unaffected by whatever the
guest left behind.

Reading the LRs back is what makes `vcpu_irq_taken` mean something: a
list register whose state field has gone from Pending to Invalid was
taken *and* completed by the guest; one still Pending was not taken.
That is the fact the generic layer has been asking for and getting
`false` for.

### 5. The guest's side needs no distributor

This is the part that makes the unit small enough to do. With
`HCR_EL2.IMO` set — which it already is — a guest's `ICC_*_EL1` accesses
at EL1 are redirected by hardware to the **virtual** CPU interface. A
guest can therefore:

- enable its interface with `ICC_SRE_EL1`, `ICC_PMR_EL1`,
  `ICC_IGRPEN1_EL1` — the same four writes the host driver's
  `gicv3_init_cpu` makes;
- take the interrupt through its own `VBAR_EL1`;
- acknowledge with `ICC_IAR1_EL1` and complete with `ICC_EOIR1_EL1`.

None of that touches a distributor, because a virtual interrupt placed
directly in a list register bypasses one: the LR *is* the pending state,
the priority and the group. **Emulating a virtual distributor —
`GICD`/`GICR` MMIO trapped through stage 2, so a guest can run an
unmodified GIC driver — is a separate and much larger unit**, named here
as the follow-up and not attempted. What this unit buys is that the
hypervisor's own injection interface works; what the follow-up buys is
that a guest kernel can drive it.

One thing to check early rather than late: `ICC_SRE_EL2.Enable` gates
whether EL1 may use the system-register interface at all. The host
kernel's own `ICC_SRE_EL1` write works today under QEMU without anyone
setting `ICC_SRE_EL2`, so QEMU is lenient; the guest's path should be
verified in step 2 rather than assumed in step 4.

### 6. The injectable range

`vcpu_inject`'s `vector < 32` check is x86's. Propose an arch-supplied
range so the check stays meaningful on both:

```c
/* The interrupt numbers this architecture's guests can be given.
 * x86-64: 32..255, because 0..31 are exceptions and vcpu_inject is not
 * how you deliver those. AArch64: 0..1019, because SGIs and PPIs are
 * private interrupts and are most of what a guest wants; LPIs are
 * excluded until something can map them. */
void arch_hv_vintr_range(unsigned *lo, unsigned *hi);
```

`vcpu_inject` validates against it. The x86 test's
`vcpu_inject(v, 3) == -EINVAL` keeps passing unchanged, which is the
point of making the range arch-supplied rather than widening it.

### 7. Deliberately out of scope

- **A virtual distributor** (above).
- **The virtual timer.** `CNTVOFF_EL2` is already zeroed by the loader,
  and a guest that programs `CNTV_CVAL_EL0` raises a physical interrupt
  that exits to EL2 — but turning that into a *virtual* PPI 27 needs a
  per-vCPU timer model and a decision about trapping `CNTV_*`. It is the
  natural next unit and this one is its prerequisite; the tests here
  inject PPI 27 from the *host* to prove the private range works,
  without pretending that is a timer.
- **Maintenance interrupts.** `ICH_HCR_EL2.UIE`/`LRENPIE` raise an EL2
  interrupt when the list registers drain. With one interrupt in flight
  and an exit after every run, polling `ICH_ELRSR_EL2` on exit tells the
  host the same thing through a mechanism that already exists. Step 6
  revisits this when several are in flight.

## Affected files

| file | change |
|---|---|
| `drivers/acpi/acpi.c`, `kernel/include/kernel/acpi.h` | store GICV, GICH and the VGIC maintenance GSIV the GICC parser already names |
| `kernel/arch/aarch64/gicv3_vgic.c` | **new**: `available()`, `lr_count()` from `ICH_VTR_EL2` |
| `kernel/arch/aarch64/include/aarch64/vgic.h` | **new**: that interface |
| `kernel/arch/aarch64/include/aarch64/hv_ctx.h` | LR array, `vgic_hcr`, `vgic_vmcr`, `vgic_misr`, `vgic_elrsr`, offsets and static asserts |
| `kernel/arch/aarch64/hv_el2_switch.S` | write `ICH_*` on entry, read back on exit |
| `kernel/arch/aarch64/hv_el2.c` | `el2_vcpu_set_irq` fills a list register; `el2_vcpu_irq_taken` reads one back; probe reports the vGIC |
| `kernel/include/arch/hv.h`, `kernel/arch/*/hv.c` | `arch_hv_vintr_range` |
| `kernel-services/virtualization/vintr.c` | validate against that range |
| `tests/hv/aarch64/guest_irq.S`, `tests/hv/hv.mk` | **new** guest fixture with a vector table |
| `kernel-services/virtualization/hvtest.c` | the new tests |
| `docs/kernel/arch/aarch64/design.md`, `invariants.md`, `testing.md`, `docs/kernel/hv/` | the vGIC, its EL2-only constraint, the GICv2 decline |

`gic.c` is not in the list, and that is deliberate: the GICv2 driver
gains nothing and stubs nothing.

## New APIs

```c
/* aarch64/vgic.h */
bool     aarch64_vgic_available(void);
unsigned aarch64_vgic_lr_count(void);

/* arch/hv.h */
void arch_hv_vintr_range(unsigned *lo, unsigned *hi);
```

`struct hv_ctx` gains a vGIC block. `arch/hv.h`'s vcpu operations do not
change at all — `vcpu_set_irq` and `vcpu_irq_taken` finally mean what
they have always claimed.

## Migration plan

1. **ACPI first, alone.** Parse and log GICV, GICH and the maintenance
   GSIV. Nothing uses them. Verifiable by reading a boot log, exactly as
   the GICv3 unit's first step was.
2. **Discovery, no delivery.** `ICH_VTR_EL2` read at EL2 init and
   reported; `aarch64_vgic_available()`; a GICv2 host logs once at probe
   that guests will get no interrupts, and `vcpu_inject` starts
   returning `-ENOTSUP` there. Also the place to confirm
   `ICC_SRE_EL2.Enable` lets a guest reach its interface.
3. **The context and the switch.** vGIC fields, written on entry and
   read back on exit, with nothing yet placed in them. Proved by the
   existing `el2-*` tests being unchanged and by a read-back assertion:
   what the host wrote is what comes back.
4. **One interrupt.** `set_irq` fills LR0; `irq_taken` from the
   read-back; the new guest fixture and `el2-guest-irq`. This is the
   step where the API stops lying.
5. **The injectable range**, so SGIs and PPIs can be injected — and the
   masking test, which needs a guest that can set `PSTATE.I`.
6. **Several in flight**: every list register, the underflow question,
   the docs sweep.

Steps 3 and 4 are separate commits so a bisect lands on "the state
crossed EL2 intact" or "the guest took it", not both.

## Tests

- **`el2-guest-irq`** — the unit's reason to exist. The guest installs
  `VBAR_EL1`, enables its virtual CPU interface, unmasks `PSTATE.I` and
  executes `wfi`; the host injects INTID 42; the guest's IRQ handler
  writes a byte to the console MMIO and does `ICC_EOIR1_EL1`. The host
  requires the byte, and requires `irq_taken` so the pending bit clears.
  Fails if the list register is never written, if the guest's interface
  is not enabled, or if the exit path does not read the LRs back.
- **`el2-guest-irq-masked`** — inject while the guest has `PSTATE.I`
  set. The guest must *not* take it, the list register must still read
  Pending, `pending_irq` must still report it, and clearing `I` must
  deliver it on the next run. This is the AArch64 analogue of the x86
  test's `sti` shadow case, and it is the test that distinguishes "the
  interrupt was delivered" from "the interrupt was dropped and the guest
  happened to write the byte for another reason".
- **`el2-guest-irq-private`** — inject PPI 27 and SGI 3, the range the
  current `vcpu_inject` refuses. Fails before step 5 with `-EINVAL`,
  which is the point.
- **`el2-vgic-decline`** — on a GICv2 host, `vcpu_inject` returns
  `-ENOTSUP` and `aarch64_vgic_available()` is false. Runs in the
  default chain shape, where the others skip, so the GICv2 machine is
  not left testing nothing.
- **`hv-guest-irq` (x86) unchanged**, including
  `vcpu_inject(v, 3) == -EINVAL`. The range change is the one part of
  this unit that can break another architecture, so the test that
  catches it is named here.

Each with the bug-proof that has become the standard: reintroduce the
defect, watch the named test fail for the stated reason, restore, verify
the tree is clean.

The lesson the last three units paid for applies directly. `irq_taken`
was a boolean that was *argued* to be correct — it is read, it is
cleared, the generic layer consumes it — and was never true. A property
that no test asserts is not a property. `el2-guest-irq` asserts it from
the guest's side, which is the only side that can tell.

## Benchmarks

Interrupt latency under TCG is not a number worth printing, for the
reason `docs/kernel/memory/testing.md` records and the GICv3 unit
repeated. What can be measured honestly:

- **Interrupts deliverable to an AArch64 guest: 0 today, all of
  0..1019 after.** That is the headline and it needs no timer.
- **Entries per delivered interrupt**, counted. One injection should
  cost one entry, not a spin in which the host re-offers a vector the
  guest never takes — which is precisely today's behaviour, and the
  count makes the difference visible rather than asserted.
- **List-register occupancy** across the tests, once step 6 uses more
  than one: how many of the implementation's LRs were ever live tells
  whether the underflow path is reachable in this tree at all, and
  therefore whether it is worth the maintenance-interrupt machinery.

## Risks

- **Every mistake is in EL2 assembly.** This is the code that already
  produced this project's worst bug — a stale `SP_EL2` written into a
  freed page, which corrupted whatever allocated that frame next and
  presented as an undefined instruction in an unrelated process. Page
  poisoning is permanent in debug builds now and should stay on for this
  work; the same instrument would find the same class of error again.
- **QEMU's vGIC is not hardware.** The list-register count from
  `ICH_VTR_EL2`, the exact `ICH_VMCR_EL2` semantics, and whether
  priority masking is enforced against `ICH_LR<n>.Priority` are all
  places QEMU may be more permissive than silicon. The unit can claim a
  guest that takes interrupts under QEMU's GICv3, not one proven on an
  ARM machine — and should say so in as many words.
- **The guest fixture is a new class of code.** It runs at EL1 inside a
  guest with its own vector table, and a mistake in it — a bad `VBAR`
  alignment, a forgotten `isb` after enabling the interface — looks
  exactly like a hypervisor bug. Build it in the smallest steps that can
  be observed: a guest that only enables the interface and exits;
  then one that unmasks and exits; then one that handles.
- **The new tests do not run in the default chain shape.** GICv2 is
  QEMU's default and the vGIC is GICv3-only, so `el2-guest-irq` and its
  siblings run in the `QEMU_GIC=3` steps. A feature exercised only in a
  non-default configuration is a feature that rots; `el2-vgic-decline`
  running in the default shape is the mitigation, and it is a weak one.
  Worth considering — separately, and on its own evidence — whether the
  chain's aarch64 default should become `gic-version=3` now that both
  drivers are tested.
- **Widening the injectable range can break x86.** Making the range
  arch-supplied rather than editing the constant is the mitigation, and
  the x86 test that would catch it is named above.
- **A guest that takes interrupts can loop taking them.** With no
  distributor and no rate limit, a test that injects on every exit and a
  guest that EOIs immediately will spin. `vcpu_run`'s `max_intr` bound
  already exists for the host-interrupt case; the tests should use it,
  and the bound's meaning should be checked rather than assumed to cover
  this.

## Alternatives considered

- **Deliver an IRQ as an injected exception**, reusing
  `vcpu_inject_exception`. Wrong, not merely inelegant: an IRQ is
  maskable and prioritised, an exception is neither. A guest could not
  mask it with `PSTATE.I`, could not prioritise it, and would have no
  `ICC_IAR1_EL1` to acknowledge — so `el2-guest-irq-masked` could not
  pass, which is why that test is in the list.
- **Emulate a virtual distributor first**, so guests can run an
  unmodified GIC driver. That is the right eventual goal and the wrong
  first step: it is several times the work, it needs stage-2 MMIO
  trapping of `GICD` and every `GICR`, and none of it is required for
  the hypervisor's own injection interface to stop lying. Doing the list
  registers first also produces the state the distributor emulation
  would have to drive anyway.
- **Do GICv2's `GICH` first, because the default chain runs GICv2.**
  Tempting for exactly one reason — test coverage in the default shape —
  and rejected for three given in §2. The coverage concern is real and is
  recorded as a risk rather than dismissed.
- **Do nothing, and document that AArch64 guests take no interrupts.**
  Defensible only if the hypervisor is decoration. It is not: it has a
  handle type, a user-space API, stage-2 translation, an EL2 world
  switch and sixteen self-tests, six of them AArch64 guests. Leaving
  `vcpu_inject` returning 0 for an operation that does nothing is the one
  option that makes the tree actively misleading.
- **Wait for the virtual timer unit and do both together.** The timer
  needs this; this does not need the timer. Combining them would make
  one commit that changes the switch, the GIC, the generic API and the
  timer model at once — and the last three units all found that the
  bugs live exactly where two such changes meet.
