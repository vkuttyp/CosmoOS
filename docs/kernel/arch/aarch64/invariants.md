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

Address-space tags rely on it, and so did the full flush that preceded
them: a kernel entry must survive a switch between user spaces, and a
user entry must not be reachable from another space's tag. `leaf_attrs`
sets `nG` exactly when `ARCH_MMU_MAP_USER` is given. **Checked by**
`asid-isolation` (two spaces mapping one address to different bytes, read
across forty switches with no flush between them: with `nG` unset on user
leaves, or the tag left out of `TTBR0`, the second space reads the
first's byte) and `process-spawn` (two processes' identical user
addresses do not alias across a switch).

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
lookup. Each driver keeps its own pair of maps -- only one driver is
ever live (A19). **Checked by** the interrupt and IRQ self-tests (route,
mask, free, MSI compose) and the `smp-call`/`smp-wake` tests under
`QEMU_SMP=4`.

## A10: PPI and SGI enables are re-established on every CPU

`arch_irqc_init_cpu` enables all SGIs plus `g_routed_ppi_mask`, and the
timer enables its PPI in `aarch64_timer_init_cpu`. A PPI routed after a
CPU came up is enabled locally by the caller (`gic_enable_local`). The
registers differ -- the distributor's banked copies under GICv2, the
CPU's own redistributor under GICv3 -- but the rule does not, and
neither does `gic_enable_local`'s meaning: *this* CPU's copy.
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

## A17: The kernel is general-registers-only; userland is not

`-mgeneral-regs-only` on the kernel and the loader, and **not** on
userland or the libc, which use the vector registers as any ordinary
program does. `CPACR_EL1.FPEN` is `0b11` at every CPU's bring-up, so the
instructions are allowed at EL0 and at EL1 -- the field has no encoding
that allows EL0 and traps EL1, and none could be useful, since EL1 is
where the registers are saved and restored.

The kernel's abstention therefore has no hardware to lean on and is
checked instead: `scripts/check-fpregs.sh` disassembles the built image
during `make analyze` and fails on any vector register outside the state
save and restore (`fpuregs.S`), the guest swap and the self-test hooks.

**Checked by** that script (proved by putting a `movi v3.16b, #0` in
`console_set_panic_mode` and watching the build fail), by the compiler
flags in `build/arch/aarch64.mk`, and by `fpu-switch` and `usertest: fpu
isolation` for the property the abstention exists to protect.

## A18: The exit status encoding is the same as x86-64

`arch_emulator_exit(code)` produces QEMU exit status `(code << 1) | 1`
through semihosting, so `run_boot_test.py` decodes 33 as success on both
architectures. **Checked by** every `make ARCH=aarch64 test` run.

## A19: One interrupt controller driver, chosen once

`irqc.c` sets `g_ops` from the MADT's distributor version on the boot
CPU, before any AP runs and before any interrupt is enabled, and never
changes it. Every `arch_irqc_*`, `arch_ipi_*` and `gic_*` call reaches a
controller only through it; no code outside `gic.c` and `gicv3.c`
touches a distributor, redistributor, CPU interface or `ICC_*_EL1`
register, and the two drivers share no state (only `gicv2m.c`, of which
each owns an instance). A version the tree cannot drive panics at
`arch_irqc_init` rather than falling back to a driver that would program
the wrong registers. **Checked by** review, and by the whole suite
passing under both `QEMU_GIC=2` and `QEMU_GIC=3`: a driver reading
another's state would not survive one boot.

## A20: An interrupt arrives on the CPU it was routed to

`arch_irqc_route(gsi, vector, cpu, flags)` means that CPU, and nothing
else. **Checked by** `irq-affinity`, which routes the distributor's
highest line to each online CPU in turn and requires the handler to
report that CPU's id; and by `smp-call`, which does the same for IPIs.
Both are vacuous with one CPU and meaningful from two.

## A21: An MSI reaches the controller or the request fails

`arch_irqc_msi_compose` composes a message only when something can
deliver it: an ITS that has recorded this device and event, or a GICv2m
SPI that is the frame's to give (A9). Otherwise it returns an error and
the driver reports a failed probe. It never hands back an address that
nothing is listening to, which is the shape of every interrupt bug this
port has had -- the SMMU's stolen line, the ITS doorbell the IOMMU was
translating away, the device id no table could hold. Where an IOMMU
stands between the device and the controller, the doorbell page
`arch_irqc_msi_doorbell` names is reserved and identity-mapped in every
domain. **Checked by** `irq-msi-overlap`, `irq-msi-devid`, and the boot
suite under each of `QEMU_MSI=its` and `QEMU_MSI=gicv2m` with and
without `QEMU_IOMMU`.

## A22: A guest's interrupt state belongs to its vCPU

Every `ICH_*_EL2` register the world switch touches is saved into that
vCPU's `struct hv_ctx` on exit and restored from it on entry: the list
register, `ICH_VMCR_EL2`, and both active-priority registers. They are
EL2 registers shared by every guest on the CPU, so anything left behind
is another guest's problem -- a vCPU destroyed inside its handler leaves
an active priority that silently refuses the next guest's interrupts.
The switch touches none of them when `vgic_on` is clear, because on a
machine without a GICv3 virtual interface they do not exist.
`ICH_HCR_EL2` is cleared on the way out: the host takes its own
interrupts through the physical interface. **Checked by**
`el2-vgic-roundtrip` (the state crosses and comes back read from
hardware) and by `el2-guest-irq-masked`, which is the test the leak
broke.

## A23: A guest's timer belongs to its vCPU, and the host's clock to the host

`CNTV_CTL_EL0`, `CNTV_CVAL_EL0` and `CNTVOFF_EL2` are restored from the
running vCPU's `struct hv_ctx` on entry and saved to it on exit, and on
exit the timer is disarmed and the offset zeroed: the host's context
holds nothing of the guest's, and a guest that armed its timer and
exited leaves the host's `CNTV_CTL_EL0` exactly as it was (measured
before the rule existed: `0x1` where `0x2` had been). The offset is one
value per VM, so every vCPU of a VM reads the same `CNTVCT_EL0`.
`CNTHCTL_EL2` is the host's value whenever the host runs and `0`
whenever a guest does, saved on entry rather than assumed, because the
host is at EL1 and its tick is the physical timer -- the wrong value
there stops the host's clock. `CNTV_CTL` is saved *before* it is
disarmed, because its `ISTATUS` is the only trustworthy account of an
expiry: `HV_EXIT_INTR` names no interrupt. **Checked by**
`el2-guest-timer-isolated`, `el2-guest-timer-offset`,
`el2-guest-phys-timer`, and by the suite as a whole -- a wrong
`CNTHCTL_EL2` restore does not fail a test, it hangs the boot.
