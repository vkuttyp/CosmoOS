# Boot: design

## The protocol structure

`struct cosmoboot_info` (`boot/protocol/cosmoboot.h`, version 2). All
fields are fixed-width integers; there are no pointers, enums, or
padding surprises, so the layout is identical on every architecture and
compiler.

| Field | Meaning |
|---|---|
| `magic` | `COSMOBOOT_MAGIC` = `0x3154424F4D534F43` ("COSMOBT1") |
| `version` | `COSMOBOOT_VERSION` = 2 (1: memory map, HHDM, kernel placement, page tables, RSDP; 2: adds the boot module) |
| `size` | `sizeof(struct cosmoboot_info)` as written by the loader; lets a newer kernel detect an older loader |
| `arch` | `COSMOBOOT_ARCH_X86_64` = 1, `COSMOBOOT_ARCH_AARCH64` = 2 |
| `firmware` | `COSMOBOOT_FIRMWARE_UEFI` = 1 |
| `loader_name[32]`, `loader_version` | for the banner: `cosmoboot-uefi`, 1 |
| `hhdm_base`, `hhdm_size` | direct map: virtual `hhdm_base + p` == physical `p` for `p < hhdm_size` |
| `kernel_phys_base`, `kernel_virt_base`, `kernel_size` | where the image landed; `kernel_virt_base` is the lowest PT_LOAD vaddr |
| `boot_pagetable_root` | physical CR3 (TTBR on AArch64) the kernel is running on |
| `mem_map_phys`, `mem_map_entries`, `mem_map_entry_size` | array of `struct cosmoboot_mem_entry { base, length, type, reserved }` |
| `acpi_rsdp` | physical RSDP from the EFI configuration table (ACPI 2.0 preferred, 1.0 fallback), 0 if absent |
| `firmware_system_table` | physical `EFI_SYSTEM_TABLE *` for future runtime-services use |
| `module_phys`, `module_size` | (v2) the one boot module, the initial user executable `\cosmo\init.elf`, in `COSMOBOOT_MEM_MODULE` memory below 4 GiB; both zero when the file is absent |
| `reserved1[6]` | zero; reserved for framebuffer and command line under a version bump |

Memory types (`COSMOBOOT_MEM_*`): `USABLE` 1, `RESERVED` 2,
`ACPI_RECLAIMABLE` 3, `ACPI_NVS` 4, `BAD` 5, `LOADER_RECLAIMABLE` 6,
`KERNEL` 7, `BOOTINFO` 8, `BOOT_PAGETABLES` 9, `FIRMWARE_RUNTIME` 10,
`MMIO` 11, `PERSISTENT` 12, `MODULE` 13 (v2). The PMM frees only
`USABLE` and `LOADER_RECLAIMABLE` ranges (`kernel/memory/bootmem.c`),
so `MODULE` memory, like `KERNEL`, stays reserved and the image is
intact when the kernel copies it into the process's address space.

The ELF note: name `"COSMO\0"` (namesz 6), type `COSMOBOOT_NOTE_TYPE` = 1,
desc = `uint32_t COSMOBOOT_VERSION`. `entry.S` emits it into
`.note.cosmoboot`, `linker.ld` places that section in both the rodata
PT_LOAD and the PT_NOTE. The loader refuses a kernel without the note or
with a different version, so a protocol change can never produce a silent
mis-boot.

## Entry state on x86-64

Set by `cpu_jump_to_kernel()` in `boot/uefi/cpu.c`:

- Long mode, CR3 = `boot_pagetable_root`, `CR0.WP` = 1, `EFER.NXE` = 1,
  interrupts disabled (`cli`). NX is mandatory: a processor without it
  (`cpu_has_nx()` false) makes the loader `die()` before any table is
  built, because the kernel's W^X guarantee cannot be met without it.
- `RDI` = HHDM virtual address of the info struct, `RSP` = top of a 16 KiB
  loader stack (4 pages, `EfiLoaderData`), `RBP` = 0.
- Segment registers are still the firmware's flat descriptors; the kernel
  loads its own GDT before anything else that depends on selectors.
- The kernel's `.bss` is zero because `alloc_pages_low()` zeroes every
  allocation before `elf_load()` copies file bytes into it.

Register roles are fixed by inline-asm constraints (`"a"` cr3, `"d"`
stack, `"D"` info, `"S"` entry) so the sequence cannot clobber an input
before consuming it. The jump is a `jmp`, not a `call`; there is no return
address and no calling convention across the boundary.

## Loader sequence (`boot/uefi/main.c`, `efi_main`)

1. `console_init()`: probe COM1 with the 16550 loopback test; note whether
   `ConOut` exists. `SetWatchdogTimer(0, ...)` so firmware does not reset a
   slow boot.
2. `HandleProtocol(LoadedImage)` on the loader's own handle: needed for the
   device handle (step 3) and for the image base/size (step 6).
3. `read_boot_file(KERNEL_PATH, EfiLoaderData, ...)`: LoadedImage →
   `DeviceHandle` → SimpleFileSystem → `OpenVolume` →
   `Open(L"\\cosmo\\kernel.elf", READ)` → `GetInfo` for the size
   (rejected if 0 or above 64 MiB) → `alloc_pages_low` → `Read`,
   verified to return exactly `FileSize`. Failure is fatal.
3a. The same function with `MODULE_PATH` (`\cosmo\init.elf`) and type
   `EFI_MEMORY_TYPE_COSMO_MODULE`: a missing file is *not* fatal (the
   loader prints `module: \cosmo\init.elf not found; the kernel will run
   without init` and passes zeros); any other error is. The module is
   not parsed by the loader at all: it is opaque bytes for the kernel's
   own ELF validator.
4. `elf_load()` (below). Produces `struct elf_image`.
5. Allocations that must precede ExitBootServices, in order: page-table
   pool (`paging_pool_size()` pages, type `COSMO_PAGETABLES`), bootinfo
   (`BOOTINFO_PAGES` = 5 pages, type `COSMO_BOOTINFO`: the struct followed
   by up to `BOOTINFO_MAX_ENTRIES` map entries), handoff stack
   (`HANDOFF_STACK_PAGES` = 4, `EfiLoaderData`).
6. `paging_build()` (below).
7. Fill the info header, including `module_phys`/`module_size`.
   `find_acpi_rsdp()` scans `ConfigurationTable` for the ACPI 2.0 GUID,
   remembering a 1.0 hit as fallback.
8. Memory map: `GetMemoryMap(size=0)` to learn the size, add 16
   descriptors of slack, allocate the buffer (`EfiLoaderData`, itself a
   map change the slack absorbs). Then up to two rounds of
   `GetMemoryMap` + `ExitBootServices(key)`; the second round exists
   because the first `GetMemoryMap` can itself change the map on some
   firmware. No allocation happens after this point.
9. `console_firmware_gone()`; `g_bs = NULL` so any later boot-services
   call is a null-pointer fault rather than undefined firmware behaviour.
10. `translate_memory_map()` into the bootinfo entry array; 0 entries
    means overflow and is fatal.
11. Print the translated map on the serial line.
12. `cpu_enable_nx()` then `cpu_enable_wp()` (NX presence was verified in
    step 4, before any allocation): NXE must be
    set before a CR3 whose entries carry bit 63 is loaded, otherwise the
    CPU raises a reserved-bit page fault.
13. `cpu_jump_to_kernel()`.

## ELF validation and loading (`boot/uefi/elf.c`)

`elf_load(file, size, img, fallback_used)` treats the file as hostile.
Checks, in order, each fatal with a one-line reason on the console:

- `size >= sizeof(Elf64_Ehdr)`; magic `\177ELF`; class 64; little-endian;
  version 1; `e_type == ET_EXEC`; `e_machine == EM_X86_64`.
- `e_phentsize == sizeof(Elf64_Phdr)`, `e_phnum > 0`, program-header
  table inside the file (`range_in_file`, overflow-safe).
- For each PT_NOTE inside the file: `parse_notes()` walks the note
  records with 4-byte alignment and bounds checks, looking for the
  cosmoboot note.
- For each PT_LOAD: `p_memsz >= p_filesz`; file range inside the buffer;
  `p_vaddr + p_memsz` does not wrap; at most `ELF_MAX_SEGMENTS` (8);
  **not both PF_W and PF_X** (W^X refused at load, not merely at link);
  page-rounded range does not overlap any earlier segment's page-rounded
  range (two permissions cannot share a page).
- At least one PT_LOAD; `e_entry` inside `[lo, hi)`; note present; note
  version equals `COSMOBOOT_VERSION`.

Loading: one `alloc_pages_low()` of `hi - lo` bytes (type `COSMO_KERNEL`)
gives `phys_base`; each PT_LOAD's `p_filesz` bytes are copied to
`phys_base + (p_vaddr - lo)`. Gaps and `.bss` are already zero.

## Bootstrap page tables (`boot/uefi/paging.c`)

Four-level tables built from a pre-sized pool so the walk never allocates.

```
PML4[0]    → PDPT_low → PD_low[0..3] : 2 MiB pages, phys 0-4 GiB, P|RW|PS|NX
                                        (loader image pages: P|RW|PS, no NX)
PML4[256]  → PDPT_low                 : same tables, HHDM at 0xFFFF800000000000
PML4[511]  → PDPT_k → PD_k → PT_k*    : kernel, 4 KiB pages, P|G, RW from PF_W,
                                        NX unless PF_X
```

`paging_pool_size(img)` = 1 (PML4) + (1 + 4) (identity PDPT + one PD per
GiB) + (1 + 1 + span/2MiB + 1) (kernel PDPT, PD, page tables) + 4 slack.
For the current 120 KiB kernel that is 24 pages, of which 13 are used;
`pool_take()` calls `die()` if the estimate is ever wrong.

Intermediate entries are always P|RW without NX; only leaves carry
permissions. `next_level()` creates tables on demand from the pool.
Sharing `PDPT_low` between PML4[0] and PML4[256] keeps identity and HHDM
identical by construction and costs no pages.

The loader's own image (`LoadedImage->ImageBase/ImageSize`, rounded out
to 2 MiB) is the one identity range left executable, because the final
`jmp` executes there after CR3 has switched. Those 2 MiB pages are
therefore writable and executable, through both the identity map and the
HHDM, until the kernel discards the bootstrap tables. This is the single
documented W^X exception; see `invariants.md` BT4.

## Memory allocation policy (`boot/uefi/memory.c`)

`alloc_pages_low(pages, type, out, fallback_used)`:
`AllocatePages(AllocateMaxAddress, type, pages, &addr)` with `addr` =
`LOADER_ALLOC_LIMIT - 1` (4 GiB), so everything the loader hands to the
kernel is inside the identity map and HHDM. Loader-defined types
(`EFI_MEMORY_TYPE_COSMO_KERNEL` `0x80000000`, `_BOOTINFO` `0x80000001`,
`_PAGETABLES` `0x80000002`, `_MODULE` `0x80000003`) live in the range
UEFI reserves for OS loaders; a firmware that returns `EFI_INVALID_PARAMETER` gets a retry
with `EfiLoaderData` and `*fallback_used` is set, which the loader
reports as a warning. The result is always zeroed.

## Memory map translation (`translate_type`, `translate_memory_map`)

Any descriptor with `EFI_MEMORY_RUNTIME` becomes `FIRMWARE_RUNTIME`
regardless of type. Otherwise: Conventional → `USABLE`; LoaderCode/Data
and BootServicesCode/Data → `LOADER_RECLAIMABLE`; RuntimeServices* →
`FIRMWARE_RUNTIME`; ACPIReclaim → `ACPI_RECLAIMABLE`; ACPIMemoryNVS →
`ACPI_NVS`; Unusable → `BAD`; MemoryMappedIO(PortSpace) → `MMIO`;
PersistentMemory → `PERSISTENT`; the four loader types → `KERNEL`,
`BOOTINFO`, `BOOT_PAGETABLES`, `MODULE`; everything else (Reserved, PalCode,
Unaccepted, unknown) → `RESERVED`. Adjacent descriptors with the same
translated type are merged. Descriptors with zero pages are skipped.

## Kernel-side consumer

`entry.S` `_start`: `cli; cld`, load `RSP` with `x86_boot_stack_top`
(64 KiB in `.bss.boot_stack`), zero `RBP`, push two zero words (fake
outermost frame, keeps 16-byte alignment), `call x86_start` with `RDI`
untouched.

`bootinfo_init(info)` (`kernel/core/bootinfo.c`) panics unless: pointer
non-null; magic and version match; `size >= sizeof`; `hhdm_base` and
`hhdm_size` non-zero; `mem_map_entry_size == sizeof(cosmoboot_mem_entry)`;
at least one entry; the map array lies inside the direct map; every entry
has non-zero, page-aligned base and length and does not wrap. Only then
are the accessors usable. `bootinfo_phys_to_virt()` panics for addresses
at or beyond `hhdm_size`.

## Concurrency

None. The loader is single-threaded, never re-entered, and runs on the
bootstrap processor with firmware interrupts still enabled until step 13;
none of its code runs from an interrupt. The kernel-side `bootinfo_*`
accessors are read-only after `bootinfo_init()` and need no locking.

## Error handling

Every firmware call's status is checked. Any failure goes through
`die(what, status)`: prints `cosmoboot: FATAL: <what> (status 0x..)`,
and while boot services are up, stalls 3 s so the message is readable on
screen and returns to firmware with `BS->Exit`; afterwards, halts with
`cli; hlt`. Nothing is ever ignored or retried silently except the
documented single `ExitBootServices` retry.

## Performance

Irrelevant at this stage: the whole loader path is a few milliseconds
under TCG; byte-wise `memcpy` of a 115 KiB kernel is not measurable.

## Security considerations

- Everything read from the file is bounds-checked before use (`elf.c`).
- W^X is enforced at three points: the ELF loader refuses W+X segments,
  the page-table builder derives permissions from the same flags, and the
  build's `check-kernel-elf.sh` catches it earlier still.
- NX is enabled before the tables that use it are loaded; WP is set so
  the kernel cannot write its own read-only mappings by accident.
- The transient loader W+X pages are a known gap closed by the Phase 2
  VMM.
- No authenticity check of the kernel file. Secure Boot signing of
  `BOOTX64.EFI` and a signature check of `kernel.elf` are future work.

## Known limitations

- Physical memory above 4 GiB is neither mapped nor used by the loader
  (all allocations are `AllocateMaxAddress` below 4 GiB); the kernel sees
  it in the map but cannot touch it until the VMM builds a full direct
  map.
- MMIO ranges inside the 0-4 GiB identity map are mapped write-back like
  RAM. The kernel must not access device memory through the bootstrap
  tables without remapping it first.
- No framebuffer or command line; `reserved1` is zero. Exactly one
  module, always read from the fixed path `\cosmo\init.elf`; there is
  no module list, name, or command line for it.
- If the loader-defined memory types fall back to `EfiLoaderData`
  (firmware rejected `0x8000000x`), the translated map would list the
  kernel image, bootinfo, page-table pool, and module as
  `LOADER_RECLAIMABLE`. The loader therefore retypes those four ranges
  from their known placements after translation (`mark_range` in
  `main.c`, splitting entries as needed), so the map the kernel sees is
  identical to the non-fallback case. As a second line, `pmm_init`
  refuses to boot if the entry covering the kernel image is usable or
  reclaimable. OVMF accepts the types, so the fallback path is covered
  by review only.
- A memory map with more than about 630 merged entries would not fit in
  `BOOTINFO_PAGES` and is fatal; real firmware produces well under 200.
- `BOOTX64.EFI` only; no 32-bit UEFI, no legacy BIOS.

## Future extensibility

- **AArch64**: the struct is unchanged. The header gains an "Entry state
  (AArch64)" section: EL1, MMU on with TTBR1 covering the kernel and the
  HHDM, `X0` = info, `SP` = handoff stack, DAIF masked. An AArch64 loader
  (`boot/uefi` compiled for `aarch64-unknown-windows`) reuses `main.c`,
  `elf.c` (with `EM_AARCH64`), `memory.c`, and `console.c` (UART via
  the EFI SerialIo or a PL011 at a firmware-reported address);
  `paging.c` and `cpu.c` become per-architecture files.
- **Protocol version 3** (framebuffer, command line, a module list
  once there is a filesystem-less multi-module need): bump
  `COSMOBOOT_VERSION`, replace `reserved1` fields, and keep `size` so a
  kernel can read an older loader's struct and refuse it cleanly. The
  kernel refuses any version other than its own (`bootinfo_init`).
- **Memory manager handoff**: reservation is by map type, and the
  loader guarantees the types are right even under the `EfiLoaderData`
  fallback by retyping its own ranges (`mark_range`). The PMM's
  kernel-image check is the safety net.
