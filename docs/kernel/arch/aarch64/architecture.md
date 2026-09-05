# AArch64 port: architecture

Constitution sections 3 (the second target; AArch64 considered from the
start so that no x86-64 assumption contaminates generic code), 4 (host
versus target: one cross toolchain, two targets), 17 (the interrupt
abstraction: AArch64 "should later provide an equivalent
implementation" of what x86 does with IDT/APIC/IOAPIC/MSI), invariants 1
(generic code independent of the CPU) and 10 (assembly isolated under
`kernel/arch/`), and the Phase 13 roadmap entry: port the generic kernel
to AArch64 with UEFI, the GIC, EL1/EL0 and stage-1 translation, "only
after the generic abstractions have stabilized".

This is stage 1 of the port: the same kernel, the same boot archive and
the same boot-test harness on QEMU's `virt` machine with EDK2 firmware,
booting to the shell and passing the architecture-independent self-tests.
The two x86-only subsystems added in Phases 11 and 12 (the Linux
personality's system-call table and the SVM virtualization backend) are
present as documented stubs; their AArch64 counterparts (the generic
Linux table, an EL2 backend) are later phases.

## Where it sits

```text
  build/arch/aarch64.mk        target triples, code model, QEMU binary
  boot/uefi/                   the loader: generic UEFI code + boot/uefi/arch/<arch>/ (CPU state, bootstrap tables, the jump)
  boot/protocol/cosmoboot.h    protocol v4: a second bootstrap table root (TTBR0) for architectures with split roots
        │
        ▼  cosmoboot_info, MMU on, high-half mapping active, x0 = info
  kernel/arch/aarch64/
        entry.S       _start: boot stack, VBAR_EL1, aarch64_start
        start.c       early console, per-CPU block, CPU identification, kernel_main
        cpu.c         arch/cpu.h: name, MIDR brand, WFI, halt, arch_dma_barrier (dsb sy)
        irq.c         arch/irq.h: DAIF save/restore
        vectors.S     the 16-entry exception vector table, the frame save/restore, aarch64_user_enter
        trap.c        arch/trap.h: ESR classification into the generic trap kinds, the SVC path, fault address and flags
        gic.c         arch/irqc.h: GICv2 distributor/CPU interface, GICv2m MSI, SGIs as IPIs, the vector map
        timer.c       arch/timer.h and arch/testhooks.h: the generic timer (CNTP tick, CNTPCT clock, CNTV for tests)
        mmu.c         arch/mmu.h: 4 KiB granule, 4 levels, TTBR0 (user) / TTBR1 (kernel), 2 MiB blocks, broadcast TLBI,
                      the early device pages, the near arena bounds
        percpu.c      arch/percpu.h: TPIDR_EL1 holds the per-CPU block
        switch.S      arch_context_switch and aarch64_context_start (callee-saved registers only)
        context.c     arch/context.h: thread frames, the switch hook (TTBR compare, TPIDR_EL0 for user TLS)
        user.c        arch/user.h: ERET to EL0, PAN windows when present, TLS base
        smp.c         arch/smp.h: PSCI CPU_ON through SMC or HVC, the per-CPU mailbox, secondary bring-up
        trampoline.S  the MMU-off secondary entry (position independent, identity-mapped page)
        pl011.c       arch/console.h: the UART from SPCR (default 0x09000000), RX interrupt into the tty
        fwcfg.c       arch/fwcfg.h: the MMIO fw_cfg interface of the virt machine
        pci.c         arch/pci.h: no legacy configuration mechanism
        modreloc.c    arch/module.h: R_AARCH64 relocations for kernel modules
        backtrace.c   arch/backtrace.h: the x29 frame chain
        shutdown.c    arch/shutdown.h: semihosting exit with the harness's status encoding
        hv.c          arch/hv.h: no backend (present = false)
        linker.ld     the same image layout as x86-64 at 0xFFFFFFFF80000000
        include/aarch64/ private headers: system registers, GIC, PL011, PSCI, descriptors
        │
        ▼  kernel/include/arch/*.h — unchanged
  generic kernel, kernel-services, drivers (unchanged apart from the ELF machine check and ACPI's GIC entries)
        │
        ▼
  libc/src/arch/aarch64/crt0.S, libc/include/cosmo/syscall.h (svc #0, x8 = number)
  userland, pkg, ports — unchanged sources, rebuilt for the target
```

## Purpose

Run CosmoOS on a second architecture without touching its design: every
subsystem above `kernel/include/arch/` keeps its code, its documents and
its tests, and the AArch64 backend fills the same contract the x86-64
backend fills. Where the contract turned out to carry an x86 assumption
(the ELF machine number, the loader's page-table builder, the boot
protocol's single table root, the harness's architecture marker) the
contract is generalized, not special-cased.

## Responsibilities

- **Build** (`build/arch/aarch64.mk`, `Makefile`): `ARCH=aarch64` selects
  the triples (`aarch64-unknown-none-elf` for kernel, modules and
  userland; `aarch64-unknown-windows` for the PE/COFF loader), the flags
  (`-mgeneral-regs-only`, small code model, no PIC), the loader name
  (`BOOTAA64.EFI`), the QEMU binary and machine, and excludes the x86-only
  test programs (`tests/linux`, `tests/hv`) from the archive.
- **Loader** (`boot/uefi/arch/aarch64/`): require EL1 (the `virt`
  machine hands over at EL1 unless `virtualization=on`; EL2 is refused
  with a message rather than demoted), build the bootstrap translation
  tables (TTBR1: the higher-half kernel image and the 4 GiB direct map
  with RAM and device attributes told apart by the EFI memory map;
  TTBR0: the identity map the loader itself runs on), program
  MAIR/TCR/SCTLR with the MMU kept on, switch tables and jump to the
  kernel at its virtual address with the handoff stack and the bootinfo
  pointer. The loader's own serial output is the PL011 at the `virt`
  address.
- **Kernel backend** (`kernel/arch/aarch64/`): everything listed above,
  behind the unchanged headers.
- **ACPI** (`drivers/acpi/`): decode the GIC entries of the MADT (CPU
  interfaces with MPIDRs, the distributor, the MSI frame) and the PSCI
  flags of the FADT; the GTDT and SPCR for the timer interrupt and the
  console. The x86 build ignores the new entries; the AArch64 build has no
  local APIC.
- **Userland ABI** (`libc/`): the same system-call numbers and structures;
  the instruction is `svc #0` with the number in `x8`, arguments in
  `x0`–`x5`, the result in `x0`. `crt0.S` has an AArch64 version with the
  same CosmoOS note.
- **Tests and harness**: `scripts/qemu-run.sh` dispatches on the
  architecture (`virt` with `virtualization=off`, `gic-version=2` for a
  GICv2 plus a GICv2m MSI frame, `cortex-a72` unless `QEMU_CPU` says
  otherwise, 4 CPUs and 256 MiB, the EDK2 code image padded to the
  64 MiB flash size in `$(OUT)/firmware-aarch64.fd`, semihosting for the
  exit status, the scratch disk attached first so the read-only boot
  image is not `vda`); `run_boot_test.py` reads `COSMO_ARCH` from the
  environment, uses per-architecture panic markers, keeps the x86-only
  marker groups (Linux ABI, virtualization) for x86 only and requires
  the two `skipped` lines on AArch64; `rc.test` runs the Linux section
  only when `/boot/tests/linux/lxhello` exists and the virtualization
  test reports itself skipped when `/dev/vmm` is absent. CI gains an
  `aarch64` matrix entry in the same container.

## Non-responsibilities

- No Linux personality table for AArch64 (the Linux AArch64 numbers
  differ from x86-64's; the personality is selected but every call is
  unknown), no virtualization backend (EL2 with stage-2 translation is a
  later phase), no FP/SIMD state in user threads (userland is built with
  `-mgeneral-regs-only` on both architectures; the kernel does not save
  the vector registers), no big-endian, no 32-bit EL0, no device tree
  (ACPI only, as on x86), no GICv3/ITS (GICv2 + GICv2m is what the
  `virt` machine offers with `gic-version=2`; a GICv3 backend fits the
  same `arch/irqc.h` later), no real hardware beyond QEMU `virt`, no KVM
  acceleration in CI.
- The generic kernel does not learn anything about ARM: no `#ifdef
  ARCH_AARCH64` outside `kernel/arch/`, the loader's arch directory, the
  ELF machine constant and the two stubs.

## Interfaces at a glance

- `kernel/include/arch/*.h`: two additions, both implemented by both
  backends. `arch/cpu.h` gains `arch_dma_barrier()` (the DMA layer's
  ordering point before a doorbell: `sfence` on x86-64, `dsb sy` here).
  `arch/mmu.h` gains `arch_mmu_near_arena()`: the module arena's bounds
  are the architecture's business because they follow its branch reach
  (x86-64 keeps `0xFFFFFFFF88000000`–`0xFFFFFFFFFF000000`; AArch64 uses
  the 2 MiB-aligned end of the image up to image base + 120 MiB so
  `CALL26` reaches every export). `arch_mmu_kernel_base()` returns the
  first higher-half address (`0xFFFF800000000000`) on both.
- `boot/protocol/cosmoboot.h`: version 4 adds `boot_pagetable_root_user`
  (TTBR0 on AArch64; 0 on x86-64) and `COSMOBOOT_ARCH_AARCH64` is now
  emitted.
- `kernel/include/kernel/elf.h`: `ELF_MACHINE_NATIVE` (`EM_X86_64` or
  `EM_AARCH64`) used by the process loader and the module loader.
- `uapi/cosmo/syscall.h`: the AArch64 calling convention documented next
  to the x86-64 one.
- `docs/kernel/arch/aarch64/`: this document, `design.md`, `api.md`,
  `invariants.md`, `testing.md`.
