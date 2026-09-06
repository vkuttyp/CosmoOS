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

`cpu_prepare` reads `CurrentEL` and requires EL1: the `virt` machine
runs EDK2 at EL1 unless `virtualization=on`, and the loader refuses EL2
(or EL0) with a message rather than demoting itself. There is no EL2
configuration code. `cpu_jump_to_kernel` runs with the MMU still on:
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
state (0x0E), trapped FP/SIMD (0x07: the kernel is built
general-regs-only and user code too) and trapped system-register access
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

IRQ → `gic_irq_dispatch(frame)`: acknowledge (`GICC_IAR`), remember the
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
flags)`; on this architecture **GSI = INTID**. `gic.c` keeps
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
`gic_enable_local`. `arch_irqc_init` programs every SPI group 0,
disabled, inactive, priority 0x80, level, and enables the distributor;
the GICv2m frame's `MSI_TYPER` gives the SPI base and count unless the
MADT entry overrides them, and a range outside the distributor's lines
disables MSI with a warning. A distributor version other than 2 panics.

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
`isb`, `tlbi vmalle1is`. `activate(user)`: `TTBR0_EL1` = root with
ASID 0, `tlbi vmalle1is` (a single ASID and a full invalidate per switch;
ASID allocation is a later optimisation; kernel entries never carry
`nG` so a `vmalle1is` costs the kernel entries too, which is accepted
today). `invalidate(ctx, va, len)`: `dsb ishst`, `tlbi vaae1is` per page
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
  the switch hook saves it. What is still missing here: FP/SIMD at EL0
  (a libc with NEON `memcpy` takes `SIGILL`; the test programs are
  built `-mgeneral-regs-only`), the `esr_context` carries syndrome 0,
  and `hello_musl` (x86-64 machine code) is not built.
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
same `arch/irqc.h`; ASID allocation instead of the full invalidate per
switch; FP/SIMD state save for userland; the Linux AArch64 table (a
generic-unistd numbering shared with RISC-V later); an EL2 virtualization
backend behind `arch/hv.h` with stage-2 tables as the GuestMemory and a
vGIC as the VirtualInterrupt; device tree as a second platform
description source; real hardware (Raspberry Pi 4/5 with UEFI).
