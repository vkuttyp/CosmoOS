# Boot: API

Two audiences: loader authors (the protocol and the loader's internal
functions) and kernel code (the `bootinfo_*` accessors). Each entry
follows constitution section 52.

## Protocol (`boot/protocol/cosmoboot.h`)

**ABI stability: stable.** Version 1. Any layout change bumps
`COSMOBOOT_VERSION`; the loader and kernel refuse each other on mismatch
(loader via the ELF note, kernel via `bootinfo_init()`). Additions use
`reserved1` and bump the version. Field meanings are in `design.md`.

Constants a loader must honour: `COSMOBOOT_MAGIC`, `COSMOBOOT_VERSION`,
`COSMOBOOT_NOTE_NAME`/`COSMOBOOT_NOTE_TYPE`, the `COSMOBOOT_MEM_*`,
`COSMOBOOT_ARCH_*`, `COSMOBOOT_FIRMWARE_*` values, and
`COSMOBOOT_LOADER_NAME_MAX` (32, NUL included).

Entry-state obligations for a loader (all architectures): boot services
exited; interrupts disabled; kernel mapped at its link addresses with
W^X from the ELF flags; `[0, hhdm_size)` mapped RW+NX at `hhdm_base`;
bootstrap tables in `COSMOBOOT_MEM_BOOT_PAGETABLES` memory; `.bss`
zeroed; first argument register = HHDM virtual address of the struct;
a valid stack of at least 16 KiB.

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

### `EFI_STATUS paging_build(struct paging_ctx *ctx, const struct elf_image *img, uint64_t loader_base, uint64_t loader_size)`

Purpose: build the bootstrap tables described in `design.md`.
Inputs: `ctx->pool_phys`, `ctx->pool_pages`, `ctx->nx` set by the caller;
the loaded image; the running loader's image range.
Outputs: `ctx->pml4_phys`, `ctx->pool_used`.
Failure: always returns `EFI_SUCCESS`; pool exhaustion is a loader bug and
calls `die()`.

### `bool cpu_has_nx(void)`, `void cpu_enable_nx(void)`, `void cpu_enable_wp(void)`

CPUID `0x80000001` EDX bit 20; `EFER.NXE` via `wrmsr`; `CR0.WP`. Order
matters: enable NX before loading a CR3 that uses bit 63.

### `void cpu_jump_to_kernel(uint64_t cr3, uint64_t stack_top, uint64_t info, uint64_t entry)` (noreturn)

`cli`, load CR3, load RSP, zero RBP, `jmp entry` with `RDI = info`. Must
be called only after ExitBootServices and after NX/WP are set.

### `memcpy`, `memset`, `memcmp`, `strlen`

Freestanding byte loops (`boot/uefi/string.c`), present because the
compiler may emit calls to them.

## x86-64 entry (`kernel/arch/x86_64/entry.S`)

`_start`: the ELF entry point. Expects the x86-64 entry state. Switches
to `x86_boot_stack_top`, pushes a zero frame, calls
`x86_start(const struct cosmoboot_info *)` (declared in
`kernel/arch/x86_64/include/x86/cpu.h`, noreturn). Exports
`x86_boot_stack_bottom`/`x86_boot_stack_top` (64 KiB, 16-byte aligned).
