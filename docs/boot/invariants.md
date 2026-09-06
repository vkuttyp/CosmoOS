# Boot: invariants

Violating any of these requires changing this file, the code, and
(where the protocol is affected) `COSMOBOOT_VERSION` in the same change.

## BT1. `cosmoboot.h` is the only thing loader and kernel share

No loader source includes a kernel header and no kernel source includes
a loader header. The protocol header itself contains no pointers, enums
in struct fields, firmware types, or architecture-specific fields, and is
usable from assembly for its constants (`__ASSEMBLER__` guard). A
protocol change is a version bump, never a silent edit.

## BT2. The kernel validates everything before trusting it

`bootinfo_init()` checks magic, version, size, map bounds, alignment,
and overflow, and panics on failure. No kernel code reads a
`cosmoboot_info` field except through `bootinfo_*` accessors after
`bootinfo_init()` has returned. The loader being in the trusted computing
base does not exempt its output from validation (constitution
Invariant 14).

## BT3. The loader rejects any kernel that would violate W^X

`elf_load()` refuses a PT_LOAD with both `PF_W` and `PF_X`, and refuses
two PT_LOADs whose page-rounded ranges overlap (so one page can never
need two permission sets). `paging_build()` derives leaf permissions
only from those flags. There is no override.

## BT4. The only writable-and-executable mappings at kernel entry are the loader's own 2 MiB identity pages

`paging_build()` marks every identity/HHDM 2 MiB page `NX` except those
covering `LoadedImage->ImageBase .. +ImageSize`, which the final `jmp`
executes from after the CR3 switch. Because the PD tables are shared,
the same physical pages are also executable through the HHDM. This is
the explicit architectural reason constitution section 15 demands. The
Phase 2 VMM must discard the bootstrap tables entirely; until then the
kernel must never execute from, or place data in, memory of type
`COSMOBOOT_MEM_LOADER_RECLAIMABLE`.

## BT5. Nothing allocates after `ExitBootServices`

Every buffer the post-exit code writes (bootinfo, map entries, handoff
stack, page-table pool, the raw EFI map buffer) is allocated before the
final `GetMemoryMap`. `g_bs` is set to NULL immediately after exit so an
accidental call faults rather than corrupting firmware state.

## BT6. Everything handed to the kernel lies below 4 GiB

`alloc_pages_low()` uses `AllocateMaxAddress` at `LOADER_ALLOC_LIMIT - 1`.
Consequently every address in `cosmoboot_info` (`mem_map_phys`,
`kernel_phys_base`, `boot_pagetable_root`, the info struct itself) is
inside both the identity map and the HHDM (`hhdm_size` = 4 GiB). A loader
that allocates above the HHDM would hand the kernel pointers it cannot
dereference.

## BT7. NX is enabled before a CR3 that uses it is loaded

`cpu_enable_nx()` precedes `cpu_jump_to_kernel()`. Loading page tables
with bit 63 set while `EFER.NXE` is clear is a reserved-bit fault. The
kernel re-asserts `NXE` and `WP` in `x86_cpu_init()` so it does not depend
on loader behaviour for its own protections.

## BT8. The kernel keeps the bootstrap page tables and boot data until it has replaced them

Memory of types `COSMOBOOT_MEM_BOOT_PAGETABLES` and
`COSMOBOOT_MEM_BOOTINFO` (and, because of the `EfiLoaderData` fallback,
the explicit ranges `boot_pagetable_root`'s pool and the info pages) is
not free memory. The future PMM must reserve these ranges from the
explicit fields, not only from the map types.

## BT9. `.bss` is zero at entry

Guaranteed by `alloc_pages_low()` zeroing the whole kernel span before
`elf_load()` copies segment bytes. The kernel does not re-zero `.bss` in
`entry.S`; a loader for another architecture must provide the same
guarantee.

## BT10. The loader's output is diagnosable on the serial line

Every `die()` reason is a distinct string; the loader prints the loaded
image geometry, page-table pool usage, the translated memory map, and
the jump target before handing over. While boot services are up, output
goes only through `ConOut` (firmware mirrors it to serial where
configured); afterwards only through COM1. Writing both would duplicate
characters on firmware that mirrors.

## BT11. The kernel entry is a jump with fixed register roles, not a call

`cpu_jump_to_kernel()` uses `jmp`; there is no return address, no stack
argument, and no ABI shared between the MS-ABI loader and the SysV
kernel. `RDI` carries the single argument on x86-64. Changing the register
set is a protocol version change.

## BT12. Exactly one kernel path, no configuration

`\cosmo\kernel.elf` on the loader's own volume. A boot menu or config
file is a future feature that adds a path, not a replacement for this
default.

## BT13. On AArch64 the kernel is entered at EL1, and EL2 — if there was any — is left reachable

 Firmware hands the loader control at EL1 or
at EL2 (`-machine virt,virtualization=on`). At EL2 the loader reserves a
page, copies the stub into it, points `VBAR_EL2` there, turns the EL2
MMU off so nothing it owns depends on firmware page tables the kernel
will reclaim, and `eret`s to the kernel entry with the EL1 translation
registers already programmed; `el2_stub_phys` names that page. Handing
over at EL2 without a stub is forbidden: EL1 could neither use EL2 nor
discover that it exists. A loader that started at EL2 and cannot reserve
the page **refuses to boot** rather than handing over: dropping to EL1
with `VBAR_EL2` still pointing at firmware vectors that
`ExitBootServices` has made meaningless would turn the first exception
to EL2 — an `HVC` from EL1, which is how PSCI is routed on some
machines — into a jump through reclaimed memory. The page is also
retyped in the memory map when the firmware refused the loader's own
memory type, exactly as the kernel image and the archive are; otherwise
the kernel would free the vectors EL2 is using. Check: the
`el2` self-test (the stub answers its version, takes a new vector table
and gives it back) and the two boot markers the harness requires when
`QEMU_EL2` is not 0; `QEMU_EL2=0` covers the other side. Gap: only QEMU
firmware has been tried, and only with a stub of one page.
