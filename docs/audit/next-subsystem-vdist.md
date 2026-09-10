# NEXT SUBSYSTEM — the virtual distributor: a guest that can run a stock GIC driver

## Problem

A guest can be interrupted and can keep time, and it still cannot run an
operating system, because it cannot **initialise its interrupt
controller**. Every GIC driver ever written begins by reading
`GICD_TYPER` to learn how many interrupt lines there are; on this
hypervisor that read faults out to the owner and nothing answers it.

Measured, with a guest whose second instruction reads `GICD_TYPER` at
QEMU's `virt` distributor base:

```text
EXPERIMENT: guest read GICD_TYPER -> exit kind 3 gpa 0x8000004 write 0
```

Exit kind 3 is `COSMO_VM_EXIT_MMIO`: the guest touched a
guest-physical address with no region, the fault came out to the owner,
and there is no distributor behind it. A real guest kernel gets exactly
this on its first controller access and goes no further.

The hand-written guests in `tests/hv/aarch64/` work only because they
sidestep the distributor entirely: `guest_irq.S` and `guest_timer.S`
program `ICC_SRE_EL1`, `ICC_PMR_EL1` and `ICC_IGRPEN1_EL1` by hand and
never read a `GICD` or `GICR` register. That is not what a guest kernel
does, and it is not something a guest kernel can be asked to do — it is
a fixture written against this exact hypervisor. **Nothing that probes a
GIC the ordinary way can boot here.**

## Current implementation

**The CPU interface is virtualised; the distributor is not.** The vGIC
unit gave each vCPU a virtual CPU interface: `ICH_LR<n>_EL2` deliver
interrupts, `ICC_IAR1_EL1`/`ICC_EOIR1_EL1` acknowledge them, and
`arch_hv_vcpu_set_irq` places one in a list register. The virtual-timer
unit feeds the guest's timer PPI through that path. Both are the
*delivery* end. What decides *which* interrupts exist, whether each is
enabled, at what priority, and to which CPU it is routed — the
distributor and the per-CPU redistributors — has no emulation at all.

**A guest's distributor access is a stage-2 fault.** `vm_mem_add` maps
guest RAM at the GPAs the owner asks for; the distributor's GPA
(`0x0800_0000` on `virt`) is not among them, so a load or store there
takes a stage-2 data abort, which `decode_exit` turns into
`HV_EXIT_MMIO` with the faulting GPA (`hv_el2.c:786`). The owner is told
the address and the direction and nothing more; the kernel holds no GIC
state for it to consult.

**SGIs from a guest are not trapped.** `ICH_HCR_EL2` is set to `En`
alone (`hv_el2.c:454`) — no `TC`, the bit that traps a guest's
`ICC_SGI1R_EL1` writes to EL2. So a guest that tries to send an
inter-processor interrupt to its own vCPUs writes `ICC_SGI1R_EL1` and
the write is not intercepted and not turned into a virtual interrupt on
the target. A single-vCPU guest never notices; a guest with more than
one vCPU cannot IPI itself, which is to say it cannot run an SMP kernel.

**The redistributors do not exist for a guest either.** A guest
configures its per-CPU interrupts — the timer PPI among them — through
`GICR` registers, and those are as absent as `GICD`. The timer PPI
reaches the guest today only because the host delivers it through the
list-register path directly, not because the guest enabled it in a
redistributor it can see.

## Why it matters

- **It is the one thing between this hypervisor and a real guest.**
  Stage-2 translation, a virtual CPU interface, LPI/vGIC delivery, a
  per-VM clock and a virtual timer are all in place. A guest kernel that
  reached its first scheduler tick would need every one of them — and
  would never reach it, because it cannot get past `gic_init`. This is
  the gating item, and it is the last gating item.
- **The interrupt guests we have prove delivery, not usability.**
  `el2-guest-irq` shows an interrupt can be delivered, but only to a
  guest hand-built to receive it without a distributor. Nothing shows a
  guest driving its own controller, because nothing can.
- **The SGI gap is a correctness hole waiting for a second vCPU.** The
  moment a guest kernel with SMP support runs — which is the point of
  having per-VM vCPUs at all — its `ICC_SGI1R_EL1` writes go nowhere,
  and it hangs waiting for secondary CPUs that never get their startup
  IPI. It is untested today only because no guest has tried.
- **A virtual distributor is where a guest's device interrupts will
  land.** Emulated devices (a virtio-console, a disk) raise SPIs, and an
  SPI is a distributor concept: without a distributor there is nowhere
  for a device model to route an interrupt to. So this unit is also the
  prerequisite for giving a guest any interrupting device at all.

## Proposed design

### 1. An in-kernel distributor, not an owner-side one

The distributor's state is bound to the vGIC's: deciding an SPI is
pending and enabled and targeted at a vCPU has to end in a write to that
vCPU's `ICH_LR<n>_EL2`, which is EL2 state the switch owns and the owner
cannot touch. Emulating the distributor in the owner (userland) would
mean every pending-state change crossing back into the kernel to reach a
list register — a round trip per interrupt. So the distributor lives
**in the kernel**, next to the vGIC that delivers from it. This is the
choice KVM made for the same reason, and it is the shape the vGIC unit
already set up: the list registers are kernel state, and the thing that
fills them belongs beside them.

The owner keeps the role it has — it runs the vCPU and pumps exits — and
gains nothing to emulate. A guest's `GICD`/`GICR` access no longer
becomes an `HV_EXIT_MMIO` it must answer; the kernel answers it.

### 2. Trap the distributor's MMIO through stage-2

`GICD` and `GICR` stay unmapped in stage 2, exactly as now, so a guest's
access still faults to EL2 — but the backend recognises the GPA as the
distributor's and emulates the access instead of returning
`HV_EXIT_MMIO`. The register is decoded from the faulting address (the
offset within the `GICD`/`GICR` window) and the access size and
direction from `ESR_EL2`, the same fields `decode_exit` already reads.

Two GPAs are fixed by convention and told to the guest anyway (a guest
learns them from its device tree or, here, from the same ACPI the host
parsed): `GICD` at `0x0800_0000`, and a `GICR` window at
`0x080A_0000` with a 128 KiB stride per vCPU — the layout QEMU's `virt`
uses and the one the host's own driver already knows.

### 3. What state the distributor holds

Per interrupt (SGIs 0–15, PPIs 16–31, SPIs 32 up to the number the
distributor advertises): an **enable** bit, a **priority**, a **group**,
a **pending** bit, and for SPIs a **target** (an affinity, from
`GICD_IROUTER`). SGIs and PPIs are private to a CPU, so their state is
per-vCPU (in the redistributor); SPIs are shared (in the distributor).
This is a few words per interrupt and a small fixed array, not a
data structure that grows.

The registers that read and write this state are the bulk of the work
and most of them are mechanical: `GICD_ISENABLER`/`ICENABLER`,
`GICD_ISPENDR`/`ICPENDR`, `GICD_IPRIORITYR`, `GICD_IGROUPR`,
`GICD_IROUTER`, `GICD_CTLR`/`TYPER`/`IIDR`, and their `GICR` equivalents
for the private range. Each is a bitfield over the per-interrupt state
above.

### 4. Routing: the distributor fills the vGIC

When an interrupt becomes both pending and enabled and its target is a
given vCPU, the distributor asks the vGIC to make it pending on that
vCPU — the same `set_irq` path the owner's `vcpu_inject` and the timer
already use. The vGIC unit's rule that a list register holds one
interrupt across entries, and that "taken" means the guest acknowledged
it, is unchanged; the distributor is a new *source* of pending
interrupts, not a new delivery path. When the guest writes `ICENABLER`
or `ICPENDR` to take one back before it is taken, the distributor clears
it from the list register.

### 5. SGIs: trap `ICC_SGI1R_EL1` and route by affinity

`ICH_HCR_EL2` gains `TC`, so a guest's `ICC_SGI1R_EL1` write traps to
EL2 as a system-register exit. The backend decodes the target affinity
and SGI number the same way the host's own `gicv3.c` composes them, and
makes that SGI pending on each targeted vCPU's redistributor — which
routes it into that vCPU's list register by §4. This is what lets a
guest bring up its secondary CPUs, and it is the piece that turns the
per-VM multi-vCPU support into something a guest can use.

### 6. Deliberately out of scope

- **LPIs and a guest ITS.** A guest that wants MSI is a guest with
  emulated PCIe, which this tree does not give a guest yet; the SPI/
  PPI/SGI distributor is what a guest needs to boot, and the ITS is a
  later unit if a guest ever gets a virtual PCIe root.
- **A GICv2 guest distributor.** GICv3 only, matching the vGIC unit and
  for the same reason: the guest-visible interface differs, and no
  machine this runs on has a hypervisor use for GICv2.
- **Device SPIs.** Nothing in this tree raises an SPI into a guest yet,
  because there is no emulated device that would. The distributor will
  accept a pending SPI (the mechanism), but the *source* — a virtio
  device model — is its own unit. This one is tested with SGIs and the
  timer PPI, which need no device.

## Affected files

| file | change |
|---|---|
| `kernel/arch/aarch64/gicv3_vdist.c` | **new**: the emulated `GICD`/`GICR`, the per-interrupt state, the MMIO decode, the routing into the vGIC |
| `kernel/arch/aarch64/include/aarch64/gicv3_vdist.h` | **new**: its interface to the backend |
| `kernel/arch/aarch64/hv_el2.c` | recognise `GICD`/`GICR` GPAs on a stage-2 abort and call the emulator; `ICH_HCR_EL2.TC`; the `ICC_SGI1R_EL1` trap; a per-VM distributor instance |
| `kernel/arch/aarch64/hv_el2_switch.S` | nothing new for the LR path; `TC` is a bit in the `vgic_hcr` already moved |
| `kernel-services/virtualization/vcpu.c` | a guest MMIO to a distributor GPA is handled in the kernel, not returned as `HV_EXIT_MMIO` |
| `tests/hv/aarch64/guest_gic.S` | **new**: a guest that initialises its GIC the ordinary way and takes an interrupt through it |
| `kernel-services/virtualization/hvtest.c` | the tests below |
| `docs/kernel/arch/aarch64/design.md`, `invariants.md`, `docs/kernel-services/virtualization/` | the virtual distributor, the SGI routing, what a guest may and may not find |

## New APIs

Internal, all AArch64-side. `struct arch_hv_vm` gains a distributor
instance; a handful of functions between `hv_el2.c` and the new
`gicv3_vdist.c`:

```c
struct gicv3_vdist;
struct gicv3_vdist *vdist_create(unsigned nr_vcpus);
void vdist_destroy(struct gicv3_vdist *d);

/* A guest access to GICD/GICR: emulate it, or false if the GPA is not
 * the distributor's. `val` is written on a read and read on a write. */
bool vdist_mmio(struct gicv3_vdist *d, unsigned vcpu, uint64_t gpa,
                unsigned size, bool write, uint64_t *val);

/* A guest's ICC_SGI1R_EL1 write, decoded to a target set and an SGI. */
void vdist_sgi(struct gicv3_vdist *d, uint64_t sgi1r);

/* The INTID this vCPU should be given next, or -1: the highest-priority
 * pending-and-enabled interrupt targeted at it. Drives the vGIC. */
int vdist_pending_for(struct gicv3_vdist *d, unsigned vcpu);
```

`arch/hv.h`'s vcpu operations do not change: a distributor interrupt
reaches the guest through the vGIC like any other.

## Migration plan

1. **Decode and identify, alone.** Trap `GICD`/`GICR` MMIO and answer
   the read-only identification registers (`TYPER`, `IIDR`, `PIDR`)
   and accept writes to `CTLR`; everything else reads as zero. A guest
   gets past `gic_init`'s probe and its first configuration writes do
   not fault, though nothing is delivered yet. Verifiable by a guest
   that reads `TYPER` and reports the line count it saw.
2. **Per-interrupt state.** `ISENABLER`/`ICENABLER`, `IPRIORITYR`,
   `IGROUPR`, `ISPENDR`/`ICPENDR`, `IROUTER`, read back correctly. No
   routing yet; the state is just stored and returned. A guest can
   configure its controller and read its own configuration back.
3. **Routing into the vGIC.** A pending, enabled, targeted interrupt is
   made pending on the vGIC of its target vCPU; a cleared one is
   withdrawn. The timer PPI now reaches the guest *through* its
   redistributor rather than by the host's direct injection — the guest
   enables PPI 27 in `GICR_ISENABLER0` and takes it.
4. **SGIs.** `ICH_HCR_EL2.TC`, the `ICC_SGI1R_EL1` trap, routing to
   target vCPUs. A two-vCPU guest sends an SGI from one to the other and
   the other takes it.
5. **A whole-GIC guest.** A fixture that initialises the distributor and
   its redistributor the ordinary way, enables the timer, and services
   a tick — the "a guest can run a stock GIC driver" test.
6. Docs, and the decision on where the redistributor stride and base
   come from (ACPI, as the host reads them, versus a fixed convention).

Steps 3 and 4 are separate commits: "an SPI/PPI the guest configured is
delivered" and "an SGI the guest sent is delivered" are different claims.

## Tests

- **`el2-guest-gicd-probe`** — a guest reads `GICD_TYPER` and `IIDR` and
  reports them; the line count matches what the distributor advertises,
  and the read does not fault to the owner. Fails today, where the read
  is `HV_EXIT_MMIO`.
- **`el2-guest-gic-config`** — a guest enables an interrupt, sets its
  priority, and reads both back through `GICD`; the values it reads are
  the values it wrote. This is the register file, checked against
  itself.
- **`el2-guest-gic-timer`** — a guest enables the timer PPI in its
  redistributor, arms `CNTV`, and takes the tick through the controller
  it configured — not through a hand-set `ICC_IGRPEN1_EL1`. This is the
  test that a guest's own GIC initialisation actually works end to end,
  and it subsumes what `el2-guest-timer` proves about a hand-built one.
- **`el2-guest-sgi`** — a two-vCPU guest sends an SGI from vCPU 0 to
  vCPU 1 with `ICC_SGI1R_EL1`; vCPU 1's handler runs and reports the SGI
  number. Fails before step 4, where the write is not trapped and vCPU 1
  never sees it. This is the test that a guest can be SMP.
- **`el2-guest-gicd-isolated`** — a guest's distributor writes change
  its VM's distributor and no other's, and touch no host GIC state
  (the host runs on the physical distributor; a guest's `GICD_CTLR`
  write must not reach it). The isolation invariant, in the shape the
  timer and vGIC units established.

Each with the bug-proof this project expects: reintroduce, watch the
named test fail for the stated reason, restore, verify the tree clean.
The lesson the last unit paid for applies here directly — the state
tests (config, isolation) need no delivery and should run on both GIC
machines; the delivery tests (timer, sgi) need the vGIC and run under
`QEMU_GIC=3`. Getting that split right is what the vtimer unit's GICv2
hang taught, and it is easy to get wrong the same way.

## Benchmarks

Counted, not timed, as every unit since the ASID one has been:

- **Distributor registers a guest can use: none today, the SPI/PPI/SGI
  file after.** The headline, and it needs no clock.
- **Exits per guest GIC initialisation.** A distributor access is a
  trap, and a guest's `gic_init` makes dozens; counting them says how
  much a guest pays to bring its controller up, and whether any
  register is hot enough to be worth a fast path (most are touched
  once). This is the number that would justify — or refuse — caching
  anything.
- **SGIs delivered per second between two vCPUs**, counted rather than
  timed: not a rate to boast about under TCG, but the count confirms
  none are lost, which is the property an SMP guest depends on.

## Risks

- **This is the largest single piece of the hypervisor.** The vGIC and
  the timer were each a handful of registers; a distributor is the whole
  GICv3 programming model. The mitigation is the migration plan: identify
  first, then state, then routing, then SGIs, each a commit that boots
  and is tested before the next. A distributor that answers `TYPER` and
  nothing else is a real, testable step.
- **The register decode is broad and every offset is a chance to be
  wrong.** `GICD` is a 64 KiB window of banked and per-INTID registers,
  and mis-decoding one silently mis-configures an interrupt. The
  `el2-guest-gic-config` test — write through one register, read back
  through another — is the guard, and it should cover each register
  class, not one representative.
- **Shared distributor state, per-vCPU redistributor state.** The SPI
  bits are the VM's and the SGI/PPI bits are a vCPU's, and confusing the
  two is exactly the class of bug the vGIC unit's active priorities and
  the timer unit's offset both were. The lesson is fresh: every piece of
  state gets an owner named up front, and a test that a second vCPU sees
  its own.
- **The SGI trap changes a hot path.** `ICH_HCR_EL2.TC` traps *every*
  `ICC_SGI1R_EL1`, and a guest that sends many IPIs takes an exit each.
  That is correct and unavoidable for routing, but it is a per-IPI exit,
  and the exit count above is where its cost shows. No guest in this
  tree sends enough for it to matter yet; recorded so it is not a
  surprise when one does.
- **QEMU is not hardware.** The redistributor layout, the `TYPER` fields
  and the exact set of registers a real guest touches are places QEMU's
  `virt` and a real machine can differ. The unit can claim a guest that
  drives QEMU's GICv3 as a guest sees it, not one proven against silicon
  — and the fixture is a guest *this* tree wrote, not a stock Linux, so
  "a stock GIC driver" is a claim about the programming model the fixture
  exercises, not a Linux boot. Booting Linux is a later, larger thing.

## Alternatives considered

- **Emulate the distributor in the owner (userland).** The natural place
  for device emulation, and wrong here: the distributor's output is a
  list-register write, which is EL2 state, so every pending change would
  round-trip into the kernel. The one device whose state the kernel
  already holds is the one device to emulate in the kernel.
- **A paravirtual interrupt interface** — a hypercall ABI a guest calls
  instead of driving a GIC. Less work, and it buys nothing this tree
  wants: it would need a guest modified to use it, which defeats the
  purpose of emulating hardware, and this project's own kernel is the
  first guest it would want to run unmodified.
- **Do a GICv2 distributor, because it is simpler.** The GICv2
  distributor is a smaller register file, but a GICv2 guest also needs a
  `GICC` MMIO frame the vGIC unit deliberately did not build, and the
  guest-visible interface would then differ from everything already
  done. The simplicity is on the wrong side of the seam.
- **Do nothing, and keep hand-writing guests.** Defensible only if the
  hypervisor is a fixture-runner. Every guest fixture that sidesteps the
  distributor is a guest that proves a mechanism and not a capability;
  at some point the thing being built has to be able to run software
  written for the architecture rather than for it.
