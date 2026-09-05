# Boot: architecture

## Purpose

Get from firmware to `kernel_main()` with the machine in a documented,
validated state, through a contract the kernel owns. The boot subsystem is
two things: the **cosmoboot protocol** (`boot/protocol/cosmoboot.h`) and
the **UEFI loader** that implements it for x86-64 and, since Phase 13,
AArch64 (`boot/uefi/`, with the CPU- and table-specific parts under
`boot/uefi/arch/<arch>/`). The kernel-side consumer is small
(`kernel/core/bootinfo.c`, `kernel/arch/<arch>/entry.S`) and belongs to
the kernel, but is described here because it is the other half of the
contract.

## Where it sits

```
UEFI firmware (OVMF / EDK2 aarch64 under QEMU; vendor firmware on hardware)
   loads \EFI\BOOT\BOOTX64.EFI or \EFI\BOOT\BOOTAA64.EFI from the FAT ESP
        │
        ▼
boot/uefi (BOOTX64.EFI / BOOTAA64.EFI)         boot services available
   main.c      sequence, memory-map snapshot, ExitBootServices, handoff
   elf.c       validate + copy PT_LOAD segments of \cosmo\kernel.elf (LOADER_ELF_MACHINE)
   memory.c    page allocation below 4 GiB with loader-typed memory
   console.c   firmware console, then the arch serial port after ExitBootServices
   efi.h       minimal UEFI definitions (no external EFI headers; EFIAPI is ms_abi on x86 only)
   arch/arch.h what the generic code asks of an architecture
   arch/x86_64/  paging.c: PML4 (identity + HHDM + kernel); cpu.c: NX probe, EFER.NXE, CR0.WP, the jump; serial.c: COM1
   arch/aarch64/ paging.c: TTBR1 (HHDM + kernel) and TTBR0 (identity) tables with RAM/device attributes
                 from the EFI map; cpu.c: EL1 check, MAIR/TCR/SCTLR, the jump; serial.c: PL011
        │  struct cosmoboot_info in COSMOBOOT_MEM_BOOTINFO memory
        │  x86-64: CR3 = bootstrap tables, RDI = info (HHDM virtual), RSP = handoff stack
        │  AArch64: TTBR1/TTBR0 = bootstrap tables, MMU on, x0 = info, SP = handoff stack, EL1, DAIF masked
        ▼
kernel/arch/<arch>/entry.S   _start: own stack, call x86_start() / aarch64_start()
kernel/arch/<arch>/start.c   console, CPU state (GDT/IDT/PIC or VBAR/TPIDR), CPU identification
kernel/core/bootinfo.c       bootinfo_init(): validate; accessors
kernel/core/main.c           kernel_main()
```

## Responsibilities

Loader:

- Find and read the kernel file from the volume the loader was started
  from.
- Reject anything that is not a well-formed ELF64 executable for the
  loader's own architecture (`LOADER_ELF_MACHINE`: 62 or 183) with W^X
  segments and a matching protocol note.
- Place the kernel in physical memory and map it at its link address with
  permissions taken from the ELF program headers.
- Provide a higher-half direct map of low physical memory so the kernel
  can reach the boot data and, later, page tables.
- Exit boot services correctly (map key discipline, retry).
- Translate the firmware memory map into the protocol's types, marking
  the kernel image, boot data, and page tables distinctly.
- Pass the ACPI RSDP and the EFI system table pointer through.
- Print enough on the serial line that a failed boot is diagnosable.

Kernel side:

- Treat the boot data as untrusted input; validate before use.
- Keep the bootstrap page tables and boot data alive until replaced.

## Non-responsibilities

- Choosing a kernel or presenting a menu. There is one path,
  `\cosmo\kernel.elf`, and no configuration file.
- Setting up a framebuffer, parsing a command line, loading an initrd.
  Fields are reserved in the protocol for a future version.
- Any memory management beyond what is needed to load the kernel. The
  loader allocates, it never frees.
- Being the kernel's page-table manager. The bootstrap tables are a
  handoff artifact; the Phase 2 VMM replaces them.
- Secure Boot policy or signature checking of the kernel file.
- Multiprocessor start-up. The loader runs on the bootstrap processor
  only; other CPUs are started by the kernel in Phase 3.
- Anything Parallels-specific. The loader speaks UEFI and nothing else.

## Interfaces

| Interface | Direction | Where |
|---|---|---|
| `struct cosmoboot_info`, memory types, magic/version, ELF note | loader to kernel | `boot/protocol/cosmoboot.h` |
| Entry state per architecture (x86-64: registers, CR3, RFLAGS.IF, segment state; AArch64: EL1, MMU on with TTBR0/TTBR1, DAIF masked, `x0` = info) | loader to kernel | header comment in `cosmoboot.h`, `boot/uefi/arch/<arch>/cpu.c` `cpu_jump_to_kernel()` |
| `boot/uefi/arch/arch.h` (`paging_*`, `cpu_*`, `arch_serial_*`, `LOADER_ELF_MACHINE`) | generic loader code to its architecture part | `boot/uefi/arch/<arch>/` |
| `.note.cosmoboot` ELF note (name `COSMO`, type 1, desc = version) | kernel to loader | emitted by `entry.S`, parsed by `elf.c` `parse_notes()` |
| `bootinfo_*()` accessors | kernel internal | `kernel/include/kernel/bootinfo.h` |
| UEFI Boot Services subset: `AllocatePages`, `GetMemoryMap`, `HandleProtocol`, `ExitBootServices`, `SetWatchdogTimer`, `Stall`, `Exit`; protocols: LoadedImage, SimpleFileSystem, File, SimpleTextOutput | loader to firmware | `boot/uefi/efi.h` |

The protocol header is the only file shared between `boot/` and
`kernel/`; both get it via `-I$(ROOT)/boot/protocol`.

## Design decisions recorded here

- **Own loader, own protocol**, rather than adopting Limine or another
  third-party boot protocol. The kernel's boot ABI is then a documented
  interface this project controls (constitution sections 6 and 24), and
  AArch64 implements the same struct without inheriting an x86-centric
  loader design: Phase 13 added one field (`boot_pagetable_root_user`)
  and a per-architecture directory, nothing else. The cost is about 900
  lines of generic loader code plus a few hundred per architecture.
- **Higher-half kernel from day one** at `0xFFFFFFFF80000000`, which
  requires the loader to build page tables. Retrofitting higher-half later
  would touch every address assumption in the kernel.
- **Loader-built identity map plus HHDM** rather than the kernel building
  its first tables from a low identity mapping. The kernel starts at its
  final virtual address and never runs low-half code.
- **Contiguous physical placement** of the kernel span so that virtual to
  physical is a single offset; simplifies both the loader and the
  kernel's early physical-address needs.
- **Loader-defined EFI memory types** (`0x80000000` to `0x80000002`) so the
  firmware memory map itself tells the kernel where its image, boot data,
  and page tables are, with explicit ranges in the struct as the
  authoritative fallback.
