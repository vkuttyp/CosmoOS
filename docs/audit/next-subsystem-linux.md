# NEXT SUBSYSTEM — booting Linux: the feature registers a guest reads, and the walls to a kernel's first breath

## Problem

The machine unit said the device tree would be right by this tree's reader
and might be wrong by Linux's, and named the Linux boot as the reader
whose opinion settles it. So I ran the reader. A stock arm64 Linux --
Alpine's `vmlinuz-virt`, gunzipped out of its EFI zboot container to a
raw `Image` -- loaded by its own header (`text_offset 0`, `image_size
0x20d0000`) into a 60 MiB VM and handed our device tree, runs **thousands
of exits** into CosmoOS and then stops:

```text
vmctl: /boot/tests/hv/Image: 34406400 bytes at 0x40000000 (Image header),
       60 MiB at 0x40000000, 1 cpu(s), device tree 1419 bytes at 0x42200000
vmctl: cpu 0: system register read (iss 0x30002b) into x1 at 0x412a476c:
       no model; stopping
```

ISS `0x30002b` decodes to `S3_0_c0_c5_0` -- **`ID_AA64DFR0_EL1`**, the
debug feature register. `HCR_EL2.TID3` traps *every* ID-register read to
the owner, and the owner models none: `vmctl` prints "no model; stopping"
and exits. The kernel's own `el2-guest-sysreg` test answers one such read
(`ID_AA64PFR0_EL1`) by hand, to prove the trap works; the real owner
answers nothing.

So the device tree was right enough to get Linux past its own header, its
memory node, its CPU node and its PSCI probe -- into the point where a
kernel reads the processor's feature registers to decide what it is
running on. That is the first wall, and there will be more behind it. The
last unit built the machine; this one is the first that runs software
written for the machine rather than for the hypervisor, and finds out
what that costs.

## Current implementation

**The traps are set for a hand-written guest, and answered for one.**
`ctx_reset` in `hv_el2.c` sets `HCR_EL2` to `VM | RW | IMO | FMO | AMO |
TWI | TWE | TID3 | TSC`. `TID3` traps ID-register reads, `TSC` traps
`SMC`, `TWI`/`TWE` trap `WFI`/`WFE`. Each trap is a `SYSREG` or `WFI` or
`HYPERCALL` exit to the owner. The owner in the tests answers exactly the
registers a fixture reads; `vmctl` answers none of them -- it prints and
stops. A real kernel reads dozens of ID registers before it finishes
`setup_arch`, and every one is a wall.

**No feature-register model exists.** There is no table of what a guest
should see when it reads `ID_AA64PFR0_EL1`, `ID_AA64MMFR0_EL1`,
`ID_AA64ISAR0_EL1` and the rest -- not in the kernel, not in `vmctl`, not
in the uapi. The `SYSREG` exit carries the ISS (the register encoding and
the destination) and the owner is left to answer, with nothing to answer
from.

**`WFI` returns to the owner.** The machine unit's round-robin treats a
`WFI` exit as a turn boundary. Linux idles in `WFI`, and a kernel that is
waiting for a timer interrupt it has armed must be re-entered when that
interrupt is pending -- the vtimer unit's "woken on time" logic exists in
the kernel (`vcpu_run` sleeps to the guest's deadline), but `vmctl`'s
`ONE_TICK` bound returns before that, so a Linux that sleeps waits only a
tick, which is correct but noisy, and a Linux that sleeps *forever*
waiting for an interrupt the owner never delivers hangs.

**The device tree is a fixed shape.** `fdt_cosmo_virt` writes what the
last unit's C guest needs. Linux needs more of it right, and some of it
it does not need at all -- but a wrong `compatible` string or a missing
`clock-names` entry is a driver that does not probe, silently, with the
console among the drivers that might not.

**No rootfs, and that is the milestone, not a failure.** A diskless
`virt` machine boots Linux to `VFS: Cannot open root device` and panics;
that panic, printed through the PL011, is the proof the kernel came up.
Reaching it is the goal, not passing it.

## Why it matters

- **It is the claim the whole hypervisor arc was for.** Stage-2, the
  vGIC, the timer, the distributor, the console and the machine
  description were each built so that, in the end, a kernel written for
  the architecture would run. Until one does, every one of those units is
  proven by a fixture that knew the answer. This is the unit that asks the
  one reader that did not.
- **The measurement already names the first work.** A feature-register
  model is not speculative: Linux stops on `ID_AA64DFR0_EL1` at exit
  `0x412a476c`, and it will stop on the next ID register after that. The
  work has a worklist, and the worklist is "wherever Linux stops next".
- **The walls are a map of what the hypervisor got subtly wrong.** Every
  place Linux stops that a fixture did not is a place the fixture was
  written to the implementation instead of to the architecture. The vGIC
  active-priority leak, the timer's GICv2 hang, the distributor's SGI
  mis-trap were all found this way at smaller scale; Linux is the same
  method at full scale.
- **It turns the guest fixtures from proofs into a comparison.** Once
  Linux drives the GIC, the timer and the console, the hand-written
  guests become the controlled, minimal version of what Linux does the
  complicated way -- and a divergence between them is a bug in one or the
  other, newly visible.

## Proposed design

### 1. A guest feature-register model, sanitized from the host

The registers a guest reads to learn what it is running on are the host's
own, filtered: a guest must not be told it has a feature the hypervisor
does not faithfully virtualise (a second stage of its own, a debug
architecture, pointer authentication with the host's keys), and must be
told the truth about the things it will use (the number of ASID bits, the
physical address range, the presence of the generic timer and GICv3
system registers). This is what KVM does -- read the host's ID register,
mask the fields the guest may not see, cache the result -- and it is what
`el2-guest-sysreg` gestured at with one register.

The model lives **in the kernel**, because the values come from the
host's `ID_AA64*_EL1`, which are EL1-readable but are the host's to
sanitize once, and because the owner would otherwise need every host
register value passed to it. A new backend service answers a trapped ID
read: the `SYSREG` exit for an ID register is completed in the kernel
(like the distributor's MMIO), with the sanitized value, and the owner
never sees it. A curated allow-list of fields per register, zeroing the
rest, with the policy written down: `docs/kernel/arch/aarch64/` gains "the
features a guest is told it has".

The registers Linux reads first, and this unit must answer:
`ID_AA64PFR0/1`, `ID_AA64DFR0/1`, `ID_AA64ISAR0/1/2`, `ID_AA64MMFR0/1/2`,
`MIDR_EL1`, `MPIDR_EL1` (already the vCPU's), `REVIDR_EL1`,
`ID_AA64AFR0/1`, and the AArch32 ID registers as zero (a guest that reads
them learns there is no 32-bit support, which is true).

### 2. Whatever traps between the feature registers and the console

This is the honest part: the report cannot list every wall, because the
walls are discovered by hitting them. What the method guarantees is that
each is a `SYSREG`, `WFI`, `MMIO` or `HYPERCALL` exit -- the exit kinds
the backend already decodes -- and each is one of three things:

- **a register the guest may read, answered** (the ID registers above,
  and any the kernel should model rather than trap -- `CTR_EL0`,
  `DCZID_EL0` via `HCR_EL2.TID2`/`TID4` if those are set);
- **a register the guest may not touch, faulted back** (an undefined
  instruction into the guest, which is what real hardware does for a
  disabled feature, so the guest's own fallback path runs);
- **a device access, routed** (the GIC and the UART, which already work;
  anything else is a device the machine does not have, and the guest's
  driver must cope with its absence, which a device tree that does not
  advertise it ensures).

The migration plan below is written to walk this, not to predict it.

### 3. The device tree, corrected against Linux's reader

`fdt_cosmo_virt` gains what Linux checks that the C guest did not: a
`/chosen/bootargs` that is honoured, the `interrupts` cells in the exact
`<type number flags>` triples Linux's GIC and timer drivers match, a
`/psci` node Linux's PSCI driver binds to (the `compatible` order
matters), and the PL011's `clock-names` in the order the driver reads
them (`uartclk` before `apb_pclk`). Each is verified not by re-reading the
blob but by Linux's own driver probing -- which is visible, because a
probed PL011 prints and an unprobed one does not.

### 4. The `WFI` and the timer, for a kernel that idles

A Linux that has taken its timer will `WFI` between ticks, and `vmctl`
must re-enter it when the timer fires. The kernel already re-enters a
`WFI`-blocked vCPU at its virtual-timer deadline (`arch_hv_vcpu_timer_
deadline`), so the machinery exists; the owner's round-robin must let the
kernel do the sleeping rather than bounding every run to a tick. A `WFI`
with a timer armed becomes a run that the kernel holds until the deadline
or an interrupt, exactly as `el2-guest-timer-ontime` already does inside
the kernel -- `vmctl` stops passing `ONE_TICK` to a vCPU that is the only
one running.

### 5. The milestone, and how it is recognised

The goal is Linux's own output through the PL011: at minimum its early
`console=ttyAMA0` banner, and the target is the diskless-root panic
(`VFS: Unable to mount root fs`), which is where a `virt` machine with no
disk and no initrd stops. The test captures the guest's console (the same
ring `vm_console_read` drains) and requires known Linux strings in it --
`"Booting Linux on physical CPU"`, the `"Machine model: cosmo,virt"` the
kernel prints from our device tree's `compatible`, and the root panic.
That the kernel printed *our machine's name*, read from *our device tree*,
through *our UART*, is the whole unit in one line, the way the machine
unit's `dtb:` line was.

### 6. Deliberately out of scope

- **A root filesystem.** virtio-blk, a disk image, an initrd -- each is a
  unit, and none is needed to prove the kernel booted. The panic is the
  milestone.
- **SMP Linux.** One vCPU first. `CPU_ON` works (the machine unit), but a
  Linux that brings up secondaries adds the secondary-CPU-park path and
  IPI storms to the first-boot debugging, and the first boot is hard
  enough on one CPU.
- **Full feature parity.** The guest is told a deliberately small feature
  set -- enough to boot, not everything the host has. SVE, pointer
  authentication, MTE, the debug architecture, the PMU are told-absent
  and stay that way until a guest that wants one is a unit.
- **A specific Linux version as a committed artifact.** The `Image` is not
  checked into the tree (it is 34 MB, and it is someone else's binary);
  the test skips cleanly when it is absent, and CI does not carry it. What
  is committed is the model, the device-tree corrections, and a test that
  runs when an `Image` is provided.
- **Booting through EFI.** The `vmlinuz` is a PE/EFI image; this unit
  loads the raw `Image` extracted from it (gunzip of the zboot payload),
  as `vmctl --machine` already does by the Image header. An EFI boot is a
  firmware unit, not this one.

## Affected files

| file | change |
|---|---|
| `kernel/arch/aarch64/hv_el2.c` | a trapped ID-register read is answered in the kernel from a sanitized host value; `HV_EXIT_EMULATED` for it, as for the distributor |
| `kernel/arch/aarch64/hv_idregs.c` | **new**: the feature-register model -- read host, mask to the allow-list, cache |
| `kernel/arch/aarch64/include/aarch64/hv_idregs.h` | **new**: the allow-list and the policy |
| `kernel-services/virtualization/vcpu.c` | an ID `SYSREG` exit is completed, not returned; a `WFI` on the last running vCPU is not bounded |
| `tools/fdt/fdt.c` | the device-tree corrections Linux's drivers need |
| `userland/system/vmctl.c` | do not pass `ONE_TICK` to a sole running vCPU; recognise a guest that powers off vs. one that idles |
| `tests/hv/aarch64/guest_idreg.c` | **new**: a C guest that reads the ID registers and reports them (the model's own test, not needing Linux) |
| `kernel-services/virtualization/hvtest.c` | `el2-guest-idreg` (the model) and, when an `Image` is present, `el2-linux-boot` |
| `tests/host/test_fdt.c` | the new device-tree properties, read back |
| `userland/etc/rc.test`, `tests/boot/run_boot_test.py` | the Linux boot when an `Image` is in the archive; skip cleanly otherwise |
| docs | `virtualization/design.md`, `aarch64/design.md` (the features a guest is told), `testing.md`, README |

## New APIs

```c
/* kernel/arch/aarch64/include/aarch64/hv_idregs.h */
/* The value a guest reads for an ID register `enc` (the ISS encoding),
 * the host's own masked to what a guest may safely see. `handled` is
 * false for an encoding this is not an ID register -- the owner gets the
 * exit as before. */
uint64_t hv_idreg_read(uint32_t enc, bool *handled);

/* kernel/arch/aarch64/hv_el2.c: the SYSREG decode gains the ID-register
 * case, completing in the kernel like el2_vdist_sysreg. */
```

No uapi change: the model is entirely kernel-side, and a guest's ID read
is completed before the exit would reach userland, so `vmctl` sees fewer
`SYSREG` stops, not a new kind.

## Migration plan

Walk the walls. Each step is "run Linux, see where it stops, clear that,
commit".

1. **The feature registers.** `hv_idregs.c`, the allow-list, the SYSREG
   completion. `el2-guest-idreg`: a C guest reads the ID registers and
   gets the sanitized host values (checked against the host's own,
   masked -- so the test is the policy). Linux gets past
   `ID_AA64DFR0_EL1` and stops at whatever is next.
2. **The next wall, and the next.** Whatever step 1 reveals: another
   trapped register (answer or fault it), a device-tree property Linux's
   driver rejects (correct it), a `WFI` that is not re-entered (step 4).
   Each is its own commit with its own before/after: "Linux stopped at
   X; now it reaches Y".
3. **The console.** The wall that matters most, because past it Linux
   speaks: its `earlycon` write to the PL011, then its real console
   driver probing our `pl011` node. When Linux prints its banner through
   our UART, the unit has its headline.
4. **The idle path.** `WFI` on the sole vCPU held to the timer deadline
   in the owner, so a Linux between ticks does not spin and does not
   hang.
5. **The milestone.** Linux to the diskless-root panic, its own strings
   captured from the console ring, `Machine model: cosmo,virt` among
   them. `el2-linux-boot`, skipped when no `Image` is present.
6. Docs, README, and the honest list of what the guest is told-absent.

The plan cannot promise step 2 is one commit or five. That is the nature
of the unit, and it is stated plainly: the report's value is the
measurement and the method, not a prediction of the wall count.

## Tests

- **`el2-guest-idreg`** (kernel, no Linux) -- a C guest reads each
  modelled ID register; the value equals the host's own with the
  allow-list mask applied, computed independently in the test from
  `READ_SYSREG`, so a field the model forgot to clear fails here. This is
  the model's real regression test, and it runs in CI without any Linux
  image.
- **`test_fdt`** (host, extended) -- the device-tree properties Linux
  needs, read back: the exact interrupt triples, the PSCI compatible
  order, the PL011 clock-names order.
- **`el2-linux-boot`** (kernel, only with an `Image`) -- loads the
  archive's `Image` and device tree, runs the vCPU (with the kernel's own
  WFI-to-deadline path), drains the console, and requires
  `"Booting Linux"`, `"Machine model: cosmo,virt"` and the root panic.
  Skips with a note when no `Image` is in the archive, so the tree builds
  and CI passes without carrying someone's 34 MB binary.
- **`HVTEST-LINUX`** (userland, only with an `Image`) -- the same through
  `vmctl run --machine`, the real owner, the real idle path.

The two vacuity traps this unit's own history warns about: `el2-guest-
idreg` must compute the expected value from the host register in the test,
not hard-code a constant (a constant passes against a model that returns
the same wrong constant); and the Linux test must require a *late* string
(the root panic), not just an early one, because an early banner followed
by a silent hang would pass a test that only checked the banner.

## Benchmarks

Counted:

- **How far Linux gets, in exits, before and after each step.** The one
  number that says the unit is progressing: today, ~16000 exits to the
  first unmodelled ID register; the target is "to the root panic", which
  is a fixed point, not a count.
- **ID registers modelled vs. trapped-to-owner.** After step 1, all of
  them in the kernel and zero to the owner; the count that says the model
  is complete enough to stop being the wall.
- **Exits per second while Linux idles.** With the WFI-to-deadline path,
  one exit per timer tick (250/s), not a spin; the number that says the
  idle path works.

## Risks

- **The wall count is unknown, and that is the headline risk.** A report
  that cannot say how much work it is is a report to read carefully. The
  mitigation is the method: every wall is a decoded exit with a clear
  disposition (answer, fault, route), the steps are independently
  committable, and the milestone (the root panic) is a fixed target that
  a partial unit can be measured against. If the walls prove to be many,
  the unit ships what it cleared and names where Linux stops, and the
  next report continues -- exactly as this one continues the machine unit.
- **A sanitized feature register is a security boundary.** Telling a guest
  it has a feature the hypervisor does not isolate (its own stage-2, the
  debug registers, the host's pointer-auth keys) is a guest escape or an
  information leak. The allow-list is deny-by-default: a field is told
  present only when it is named, and the policy doc justifies each. This
  is the one place in the unit where "make Linux happy" must lose to
  "keep the guest contained", and the test asserts the *masked* value, so
  a field opened by accident fails.
- **Linux is a moving target.** A different kernel version stops in a
  different place. The unit pins the `Image` it was developed against (by
  documenting its source and its header values, not by committing it) and
  the test skips when a different or absent image is provided, so the
  tree is never red for lack of a binary. The model and the device tree
  are version-independent; the exact wall sequence is not, and the doc
  says so.
- **The idle path can hang a real kernel.** If `vmctl` bounds a sole
  vCPU's `WFI` wrong, a Linux waiting on its timer either spins (a tick
  bound) or hangs (no re-entry). The kernel's `arch_hv_vcpu_timer_deadline`
  is the tested primitive; the risk is in the owner using it, and
  `el2-guest-timer-ontime` is the in-kernel proof the primitive is right.
- **34 MB in a 64 MiB VM.** `COSMO_HV_VM_MEM_MAX` is 64 MiB; a 34 MB
  `Image` plus its `.bss` plus the device tree plus room to run is tight,
  and a real Linux wants more. The limit is a `#define`
  (`COSMO_HV_VM_MEM_MAX`) and the owner's rlimit; raising it is a small,
  separate change this unit will need and must justify, not smuggle.

## Alternatives considered

- **Answer the ID registers in the owner, not the kernel.** The
  `SYSREG` exit already reaches `vmctl`; it could carry a table. But the
  values are the host's `ID_AA64*_EL1`, which the owner cannot read
  (they are EL1 registers of the *host*, and the guest runs in
  userland's VM), so the kernel would have to pass all of them out --
  and the sanitization is a security policy that belongs where the
  isolation is, in the kernel. The distributor made this same call for
  the same reason.
- **Trap nothing -- clear `TID3` and let the guest read the host's raw
  ID registers.** Simplest, and wrong: the guest would learn it has
  features the hypervisor does not virtualise and use them, which is the
  escape the allow-list exists to prevent. `TID3` stays set; the reads
  are answered, filtered.
- **Boot a smaller kernel, or a unikernel, first.** A minimal guest that
  reads fewer registers would reach a milestone sooner and prove less: the
  point of the unit is the *stock* kernel, the reader that does not know
  or care about CosmoOS. A unikernel written to the machine is another
  fixture written to the implementation.
- **Wait until virtio-blk exists so Linux can reach userspace.** Reaching
  an Alpine login is a better demo and a much larger unit (virtio-mmio, a
  block device, a disk image, an init). The kernel coming up -- to the
  panic a diskless machine reaches -- is the architecturally complete
  claim: the hypervisor runs a kernel written for the architecture. The
  root filesystem is the next report, not this one.
