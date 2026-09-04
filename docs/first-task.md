# First engineering task: deliverables

Constitution section 70 defines the first implementation task:

> Establish the project repository, cross-compilation toolchain, QEMU boot
> environment, architecture abstraction, boot protocol, kernel entry point,
> serial console, linker script, build system, and CI pipeline.

This document is the section 72 deliverable set (A through L) for that
task. Subsystem detail lives in `docs/build/` and `docs/boot/`; this file
is the map.

## Verified results

Measured on the macOS secondary host with QEMU 9 under TCG (no hardware
acceleration):

| Check | Result |
|---|---|
| `make BUILD=debug test` | PASS in about 2 s; loader, kernel banner, 5 self-tests, exit status 33 |
| `make BUILD=release test` | PASS in about 2 s; self-tests compiled out |
| `make analyze` | clean, no analyzer reports across kernel and loader sources |
| `make reproducible` | `kernel.elf` and `BOOTX64.EFI` byte-identical across two builds |
| `scripts/check-kernel-elf.sh` (runs at link) | three PT_LOAD segments r-x / r-- / rw-, one PT_NOTE |

Self-tests that pass: `printf`, `string`, `bootinfo`, `irq-state`,
`breakpoint-trap`. The last one takes a real `int3` through the IDT, the
`isr.S` stub, `x86_trap_dispatch()`, `interrupt_dispatch()`, and a
registered handler, then verifies the interrupted PC and that interrupts
are still enabled afterwards.

## A. Architecture

```
UEFI firmware
  └─ boot/uefi  (BOOTX64.EFI, PE/COFF, MS ABI)
       reads \cosmo\kernel.elf, validates ELF, builds bootstrap page
       tables, exits boot services, fills struct cosmoboot_info, jumps
            │  boot/protocol/cosmoboot.h  (the only shared contract)
            ▼
kernel/arch/x86_64/entry.S      switch to kernel stack, call x86_start
kernel/arch/x86_64/start.c      console, GDT/TSS, IDT, PIC mask, CPU features
            │
            ▼  arch/*.h interface headers (kernel/include/arch/)
kernel/core/main.c              validate bootinfo, banner, interrupts on,
                                self-tests, kernel_shutdown()
kernel/interrupt/interrupt.c    vector -> handler table, arch-neutral
kernel/core/{console,log,panic,printf,string,bootinfo,shutdown,selftest}.c
```

Two layers are new architectural elements that outlive this task:

- **The boot protocol** (`boot/protocol/cosmoboot.h`). The kernel depends on
  this struct and nothing else about how it was loaded. Any loader on any
  architecture that produces it can boot the kernel.
- **The architecture interface** (`kernel/include/arch/*.h`): `cpu.h`,
  `irq.h`, `trap.h`, `console.h`, `shutdown.h`, `backtrace.h`. Generic code
  includes only these. Their x86-64 implementation is under
  `kernel/arch/x86_64/`, whose private headers in
  `kernel/arch/x86_64/include/x86/` are not on the generic include path
  (Invariant 1, enforced by `kernel/kernel.mk`).

## B. Interfaces

| Interface | Header | Consumers |
|---|---|---|
| Boot protocol | `boot/protocol/cosmoboot.h` | loader (producer), `kernel/core/bootinfo.c` (consumer) |
| Boot data accessors | `kernel/include/kernel/bootinfo.h` | `main.c`, `selftest.c`, future PMM |
| Console sinks | `kernel/include/kernel/console.h` | `log.c`, `serial.c` |
| Logging | `kernel/include/kernel/log.h` | everything |
| Panic/assert | `kernel/include/kernel/panic.h` | everything |
| Formatter | `kernel/include/kernel/printf.h` | `log.c`, tests |
| Strings | `kernel/include/kernel/string.h` | everything, and the compiler |
| Interrupt dispatch | `kernel/include/kernel/interrupt.h` | arch trap path, future drivers/VMM |
| Shutdown | `kernel/include/kernel/shutdown.h` | `main.c` |
| Arch: CPU | `kernel/include/arch/cpu.h` | `main.c`, `panic.c` |
| Arch: IRQ state | `kernel/include/arch/irq.h` | `interrupt.c`, `panic.c`, tests |
| Arch: traps | `kernel/include/arch/trap.h` | `interrupt.c`, `panic.c`, tests |
| Arch: console | `kernel/include/arch/console.h` | `start.c` |
| Arch: emulator exit | `kernel/include/arch/shutdown.h` | `shutdown.c`, `panic.c` |
| Arch: backtrace | `kernel/include/arch/backtrace.h` | `panic.c` |
| Build | `Makefile` targets and variables | developers, CI |

Per-function contracts are in `docs/boot/api.md`, `docs/build/api.md`,
and the header comments of the kernel headers listed above.

## C. Data structures, ownership, lifetime

| Structure | Defined in | Owner | Lifetime |
|---|---|---|---|
| `struct cosmoboot_info` + memory map | `cosmoboot.h` | loader writes, kernel reads | memory typed `COSMOBOOT_MEM_BOOTINFO`; must stay reserved while `bootinfo_*` is used |
| bootstrap page tables | loader `paging.c` pool | loader creates, kernel keeps | typed `COSMOBOOT_MEM_BOOT_PAGETABLES`; kernel keeps until the Phase 2 VMM installs its own CR3 |
| `struct arch_trap_frame` | `x86/trapframe.h` | the stack of the interrupted context | exists only for the duration of one trap; handlers must not retain pointers |
| interrupt slot table | `interrupt.c` (`g_slots`) | interrupt subsystem | static; holds `fn`/`arg`/`name` pointers it does not own |
| console sink list | `console.c` (`g_sinks`) | console | static; sinks must be immortal (`g_serial_sink` is static) |
| GDT, TSS, double-fault stack | `gdt.c` | arch layer | static, one set for CPU 0; becomes per-CPU in Phase 3 |
| IDT | `idt.c` (`g_idt`) | arch layer | static, shared by all CPUs |
| boot stack (64 KiB) | `entry.S` (`.bss.boot_stack`) | arch layer | static; the boot CPU's stack until threads exist |
| `struct x86_cpu_info` | `cpu.c` (`g_cpu`) | arch layer | static, filled once by `x86_cpu_init()` |

No structure has hidden ownership. Every pointer stored in a table is
documented as borrowed.

## D. Concurrency

There is one CPU and no threads. Every "lock" in this task is interrupt
disabling on the local CPU:

- `interrupt_register/unregister()` run with interrupts disabled via
  `arch_irq_save/restore()`; the handler pointer is published with a release
  store and read with an acquire load in `interrupt_dispatch()`. Phase 3
  adds a grace period before a handler's memory may be reused.
- `console_write()` and `klog()` take no lock. Interleaving between a
  handler and interrupted code is possible but cannot corrupt state.
- `panic()` disables interrupts and is re-entrancy safe through
  `g_panicking`.
- Registration of console sinks and `interrupt_init()` happen before
  interrupts are enabled.

Every function in the trap path (`x86_trap_dispatch`, `interrupt_dispatch`,
handlers, `klog`, `panic`) runs in interrupt context: no sleeping, no
allocation. There is nothing to sleep on or allocate from yet, which is
why this is true by construction; the contracts in the headers make it
binding for later phases.

## E. Memory

The kernel allocates nothing. All memory is static (`.data`/`.bss`) or
loader-provided:

- The loader allocates everything below 4 GiB with `AllocatePages`
  (`alloc_pages_low()`), zeroes it, and types kernel, bootinfo, and
  page-table memory with loader-defined EFI types `0x80000000` to
  `0x80000002`, falling back to `EfiLoaderData` with a warning.
- The kernel image is mapped at `0xFFFFFFFF80000000` (`linker.ld`) with
  permissions from the ELF program headers; physical placement is
  wherever the firmware had room.
- The higher-half direct map at `0xFFFF800000000000` covers physical
  0 to 4 GiB. `bootinfo_phys_to_virt()` is the only translation function
  and panics outside that range.
- `klog()` uses a 256-byte stack buffer; `panic()` a 32-entry PC array.
  Stack depth is bounded and small.

## F. Security

Trust boundaries in this task:

- **Firmware to loader**: the loader trusts the UEFI tables (it has to) but
  bounds-checks the memory map buffer and translates types explicitly.
- **Kernel file to loader**: untrusted bytes. `elf_load()` checks magic,
  class, endianness, machine, type, program-header table bounds, every
  PT_LOAD's file range and address arithmetic, refuses writable+executable
  segments, refuses segments that share a page, requires the entry point
  inside a loaded segment, and requires the protocol note with a matching
  version.
- **Loader to kernel**: `bootinfo_init()` re-validates magic, version, size,
  entry size, map bounds inside the direct map, page alignment, and
  overflow, and panics on any failure.
- **Memory protections in force at `kernel_main`**: W^X on the kernel image
  (RX text, R rodata, RW+NX data), NX on the direct map, `CR0.WP`,
  `CR4.SMEP/SMAP/UMIP` when the CPU has them (QEMU's default `qemu64` CPU
  reports none of the three; the code path is exercised on hardware),
  `CR4.PGE` with the G bit on kernel pages. The transient exception is the
  loader's own 2 MiB identity pages, which stay executable until the Phase
  2 VMM discards the bootstrap tables (`docs/boot/invariants.md`).
- The double fault handler runs on its own IST stack so a kernel stack
  overflow produces a report instead of a triple fault.
- No signature verification of the kernel file yet; Secure Boot
  integration is a later security milestone.

## G. Implementation file map

```
boot/protocol/cosmoboot.h        boot protocol (shared)
boot/uefi/efi.h                  minimal UEFI definitions
boot/uefi/loader.h               loader-internal declarations
boot/uefi/main.c                 efi_main: sequence described in docs/boot/design.md
boot/uefi/elf.c                  ELF64 validation and segment copy
boot/uefi/paging.c               bootstrap page tables
boot/uefi/memory.c               alloc_pages_low()
boot/uefi/cpu.c                  CPUID/NX/WP, cpu_jump_to_kernel()
boot/uefi/console.c              firmware console + COM1, lprintf(), die()
boot/uefi/string.c               memcpy/memset/memcmp/strlen
kernel/include/kernel/*.h        generic kernel headers
kernel/include/arch/*.h          architecture interface
kernel/core/main.c               kernel_main()
kernel/core/bootinfo.c           boot data validation and accessors
kernel/core/console.c            sink fan-out
kernel/core/log.c                klog()/kprintf()
kernel/core/panic.c              panic()/backtrace_print()
kernel/core/printf.c             kvsnprintf()
kernel/core/string.c             string primitives
kernel/core/shutdown.c           kernel_shutdown()
kernel/core/selftest.c           boot-time self-tests
kernel/interrupt/interrupt.c     dispatch table
kernel/arch/x86_64/entry.S       _start, boot stack, .note.cosmoboot
kernel/arch/x86_64/isr.S         256 stubs + common trap path
kernel/arch/x86_64/start.c       x86_start()
kernel/arch/x86_64/cpu.c         CPUID, protection features, arch/cpu.h, arch/irq.h
kernel/arch/x86_64/gdt.c         GDT + TSS + IST stack
kernel/arch/x86_64/idt.c         IDT
kernel/arch/x86_64/pic.c         8259A remap and mask
kernel/arch/x86_64/trap.c        x86_trap_dispatch(), arch/trap.h
kernel/arch/x86_64/serial.c      16550 driver, arch/console.h
kernel/arch/x86_64/backtrace.c   frame-pointer walk
kernel/arch/x86_64/shutdown.c    isa-debug-exit write
kernel/arch/x86_64/linker.ld     image layout
kernel/arch/x86_64/include/x86/  private headers: io, cpu, gdt, idt, pic, serial, trapframe
```

## H. Build integration

`Makefile` at the root includes `build/config.mk`, `build/toolchain.mk`,
`build/rules.mk`, then `kernel/kernel.mk` and `boot/uefi/boot.mk`. See
`docs/build/`. New generic kernel sources are added to
`KERNEL_GENERIC_SRCS` in `kernel/kernel.mk`; new x86-64 sources to
`KERNEL_ARCH_SRCS` in `kernel/arch/x86_64/arch.mk`; new loader sources to
`LOADER_SRCS` in `boot/uefi/boot.mk`.

## I. Tests

| Test | Where | What it proves |
|---|---|---|
| `make check-tools` | `scripts/check-tools.sh` | toolchain present and able to target both triples; firmware found |
| link-time ELF check | `scripts/check-kernel-elf.sh` | W^X segments, PT_NOTE present |
| boot self-tests | `kernel/core/selftest.c` | formatter, strings, boot data, IRQ state, trap path |
| boot test | `tests/boot/run_boot_test.py` | full chain under QEMU; exit status and log markers agree |
| crash test | `make test-crash` | a deliberate fault produces a panic and the harness reports failure (exit 35) |
| static analysis | `make analyze` | no analyzer findings |
| reproducibility | `make reproducible` | identical binaries from identical sources |

There are no host-side unit tests yet: the formatter and string code are
exercised through the in-kernel self-tests. A host harness that compiles
`printf.c`/`string.c` natively is a natural addition once host tooling
(tools/) starts.

## J. QEMU verification

```sh
make test                  # debug: self-tests on
make BUILD=release test    # release: self-tests off
make run                   # interactive, serial on the terminal, Ctrl-A X to quit
```

The harness invokes `scripts/qemu-run.sh` with `-machine q35`,
`-cpu qemu64,+nx`, the OVMF code image on pflash, the FAT image as a SATA
disk, `isa-debug-exit` at port 0xF4, serial on stdio, no display, and
`-no-reboot` so a triple fault ends the run instead of looping. See
`docs/development.md` for the exit-code contract and log format.

## K. Failure modes

| Symptom | Likely cause |
|---|---|
| `cosmoboot: FATAL: cannot read \cosmo\kernel.elf` | image built without the kernel, or firmware has no FAT driver for the medium |
| `cosmoboot: kernel carries no .note.cosmoboot` | kernel linked without `entry.S` or the note section was discarded |
| `cosmoboot: kernel wants boot protocol vN` | loader and kernel built from different protocol versions |
| `cosmoboot: refusing writable+executable segment` | linker script regression; `check-kernel-elf.sh` should have caught it at link time |
| `page-table pool exhausted` | `paging_pool_size()` under-estimates; kernel span grew past the formula |
| `ExitBootServices failed` twice | memory map changed between calls; the retry loop already handles the normal case |
| doubled characters in the loader output | writing both ConOut and COM1 (fixed: ConOut only while boot services are up) |
| `bootinfo: bad magic` | wrong register at entry (RDI) or the info page was overwritten |
| `KERNEL PANIC: unhandled exception 14 (#PF ...)` | access outside the direct map or kernel image; `CR2` line names the address |
| `KERNEL PANIC: unhandled exception 13 (#GP ...)` | bad descriptor or privileged instruction fault; dump shows RIP |
| harness `unexpected QEMU exit code 1` | QEMU failed to start (firmware path, image path); log has the QEMU error |
| harness timeout | kernel hung before `kernel_shutdown()`; log shows the last line reached |
| triple fault (QEMU exits, no panic text) | fault before the IDT was loaded, or a fault on the double-fault path; boot with `QEMU_EXTRA="-d int,cpu_reset"` |

## L. Future compatibility

- **AArch64 (Phase 13)**: the boot protocol struct is architecture-neutral;
  an AArch64 loader fills the same struct and the header gains an
  AArch64 entry-state section. `kernel/arch/aarch64/` implements the same
  six `arch/*.h` headers. `interrupt.c` sizes its table from
  `arch_trap_vector_count()` and maps trap kinds through
  `arch_trap_vector()`, so GIC INTIDs and synchronous exception classes fit
  without generic changes. `arch_emulator_exit()` becomes a semihosting
  call.
- **Memory management (Phase 2)**: the PMM reads `bootinfo_mem_map()`,
  reserves the explicit ranges in `cosmoboot_info` regardless of map
  types, and the VMM replaces `boot_pagetable_root` with its own tables,
  at which point the loader's transient W+X pages and the cacheable MMIO in
  the identity map disappear.
- **Scheduler and SMP (Phase 3)**: `gdt.c`/`idt.c` are written so the IDT
  is shared and the GDT/TSS become per-CPU; `interrupt.c` needs only the
  grace period; `console.c`/`log.c` gain a lock; `arch_cpu_id()` reads the
  LAPIC ID.
- **User mode and syscalls (Phase 4)**: the GDT selector layout already
  satisfies SYSRET's ordering; `isr.S` needs SWAPGS conditioned on the
  saved CS; the TSS `rsp0` setter exists (`gdt_set_kernel_stack()`).
- **Linux ABI (Phase 11)**: nothing here is personality-specific; the trap
  and syscall entry are separate paths by design (constitution section 18).
- **Hypervisor (Phase 12)**: `x86_cpu_info` already records the feature
  bits later phases query; `arch_trap_vector()` keeps VMX/SVM vector
  choices inside the arch layer.
- **Framebuffer, command line, initrd**: `cosmoboot_info.reserved1[8]` is
  reserved for them under a protocol version bump.
