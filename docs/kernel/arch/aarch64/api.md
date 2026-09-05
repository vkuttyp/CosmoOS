# AArch64 port: API

The AArch64 backend implements the architecture interface documented in
`docs/kernel/arch/api.md` (`kernel/include/arch/*.h`) and the per-subsystem
contracts in `docs/kernel/interrupt/api.md` (`arch/irqc.h`),
`docs/kernel/memory/api.md` (`arch/mmu.h`), `docs/kernel/timer/api.md`
(`arch/timer.h`), `docs/kernel/smp/api.md` (`arch/smp.h`, `arch/percpu.h`),
`docs/kernel/scheduler/api.md` (`arch/context.h`), `docs/kernel/process/api.md`
(`arch/user.h`), `docs/kernel/module/api.md` (`arch/module.h`) and
`docs/kernel-services/virtualization/api.md` (`arch/hv.h`). This document
records what those contracts mean on this architecture, the two interface
additions the port made, and the private AArch64 ABI between backend files.
Everything here is **internal kernel ABI**.

---

## Interface additions (both architectures implement them)

### `void arch_dma_barrier(void)` (`arch/cpu.h`)
- **Purpose**: order this CPU's memory writes before a device observes
  them (a DMA descriptor before its doorbell). `dma_sync_for_device` calls
  it.
- **AArch64**: `dsb sy`. x86-64: `sfence`.
- **Concurrency**: pure; any context.

### `void arch_mmu_near_arena(vaddr_t *lo, vaddr_t *hi)` (`arch/mmu.h`)
- **Purpose**: the kernel virtual range modules are loaded into, chosen so
  the architecture's direct branches and code model reach the kernel
  image from there. `vmm_init` copies it into
  `kernel_space.near_lo/near_hi`.
- **AArch64**: `lo` = `__kernel_end` rounded up to 2 MiB, `hi` =
  `0xFFFFFFFF80000000 + 120 MiB`; every module is within ±128 MiB of every
  kernel export (`R_AARCH64_CALL26`). x86-64: `0xFFFFFFFF88000000`–
  `0xFFFFFFFFFF000000`.
- **Outputs**: page aligned, `lo < hi`.

---

## Contract semantics on AArch64

### `arch/cpu.h`
- `arch_name()` = `"aarch64"` (the banner, `Architecture:` line and
  `kernel.arch` sysctl). `arch_cpu_brand_string` = the MIDR decoded to a
  part name (`Cortex-A72 r0p3`, Neoverse parts, ...) or `AArch64
  implementer 0xNN part 0xNNN rNpN`; `"AArch64"` before `aarch64_cpu_init`.
- `arch_cpu_id()` = `TPIDR_EL1->cpu_id`. `arch_cpu_relax` = `yield`.
  `arch_cpu_wait_for_interrupt` = `wfi` (the caller has interrupts
  enabled; WFI wakes on a pending IRQ and the handler runs).
  `arch_cpu_halt_forever` = `msr daifset, #0xF; wfi` loop.

### `arch/irq.h`
- `arch_irq_state_t` is the saved `DAIF`. Save/disable set `DAIF.I`
  (`msr daifset, #2`), restore writes `DAIF` back, `arch_irq_enabled` is
  `DAIF.I == 0`. FIQ, SError and Debug stay masked at all times.

### `arch/trap.h`
- `arch_trap_vector_count()` = 1312. `arch_trap_vector(kind)` = 1024 +
  `kind`; `arch_trap_is_exception(v)` is `1024 <= v < 1030`.
- `arch_trap_name`: the kind names (`"breakpoint"`, `"debug (software
  step)"`, `"divide error (never raised on AArch64)"`, `"undefined
  instruction"`, `"synchronous exception"`, `"page fault"`), `"spurious
  interrupt"` for 1020, else `"SGI"`, `"PPI"`, `"SPI"` or `"interrupt"`.
- `struct arch_trap_frame` (`include/aarch64/trapframe.h`): `x[31]`, `sp`
  (SP_EL0 for a user frame, the interrupted SP for a kernel frame), `elr`,
  `spsr`, `esr`, `far`, `vector`, `kind`; 0x130 bytes. `frame_pc` = `elr`,
  `frame_sp` = `sp`, `frame_fp` = `x[29]`.
- `arch_trap_frame_dump` prints `trap V (name) ESR= EC= FAR=`, `ELR= SPSR=
  SP= EL0|EL1`, `X0..X30`, `SCTLR TCR TTBR0 TTBR1`, and for a page fault
  `FAR=... (protection|not-present read|write user|kernel [reserved-bit]
  [instruction-fetch])`. The crash-test harness matches these lines.
- `arch_trap_unhandled`: an exception vector panics with the frame; an
  interrupt vector is counted and logged as a warning.
- `arch_debug_break()` = `brk #0`; execution resumes after it (see
  `design.md`, BRK).
- `arch_trap_fault_address` = `FAR_EL1`; `arch_trap_fault_flags`: `EXEC`
  for an instruction abort, `WRITE` from `ESR.WnR`, `USER` for a lower-EL
  abort, `PRESENT` for a permission or access-flag fault, `RESERVED` for
  any fault status that is not a translation fault.

### `arch/console.h`
- `arch_console_early_init`: the PL011 at the `virt` default through the
  direct map; registers the `pl011` sink. `arch_console_input_init`:
  SPCR, `irq_request` on the GSIV (33), RX/RT interrupts into `tty_input`.

### `arch/shutdown.h`
- `arch_emulator_exit(code)`: semihosting `SYS_EXIT_EXTENDED` with
  `ADP_Stopped_ApplicationExit` and status `(code << 1) | 1`; QEMU exits
  with that status when `-semihosting-config enable=on,target=native` is
  given, so the harness decodes both architectures identically. Without
  semihosting the `hlt #0xF000` is undefined and the caller's halt follows.

### `arch/backtrace.h`
- Walks the `x29` chain; frames must be 16-byte aligned, lie in kernel
  text or the current thread's stack, and increase monotonically. From a
  frame, `elr` is entry 0 and the walk continues from `x[29]`.

### `arch/fwcfg.h`
- `arch_fwcfg_read`: the MMIO fw_cfg block at the `virt` address (selector
  at +8 big-endian, data at +0 byte-wise); `-ENODEV` without the `QEMU`
  signature, `-ENOENT` for an unknown file, else the file size.

### `arch/irqc.h` (see `docs/kernel/interrupt/api.md`)
- GSI = INTID; `arch_irqc_gsi_count()` = 1020; `arch_irqc_spurious_vector()`
  = 1020; `arch_vector_alloc` returns 1056..1311 (`-ENOSPC` when
  exhausted). `arch_irqc_route(gsi, vector, cpu, flags)`: `-EINVAL` for an
  INTID beyond the distributor's lines, a non-dynamic vector or an
  unregistered CPU. `arch_irqc_msi_compose`: `addr` = GICv2m frame + 0x40,
  `data` = the allocated SPI; `-EINVAL` without a frame, `-ENOSPC` when
  its SPIs are taken. `arch_irqc_eoi(vector)` writes the INTID
  acknowledged on this CPU. `arch_ipi_send/broadcast_others` use SGIs
  0..15, bound to IPI vectors on first use (a seventeenth panics).

### `arch/mmu.h` (see `docs/kernel/memory/api.md`)
- Kernel context = TTBR1 table; user context = TTBR0 table; a VA belongs
  to the context of its bit-55 half (`-EINVAL` otherwise, `query` returns
  false). `arch_mmu_context_init_user` allocates an empty root; nothing is
  copied. `arch_mmu_large_page_sizes()` = 2 MiB. `VM_CACHE_UC` = device
  nGnRnE, `VM_CACHE_WT` = normal non-cacheable, `VM_CACHE_WB` = normal
  write-back. `protect` refuses to split a block (`-EINVAL`); `map` over an
  existing entry or under a block is `-EEXIST`. `arch_mmu_kernel_base()` =
  `0xFFFF800000000000`. `arch_mmu_shootdown` needs no IPI (hardware
  broadcast) and reports one `initiated` plus `n−1` `acks_received` per
  call when other CPUs are online; `arch_mmu_shootdown_ipi_handler` exists
  for the contract and only counts.

### `arch/timer.h`, `arch/testhooks.h`
- Clock `"arch-timer"`, `CNTPCT_EL0` at `CNTFRQ_EL0` Hz. Tick on the EL1
  physical timer PPI (GTDT, default 30) with absolute `CNTP_CVAL_EL0`
  compares; `arch_timer_vector()` is the dynamic vector bound to it.
  `arch_test_periodic_irq_start(hz)` returns INTID 27 (the EL1 virtual
  timer) and runs it with `CNTV_TVAL_EL0` reloads.

### `arch/smp.h`, `arch/percpu.h`
- `arch_smp_boot_hw_id()` = the boot CPU's MPIDR affinity fields packed
  into 32 bits; `hw_id` values come from the MADT GICC entries.
  `arch_smp_start_cpu(cpu, hw_id, stack_top)`: PSCI `CPU_ON`; `-EIO` for a
  PSCI failure, `-ETIMEDOUT` after 200 ms without the CPU reporting in.
  `arch_smp_finish` frees the trampoline's identity table unless a CPU is
  stranded. `arch_percpu_install/get` use `TPIDR_EL1`.

### `arch/context.h`, `arch/user.h`
- `arch_context_init(ctx, stack_top, entry)`: a 96-byte callee-saved frame
  whose first return lands in `aarch64_context_start`. `arch_boot_stack`
  = the 64 KiB `.bss` boot stack. `arch_thread_switch_prepare` switches
  `TTBR0/TTBR1` when they differ and writes `TPIDR_EL0` for process
  threads.
- `arch_user_enter(entry, sp)` never returns: EL0t, interrupts enabled,
  every general register zero. `arch_user_access_begin/end` toggle
  PSTATE.PAN when the CPU has it, else no-ops. `arch_set_tls_base` writes
  `TPIDR_EL0`. `arch_syscall_init_cpu` does nothing (SVC is a vector).
- System-call convention (`uapi/cosmo/syscall.h`, `libc/include/cosmo/
  syscall.h`): `svc #0`, number in `x8`, arguments `x0`–`x5`, result in
  `x0`; `x0`–`x5` and every other register are preserved except `x0`.

### `arch/module.h`
- `arch_module_reloc` applies the `R_AARCH64_*` types listed in `design.md`
  through `aarch64_reloc_apply(type, where, P, S, A, &why)` (pure, tested
  natively). `-ERANGE` out of range, `-ENOEXEC` unknown type.

### `arch/hv.h`
- `arch_hv_probe` returns `-ENOTSUP` and caps `{present = false, name =
  "none"}`; every other function is a stub returning `-ENOTSUP`, `false`,
  zero or nothing. `arch_hv_host_tsc` reads `CNTPCT_EL0`.

---

## Private AArch64 ABI (`kernel/arch/aarch64/include/aarch64/`)

Only files under `kernel/arch/aarch64/` include these.

- `sysreg.h`: `READ_SYSREG(name)` / `WRITE_SYSREG(name, v)`, `isb`,
  `dsb_sy/ish/ishst`, `wfi`, `yield_hint`, `tlbi_vmalle1is`,
  `tlbi_vaae1is`, `current_el`, bit constants for `DAIF`, `SCTLR_EL1`
  (`M`, `C`, `I`, `WXN`, `SPAN`), `TCR_EL1`, `MAIR_EL1` (index 0 WB, 1
  device, 2 NC; `MAIR_VALUE`), `SPSR`, `ESR` (`ESR_EC`, `ESR_ISS`, the EC
  codes, `ESR_ISS_WNR`, `ESR_ISS_FSC`, `FSC_*` classifiers), `CNT_CTL_*`,
  `ID_AA64MMFR0_PARANGE`, `ID_AA64MMFR1_PAN`, `ID_AA64PFR0_GIC`,
  `MPIDR_AFFINITY`, descriptor bits (`DESC_VALID/TABLE/PAGE/AF/NG/AP_USER/
  AP_RO/SH_INNER/PXN/UXN`, `DESC_ATTRIDX`, `DESC_ADDR_MASK`).
- `trapframe.h`: `struct arch_trap_frame` and its `FRAME_OFF_*`/`FRAME_SIZE`
  offsets (assembly ABI with `vectors.S`), `AARCH64_ENTRY_EL1_SYNC/EL1_IRQ/
  EL0_SYNC/EL0_IRQ` and `AARCH64_ENTRY_BAD_BASE` (+ slot index),
  `aarch64_trap_entry(frame)`.
- `platform.h`: `struct aarch64_cpu_info {midr, mpidr, has_pan, parange,
  gic_sysreg, brand[48]}`, `aarch64_cpu_init`, `aarch64_cpu_info`;
  `aarch64_start(info)`, `aarch64_ap_entry(cpu)`; the vector-space
  constants (`GIC_INTID_COUNT` 1020, `GIC_SGI_COUNT` 16, `GIC_PPI_BASE` 16,
  `GIC_SPI_BASE` 32, `VEC_SPURIOUS` 1020, `VEC_SYNC_BASE` 1024,
  `VEC_DYNAMIC_BASE` 1056, `VEC_DYNAMIC_COUNT` 256, `VEC_COUNT` 1312);
  `gic_irq_dispatch(frame)`, `gic_current_intid()`, `gic_bind_ppi(intid,
  vector)`, `gic_enable_local/disable_local(intid)`; `aarch64_hhdm_base`
  (set by `start.c` from the bootinfo); `aarch64_timer_init_cpu()`,
  `aarch64_timer_ack(intid)`; `pl011_early_putc(c)`; the `virt` defaults
  (`VIRT_GICD_BASE` 0x08000000, `VIRT_GICC_BASE` 0x08010000,
  `VIRT_GICV2M_BASE` 0x08020000, `VIRT_PL011_BASE` 0x09000000,
  `VIRT_PL011_INTID` 33, `VIRT_FWCFG_BASE` 0x09020000, timer PPIs 30/27);
  PSCI function ids and return codes; the FADT ARM boot flags.
- `modreloc.h`: the `R_AARCH64_*` type numbers, `aarch64_reloc_apply`,
  `aarch64_reloc_width`.
- Assembly entry points: `_start` (`entry.S`), `aarch64_vectors`
  (`vectors.S`, 2 KiB aligned), `aarch64_user_enter(entry, sp)`,
  `arch_context_switch(from, to)` and `aarch64_context_start`
  (`switch.S`), `aarch64_ap_trampoline`/`aarch64_ap_trampoline_end`
  (`trampoline.S`; mailbox layout at fixed offsets 0x00–0x38 shared with
  `struct aarch64_ap_mailbox` in `smp.c`), `aarch64_boot_stack_bottom/top`.

## Loader interface (`boot/uefi/arch/arch.h`)

Documented in `docs/boot/api.md`: `struct paging_ctx {pool_phys,
pool_pages, pool_used, root, root_user, nx}`, `paging_pool_size`,
`paging_build(ctx, img, loader_base, loader_size, mmap, mmap_size,
desc_size)`, `cpu_prepare` (returns false at any EL but EL1),
`cpu_finish` (nothing), `cpu_halt`, `cpu_jump_to_kernel(pg, stack_top,
info, entry)`, `arch_serial_init/present/putc` (the PL011 at 0x09000000),
`LOADER_ELF_MACHINE` 183, `COSMOBOOT_ARCH_NATIVE` =
`COSMOBOOT_ARCH_AARCH64`.
