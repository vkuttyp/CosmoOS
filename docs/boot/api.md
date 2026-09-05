# Boot: API

Two audiences: loader authors (the protocol and the loader's internal
functions) and kernel code (the `bootinfo_*` accessors). Each entry
follows constitution section 52.

## Protocol (`boot/protocol/cosmoboot.h`)

**ABI stability: stable.** Version 3 (version 1 plus the boot archive
fields `archive_phys`/`archive_size` and memory type
`COSMOBOOT_MEM_ARCHIVE` = 13, taken from `reserved1`; version 2 used the
same slots for one raw ELF module, `module_phys`/`module_size` and
`COSMOBOOT_MEM_MODULE`). Any layout change
bumps `COSMOBOOT_VERSION`; the loader and kernel refuse each other on
mismatch (loader via the ELF note, kernel via `bootinfo_init()`).
Additions use `reserved1` and bump the version. Field meanings are in
`design.md`.

Constants a loader must honour: `COSMOBOOT_MAGIC`, `COSMOBOOT_VERSION`,
`COSMOBOOT_NOTE_NAME`/`COSMOBOOT_NOTE_TYPE`, the `COSMOBOOT_MEM_*`,
`COSMOBOOT_ARCH_*`, `COSMOBOOT_FIRMWARE_*` values, and
`COSMOBOOT_LOADER_NAME_MAX` (32, NUL included).

Entry-state obligations for a loader (all architectures): boot services
exited; interrupts disabled; kernel mapped at its link addresses with
W^X from the ELF flags; `[0, hhdm_size)` mapped RW+NX at `hhdm_base`;
bootstrap tables in `COSMOBOOT_MEM_BOOT_PAGETABLES` memory; `.bss`
zeroed; first argument register = HHDM virtual address of the struct;
a valid stack of at least 16 KiB. Archive (v3): either `archive_size`
is 0 and `archive_phys` is 0, or `[archive_phys, archive_phys+archive_size)`
is page-aligned memory inside the direct map holding the unmodified
bytes of the file `\cosmo\boot.tar`, reported as
`COSMOBOOT_MEM_ARCHIVE` (the loader retypes the range itself when the
firmware rejected its memory types, see `mark_range` in `design.md`);
the loader does not validate the archive's contents. The kernel keeps
`ARCHIVE` ranges reserved by map type and treats the bytes as untrusted
input: `bootarchive_init` validates every tar header, `elf_validate`
the `init` entry, `modelf_validate` and `modsig_check` each module. The
archive format is plain ustar as `scripts/mkbootarchive.py` writes it
(`docs/kernel/module/design.md`).

## Kernel-side accessors (`kernel/include/kernel/bootinfo.h`)

Common properties: no allocation, no locks, no sleeping, no I/O; safe in
interrupt and panic context; all are ABI-internal (kernel API, may
change).

### `void bootinfo_init(const struct cosmoboot_info *info)`

Purpose: validate the loader's structure and publish it.
Inputs: `info`, the pointer received at entry (HHDM virtual).
Outputs: none; on success every other accessor becomes usable.
Ownership: does not copy; the loader's memory stays the backing store.
Lifetime: call exactly once, first thing in `kernel_main()`.
Failure: `panic()` with the failing field and value (null pointer, bad
magic, version mismatch, short struct, bad entry size, empty map, map
outside the direct map, unaligned/zero/overflowing entry).

### `const struct cosmoboot_info *bootinfo_get(void)`

Purpose: the validated structure.
Outputs: pointer into `COSMOBOOT_MEM_BOOTINFO` memory; read-only.
Failure: `KASSERT` if called before `bootinfo_init()`.

### `const struct cosmoboot_mem_entry *bootinfo_mem_map(uint32_t *count)`

Purpose: the memory map array.
Inputs: `count` may be NULL.
Outputs: array pointer (HHDM virtual) and entry count. Entries are in
firmware order, merged by type, page aligned, non-overlapping (the last
is checked by the `bootinfo` self-test, not by `bootinfo_init()`).
Failure: `KASSERT` before init.

### `uint64_t bootinfo_usable_bytes(void)`

Purpose: sum of `COSMOBOOT_MEM_USABLE` lengths, for the banner and the
future PMM's initial accounting.

### `uint64_t bootinfo_phys_limit(void)`

Purpose: end address of the highest RAM entry (types usable, ACPI
reclaimable/NVS, loader reclaimable, kernel, bootinfo, page tables,
firmware runtime). MMIO, reserved, bad, and persistent ranges are
excluded, so the value is "how far the PMM's page array must reach".

### `void *bootinfo_phys_to_virt(uint64_t phys)`

Purpose: translate through the HHDM.
Inputs: physical address.
Outputs: `hhdm_base + phys`.
Failure: `panic()` if `phys >= hhdm_size`. This is the only sanctioned
physical-to-virtual conversion until the VMM exists.

### `const char *bootinfo_mem_type_name(uint32_t type)`

Purpose: short name for logs; `"unknown"` for anything unrecognised.
Never NULL.

## Loader internals (`boot/uefi/loader.h`)

All run before ExitBootServices unless stated; single-threaded; none may
be called from an interrupt (the loader has none). Errors are
`EFI_STATUS` values or, where noted, `die()`. **ABI stability: internal
to the loader; free to change.**

### `EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)`

The firmware entry (`/entry:efi_main`). Never returns on success; on
failure `die()` returns `EFI_LOAD_ERROR`-class statuses to the firmware.

### `void console_init(void)`, `void console_firmware_gone(void)`

Probe COM1 and record whether `ConOut` exists; after
`console_firmware_gone()` output goes to the UART only. Neither allocates.

### `void lputs(const char *)`, `void lprintf(const char *fmt, ...)`

Text output. `lprintf` supports `%s %c %d %u %x %X %p %%` with `0` flag,
width, and `l`/`ll`/`z` modifiers read at their real C width (the loader
is LLP64: `long` is 32-bit; use `%llx` with `unsigned long long` casts for
64-bit values). Blocking: polls the UART transmit-empty bit.

### `void die(const char *what, EFI_STATUS status)` (noreturn)

Prints `cosmoboot: FATAL: <what> (status 0x..)`. With boot services:
`Stall(3 s)` then `BS->Exit(image, status)`; without: `cli; hlt` forever.

### `EFI_STATUS alloc_pages_low(UINTN pages, uint32_t type, EFI_PHYSICAL_ADDRESS *out, bool *fallback_used)`

Purpose: zeroed pages below 4 GiB.
Inputs: page count (>0), EFI memory type (standard or `0x80000000`+),
optional fallback flag.
Outputs: physical address in `*out`; `*fallback_used` set if the firmware
rejected a loader-defined type and `EfiLoaderData` was used.
Ownership: the loader never frees; the pages reach the kernel via the
memory map.
Failure: `EFI_INVALID_PARAMETER` for bad arguments; the firmware's
status (typically `EFI_OUT_OF_RESOURCES`) otherwise.

### `KERNEL_PATH`, `ARCHIVE_PATH`

`L"\\cosmo\\kernel.elf"` and `L"\\cosmo\\boot.tar"` on the loader's own
volume (`scripts/mkimage.sh` places both). The kernel is mandatory; the
archive is optional and read by the same static helper
(`read_boot_file` in `main.c`) with `EFI_MEMORY_TYPE_COSMO_ARCHIVE`
(`0x80000003`, `efi.h`). Size limits are the kernel's (0 < size ≤ 64
MiB).

### `EFI_STATUS elf_load(const uint8_t *file, size_t size, struct elf_image *img, bool *fallback_used)`

Purpose: validate and load the kernel.
Inputs: untrusted file buffer and its size.
Outputs: `img->entry`, `virt_base`, `virt_end`, `phys_base`,
`note_version`, `segment_count`, `segments[]` (page-rounded vaddr, size,
PF flags).
Memory: one `alloc_pages_low` of the whole span, type `COSMO_KERNEL`.
Failure: `EFI_LOAD_ERROR` with a printed reason for every malformed
input; `EFI_UNSUPPORTED` for a protocol version mismatch; allocation
status on out-of-memory.

### `UINTN paging_pool_size(const struct elf_image *img)`

Purpose: number of 4 KiB pages `paging_build()` needs, with 4 pages of
slack. Pure function.

### `EFI_STATUS paging_build(struct paging_ctx *ctx, const struct elf_image *img, uint64_t loader_base, uint64_t loader_size, const uint8_t *mmap, UINTN mmap_size, UINTN desc_size)`

Purpose: build the bootstrap tables described in `design.md`
(`boot/uefi/arch/<arch>/paging.c`).
Inputs: `ctx->pool_phys`, `ctx->pool_pages`, `ctx->nx` set by the caller;
the loaded image; the running loader's image range; a snapshot of the
EFI memory map (descriptors of `desc_size` bytes) taken by `main.c`
before the call, which AArch64 uses to give RAM and MMIO different
attributes and x86-64 ignores.
Outputs: `ctx->root` (x86-64: the PML4 for CR3; AArch64: the TTBR1
table), `ctx->root_user` (AArch64: the TTBR0 identity table; x86-64: 0),
`ctx->pool_used`.
Failure: always returns `EFI_SUCCESS`; pool exhaustion is a loader bug and
calls `die()`.

### `bool cpu_prepare(void)`, `void cpu_finish(void)`, `void cpu_halt(void)` (noreturn)

`cpu_prepare` runs first and refuses a CPU the kernel cannot run on:
x86-64 without NX (CPUID `0x80000001` EDX bit 20); AArch64 not at EL1
(the `virt` machine must run with `virtualization=off`). `cpu_finish`
runs after ExitBootServices: x86-64 sets `EFER.NXE` via `wrmsr` and
`CR0.WP` (order matters: enable NX before loading a CR3 that uses
bit 63); AArch64 does nothing. `cpu_halt` masks interrupts and halts.

### `void cpu_jump_to_kernel(const struct paging_ctx *pg, uint64_t stack_top, uint64_t info, uint64_t entry)` (noreturn)

x86-64: `cli`, load CR3 from `pg->root`, load RSP, zero RBP, `jmp entry`
with `RDI = info`. AArch64: mask DAIF, program `MAIR_EL1`, `TCR_EL1`,
`TTBR0_EL1` = `pg->root_user`, `TTBR1_EL1` = `pg->root`, `tlbi vmalle1`,
`SCTLR_EL1` (M, C, I; WXN and A clear) with the MMU kept on throughout,
set `sp`, zero `x29`/`x30`, `br entry` with `x0 = info`. Must be called
only after ExitBootServices and `cpu_finish`.

### `void arch_serial_init(void)`, `bool arch_serial_present(void)`, `void arch_serial_putc(char)`

The loader's own serial output after the firmware console is gone: COM1
(probed) on x86-64, the PL011 at 0x09000000 (assumed present) on AArch64.

### `LOADER_ELF_MACHINE`, `LOADER_ELF_MACHINE_NAME`, `COSMOBOOT_ARCH_NATIVE`

62 / `"x86-64"` / `COSMOBOOT_ARCH_X86_64`, or 183 / `"AArch64"` /
`COSMOBOOT_ARCH_AARCH64`; `elf.c` checks `e_machine` against the first
and `main.c` writes the last into `arch`.

### `memcpy`, `memset`, `memcmp`, `strlen`

Freestanding byte loops (`boot/uefi/string.c`), present because the
compiler may emit calls to them.

## x86-64 entry (`kernel/arch/x86_64/entry.S`)

`_start`: the ELF entry point. Expects the x86-64 entry state. Switches
to `x86_boot_stack_top`, pushes a zero frame, calls
`x86_start(const struct cosmoboot_info *)` (declared in
`kernel/arch/x86_64/include/x86/cpu.h`, noreturn). Exports
`x86_boot_stack_bottom`/`x86_boot_stack_top` (64 KiB, 16-byte aligned).

## AArch64 entry (`kernel/arch/aarch64/entry.S`)

`_start`: expects EL1, the MMU on with the loader's TTBR0/TTBR1 tables,
DAIF masked and `x0` = the bootinfo pointer (a direct-map address). Masks
DAIF again, switches to `aarch64_boot_stack_top`, installs `VBAR_EL1` =
`aarch64_vectors`, zeroes `x29`/`x30` and calls `aarch64_start(const
struct cosmoboot_info *)` (declared in
`kernel/arch/aarch64/include/aarch64/platform.h`, noreturn). Exports
`aarch64_boot_stack_bottom`/`aarch64_boot_stack_top` (64 KiB, 16-byte
aligned). The `.note.cosmoboot` note is emitted here as on x86-64.
