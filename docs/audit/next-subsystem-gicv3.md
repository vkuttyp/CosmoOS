# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is the tenth such report
(the NIC, the USB host stack, AHCI, the console, floating point, signals,
job control, terminal modes, and address-space tags; all nine record
their outcomes). Nothing in this one is implemented.

**Subsystem: GICv3 — the interrupt controller of every ARM machine built
since about 2015, and the eight-CPU ceiling GICv2 imposes on this
kernel.**

## Problem

Two problems that are the same problem.

**This kernel refuses to boot on modern ARM hardware.** `arch_irqc_init`
reads the GIC version out of the MADT and then:

```c
if (gic.version != 0 && gic.version != 2)
    panic("gic: distributor version %u; only GICv2 is implemented", gic.version);
```

Every ARMv8 server part, every recent SoC, and QEMU's own `virt` machine
from `gic-version=3` upward reports 3 or 4 there. The kernel does not
mis-handle them; it stops.

**And on AArch64 it can never use more than eight CPUs.** That is not a
policy of this kernel — `CONFIG_MAX_CPUS` is 64, `ACPI_MAX_CPUS` is 64,
`cpumask_t` is a `uint64_t` — it is GICv2's structure. A CPU is named to
the distributor by a bit in an eight-bit field, which appears in this
tree as one line in the IPI path:

```c
gicd_wr(GICD_SGIR, ((uint32_t)g_cpu_iface_mask[cpu] << 16) | (uint32_t)sgi);
```

and again in routing, `gicd_wr8(GICD_ITARGETSR + intid, g_cpu_iface_mask[cpu])`.
Eight bits, eight CPUs. The emulator agrees, and refuses before the
kernel gets a chance to:

```text
$ qemu-system-aarch64 -machine virt,gic-version=2 -smp 9
qemu-system-aarch64: Number of SMP CPUs requested (9) exceeds max CPUs
                     supported by machine 'mach-virt' (8)
```

The same command with `gic-version=3` and `-smp 16` starts without
complaint. So the ceiling is not merely unreached: **on this
architecture it cannot even be measured.** Every scalability claim the
tree makes about AArch64 — the address-space tag unit's included — is a
claim about at most eight CPUs, and the audit's remaining ceilings (one
TCP lock, one RX worker, one cosmofs lock) cannot be exercised past that
point on this architecture at all.

There is a third consequence, smaller but real: **every device interrupt
in the machine is delivered to CPU 0.** The API has taken a CPU since it
was written — `irq_request(gsi, fn, arg, name, flags, cpu)` — and every
caller in the tree passes `0`. Spreading them is a policy change, not a
mechanism change, but it is pointless while the machine has eight CPUs
and one of them takes every interrupt.

## Current implementation

`kernel/arch/aarch64/gic.c` is a GICv2 driver in about 450 lines: a
memory-mapped distributor (`GICD`) and CPU interface (`GICC`), SGIs
0..15 bound lazily to IPI vectors, and **GICv2m for MSI** — a frame that
turns a memory write into one of a contiguous block of SPIs. The boot
log shows it in use: `v2m (SPIs 80+64)`, and `virtio-pci` and `nvme`
take their interrupts through it (`msi`, `msix`).

The interface above it is already version-neutral. `arch/irqc.h` speaks
GSIs, vectors and CPU indices — `arch_irqc_route`, `_mask`, `_unmask`,
`_eoi`, `arch_irqc_msi_compose`, `arch_ipi_send`,
`arch_ipi_broadcast_others` — and says so: "controller registers never
appear above this line". No generic code knows what a distributor is.
That seam is what makes this unit tractable; it is the same seam the
x86-64 side sits behind with its APIC.

**What the ACPI parser already provides**, which is more than it looks:

| | |
|---|---|
| `struct acpi_gic.version` | already read from the GICD entry (`p[20]`): 0, 2, 3 or 4 |
| MPIDR affinity | already captured per CPU from GICC offset 68, stored as `apic_id` — *exactly what GICv3 needs to target an SGI* |
| GICR base | documented in the GICC parser's own comment (offset 60) and not stored |
| MADT entry types | GICC (11), GICD (12), GIC MSI frame (13). **Not** GICR (14), **not** ITS (15) |

So the kernel already knows it is looking at a GICv3 and already knows
each CPU's affinity; what it lacks is the redistributors, the
system-register interface, and an MSI path that is not GICv2m.

**What QEMU offers**, which decides what can be tested:
`-machine virt,gic-version={2,3,4,x-5,host,max}` and
`msi={auto,gicv2m,its,off}`, plus `highmem-redists` for the
redistributor window.

## Why it matters

The last unit removed one of the six scalability ceilings the audit
named, and could only demonstrate it on a four-CPU machine. This is the
ceiling that decides whether any of the others can be demonstrated at
all: past eight CPUs, AArch64 does not run. It is also the difference
between a kernel that boots on the machines people have and one that
boots on a machine QEMU can still be asked to pretend to be.

It is second in the constitution's §69 order after the scalability work
this continues, and the AArch64 design document has listed it under
"Future extensibility" since the port was written — "GICv3
(system-register interface, redistributors, ITS for MSI) behind the same
`arch/irqc.h`". This report proposes taking that sentence at its word.

## Proposed design

### 1. The architectural question: what replaces GICv2m

The distributor and CPU interface port straightforwardly — the
system-register interface (`ICC_*_EL1`) is simpler than the MMIO one,
and redistributors are per-CPU versions of what `GICD` did for banked
interrupts. The question is MSI, because **the MSI path is not optional
here**: `virtio-pci` and `nvme` already use MSI-X on AArch64 through
GICv2m, so whatever replaces it must work on day one or those devices
lose their interrupts.

Three answers:

**(a) Keep GICv2m under GICv3.** QEMU will do it (`msi=gicv2m` with
`gic-version=3`), and it is nearly no work: the existing frame code
keeps running. But it is an emulator convenience, not a hardware one —
GICv2m frames are rare on GICv3 silicon, which is the hardware this unit
exists to support. It would produce a kernel that boots on real GICv3
machines and finds no MSI there. Rejected as the design; **kept as the
fallback** when the MADT describes a frame and no ITS (§4).

**(b) Message-Based Interrupts (`GICD_SETSPI_NSR`).** GICv3 lets a
device write an SPI number to a distributor register. No translation
tables, no LPI configuration — much less machinery than an ITS. But MBI
is optional in the architecture, QEMU's `virt` does not offer it, and it
keeps every MSI in the SPI space, which is the space that does not
scale. Rejected.

**(c) The ITS, with LPIs.** Proposed. It is what GICv3 hardware and
QEMU's `virt` both provide (`msi=its`, the default when `gic-version=3`),
it is how a device's MSI reaches an arbitrary CPU without an SPI, and it
is the only one of the three that still works on a machine with more
CPUs than SPIs. The cost is real and should be stated plainly: an ITS
needs a command queue, device tables, interrupt-translation tables per
device, collection tables per CPU, and a `MAPD`/`MAPTI`/`INVALL` command
dance at device-attach time. It is the largest single piece of this
unit.

**And it needs something the MSI interface does not currently carry: the
device's identity.** `MAPD` and `MAPTI` are per-device commands keyed by
a DeviceID -- on PCIe, the requester id -- and today
`irq_request_msi(fn, arg, name, cpu, &msg)` and
`arch_irqc_msi_compose(vector, cpu, addr, data)` know only a vector and a
CPU. The PCI layer *has* the device at the call site (`pci_msix_request`
holds `p`) and drops it. Without plumbing it through, an ITS backend
cannot give two devices distinct translations, and `gic-its-map` below
could not be written. So this unit changes the MSI interface, which is
the one part of `arch/irqc.h` that does have to move: `msi_compose`
gains a device id, x86-64 ignores it, and the PCI layer computes it from
bus:device:function.

### 2. Two drivers behind one seam

`arch/irqc.h` does not change. `kernel/arch/aarch64/gic.c` keeps the
GICv2 driver; `gicv3.c` is new; a small `irqc.c` picks between them from
`acpi_gic.version` at `arch_irqc_init` and dispatches through a
`struct aarch64_irqc_ops`. This is the shape the hypervisor already uses
for SVM and VMX, and it is the shape that keeps GICv2 machines working —
QEMU's `virt` still offers `gic-version=2`, and the chain should keep
testing it.

Not a rewrite of `gic.c`. The GICv2 path stays exactly as it is, so a
regression in this unit cannot break the configuration everything else
is tested on.

### 3. What each piece needs

- **Distributor.** Mostly shared: `GICD_CTLR` gains affinity routing
  (`ARE_NS`), `GICD_IROUTER` (a 64-bit affinity per SPI) replaces the
  eight-bit `GICD_ITARGETSR`, and the register file is otherwise
  familiar.
- **Redistributors.** One per CPU, found from the GICR MADT entry or the
  GICC entry's GICR base (already in the parser's comment, not yet
  stored). SGIs and PPIs are configured there rather than in the
  distributor. Each has a `GICR_WAKER` handshake to bring the CPU's
  interface out of sleep.
- **CPU interface.** System registers: `ICC_SRE_EL1` to select them at
  all, `ICC_PMR_EL1`, `ICC_IGRPEN1_EL1`, `ICC_IAR1_EL1` / `ICC_EOIR1_EL1`
  where the MMIO `GICC_IAR`/`GICC_EOIR` were.
- **SGIs.** `ICC_SGI1R_EL1`, targeted by affinity — Aff3.Aff2.Aff1 plus a
  sixteen-bit target list over Aff0. The affinity is already parsed. This
  is where the eight-CPU cap disappears.
- **LPIs and the ITS.** Property and pending tables, a command queue,
  `MAPD` per device, `MAPTI` per interrupt, `MAPC` per collection (one
  per CPU), `SYNC`. `arch_irqc_msi_compose` returns the ITS translator
  address and the device's event id instead of a v2m frame address.

### 4. Choosing at boot, and what to do when there is no ITS

`arch_irqc_init` reads the version and dispatches. Within GICv3, MSI has
a fallback order: an ITS if the MADT describes one; otherwise a GICv2m
frame if it describes one (the existing code, unchanged); otherwise
`arch_irqc_msi_compose` returns `-ENODEV`.

**And then the machine loses its disks**, which is worth stating rather
than discovering: there is no INTx fallback in this tree. NVMe treats a
failed MSI-X request as a failed probe (`goto fail_msix`), and AHCI says
so in as many words -- `"neither MSI-X nor MSI (%d); INTx is not
driven"`. So `msi=off` is not a configuration this kernel supports
today, and a boot in it would lose the very device markers the boot test
requires. Two consequences: the no-MSI path is a decline, not a
fallback, and it cannot be a chain step; and **adding INTx to those
drivers is a separate unit**, recorded here as the gap it is rather than
smuggled into this one.

### 5. Interrupt affinity, which becomes possible here

With `GICD_IROUTER` and an ITS collection per CPU, routing a device
interrupt somewhere other than CPU 0 costs one register write. The
mechanism belongs in this unit; the *policy* — which CPU gets which
device — does not, and is named as a follow-up. What this unit should do
is stop hard-coding `0` at the call sites and let `irq_request` place
interrupts round-robin across online CPUs, which is one line and
measurable.

## Affected files

| File | Change |
|---|---|
| `kernel/arch/aarch64/gicv3.c` (new) | distributor, redistributors, system-register CPU interface, SGIs by affinity |
| `kernel/arch/aarch64/gic_its.c` (new) | command queue, device/collection/translation tables, LPI configuration |
| `kernel/arch/aarch64/irqc.c` (new) | version detection and `struct aarch64_irqc_ops` dispatch |
| `kernel/arch/aarch64/gic.c` | unchanged behaviour; its entry points become the v2 ops table |
| `kernel/arch/aarch64/include/aarch64/sysreg.h` | `ICC_*_EL1` accessors |
| `drivers/acpi/acpi.c`, `kernel/include/kernel/acpi.h` | GICR (14) and ITS (15) entries; store the GICC redistributor base; expose them on `struct acpi_gic` |
| `kernel/interrupt/irq.c` | place interrupts across CPUs instead of every caller passing 0 |
| every `irq_request` caller | stop passing a literal 0 |
| `scripts/qemu-run.sh` | `QEMU_GIC` selecting `gic-version` and `msi=` |
| the verify chain | `boot gicv3 aarch64`, `boot gicv3-nots aarch64`, `boot smp16 aarch64` |
| docs | `docs/kernel/arch/aarch64/design.md` (the GIC section and its future-work line), `invariants.md`, `docs/kernel/interrupt/*`, the audit's GICv3 and 8-CPU rows |

## New APIs

No UAPI change; no change to `arch/irqc.h`, which is the point.

```c
/* aarch64-private */
struct aarch64_irqc_ops {           /* what gic.c and gicv3.c each provide */
    void (*init)(const struct acpi_gic *);
    void (*init_cpu)(void);
    int  (*route)(unsigned gsi, unsigned vector, unsigned cpu, unsigned flags);
    int  (*mask)(unsigned gsi), (*unmask)(unsigned gsi);
    void (*eoi)(unsigned vector);
    int  (*msi_compose)(unsigned vector, unsigned cpu, uint32_t devid,
                        uint64_t *addr, uint32_t *data);
    void (*ipi_bind)(unsigned vector);
    void (*ipi_send)(unsigned cpu, unsigned vector);
    void (*ipi_broadcast_others)(unsigned vector);
};

/* The MSI interface gains the device's identity, which an ITS needs for
 * MAPD/MAPTI and every other backend ignores. The PCI layer computes it
 * from bus:device:function; x86-64 and GICv2m discard it. */
int irq_request_msi(interrupt_handler_fn fn, void *arg, const char *name,
                    unsigned cpu, uint32_t devid, struct irq_msi_msg *msg);
int arch_irqc_msi_compose(unsigned vector, unsigned cpu, uint32_t devid,
                          uint64_t *addr, uint32_t *data);

/* acpi.h additions */
struct acpi_gic {
    ...                             /* as today */
    paddr_t gicr_base;              /* GICR entry, or the GICC entry's field */
    uint64_t gicr_stride;
    paddr_t its_base;               /* 0 when the MADT describes no ITS */
};
```

## Migration plan

1. **ACPI first, alone.** Parse GICR and ITS entries, store the GICC
   redistributor base, log all three. Nothing uses them; GICv2 machines
   are unaffected. This is the step that can be verified by reading a
   boot log under `gic-version=3` — the kernel still panics, but it
   panics having printed what it found.
2. **The ops table**, with GICv2 as its only implementation. Pure
   refactor: identical behaviour, and the chain proves it.
3. **GICv3 with the *existing* MSI path, one CPU**: distributor, one
   redistributor, system-register interface, `-smp 1`, and
   `gic-version=3,msi=gicv2m` -- the v2m frame code unchanged. This is
   the better first step and it is Greptile's finding that produced it:
   `msi=off` would have lost NVMe and AHCI, which have no INTx path, so
   the first GICv3 boot could not have kept the boot test's device
   markers. Keeping v2m also isolates the distributor and CPU-interface
   work from the ITS work, so a failure in step 3 has one cause.
4. **SGIs by affinity, then SMP.** `-smp 4`, then `-smp 16` — the step
   that could not previously exist.
5. **The ITS.** The device id plumbed through `irq_request_msi` first
   (a mechanical change every backend but this one ignores), then the
   command queue, tables, `MAPD`/`MAPTI`/`MAPC` and LPI configuration;
   `msi_compose` returns the translator address and the event id.
   `msi=its` becomes the default GICv3 configuration, and `msi=gicv2m`
   stays as a chain step so the fallback keeps being exercised.
6. **Affinity policy and the docs sweep.**

Steps 3 and 4 are separate commits so that a bisect lands on "the
interrupt arrived" or "the interrupt arrived on the right CPU", not
both.

## Tests

The existing suite is the substance: every self-test in the tree depends
on interrupts, so `boot gicv3 aarch64` passing 196 self-tests is a
stronger statement than any new test. What is added is what that does
*not* cover:

- **Three chain steps, one per MSI path**: `boot gicv3 aarch64`
  (`gic-version=3,msi=its`), `boot gicv3-v2m aarch64`
  (`gic-version=3,msi=gicv2m`, the fallback), and the existing
  `gic-version=2` steps. The middle one exists because a fallback that
  nothing runs is a fallback that regresses -- and because `msi=off`,
  which an earlier draft proposed for it, exercises neither path and
  cannot boot at all (§4).
- **`boot smp16 aarch64`** — sixteen CPUs, which no configuration in
  this tree has ever booted. Reintroducing the eight-bit target mask
  fails it at CPU 8.
- **`gic-affinity`**: route a GSI to CPU N, raise it, and require the
  handler to run on CPU N. Today this passes vacuously (everything is
  CPU 0); under GICv3 it is the test that `GICD_IROUTER` and the ITS
  collections are right. It must fail when the route is forced to CPU 0.
- **`gic-ipi-affinity`**: an IPI to each CPU in turn, each acknowledged
  by that CPU and no other. With the SGI target list built from the
  wrong affinity fields, the interrupt lands on a CPU whose Aff0 happens
  to match — which a broadcast-shaped test would not notice.
- **`gic-its-map`**: attach two devices with *different* device ids, map
  an event each, and require each device's write to raise its own
  vector. This is the test that cannot be written without the device id
  reaching `msi_compose`. The failure mode this
  catches is a shared translation table, which delivers *an* interrupt
  and so looks fine until two devices are busy at once.
- **`msi-decline`**: with neither an ITS nor a frame in the MADT,
  `arch_irqc_msi_compose` reports `-ENODEV` rather than composing a
  message nothing will deliver. A unit test of the compose path, not a
  boot configuration -- booting that machine is what §4 says this tree
  cannot do until the drivers learn INTx.

The lesson the tag unit paid for applies directly here: a property about
two CPUs needs a test with two CPUs. `gic-affinity` and
`gic-ipi-affinity` are both written that way, with the handler recording
`arch_cpu_id()` and the test asserting on it.

## Benchmarks

Interrupt latency under TCG is not a real number — the same caution the
tag unit recorded, for the same reason. What can be measured honestly:

- **CPUs that boot**: 8 today, 16 (or more) after. That is the headline
  and it needs no timer.
- **Interrupts per CPU** over a boot, from the existing per-CPU counters:
  today every device interrupt is on CPU 0; after the affinity step it
  should be spread, and the distribution is the measurement.
- **`net-bench` and `net-nicbench` at 8 and 16 CPUs** — not to claim a
  speedup, but to see whether the remaining ceilings (one TCP lock, one
  RX worker) become visible once there are enough CPUs to contend. That
  is the real value of this unit to the ones after it.

## Risks

- **The ITS is the largest piece and the least forgiving.** Its tables
  have alignment and shareability requirements, the command queue needs
  a `SYNC` and a doorbell in the right order, and a mistake produces no
  interrupt at all rather than a wrong one. Mitigation: build it last,
  behind a machine configuration (`msi=off`) that works without it, so
  the unit is useful before the ITS lands.
- **Two drivers is a maintenance cost.** Accepted deliberately: GICv2
  machines exist, the chain tests them, and a single driver that tried
  to be both would be worse than two that are each simple. The `ops`
  table is the whole of the coupling.
- **`GICR_WAKER` and CPU bring-up order.** A redistributor left asleep
  delivers nothing to its CPU, and the symptom is a CPU that boots and
  then never takes an interrupt — easy to mistake for a scheduler bug.
  The `gic-ipi-affinity` test catches it directly.
- **Sixteen CPUs is new ground for everything else.** The first
  `-smp 16` boot will likely find bugs that have nothing to do with the
  GIC — in the scheduler, the shootdown mask, or the locks the audit
  already names. That is the point of the unit, but it means step 4 may
  uncover work that belongs to other subsystems, and those should be
  recorded rather than absorbed.
- **No driver in this tree falls back to INTx.** NVMe fails its probe
  when MSI-X setup fails and AHCI says so explicitly; virtio-pci is the
  same shape. So every GICv3 configuration this unit ships must provide
  a working MSI path from its first boot -- there is no degraded mode to
  fall back on while the ITS is built, which is why step 3 keeps GICv2m.
  Teaching those drivers INTx is a real and separable unit; it is
  recorded here, not attempted.
- **Sixteen CPUs may be too slow to test on this host.** Measured now, at
  the current maximum: four CPUs boot the test in ~60 s, eight in 78-85 s,
  and one run of two at eight failed on the recorded timer/host-load
  flake. TCG emulates each vCPU on a host thread, so sixteen plausibly
  lands at 120-160 s against a 180 s boot timeout -- a chain step that is
  slow when the machine is idle and red when it is not. The step is
  proposed anyway, because the ceiling is the point, but with two
  provisos: it should be measured before it is added to the chain, and if
  it cannot be made reliable it belongs in a named target that CI runs
  rather than in the default chain. Recording the number now so the
  decision is made on evidence rather than on the first red run.
- **QEMU is not hardware.** A GICv3 that works under TCG may still be
  wrong about `GICR_WAKER`, cache maintenance on the ITS tables, or
  affinity above Aff1, none of which QEMU models strictly. The report
  claims a kernel that boots on QEMU's GICv3, not one proven on silicon,
  and the outcome section should say so.

## Alternatives considered

- **Do nothing.** Defensible only while the tree is a QEMU project. The
  moment anyone runs it on an ARM machine bought in the last decade, it
  panics on line 107.
- **GICv3 without SMP above eight.** Would fix the boot-on-hardware half
  and leave the ceiling. Since the affinity SGI path is the smaller part
  of the work and the reason the ceiling exists, splitting them wastes
  the opportunity.
- **Raise `CONFIG_MAX_CPUS` and leave the GIC.** Changes nothing: the
  cap is the controller's, and QEMU refuses the command line before the
  kernel runs.
- **GICv4 (direct injection of virtual interrupts).** A real feature for
  the hypervisor, and out of scope: it needs vPE tables and a scheduling
  model for them. GICv3 is the prerequisite either way, and the EL2
  backend does not yet deliver virtual interrupts at all
  (`arch_hv_vcpu_set_irq` records the offer and never delivers it).
