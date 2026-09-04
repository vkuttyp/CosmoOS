# Memory Subsystem: Invariants

Rules the memory code relies on. Violating one requires changing this
document and the code together. "Checked by" names the mechanism that
turns a violation into a visible failure; "review" means no mechanism
exists yet and the rule is enforced by reading the code.

## Page frames

**M1. `struct page` is 32 bytes.** The page array is `max_pfn * 32` bytes
and every conversion in `kernel/page.h` is index arithmetic on it.
Checked by: `STATIC_ASSERT` in `kernel/include/kernel/page.h`.

**M2. Free-list links live in descriptors, never in frames.** The buddy
never reads or writes a frame's contents; only `PMM_FLAGS_ZERO` and
callers touch frame memory. This is what allows `PG_DEFERRED` frames
(RAM the bootstrap direct map cannot reach) to be tracked before they are
mapped.
Checked by: host `test_buddy` runs with a page array and no memory behind
it at all; any frame access would be a sanitizer fault.

**M3. A frame is in exactly one state:** reserved (`PG_RESERVED`,
refcount 0), deferred (`PG_DEFERRED`, refcount 0), free (`PG_BUDDY` on
the block head, refcount 0), or allocated (refcount ≥ 1, none of those
flags). `pmm_free_pages` panics on any other combination.
Checked by: panics in `pmm_free_pages`, `KASSERT`s in `buddy_free_block`,
`buddy_zone_check` in `pmm_dump` and the host tests.

**M4. `refcount == 0` if and only if the frame is free, reserved, or
deferred.** `pmm_page_get` on a zero count panics; `pmm_page_put` from 1
frees.
Checked by: panics in `pmm_page_get`/`pmm_page_put`, `selftest_pmm`
refcount sequence.

**M5. `zone->nr_pages_free` counts exactly the pages on the free lists.**
`buddy_free_block` adds `1 << order` for the block being returned *before*
coalescing; merged buddies were already counted when they were freed and
are never counted again. (Bring-up bug: adding the coalesced size double
counted every merge; `selftest_pmm` caught it.) `buddy_alloc_block`
subtracts `1 << requested_order`; the halves it pushes back stay counted.
Checked by: `buddy_zone_check` (sums the lists and compares),
`selftest_pmm` and every memory self-test returning to the baseline free
count, host `random_stress` asserting the count after every operation.

**M6. Every block is naturally aligned to its order.** Guaranteed by
construction (`pfn ^ (1 << order)` arithmetic) and required on free.
Checked by: `KASSERT` in `buddy_free_block`, `buddy_zone_check`, host
`alignment_and_orders` and `random_stress`, `selftest_pmm` order-3 and
max-order checks.

**M7. Buddies never merge across a zone end.** A block whose buddy lies
outside `[zone->start_pfn, zone->end_pfn)` stays at its order; a block
never spans two zones.
Checked by: bounds tests in `buddy_free_block`, host
`no_merge_across_zone_end`.

**M8. Owned frames are never freed directly to the buddy.** A frame with
`PG_SLAB`, `PG_KMALLOC_LARGE`, or `PG_PAGETABLE` is returned only by its
owner (`slab_release`, `kfree`, and, for tables, never yet).
Checked by: panic in `pmm_free_pages`.

**M9. Reserved frames are freed only by `pmm_free_reserved_range` and
only for loader page tables.** Everything else reserved (kernel image,
boot info, bootmem allocations, legacy low memory, firmware, holes) stays
reserved for the life of the kernel.
Checked by: `pmm_free_reserved_range` panics on a non-reserved page;
`vmm_init` is the only caller and passes only
`COSMOBOOT_MEM_BOOT_PAGETABLES` entries (review).

**M10. Deferred frames are released only after the direct map covers
them.** `pmm_release_deferred` runs from `vmm_init` after
`arch_mmu_activate` and after `pmm_hhdm_limit` is raised.
Checked by: `KASSERT(phys_in_direct_map(...))` per run in
`pmm_release_deferred`; `selftest_pmm` checks `deferred_pages == 0`.

**M11. No bootmem allocation after sealing.** `bootmem_alloc` is valid
only inside `pmm_init`; the page array and any zone tables are the only
bootmem allocations and are permanent.
Checked by: `KASSERT(!g_sealed)` in `bootmem_alloc`.

**M12. Zeroed allocations are inside the direct map.** `PMM_FLAGS_ZERO`
writes through `page_to_virt`, so the block must be below
`pmm_hhdm_limit`. Before `vmm_init` only frames below 4 GiB are in the
buddy; afterwards the direct map covers all RAM.
Checked by: `KASSERT` in `pmm_alloc_pages`; `table_at` in `mmu.c`
`KASSERT`s the same for page-table pages.

## Page tables

**M13. Kernel page tables are W^X.** Image sections take their
permissions from the linker-script segments (`kernel-text` RX,
`kernel-rodata` R, `kernel-data` RW); the direct map, every arena
allocation, and every `vm_map_phys` window are NX. No code path creates a
writable+executable leaf: `leaf_flags` sets NX whenever `VM_PROT_EXEC` is
absent and no caller passes `VM_PROT_WRITE | VM_PROT_EXEC`.
A processor without NX cannot honour this, so it is refused outright:
the loader dies before building tables (`boot/uefi/main.c`) and the
kernel's MMU probe panics before creating a context and also verifies
`EFER.NXE` is set (`kernel/arch/x86_64/mmu.c`). There is no NX-less mode.
Checked by: `selftest_vmm` queries text (RX), rodata (R), data (RW), and
the direct map (RW); the NX refusal is review (QEMU's `-cpu qemu64,+nx`
always has it).

**M14. Guard pages are unmapped.** `VM_KALLOC_GUARD` reserves one page
below and above in the arena footprint and never maps them; the demand
fault path only populates addresses inside a region's `[base, base+size)`.
Checked by: `selftest_vmm` queries both guards; a touch would panic via
the fault handler (`no region`).

**M15. The direct map covers RAM entries only.** `map_direct_map` walks
the boot memory map and maps entries for which
`bootinfo_mem_type_is_ram` is true; MMIO, reserved, bad, and persistent
ranges are not mapped, so speculative or accidental access to device
memory through the direct map is impossible.
Checked by: review of `map_direct_map`; `phys_to_page` returning NULL
beyond RAM.

**M16. The arena is the only place dynamic kernel mappings appear.**
`vm_kernel_alloc` and `vm_map_phys` allocate from
`[0xFFFFC00000000000, 0xFFFFE00000000000)`; regions never overlap
(footprints including guards are checked on insert).
Checked by: `space_insert` returns `-EEXIST` on overlap; `selftest_vmm`
checks two allocations are disjoint and inside the arena.

**M17. Mapping over an existing mapping is refused.** `arch_mmu_map`
returns `-EEXIST` rather than replacing a present leaf or a large page
covering the range. Callers unmap first.
Checked by: explicit test in `descend`/`arch_mmu_map`; review of callers.

**M18. Unmap and protect never split a large page.** Both refuse with
`-EINVAL` before changing anything if the range does not cover a large
leaf entirely.
Checked by: the two-pass structure in `arch_mmu_unmap`/`arch_mmu_protect`.

**M19. Intermediate page-table pages are not reclaimed (documented
gap).** `arch_mmu_unmap` clears leaves only; empty PT/PD/PDPT pages stay
allocated and flagged `PG_PAGETABLE`. Consequently the first mapping in a
fresh 2 MiB/1 GiB/512 GiB window permanently costs up to three frames.
Tests that compare free-page counts warm the arena up first
(`selftest_vmm`). Reclaiming empty tables is planned for the Phase 4
address-space work.
Checked by: nothing; recorded so nobody "fixes" the warm-up.

## Faults and locking

**M20. A page fault while the calling CPU holds `vm_space.lock` is a
panic, not a hang.** The fault handler checks `spin_is_held` before
taking the lock and reports the address; the spinlock's owner check would
otherwise panic with a less specific message.
Checked by: explicit test in `vm_fault_handler`; `spin_lock` panics on
re-acquisition.

**M21. Only not-present faults on `VM_REGION_ANON` regions within their
protection are handled.** Protection faults, reserved-bit faults, faults
in `VM_REGION_PHYS` regions, faults outside any region, and user-mode
faults all panic with the VMM report. There is no retry and no signal
delivery yet.
Checked by: `make test-crash` (fault outside any region must produce
`page fault: kernel write at 0xffff900000000000 (not present): no
region`); `selftest_vmm` demand-zero path for the handled case.

**M22. Lock order is `vm_space.lock → kmem_cache.lock → pmm_zone.lock`.**
The fault handler takes `vm_space.lock` then `pmm_zone.lock` and never a
cache lock (region lookup does not allocate). The slab takes its cache
lock then a zone lock and never `vm_space.lock`. Nothing takes locks in
the reverse order.
Checked by: review. A lock-order checker is planned with Phase 3
diagnostics.

**M23. Every memory function is non-blocking and interrupt-safe.** All
locks are spinlocks taken with interrupts disabled; nothing sleeps,
waits, or performs I/O.
Checked by: there is no sleeping primitive to call (review).

**M24. No user address spaces exist.** `vm_fault_handler` treats
addresses below `arch_mmu_kernel_base()` as errors; `arch_mmu_map` never
sets the user bit. Phase 4 changes both and this document.
Checked by: review.

## Heap

**M25. Slab double frees are detected.** The per-slab bitmap (1 = free)
is checked on every free; a set bit panics. Free objects are never
written, so a freed object cannot corrupt the freelist.
Checked by: panic in `kmem_cache_free`; host `cache_misuse` and
`kfree_misuse` assert the panic with `EXPECT_PANIC`.

**M26. `kfree` needs no size.** Every frame of a slab carries `PG_SLAB`
and `page->slab`; every large allocation's head carries
`PG_KMALLOC_LARGE` and its order. A pointer that resolves to neither is
not heap memory and panics.
Checked by: panics in `kfree`/`kmem_cache_free`; host `kfree_misuse`.

**M27. Heap results are at least 16-byte aligned.** Slot sizes are rounded
to the cache alignment (≥ 16) and the on-slab header is rounded the same
way; large allocations are page aligned.
Checked by: `selftest_kmalloc` and host `kmalloc_classes`,
`cache_growth_and_shrink` (64-byte alignment).

**M28. Objects never overlap a slab header or each other.** Layout
accounts for the header before placing objects; ASan in the host tests
would flag any write that crossed into a neighbour or the header.
Checked by: host tests under `-fsanitize=address` writing the full
`kmalloc_size` of every allocation.

**M29. At most two empty slabs are retained per cache.** Beyond
`SLAB_KEEP_EMPTY` an emptied slab is returned to the buddy immediately.
Self-tests that compare free-page counts allow for this bounded retention
(`+64` pages of slack in `selftest_kmalloc`).
Checked by: host `cache_growth_and_shrink` (`nr_slabs <= 2` after
freeing everything).

## Kernel arenas (Phase 5)

**M30. The near arena lies inside the top 2 GiB and above the kernel
image.** `KERNEL_NEAR_LO` = `0xFFFFFFFF88000000`, `KERNEL_NEAR_HI` =
`0xFFFFFFFFFF000000`; `vmm_init` panics if `__kernel_end` exceeds
`KERNEL_NEAR_LO`. Code built with `-mcmodel=kernel` placed there can
address itself and the image with sign-extended 32-bit relocations.
Checked by: assert (`vmm_init`), test `module-load` (a module's exported
function is called through a `PC32`-relocated call and returns the right
value), review.

**M31. A kernel allocation is never writable and executable at once.**
`vm_kernel_alloc` and `vm_kernel_protect` refuse `VM_PROT_WRITE |
VM_PROT_EXEC`; a protection change to RX is followed by a shootdown
before the caller sees `0`.
Checked by: test `module-load` (`vm_query` reports RX for module text, R
for rodata, RW for data), review.
