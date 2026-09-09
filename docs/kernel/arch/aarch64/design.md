# AArch64 port: design

## The platform

QEMU `virt` (`-machine virt,gic-version=2`, so MSI comes from the GICv2m
frame the machine instantiates), CPU `cortex-a72` by default (`QEMU_CPU`
overrides; `max` adds PAN and is also supported), 4 CPUs, 256 MiB of RAM
at 0x40000000, EDK2 firmware (`AAVMF`/`QEMU_EFI`) in a 64 MiB pflash
(smaller images are padded into `$(OUT)`), the same virtio-pci devices as
x86, `-serial stdio` on the PL011, fw_cfg over MMIO, semihosting enabled
for the exit status. The machine's fixed addresses are only defaults;
ACPI tables (MADT, GTDT, SPCR, MCFG, FADT) are the source of truth as on
x86.

```text
0x0800_0000  GICD           0x0801_0000  GICC          0x0802_0000  GICv2m frame (INTIDs 80..143 on QEMU: base 80, 64 SPIs)
0x0900_0000  PL011 (SPI 1 → INTID 33)    0x0902_0000  fw_cfg (MMIO)
0x0905_0000  SMMUv3 (iommu=smmuv3; event queue INTID 106, PRI 107, global error 109) — read from the ACPI IORT, not assumed
0x1000_0000  PCI 32-bit MMIO window      0x4010_0000_0000  ECAM (from MCFG)   0x80_0000_0000  PCI 64-bit window
0x4000_0000  RAM
```

## Address space

Identical to x86-64: user `[0x400000, 0x7FFFFFFFF000)` in TTBR0's half
(48-bit VA, T0SZ = 16), direct map at 0xFFFF800000000000 (4 GiB), the
arena at 0xFFFFC00000000000, the kernel image at 0xFFFFFFFF80000000 —
all inside TTBR1's half (T1SZ = 16). The generic VMM's constants
therefore stay; only the translation-table format differs.
`arch_mmu_kernel_base()` returns 0xFFFF800000000000, the first address
of TTBR1's half, on both architectures; the VMM uses it to tell a
kernel-address fault from a user one, so an arena or image fault is
reported as a kernel fault here too.

The near arena is the one range the architecture chooses
(`arch_mmu_near_arena`, new in `arch/mmu.h`): x86-64 keeps
0xFFFFFFFF88000000–0xFFFFFFFFFF000000 for `-mcmodel=kernel`; AArch64
returns `[align2M(__kernel_end), 0xFFFFFFFF80000000 + 120 MiB)` so that
every module sits within ±128 MiB of every kernel export and `bl`
(`R_AARCH64_CALL26`) reaches. `-mcmodel=small` reaches every kernel
symbol with `adrp`/`add` (PC-relative, ±4 GiB). `vmm_init` reads the
bounds into `kernel_space.near_lo/near_hi` instead of using constants.

## Boot

### Protocol v4

`struct cosmoboot_info` gains `uint64_t boot_pagetable_root_user` (taken
from `reserved1[0]`): the loader's identity-map root that must live in
TTBR0 while the kernel adopts the higher half from `boot_pagetable_root`
(TTBR1). x86-64 writes 0. `COSMOBOOT_VERSION` becomes 4 on both
architectures; the kernel still requires an exact match.

### The loader (`boot/uefi/arch/aarch64/`)

`main.c` keeps its sequence; the CPU- and table-specific steps call into
`boot/uefi/arch/arch.h`, implemented per architecture in
`boot/uefi/arch/<arch>/{cpu.c,paging.c,serial.c}`:

```c
struct paging_ctx { uint64_t pool_phys; UINTN pool_pages, pool_used;
                    uint64_t root;        /* x86-64: PML4 (CR3); AArch64: the TTBR1 table */
                    uint64_t root_user;   /* AArch64: the TTBR0 identity table; x86-64: 0 */
                    bool nx; };
UINTN paging_pool_size(const struct elf_image *img);
EFI_STATUS paging_build(struct paging_ctx *ctx, const struct elf_image *img, uint64_t loader_base,
                        uint64_t loader_size, const uint8_t *mmap, UINTN mmap_size, UINTN desc_size);
bool cpu_prepare(void);       /* refuse a CPU the kernel cannot run on: x86-64 without NX, AArch64 not at EL1 */
void cpu_finish(void);        /* after ExitBootServices: x86-64 enables NX and WP; AArch64 nothing */
void cpu_halt(void) __noreturn;
void cpu_jump_to_kernel(const struct paging_ctx *pg, uint64_t stack_top, uint64_t info, uint64_t entry) __noreturn;
void arch_serial_init(void); bool arch_serial_present(void); void arch_serial_putc(char c);
#define LOADER_ELF_MACHINE / LOADER_ELF_MACHINE_NAME / COSMOBOOT_ARCH_NATIVE   /* 183 "AArch64" / 62 "x86-64" */
```

`main.c` takes a snapshot of the EFI memory map before calling
`paging_build`, because AArch64 needs it: the direct map gives RAM
normal write-back attributes and everything else (MMIO, reserved,
unusable, and any range the map does not describe) device nGnRnE;
mapping MMIO as cacheable is not tolerated the way x86 tolerates it.
`paging_build` fills two roots with 4 KiB descriptors and 2 MiB blocks
everywhere except the kernel image: TTBR1 maps the direct map (PA
0–4 GiB at 0xFFFF800000000000), the kernel segments with their ELF
permissions (text RX, rodata R, data RW; PXN/UXN as appropriate); TTBR0
identity-maps the same 0–4 GiB with RAM executable at EL1, because the
loader itself keeps running on it while the tables are switched. The
bootinfo and handoff stack are in RAM the direct map covers.

`cpu_prepare` reads `CurrentEL` and accepts EL1 or EL2 (EL0 is refused).
The `virt` machine runs EDK2 at EL1 unless `virtualization=on`, which
the test harness now passes: firmware then hands off at EL2 and the
loader **installs a resident EL2 stub and drops to EL1**, so everything
downstream is unchanged. See "Exception level 2" below. `cpu_jump_to_kernel` runs with the MMU still on:
mask DAIF, `dsb sy; isb`, `MAIR_EL1` = 0x0000000000440000FF (attr 0
normal WB 0xFF, attr 1 device 0x00, attr 2 normal non-cacheable 0x44),
`TCR_EL1` (T0SZ/T1SZ 16, 4 KiB granules both halves, inner-shareable
write-back walks, IPS from `ID_AA64MMFR0_EL1.PARange` capped at 6),
`TTBR0/1`, `isb; tlbi vmalle1; dsb sy; isb`, `SCTLR_EL1` with M|C|I set
and WXN|A clear (the VMM enforces W^X itself; the direct map is RW),
`isb`, then `mov sp, stack_top; mov x0, info; x29 = x30 = 0; br entry`.
The image is linked with `lld-link /machine:arm64` (`LOADER_ARCH_LDFLAGS`)
as `BOOTAA64.EFI` (`LOADER_EFI_NAME`); `efi.h` defines `EFIAPI` empty on
non-x86 targets since AAPCS64 is the UEFI calling convention there.

### Kernel entry (`entry.S`, `start.c`)

`_start`: `x0` is the bootinfo pointer; set `sp` to the boot stack in
`.bss`, `msr vbar_el1, vectors`, clear `x29`, `bl aarch64_start`.
`aarch64_start`: record `info->hhdm_base` in `aarch64_hhdm_base` (the
direct-map base the early device code uses before the PMM publishes its
own), `arch_console_early_init` (PL011 through the direct map, whose
device page the loader's tables provide and `map_early_devices` later
carries into the kernel's own root), `percpu_init_boot` (the static boot
block into `TPIDR_EL1`),
`aarch64_cpu_init` (MIDR/MPIDR, `ID_AA64MMFR1.PAN`, `ID_AA64PFR0.GIC`,
`CNTFRQ`), `arch_syscall_init_cpu` (nothing to program: SVC is a vector),
then `kernel_main(info)`.

## Exceptions and interrupts

### Vector table (`vectors.S`) and frame

Sixteen 128-byte slots (`VBAR_EL1`, 2 KiB aligned): current EL with SP0
(unused: the kernel always runs on SP_EL1 — panics), current EL with SPx
(sync, IRQ, FIQ, SError from the kernel), lower EL AArch64 (from user),
lower EL AArch32 (panic). Every used slot saves the frame:

```c
struct arch_trap_frame {
    uint64_t x[31];        /* x0..x30 */
    uint64_t sp;           /* SP_EL0 for user frames; the interrupted SP for kernel frames */
    uint64_t elr, spsr, esr, far;
    uint64_t vector;       /* filled by trap.c after classification */
    uint64_t kind;         /* AARCH64_ENTRY_* : which vector slot delivered the frame */
};                         /* 0x130 bytes; offsets are assembly ABI (trapframe.h, _Static_assert) */
```

The sync handler classifies `ESR_EL1.EC`: SVC (0x15) → the syscall path
(`x8` number, `x0..x5` copied into the argument array, interrupts
enabled around `syscall_dispatch(x8, args, frame)`, result to the frame's
`x0`; an SVC from EL1 panics); instruction abort (0x20 from EL0, 0x21
from EL1) and data abort (0x24, 0x25) → `ARCH_TRAP_PAGE_FAULT`; BRK
(0x3C), breakpoint and watchpoint → `ARCH_TRAP_BREAKPOINT`; software
step (0x32/0x33) → `ARCH_TRAP_DEBUG`; unknown (0x00), illegal execution
state (0x0E), trapped FP/SIMD (0x07, which no longer happens: FPEN is
0b11 at every CPU's bring-up, so the instructions are allowed at EL0 and
EL1; the kernel abstains by its build flag and a build check, and user
code does not abstain at all) and trapped system-register access
→ `ARCH_TRAP_INVALID_OPCODE`; everything else (PC/SP alignment, SError)
→ `ARCH_TRAP_GENERAL_PROTECTION`. There is no divide-error exception on
AArch64; the kind exists for the contract and never fires. BRK is a
fault-class exception (`ELR` points at the `brk` itself) while x86's
`int3` is a trap; to keep the contract's semantics `trap.c` advances
`ELR` by 4 after the handler returns unless the handler moved `ELR`
itself, so `arch_debug_break()` resumes after the instruction instead of
re-faulting forever. After every sync or IRQ frame from EL0 the common
exit runs `process_return_to_user` (kill delivery) when the CPU is not
in an interrupt and preemption is enabled.

IRQ → `gic_irq_dispatch(frame)`: acknowledge (`GICC_IAR` on GICv2,
`ICC_IAR1_EL1` on GICv3), remember the
INTID for EOI (`g_cur_intid[cpu]`), map INTID → vector, call
`aarch64_timer_ack` first when the INTID is a PPI (the timers re-arm
there), `interrupt_dispatch(vector, frame)`, `arch_irqc_eoi`; an
unrouted SGI or an INTID ≥ 1020 counts as spurious and is EOI'd without
dispatch. After the handler, a pending reschedule with interrupts
enabled in the interrupted context runs `sched_preempt`. FIQ, SError,
the SP0 and AArch32 slots build a frame, call `aarch64_trap_entry`, which
panics with it, and never return.

### Vector numbering (`arch/trap.h` contract, `gic.c`, `trap.c`)

```text
0 .. 1019     INTIDs (SGI 0..15, PPI 16..31, SPI 32..1019): a vector is an interrupt id
1020          spurious (GICC_IAR 1023 and 1022)
1024 .. 1029  synchronous exception kinds, in enum arch_trap_kind order
1056 .. 1311  dynamic software vectors: arch_vector_alloc(); routed to INTIDs by the maps below
arch_trap_vector_count() = 1312
```

The generic layers treat vectors as opaque, allocate with
`arch_vector_alloc`, and route with `arch_irqc_route(gsi, vector, cpu,
flags)`; on this architecture **GSI = INTID**.

### Two controllers, one seam (`irqc.c`, `gic.c`, `gicv3.c`)

`arch/irqc.h` says nothing about controller generations, and this
machine has two. `irqc.c` reads the MADT once at `arch_irqc_init`,
picks a driver from the reported distributor version — 0, 1 or 2 →
`aarch64_gicv2_ops` in `gic.c`, 3 or 4 → `aarch64_gicv3_ops` in
`gicv3.c`, anything else panics — and forwards every `arch_irqc_*`,
`arch_ipi_*` and `gic_*` call through `struct aarch64_irqc_ops`
(`aarch64/irqc.h`). The pointer is set by the boot CPU before any AP
runs or any interrupt is enabled, and read without synchronisation
after.

The two drivers share no state. They share one file: `gicv2m.c`, the
MSI frame, because a GICv3 whose MADT describes a frame and no ITS uses
exactly the frame a GICv2 does. `arch_irqc_spurious_vector` is also not
an op — the vector map below is the architecture's, not a driver's.

### GICv2 (`gic.c`)

`gic.c` keeps
`g_vector_of[1020]` (INTID → vector, identity for unrouted INTIDs) and
`g_intid_of[256]` (dynamic vector → INTID, 0xFFFF for none). `route`
writes both maps and, for an SPI, sets `GICD_IPRIORITYR` to 0x80,
`GICD_ITARGETSR` to the requesting CPU's interface bit (read from
`ITARGETSR0` on that CPU at `init_cpu`) and `GICD_ICFGR` from the
edge/level flag; for a PPI it records the line in `g_routed_ppi_mask`.
`mask/unmask` use `ICENABLER/ISENABLER`. `eoi(vector)` writes the INTID
recorded at acknowledge time on this CPU (`g_cur_intid[cpu]`) — no
reverse lookup; synchronous and spurious vectors are ignored.
`arch_vector_free` unbinds: disables the SPI, restores the identity map
entry, releases a GICv2m SPI or an SGI. `arch_irqc_gsi_count` = 1020
(PPIs included). PPI and SGI enables are banked per CPU, so `init_cpu`
enables all sixteen SGIs plus `g_routed_ppi_mask` on each CPU; the timer
binds its PPI once with `gic_bind_ppi` and enables it per CPU with
`gic_enable_local`. `init` programs every SPI group 0,
disabled, inactive, priority 0x80, level, and enables the distributor;
the GICv2m frame's `MSI_TYPER` gives the SPI base and count unless the
MADT entry overrides them, and a range outside the distributor's lines
disables MSI with a warning.

IPIs: the generic layer allocates a dynamic vector and calls
`arch_ipi_send(cpu, vector)` or `arch_ipi_broadcast_others(vector)`;
`gic.c` binds SGIs 0..15 to IPI vectors lazily, on first send
(`g_sgi_vector[16]`, `g_sgi_of_vector[256]`; a seventeenth IPI vector
panics), issues `dsb ishst` then `GICD_SGIR` with the target list or the
"all but self" filter, and the receiving CPU maps the SGI id back through
`g_sgi_vector`.

MSI: `arch_irqc_msi_compose(vector, cpu, &addr, &data)` takes the lowest
free SPI from the GICv2m frame's range (QEMU: INTIDs 80..143), routes it
edge-triggered to `vector` on `cpu`, enables it, and returns `addr` =
frame + 0x40 (`MSI_SETSPI_NS`), `data` = INTID. Freeing the vector
releases the SPI.

**A frame's range can overlap lines firmware wired to devices** -- on
QEMU's `virt` the SMMU's event and error interrupts are INTIDs 106 and
109, inside the frame's 80..143 -- and the frame's bitmap cannot see
that. So `route` refuses an INTID already bound to another vector
(`-EBUSY`) and `msi_compose` walks past such a line, leaving it marked
used because it is not the frame's to hand out, with one warning naming
it. Before this, the twenty-seventh MSI took the SMMU's line and the
SMMU stopped being interrupted; nothing noticed until sixteen CPUs
asked for twenty-seven queues.

The frame itself is `gicv2m.c`: a `struct gicv2m`
holding the mapped window, the SPI range and a bitmap under its own
lock, which both drivers own an instance of. Its lock is a leaf: the
driver's `g_lock` may be taken around it and never the other way.

### GICv3 (`gicv3.c`)

Same vector map, same bookkeeping arrays, three differences, which are
the whole of the file:

- **The CPU interface is system registers.** `ICC_SRE_EL1` selects them
  (`init_cpu` sets SRE, DFB and DIB, then panics if SRE reads back
  clear), `ICC_PMR_EL1` = 0xF0 admits everything, `ICC_BPR1_EL1` = 0,
  `ICC_CTLR_EL1` clears CBPR and EOImode so one `ICC_EOIR1_EL1` write
  both drops priority and deactivates, and `ICC_IGRPEN1_EL1` = 1 opens
  the interface. Acknowledging is `ICC_IAR1_EL1`. Interrupts are
  **Group 1**, not GICv2's Group 0: the system-register interface
  delivers Group 1 as IRQ and Group 0 as FIQ, and this kernel takes
  IRQs. The default priority moves with it, to that view's 0xA0.
  *(EL2 must leave `ICC_SRE_EL2.Enable` set for EL1 to reach these at
  all. The loader does not program it — QEMU's does not need it — so a
  machine whose firmware cleared it would trap on the first access.)*
- **SGIs and PPIs live in a redistributor.** One frame pair per CPU,
  found by walking the GICR window from the MADT (or, without one, the
  GICC entry's own base) for a frame whose `GICR_TYPER[63:32]` is this
  CPU's MPIDR affinity; the stride is 0x20000, or 0x40000 when the
  first frame reports VLPIS (GICv4). `GICR_WAKER.ProcessorSleep` is
  cleared and `ChildrenAsleep` waited on before anything else. So
  `mask`, `unmask`, `gic_enable_local` and `gic_disable_local` go to the
  calling CPU's redistributor for an INTID below 32 and to the
  distributor above it.
- **Routing is by affinity.** `GICD_CTLR` gains `ARE_NS`, and an SPI is
  routed by writing the target's affinity to `GICD_IROUTER` (64 bits per
  SPI, programmed after ARE is set because it means nothing before);
  `arch_ipi_send` writes `ICC_SGI1R_EL1` with Aff3/Aff2/Aff1 and a
  sixteen-bit target list over Aff0, and `arch_ipi_broadcast_others`
  sets IRM. This is where GICv2's eight-bit `GICD_ITARGETSR` mask — and
  its eight-CPU ceiling — stops being the limit; QEMU's `virt` numbers
  sixteen CPUs per cluster under `gic-version=3` precisely so the target
  list covers a cluster.

A write to an enable or control register is not in effect until the
controller says so, so `GICD_CTLR.RWP` and `GICR_CTLR.RWP` are polled
after the writes that need it.

### MSI under GICv3: the ITS (`gicv3_its.c`)

MSI has a fallback order, decided at `init` from the MADT: an ITS if
firmware described one, otherwise a GICv2m frame, otherwise
`msi_compose` returns `-ENODEV`. That last is a **decline, not a
fallback** -- no driver in this tree falls back to INTx, so a machine
with neither loses its disks; it is a configuration the kernel reports
rather than one it survives.

An ITS *translates* instead of raising a fixed line. A device writes an
event number to `GITS_TRANSLATER`; the ITS looks up (DeviceID, EventID)
in tables the kernel built and raises an **LPI** -- an interrupt id from
8192 up -- on the redistributor of whichever CPU that event belongs to.
Three consequences run through the code:

- **The device's identity matters.** `arch_irqc_msi_compose` and
  `irq_request_msi` carry a device id, which `pci_requester_id()`
  computes from bus:device:function -- the same number the IOMMU calls
  a stream id. Backends that do not translate per device ignore it.
  `irq-msi-devid` is the test that it arrives: an id no device table can
  hold must be refused.
- **LPIs need tables of their own.** A shared property table (one byte
  per LPI: priority and an enable bit) in `GICR_PROPBASER`, a pending
  table per redistributor in `GICR_PENDBASER`, and `GICR_CTLR.EnableLPIs`
  -- a one-way switch, so both tables are in place first. This kernel
  uses one LPI per dynamic vector (256), though the property table must
  still cover the fourteen id bits the architecture's minimum implies
  and the pending table must be 64 KiB aligned.
- **Everything is said through a command queue.** MAPD gives a device
  its translation table, MAPC gives a CPU a collection pointing at its
  redistributor (by address or by processor number, as
  `GITS_TYPER.PTA` dictates), MAPTI binds an event to an LPI in a
  collection, INV drops what the redistributor cached, SYNC waits.
  `its_map_event` and `its_unmap_event` are those sequences; the LPI is
  released outside the driver's lock, because draining the queue is a
  spin.

Two simplifications, both deliberate. **The event id is the LPI's
index**, so no device needs an event-number allocator of its own and
every device's table is the same size. **The device table is flat and
capped at 64 KiB**; `GITS_TYPER` may claim twenty device-id bits, which
is an eight-megabyte flat table, and the architecture's answer -- a
two-level table -- is a follow-up. A device id beyond the table is
refused with a warning, never mistranslated.

**And an MSI write is a DMA.** Where devices sit behind an IOMMU, the
doorbell page has to be kept out of the IOVA space and identity-mapped
into every domain, or the write faults and the interrupt never arrives.
`arch_irqc_msi_doorbell()` is where the controller says which page that
is; `arm_smmuv3.c` asks rather than assuming, because the answer moved
when the ITS replaced the frame. On x86-64 it is false: a write to
0xFEE00000 is an interrupt request, not a DMA, and the IOMMU never sees
it.

### Timer (`timer.c`)

Clock: `CNTPCT_EL0` (after an `isb`) at `CNTFRQ_EL0` Hz (62.5 MHz on
QEMU), name `"arch-timer"`; no calibration needed: `arch_timer_calibrate`
reads the frequency (panics outside 1 MHz–10 GHz), reads the GTDT for
the non-secure EL1 and virtual timer GSIVs (offsets 80 and 88; defaults
30 and 27 with a warning when the table is absent), allocates the tick
vector and binds it to the physical-timer PPI. Tick: `start_tick(hz)`
computes the period in counter ticks and arms `CNTP_CVAL_EL0` with an
**absolute** compare, `now + period`; on every tick `aarch64_timer_ack`
(called by `gic.c` before the PPI is dispatched) re-arms with the
previous compare plus the period, skipping ahead to `now + period` only
if that value is already in the past. A `CNTP_TVAL_EL0` reload from the
handler drifted by the interrupt latency on every tick and made the
timer self-tests miss their rate window; absolute compares keep the
average rate exact. `stop_tick` masks `CNTP_CTL_EL0`, disables the PPI
locally and forgets the compare. Each secondary enables the PPI in
`aarch64_timer_init_cpu`; the compare state is per CPU. The test periodic
IRQ (`arch/testhooks.h`) uses the EL1 virtual timer (INTID 27) with a
`CNTV_TVAL_EL0` reload, which is adequate for the interrupt tests.

## MMU (`mmu.c`)

4 KiB granule, four levels (L0..L3 covering 512 GiB, 1 GiB, 2 MiB, 4 KiB
per entry). The kernel context's root is TTBR1's table, a user
context's root is TTBR0's; `arch_mmu_context_init_user` allocates an
empty root (there is nothing to copy: the kernel half lives in TTBR1,
which every context shares). Which root a VA belongs to is decided by bit
55 (the TTBR select bit): the kernel context refuses low VAs and a user
context refuses high ones (`-EINVAL`), keeping the generic invariant that
a mapping lives in exactly one context.

Descriptor bits: `AF` always set (no access-flag faults), `SH` inner
shareable, `AttrIndx` 0 (WB) / 1 (device) / 2 (non-cacheable, used for
`VM_CACHE_WT`, which ARM lacks), `nG` on user leaves, `AP[2:1]`: kernel
RW 00, kernel RO 10, user RW 01, user RO 11 (a user page is also
kernel-accessible, as on x86), `UXN`/`PXN`: kernel code PXN 0 UXN 1, user
code UXN 0 PXN 1, data both set. Intermediate tables carry no attribute
restrictions (`APTable`/`PXNTable` zero) so the leaf decides, like x86.
2 MiB blocks at L2 when the caller allows large pages and alignment
permits (`arch_mmu_large_page_sizes` = 2 MiB). `protect` rewrites leaf
attribute bits keeping the output address; splitting a block is refused
(`-EEXIST`), as on x86. `query` walks and reports the leaf's page size.

`activate(kernel)`: first `map_early_devices` — the generic VMM builds
the direct map for RAM only, so the PL011 and fw_cfg pages at the `virt`
default addresses are mapped device-type into the kernel root if
`query` finds them absent (the first boot of the port hung silently at
takeover because the console page vanished); then `TTBR1_EL1` = root,
`TTBR0_EL1` = `g_empty_root` (an empty table allocated with the first
context so no stale user mapping is reachable from kernel context),
`isb`, `tlbi vmalle1is`. `activate(user)`: `TTBR0_EL1` = root **with
the space's ASID in bits [63:48]**, and a flush only when the caller says
so -- `tlbi vmalle1`, *non*-shareable, because a CPU flushes on its own
account after a tag-generation rollover and is never behind on another
CPU's. Until the ASID unit this was ASID 0 and an unconditional `tlbi
vmalle1is`: a broadcast that emptied the user TLB of every CPU in the
machine on every process switch made by any of them, and that took the
kernel's own entries with it since kernel leaves never carry `nG`.
`TCR.AS` is set at boot when `ID_AA64MMFR0.ASIDBits` reports 16-bit
ASIDs -- the kernel's only write to `TCR_EL1`, made before any space
exists and while every `TTBR0` still carries ASID 0, which reads the same
at either width. `invalidate(ctx, va, len)`: `dsb ishst`, `tlbi vaae1is`
per page -- by address and for *every* tag, deliberately: after a
rollover a space can be re-tagged on one CPU while another still runs it
under the old tag, so naming the current tag would leave that CPU's
entries behind. `invalidate_asid(ctx)` is the one call that names a tag
(`tlbi aside1is`), used only when a space is destroyed and nothing runs
it
(or `vmalle1is` above 64 pages), `dsb ish; isb`. `shootdown` is the same
instruction sequence: AArch64 broadcasts TLB maintenance to the
inner-shareable domain in hardware, so no IPI is sent and
`arch_mmu_shootdown_ipi_handler` only counts `handled` if anything ever
calls it. The generic shootdown test still checks the contract's
accounting, so when other CPUs are online a shootdown counts one
`initiated` and `n−1` `acks_received` (the completing DSB is the
acknowledgement of every other CPU).

Table pages come from the PMM's DMA32 zone (zeroed) and are reached
through the direct map. The loader's bootstrap tables are freed after
takeover exactly as on x86 (`COSMOBOOT_MEM_BOOT_PAGETABLES`).
`context_destroy` asserts the root is not the live `TTBR0` and frees the
tree; empty intermediate tables are not reclaimed on unmap (the same
documented gap as x86).

### Page-fault translation (`trap.c`)

`arch_trap_fault_address` = `FAR_EL1`. Flags from `ESR_EL1`: instruction
abort → `ARCH_FAULT_EXEC`; data abort with `WnR` → `ARCH_FAULT_WRITE`;
EC from EL0 → `ARCH_FAULT_USER`; DFSC/IFSC translation fault (0b0001xx)
→ not present; permission fault (0b0011xx) or access flag (0b0010xx) →
`ARCH_FAULT_PRESENT`; anything else (address size, synchronous external,
alignment) → `ARCH_FAULT_RESERVED`, which the VMM treats as fatal.

Milestone 5 additions: `arch_trap_fixup` moves `ELR_EL1` to the
exception-table fixup of a faulting kernel PC (`kernel/extable.h`);
`arch_copy_user_raw` (`uaccess.S`) is an aligned 8-byte loop and a byte
loop whose four loads and stores are the table's entries, run with PAN
cleared by the caller; a `PROT_NONE` page is a level-3 descriptor with
VALID clear and the software bit 55 (`DESC_SW_NONE`) set, which the
walker treats as a leaf (`docs/kernel/memory/design.md` §6.2).
`arch_mmu_shootdown_cpus` counts the CPUs in the mask as acknowledged
by the broadcast TLBI's DSB; `arch_mmu_prepopulate` is a no-op (TTBR1).

## Threads, per-CPU, user mode

`struct percpu *` lives in `TPIDR_EL1` (`arch_percpu_get` is one `mrs`).
`arch_context_switch` (`switch.S`) saves `x19..x28`, `x29`, `x30` in a
96-byte frame on the outgoing stack, records `sp` in `from->sp`, loads
`to->sp` and restores; `arch_context_init` (`context.c`) builds that
frame with `x19` = entry and `x30` = `aarch64_context_start`, which
zeroes the frame pointer (backtraces stop there) and calls the entry (the
`x86_context_start` shape). `arch_thread_switch_prepare(next)` records
the thread's kernel stack top in the per-CPU block, switches the address
space with `arch_mmu_activate` if `TTBR0` or `TTBR1` differs from the
next thread's roots, and writes `TPIDR_EL0` from `next->tls_base` for
process threads (`arch_set_tls_base` writes it too). There is no TSS: the
kernel stack for the next exception from EL0 is simply SP_EL1 at the
moment of `eret`, i.e. the thread's own kernel stack.

`arch_user_enter(entry, sp)`: interrupts off, `TPIDR_EL0` = the thread's
TLS base, then `aarch64_user_enter` (`vectors.S`): `ELR_EL1` = entry,
`SP_EL0` = sp, `SPSR_EL1` = 0 (EL0t, DAIF clear), zero `x0..x30`, `eret`.
`arch_user_enter_regs(regs)` (milestone 10, a clone's first entry and
the shape every signal return takes) loads all 31 registers, `sp`, `pc`
and the user bits of `pstate` from a `struct arch_user_regs`
(`aarch64_user_enter_regs`). `TPIDR_EL0` is user-writable, so
`arch_thread_switch_prepare` saves the outgoing thread's value into its
`tls_base` before loading the incoming one's.
`arch_user_access_begin/end` clear/set PSTATE.PAN through the raw
encodings (`.inst 0xd500409f/0xd500419f`; the ARMv8.0 assembler refuses
`msr pan`) when `ID_AA64MMFR1_EL1.PAN` reports the feature (`-cpu max`;
not on `cortex-a72`) and are no-ops otherwise; `aarch64_cpu_init` and
each secondary set `SCTLR_EL1.SPAN` so exception entry leaves PAN alone,
then set PAN. The copy routines validate ranges before touching user
memory regardless. `arch_trap_frame_is_user` = `SPSR.M[3:0]` is EL0t and
`M[4]` (AArch32) clear.

## SMP (`smp.c`, `trampoline.S`)

CPUs come from the MADT GICC entries (type 11: flags at 12, physical
base at 32, MPIDR at 68, ACPI UID at 8), replacing the x86 local-APIC
ids in the same generic structure: `hw_id` is the MPIDR's affinity
fields packed into 32 bits (Aff0–2 in bits 0–23, Aff3 in 24–31);
`arch_smp_boot_hw_id` reads them from `MPIDR_EL1`. The first port of the
parser used the wrong offsets and reported three CPUs with a garbage GICC
base, which is why the layout is spelled out in the parser's comment.

`arch_smp_start_cpu(cpu, hw_id, stack_top)` first installs the
trampoline once: `psci_probe` decides the conduit (HVC when the FADT's
ARM boot flags say `PSCI_USE_HVC`, when the FADT is absent or when it
does not declare PSCI; SMC otherwise — `virt` without EL2 uses HVC) and
logs the PSCI version; then a temporary user context (`g_tramp_ctx`,
`arch_mmu_context_init_user`) identity-maps **only the trampoline's
page** RX. The mailbox (`struct aarch64_ap_mailbox {ttbr0, ttbr1, mair,
tcr, sctlr, stack_top, entry, cpu}`, 64-byte aligned, a static per-CPU
array in the kernel image's writable segment) is filled with the
trampoline table as `ttbr0`, the kernel root as `ttbr1`, the boot CPU's
`MAIR/TCR/SCTLR`, the stack, `aarch64_ap_entry` and the index, cleaned
to the point of coherency (`dc cvac` per line, `dsb sy`), and PSCI
`CPU_ON(mpidr, trampoline_pa, mailbox_pa)` is issued. Both physical
addresses come from `kernel_va_to_pa` (image virtual address minus
`kernel_virt_base` plus `kernel_phys_base`), not from `virt_to_phys`: the
mailbox and the trampoline live in the image, not in the direct map, and
the first attempt handed PSCI a bogus address. The caller then polls a
per-CPU flag for up to 200 ms (2000 × 100 µs); a PSCI status other than
`SUCCESS` is `-EIO`, a silent CPU `-ETIMEDOUT`, and in the latter case
the trampoline table is kept forever because the stranded CPU might
still run on it; otherwise `arch_smp_finish` destroys it.

The trampoline (`aarch64_ap_trampoline`, position-independent, in
`.text`) runs with the MMU off at EL1, `x0` = the mailbox's physical
address. It reads **every** mailbox field before touching a system
register — the mailbox is not identity-mapped, only the trampoline page
is, so a read after the MMU came on faulted — then writes `TTBR0/1`,
`MAIR`, `TCR`, `dsb sy; tlbi vmalle1; dsb sy; isb`, `SCTLR_EL1`, `isb`,
sets `sp`, clears `x29/x30` and branches to the mailbox's entry, a
higher-half address, leaving the identity map behind. `aarch64_ap_entry`
sets the started flag, installs `VBAR_EL1` and `TPIDR_EL1`, runs
`arch_mmu_activate(&kernel_space.mmu)` (which replaces the trampoline
table in `TTBR0` with the empty root), enables PAN where present,
initialises the GIC CPU interface (`arch_irqc_init_cpu`), records
`hw_id`, calls `timer_init_cpu` and enters `sched_start_cpu` like the
x86 secondary entry.

## Console, fw_cfg, PCI, shutdown

- **PL011** (`pl011.c`): `arch_console_early_init` uses the `virt`
  default base (0x09000000) through the direct map, resets the mask and
  pending bits, sets 8N1 with FIFOs and enables TX/RX, and registers the
  `pl011` console sink; that page is one of the two `map_early_devices`
  keeps alive across takeover. `arch_console_input_init` reads the SPCR
  (interface type 3 or 0x0E, base at 44, GSIV at 54; a different base is
  mapped with `vm_map_phys`; a missing table or a foreign type keeps the
  defaults with a warning), requests the GSIV (33 on `virt`) level
  triggered with the generic `irq_request`, drains the FIFO, enables
  `RXIM|RTIM` and feeds `tty_input` from the handler as the 16550 driver
  does.
- **fw_cfg** (`fwcfg.c`): the MMIO register block (`virt`: 0x09020000;
  selector at +8 big-endian 16-bit, data at +0 read a byte at a time),
  presence by the `QEMU` signature, a spinlock around every transaction;
  the same file-directory walk as x86. The second early device page.
- **PCI**: `arch_pci_legacy_available` = false; ECAM from the MCFG (the
  `virt` high ECAM at 0x4010000000 is mapped with `vm_map_phys`, which
  handles addresses beyond the direct map); BARs as programmed by the
  firmware, including 64-bit BARs above 4 GiB; MSI-X through GICv2m.
- **Shutdown** (`shutdown.c`): `arch_emulator_exit(code)` issues
  semihosting `SYS_EXIT_EXTENDED` (`hlt #0xF000`, `w0 = 0x20`, `x1` → {
  `ADP_Stopped_ApplicationExit`, `(code << 1) | 1` }) so QEMU's exit
  status matches what the harness already decodes for the x86
  isa-debug-exit device.
- **DMA**: `dma_sync_for_device/for_cpu` call the new `arch_dma_barrier()`
  (`dsb sy` here, a store fence on x86). The `virt` PCI bus is
  DMA-coherent; cache maintenance for non-coherent devices is out of
  scope and stated as such in `docs/kernel/device/`.

## Modules (`modreloc.c`)

`ld.lld -r` objects built with the kernel flags; relocations handled
(`include/aarch64/modreloc.h`): `R_AARCH64_NONE` (0 and 256), `ABS64`,
`ABS32`, `ABS16`, `PREL64`, `PREL32`, `PREL16`, `LD_PREL_LO19`,
`ADR_PREL_LO21`, `ADR_PREL_PG_HI21`, `ADD_ABS_LO12_NC`,
`LDST8/16/32/64/128_ABS_LO12_NC`, `TSTBR14`, `CONDBR19`, `JUMP26`,
`CALL26`. Range checks: `CALL26/JUMP26` ±128 MiB (satisfied by the near
arena), `ADR_PREL_PG_HI21` ±4 GiB, `CONDBR19` ±1 MiB, and the narrow
absolute and PC-relative forms; out of range → `-ERANGE`, an unknown
type → `-ENOEXEC`, both with a reason string the module loader logs.
`aarch64_reloc_apply(type, where, P, S, A, &why)` is a pure function over
a buffer, so `tests/host/test_reloc_aarch64.c` checks each encoding and
each range limit natively. `scripts/check-module-elf.py` takes the
architecture as its second argument and expects `e_machine` 183.

## Stubs and exclusions

- `kernel/arch/aarch64/hv.c`: `arch_hv_probe` returns `-ENOTSUP` with
  caps `none`; every other backend function returns `-ENOTSUP`, zero or
  nothing, and `arch_hv_host_tsc` reads `CNTPCT_EL0`. `/dev/vmm` is not
  created, `vmctl probe` fails and `rc.test` prints `HVTEST: skipped`,
  which the harness requires on this architecture and forbids on x86-64.
- `compat/linux/`: since milestone 10 the personality compiles for
  both architectures with the AArch64 numbers (`nr_aarch64.h`), the
  128-byte `struct stat`, `uname` `aarch64`, the arm64 `rt_sigframe`
  and `clone`'s argument order; user code sets `tpidr_el0` itself and
  the switch hook saves it. The signal frame carries an `fpsimd_context`
  since the FP/SIMD unit, so a handler may use the vector registers
  without losing the interrupted code's; what is still missing here is
  the `esr_context`'s syndrome (carried as 0) and `hello_musl`, which is
  x86-64 machine code and not built for this architecture.
- `tests/hv` and its archive entries are included by the Makefile only
  when `ARCH` is `x86_64`; `tests/linux` builds for both. `rc.test` runs
  `/etc/rc.linux` when `/boot/tests/linux/lxhello` exists (both
  machines now) and prints `LINUXTEST: skipped` otherwise; the kernel
  self-tests that use images or the backend (`hv-*`) skip when they are
  absent.
- `ELF_MACHINE_NATIVE`/`ELF_MACHINE_NATIVE_NAME` (`kernel/elf64.h`) replace
  the literal `EM_X86_64` in the process loader, the module validator and
  their tests (the module test's foreign-machine mutation now uses 0x1234
  since AArch64 is native here).
- `pkg`, `ports`, `libc`, `userland`: unchanged sources; `tools/pkgbuild.py`
  receives the target flags from make as today.

## Ownership, concurrency, memory, errors

Nothing changes above the arch layer. Within it: the GIC maps are
protected by one spinlock (`g_lock`, "gic") taken by route, vector
alloc/free, MSI compose and SGI binding; `mask/unmask` are single
register writes; the IRQ path reads `g_vector_of` and `g_sgi_vector`
without the lock (an INTID is only ever routed once while enabled). The
MMU code uses the generic VMM's locks as on x86 and never sleeps. PSCI
calls are made from the boot CPU during SMP bring-up. The mailbox is a
static per-CPU array in the image. Per-CPU GIC interface and timer
initialisation run on the CPU itself. Errors follow the contract: a
missing GTDT or SPCR is a warning plus the `virt` default; a missing
MADT GIC entry falls back to the `virt` addresses with a warning; a
missing MADT panics as on x86.

## Testing strategy

Details in `testing.md`. In outline:

- The acceptance test is the unchanged boot-test harness: `make
  ARCH=aarch64 test` produces every generic marker (`Architecture:
  aarch64`, the module load lines, `eth0`, `vda`, `USERTEST: PASS`,
  `SELFTEST: PASS`, `SHTEST: PASS`, the package markers,
  `interactive-ok`, `boot complete`) plus `HVTEST: skipped` and
  `LINUXTEST: skipped`, which `run_boot_test.py` requires when
  `COSMO_ARCH` is `aarch64` and forbids on x86-64. The same chain as x86
  follows: `QEMU_SMP=1`, `BUILD=release`, `test-crash` (with
  AArch64-specific panic markers), `MODULE_SIG_ENFORCE=0`, `host-test`,
  `analyze`, `reproducible`.
- The kernel self-tests are architecture-independent and run unchanged
  apart from three tolerances made generic: the PMM and DMA tests accept
  an empty DMA zone (`virt` has no RAM below 16 MiB), the scheduler's
  ACPI check accepts a GIC in place of a LAPIC, and the ELF tests use
  `ELF_MACHINE_NATIVE`. `linux-elf` and the `hv-*` tests log `skipped`.
- Host tests: `tests/host/test_reloc_aarch64.c` (relocation encodings and
  range checks over buffers, ASan/UBSan) joins the suite on every host;
  `test_modelf`'s wrong-machine message became architecture neutral.
- CI: matrix `arch: [x86_64, aarch64]` in the trixie container
  (`qemu-system-arm`, `qemu-efi-aarch64`); artifacts per architecture;
  `check-reproducible.sh` takes the loader name from the architecture.
- Debugging aids: `QEMU_EXTRA="-d int"` works for the GIC path;
  `-semihosting-config` is on so `SYS_WRITE0` could be used from the early
  entry if the UART fails (not wired: the PL011 default is reliable).

## Future extensibility

GICv3 (system-register interface, redistributors, ITS for MSI) behind the
same `arch/irqc.h`; the Linux AArch64 table (a
generic-unistd numbering shared with RISC-V later); an EL2 virtualization
backend behind `arch/hv.h` with stage-2 tables as the GuestMemory and a
vGIC as the VirtualInterrupt; device tree as a second platform
description source; real hardware (Raspberry Pi 4/5 with UEFI).

## Exception level 2

The machine the tests run on (`-machine virt,virtualization=on`) has the
virtualization extensions, and firmware hands the loader control at EL2.
The kernel keeps running at EL1 — a higher-half kernel cannot run at EL2
without VHE, and `cortex-a72`, the CPU model the tests use, is ARMv8.0
and has none — so the arrangement is the one Linux calls nVHE: **the
kernel runs at EL1, and a small resident stub owns EL2 until the
hypervisor claims it.**

### Who installs what

Only code already at EL2 can leave a way back to EL2, and the loader is
the only thing that runs there. So:

1. `cpu_prepare` accepts EL2 and, when it sees it, allocates one page
   (EFI type `EFI_MEMORY_TYPE_COSMO_EL2`, reported as
   `COSMOBOOT_MEM_EL2_STUB`, which the kernel never frees) and copies the
   stub into it. If that page cannot be had, the loader refuses to boot:
   there is no safe way to drop to EL1 leaving `VBAR_EL2` on firmware
   vectors that `ExitBootServices` has invalidated. If the firmware
   refused the loader's memory type, the range is retyped in the map
   with the other loader ranges, so the kernel does not hand the EL2
   vectors to its own allocator. The stub is position-independent and self-contained: a
   vector table plus a handler.
2. `cpu_jump_to_kernel` programs the EL1 translation registers exactly
   as it always did, then programs EL2 — `HCR_EL2.RW` (EL1 is AArch64),
   `CPTR_EL2` with FP traps off, `CNTHCTL_EL2` letting EL1 read the
   counters, `CNTVOFF_EL2 = 0`, `VTTBR_EL2 = 0`, `VPIDR_EL2`/`VMPIDR_EL2`
   mirroring the CPU's own ids, `VBAR_EL2` = the stub — turns the EL2
   MMU **off** (so the stub never depends on firmware page tables the
   kernel will reclaim), and `eret`s to the kernel entry with
   `SPSR_EL2 = EL1h, DAIF masked`. The ERET is the drop to EL1: the
   instruction stream after it runs under the loader's EL1 tables, which
   were already programmed.
3. `cosmoboot_info` gains `el2_stub_phys` (version 5): the stub's
   physical address, or 0 when the machine had no EL2. That is how the
   kernel knows EL2 exists and where its door is.
4. Secondary CPUs come up at EL2 too (PSCI CPU_ON enters at the highest
   implemented level), so `aarch64_ap_trampoline` does the same
   programming before its own `eret` to EL1 — with the MMU off at that
   point, which is simpler than the loader's case.

### The stub's ABI

`HVC #0` from EL1, with the call selector in `x0`:

| `x0` | meaning | returns |
|---|---|---|
| 0 | version | the stub's version (1) |
| 1 | set `VBAR_EL2` to `x1` (a physical address; the EL2 MMU is off) | 0 |
| 2 | restore `VBAR_EL2` to the stub itself | 0 |
| other | — | `-1` |

Anything else that reaches EL2 (an exception, an unknown HVC) returns to
where it came from: the stub is not a hypervisor and refuses to pretend.
`el2_set_vectors` is what the EL2 backend will use to install its own
world-switch vectors; until then the stub is all there is.

PSCI keeps working, and the reason is worth writing down because it is
not what one would guess: with `virtualization=on` the firmware declares
the **SMC** conduit in the FADT (`smp: PSCI 1.1 via SMC` in the boot
log), so PSCI calls never pass through EL2 at all and the stub cannot
intercept them. Without EL2 the same firmware declares HVC
(`via HVC`), which is equally fine because nothing owns EL2 then. A
machine that routed PSCI through EL2 while the kernel held the stub
would need the stub to forward those calls; that case does not arise
here and is recorded as a gap rather than written blind.

## Giving a guest an interrupt (`hv_el2.c`, `hv_el2_switch.S`)

A guest is interrupted by a **virtual** interrupt placed in one of the
GIC's list registers. With `HCR_EL2.IMO` set -- which it is, so that a
host interrupt exits the guest -- the guest's own `ICC_*_EL1` accesses
are redirected by hardware to the *virtual* CPU interface, so it
enables, acknowledges and completes exactly as the host does on the
physical one. **No distributor is emulated and none is needed**: a
virtual interrupt in a list register bypasses one, because the register
*is* the pending state, the group and the priority. Emulating a virtual
distributor, so a guest can run an unmodified GIC driver, is a separate
unit.

**Everything about this is an EL2 register, and this kernel runs at
EL1.** `ICH_HCR_EL2`, `ICH_VMCR_EL2`, `ICH_LR<n>_EL2`, `ICH_AP<n>R0_EL2`
and `ICH_VTR_EL2` cannot be touched from EL1 at all, so the state
travels the way `vttbr` and `hcr` already do: fields in `struct hv_ctx`,
written by the world switch on the way in and read back on the way out.
`vgic_on` gates the lot -- on a machine with no GICv3 virtual interface
those registers do not exist and the accesses would be UNDEFINED.
`HV_EL2_CALL_VGIC` is how the host asks EL2 the two questions it cannot
answer itself: it sets `ICC_SRE_EL2.{SRE,Enable}` so EL1 may use the
system-register interface, and returns `ICH_VTR_EL2`.

| register | why it is per-vCPU |
|---|---|
| `ICH_LR0_EL2` | the interrupt itself: state, group, priority, INTID |
| `ICH_VMCR_EL2` | the guest's *own* `ICC_PMR_EL1` and group enables land here |
| `ICH_AP0R0_EL2`, `ICH_AP1R0_EL2` | the priority the guest is **running at** |
| `ICH_ELRSR_EL2`, `ICH_MISR_EL2` | read back only: what the guest left behind |

The active-priority registers are the subtle ones. A guest that has
acknowledged an interrupt and not completed it is running at that
priority, and the fact lives in an EL2 register shared by every guest on
the CPU. Unsaved, a vCPU destroyed inside its handler leaves its
priority active and the *next* guest on that CPU is refused every
interrupt that does not outrank a dead one's -- which is exactly how
this was found. One register per group covers an implementation with
five priority bits; probe warns above that.

**Taken means delivered, not completed.** `ICC_IAR1_EL1` moves a list
register from Pending to Active and only `ICC_EOIR1_EL1` makes it
Invalid, so `arch_hv_vcpu_irq_taken` is `state != Pending`. A guest that
exits between the two -- a hypercall in its handler, or a host interrupt
-- is holding the interrupt, and calling that untaken would deliver it
a second time.

**One list register.** The generic layer offers one vector at a time, so
a second would carry nothing; the boot counts entries where a
*different* interrupt had to wait for the register, and that number is
0 across every test. A GICv2 machine gets none of this: it virtualises
through `GICH`/`GICV` MMIO frames this kernel does not drive, so
`hv_caps.inject_irq` is false there and `vcpu_inject` returns `-ENOTSUP`.

## The guest's timer (`hv_el2.c`, `hv_el2_switch.S`, `vcpu.c`)

The host's tick is the **physical** timer (`CNTP`, PPI 30), so the
**virtual** one (`CNTV`, PPI 27) is a guest's -- which is what a guest
kernel expects to find at EL1 anyway. That division was already nearly
true; what was not true, and was measured before it was fixed, is that
anything separated them: `CNTV_CTL_EL0` read `0x1` in the host after a
guest armed it, the guest's `ENABLE` live in the host's context, because
no timer register crossed the switch.

Three registers travel now, the way the vGIC's do:

| register | on entry | on exit |
|---|---|---|
| `CNTVOFF_EL2` | the VM's offset | zero: the host's clock is unshifted |
| `CNTV_CVAL_EL0` | the guest's compare | saved |
| `CNTV_CTL_EL0` | the guest's control | saved **before** being disarmed |

`CNTVOFF_EL2` is **one value per VM** -- `struct arch_hv_vm.cntvoff`,
taken from `CNTPCT_EL0` once at creation -- copied into each vCPU's
context, because EL2 assembly reads the context page and nothing else. A
vCPU created later than its siblings therefore sees the same clock they
do; a per-vCPU source would have had a guest's CPUs disagree by however
long apart they were made.

`CNTHCTL_EL2` gates EL1's access to the *physical* counter and timer. The
host is at EL1 and its tick is the physical timer, so the loader's `0x3`
must be in force whenever the host runs; a guest is at EL1 too and must
get neither, so the switch writes `0` for the guest and puts the host's
saved value back on exit. A guest's `mrs CNTPCT_EL0` or `msr CNTP_*` is
then a `SYSREG` exit the owner sees. Getting the restore wrong stops the
host's clock -- a hang, not a wrong number -- which is why the value is
saved on entry rather than assumed.

**An expiry is identified from the timer, not from the exit.**
`HV_EXIT_INTR` carries no INTID and fires for the host's tick just as
readily. The switch saves `CNTV_CTL` *before* disarming it, so the saved
value carries `ISTATUS`, and after every run the backend reads
`ENABLE && !IMASK && ISTATUS` from it -- once per expiry, since the
guest's handler masking or re-arming clears the condition. The generic
layer then injects `arch_hv_guest_timer_intid()` into the same pending
set anything else uses, and the vGIC delivers it: a timer is not a
second kind of delivery. For the expiry to be an exit at all, PPI 27 has
to be enabled in the redistributor, so the backend binds it at probe and
enables it on each CPU as that CPU's switch is installed; its handler
acknowledges and does nothing else, because by the time the host is at
EL1 the level source has been disarmed and the decision was already
made.

**An expired timer would storm.** Restoring `ENABLE` on entry with the
condition already met asserts the PPI before the guest executes an
instruction; `IMO` makes that an exit; the next entry does it again,
forever, and the handler is never reached -- which is how the watchdog
found it. While an expiry is queued the PPI is *disabled* in this CPU's
redistributor: the guest's own `CNTV_CTL` is untouched and reads what it
wrote, the virtual interrupt in the list register is unaffected, and the
physical one cannot exit. Re-enabled once the saved control stops saying
`ISTATUS`.

**Woken on time.** A guest that executes `WFI` with its timer armed and
unexpired has something to wait for and knows exactly when.
`vcpu_run` waits -- until `CVAL + CNTVOFF` on the host's counter, or
until something else becomes pending, in millisecond slices -- *before*
returning the `WFI` exit, which is otherwise unchanged. The owner
re-enters, the timer has by then expired, and the guest takes it on the
first entry. Measured: asked 15.6 ms, the run held 17 ms, fired 2.6 ms
late -- the 250 Hz tick plus the re-entry.

The periodic interrupt the `irq-route` self-test uses was `CNTV`, and is
not now: `arch_test_periodic_irq_start` raises the distributor's spare
SPI from a kernel timer instead. The line the test requests, enables,
counts, masks and releases is as real as before; what asserts it moved
from a compare register to a callback, because the compare register is
a guest's and its PPI is the hypervisor's.

### What this does not do

A virtual distributor (so a guest can run a stock GIC driver), maintenance
interrupts, and the GICv2 `GICH` interface. Each is named rather than
half-built.
