# AArch64 port: invariants

The generic invariants (`docs/kernel/arch/invariants.md` I-ARCH-*, and
each subsystem's) hold unchanged. These are the AArch64-specific ones.
Violating any requires revising this document and the code together.

## A1: No AArch64 knowledge outside the architecture directories

`#ifdef ARCH_AARCH64` / `__aarch64__` appear only under `kernel/arch/`,
`boot/uefi/arch/`, `libc/include/cosmo/syscall.h`, `libc/src/arch/`,
`kernel/include/kernel/elf64.h` (`ELF_MACHINE_NATIVE`) and the two
documented stubs' selectors (`compat/linux/syscalls.c`, the Makefile's
x86-only test lists). **Checked by review**; a grep for `ARCH_AARCH64`
and `__aarch64__` outside those paths must return nothing.

## A2: The exception frame layout is one definition

`struct arch_trap_frame` is 0x130 bytes and `FRAME_OFF_*` in
`trapframe.h` are the offsets `vectors.S` stores to. **Checked by**
`_Static_assert(sizeof == 0x130)` and by the `breakpoint-trap` self-test
reading `elr` from a live frame and finding it in kernel text.

## A3: The kernel runs on SP_EL1 with the vector table 2 KiB aligned

`VBAR_EL1` = `aarch64_vectors` on every CPU before interrupts are enabled
(`_start`, `aarch64_ap_entry`); the SP0, AArch32, FIQ and SError slots
build a frame and panic. **Checked by** the `.balign 2048` in `vectors.S`
and by any boot: a misaligned VBAR write is `UNDEFINED`.

## A4: One context per VA half

A kernel-context mapping has bit 55 set and lives in the TTBR1 table; a
user-context mapping has it clear and lives in that context's TTBR0
table. `arch_mmu_map/unmap/protect` refuse a VA in the wrong half
(`-EINVAL`) and `query` returns false for it. While the kernel context is
active `TTBR0_EL1` holds `g_empty_root`, so no user mapping is reachable.
**Checked by** the VMM self-tests (`vmm`, `process-fault`,
`process-user`) and by `context_destroy`'s assertion that the root
being freed is not the live `TTBR0`.

## A5: Kernel leaves are never `nG`; user leaves always are

The single ASID (0) and the per-switch `tlbi vmalle1is` rely on kernel
entries being global and user entries being tagged. `leaf_attrs` sets
`nG` exactly when `ARCH_MMU_MAP_USER` is given. **Checked by review** and
by the process self-tests (`process-spawn`: two processes' identical user addresses do
not alias across a switch).

## A6: Every mapping decides its own execute permission

Kernel code is `PXN 0, UXN 1`; user code is `UXN 0, PXN 1`; data is both;
intermediate tables carry no `APTable/PXNTable` restriction. `SCTLR.WXN`
stays clear because the direct map is RW and the VMM enforces W^X itself
(`M31`). **Checked by** the `vmm` self-test (protect and fault classification)
and the module loader's per-section permissions.

## A7: The early device pages survive takeover

Every kernel root activated by `arch_mmu_activate` contains the PL011 and
fw_cfg pages at `hhdm_base + 0x09000000/0x09020000` as device memory
(`map_early_devices`). **Checked by** any boot: console output after
`vmm_init` proves it (the first port hung silently here).

## A8: RAM and device memory never share an attribute

The loader's direct map and identity map take the attribute from the EFI
memory map (`is_ram`); undescribed ranges are device. The kernel maps
MMIO with `VM_CACHE_UC` (AttrIdx 1, nGnRnE) and RAM with AttrIdx 0 or 2.
A cacheable MMIO mapping is a fault or silent corruption on real
hardware, so this is enforced at every mapping site. **Checked by
review**; the `virt` machine tolerates less than x86 and the GIC and
PL011 work only because of it.

## A9: GSI = INTID and every routed INTID has exactly one vector

`g_vector_of[intid]` is either the identity (unrouted) or one dynamic
vector, and `g_intid_of[vector]` is its inverse; both change only under
`g_lock`; the IRQ path reads them lock-free. EOI writes the INTID
acknowledged on the same CPU (`g_cur_intid[cpu]`), never a reverse
lookup. **Checked by** the interrupt and IRQ self-tests (route, mask,
free, MSI compose) and the `smp-call`/`smp-wake` tests under `QEMU_SMP=4`.

## A10: PPI and SGI enables are re-established on every CPU

`arch_irqc_init_cpu` enables all SGIs plus `g_routed_ppi_mask`, and the
timer enables its PPI in `aarch64_timer_init_cpu`. A PPI routed after a
CPU came up is enabled locally by the caller (`gic_enable_local`).
**Checked by** the `smp-ticks` self-test: every CPU's tick count advances.

## A11: The tick is an absolute compare

`CNTP_CVAL_EL0` is always the previous compare plus the period, unless
that is already in the past; `g_next_cval[cpu]` is reset to 0 by
`stop_tick`. **Checked by** the `timer` and `smp-ticks` self-tests' rate windows, which
failed with `TVAL` reloads.

## A12: TLB shootdown accounting matches the contract without an IPI

`arch_mmu_shootdown` is a broadcast `tlbi` plus `dsb ish`; when other
CPUs are online it adds one `initiated` and `n−1` `acks_received`. No
IPI vector is used by the MMU. **Checked by** the `smp-shootdown`
self-test's counters.

## A13: The secondary trampoline reads its mailbox before enabling the MMU

Only the trampoline's page is identity-mapped in the temporary TTBR0
table; the mailbox is not. `trampoline.S` loads all eight fields into
registers first. The mailbox is cleaned to the point of coherency before
`CPU_ON`, and both physical addresses passed to PSCI come from
`kernel_va_to_pa` (image addresses), never `virt_to_phys`. **Checked by**
`make ARCH=aarch64 test` bringing up 4 CPUs; a stranded CPU keeps the
identity table alive (`g_ap_stranded`).

## A14: The loader runs at EL1 and refuses anything else

`cpu_prepare` returns false at EL2 or EL0 with a message naming
`virtualization=off`. No EL2 register is ever written. **Checked by** the
harness: `qemu-run.sh` never passes `virtualization=on`.

## A15: BRK resumes after the instruction

After the breakpoint handler returns, `trap.c` adds 4 to `ELR` unless the
handler changed it, so `arch_debug_break()` behaves like x86's `int3`
(trap semantics). **Checked by** the `breakpoint-trap` self-test
completing (a re-fault would loop forever).

## A16: The module arena is within `CALL26` reach

`arch_mmu_near_arena` returns `[align2M(__kernel_end), image base +
120 MiB)`; the relocator rejects a `CALL26/JUMP26` outside ±128 MiB with
`-ERANGE`. **Checked by** the `module-load` self-test (a module calls a
kernel export) and `tests/host/test_reloc_aarch64.c` (range limits).

## A17: The kernel and userland are general-registers-only

`-mgeneral-regs-only` on the kernel, the loader and userland; a trapped
FP/SIMD access is `ARCH_TRAP_INVALID_OPCODE`. The kernel saves no vector
state on a context switch or exception. **Checked by** the compiler flags
in `build/arch/aarch64.mk` (kernel, loader and user flags alike); a violation is a trap at
run time.

## A18: The exit status encoding is the same as x86-64

`arch_emulator_exit(code)` produces QEMU exit status `(code << 1) | 1`
through semihosting, so `run_boot_test.py` decodes 33 as success on both
architectures. **Checked by** every `make ARCH=aarch64 test` run.
