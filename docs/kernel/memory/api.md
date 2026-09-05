# Memory Subsystem: API

Every function below is part of the kernel-internal ABI. None of it is
user ABI; nothing here is stable across kernel versions, and a change is a
normal code change reviewed against `invariants.md`.

Common contracts, stated once:

- **Blocking:** nothing in this subsystem blocks or sleeps. There is
  nothing to sleep on yet; the contract is recorded so it never becomes a
  surprise when sleeping locks exist.
- **Locks:** the three spinlocks are always taken with interrupts disabled
  (`spin_lock_irqsave`). Lock order, outermost first:
  `vm_space.lock → kmem_cache.lock → pmm_zone.lock`. A function that
  takes a lock says which one.
- **Interrupt context:** every allocator entry point is callable from
  interrupt context. The page-fault handler runs in exception context.
- **Failure modes:** allocation failure returns `NULL`, `0`, or `-ENOMEM`
  and is never silent. Misuse (double free, wrong order, freeing reserved
  memory, unmapping something not owned) is a bug and panics with the
  offending address.

## Kernel virtual layout (x86-64)

| Range | Size | Use |
|---|---|---|
| `0xFFFF800000000000` | 64 TiB reserved | Higher-half direct map (HHDM): every RAM entry of the boot map, RW, NX, write-back, large pages where aligned |
| `0xFFFFC00000000000` – `0xFFFFE00000000000` | 32 TiB | Kernel VA arena: `vm_kernel_alloc`, `vm_map_phys` |
| `0xFFFFFFFF80000000` | 128 MiB | Kernel image at its link address, W^X from ELF flags (`vmm_init` panics if it reaches the near arena) |
| `arch_mmu_near_arena()` | x86-64: `0xFFFFFFFF88000000` – `0xFFFFFFFFFF000000` (~1.9 GiB); AArch64: `align2M(__kernel_end)` – `0xFFFFFFFF80000000 + 120 MiB` | Near arena: `vm_kernel_alloc(VM_KALLOC_NEAR_KERNEL)`, kernel module text/rodata/data (`docs/kernel/module/`); the bounds follow the architecture's branch reach (`-mcmodel=kernel` / `CALL26`) |

Below `0xFFFF800000000000` (`arch_mmu_kernel_base()`, the same value on
both architectures) is user space.

## `kernel/types.h`

| Item | Meaning |
|---|---|
| `paddr_t` | physical address |
| `vaddr_t` | kernel virtual address |
| `pfn_t` | page frame number, `paddr >> 12` |
| `PAGE_SHIFT`, `PAGE_SIZE`, `PAGE_MASK` | 12, 4096, and the address mask |
| `PAGE_2M_SIZE`, `PAGE_1G_SIZE` | large-page sizes |
| `phys_to_pfn(pa)`, `pfn_to_phys(pfn)` | shifts |
| `is_page_aligned(x)`, `page_align_down(x)`, `page_align_up(x)` | 4 KiB alignment helpers |

Pure inline functions; no locks, no failure.

## `kernel/list.h`

Intrusive circular doubly linked list. `struct list_node` is embedded in
the object. `LIST_HEAD(name)`, `list_init`, `list_empty`,
`list_insert_after`, `list_insert_before`, `list_push_front`,
`list_push_back`, `list_remove`, `list_pop_front`, `list_entry`,
`list_first_entry`, `list_for_each`, `list_for_each_safe`,
`list_for_each_entry`, `list_for_each_entry_safe`.

- **Ownership:** the list does not own the objects; the embedding
  structure's owner does.
- **Concurrency:** none; the caller serializes. `list_remove` leaves the
  node self-linked so a removed node is safe to remove again.
- **Failure:** none. `list_pop_front` returns NULL on empty.

## `kernel/spinlock.h`

`spinlock_t`, `SPINLOCK_INIT(name)`, `spinlock_init(lock, name)`.

| Function | Contract |
|---|---|
| `spin_lock(lock)` | Acquire, spinning. Panics if the calling CPU already holds it. Does not touch interrupt state: use only for locks never taken from interrupt context. |
| `spin_unlock(lock)` | Release. `KASSERT`s the lock is held. |
| `spin_trylock(lock)` | One attempt; `true` if acquired. |
| `spin_lock_irqsave(lock)` | Disable interrupts, acquire; returns the state for restore. The form every memory lock uses. |
| `spin_unlock_irqrestore(lock, state)` | Release, then restore interrupts. |
| `spin_is_held(lock)` | `true` if the calling CPU holds it. Used by `KASSERT` in the buddy core. |
| `spin_lock_nested(lock, subclass)`, `spin_lock_irqsave_nested(lock, subclass)` | As the plain forms, annotated for nesting inside another lock of the same class (`docs/kernel/lockdep/api.md`). |

Implemented in `kernel/core/spinlock.c` with `__atomic_exchange_n`
(acquire) and a release store. Not recursive. Debug builds run the
lock-order checker on every acquisition (`docs/kernel/lockdep/`): the name
is the class, and
the documented order is enforced by review.

## `kernel/page.h`

`struct page` (32 bytes, `STATIC_ASSERT`ed): `flags`, `refcount`, `order`,
`zone`, and a union of the buddy list link or the owning `struct slab *`.

Flags: `PG_RESERVED`, `PG_BUDDY`, `PG_SLAB`, `PG_KMALLOC_LARGE`,
`PG_PAGETABLE`, `PG_DEFERRED`.

Globals set by `pmm_init`/`vmm_init`: `pmm_page_array`, `pmm_max_pfn`,
`pmm_hhdm_base`, `pmm_hhdm_limit`.

| Function | Contract |
|---|---|
| `pfn_valid(pfn)` | `pfn < pmm_max_pfn` |
| `page_to_pfn(page)`, `pfn_to_page(pfn)` | array index arithmetic; `pfn_to_page` does not check validity |
| `page_to_phys(page)` | physical address of the frame |
| `phys_to_page(pa)` | descriptor or `NULL` when `pa` is beyond RAM (MMIO, holes above the last RAM entry) |
| `phys_to_virt(pa)`, `virt_to_phys(va)` | direct-map translation; valid only for `pa < pmm_hhdm_limit` (4 GiB before `vmm_init`, all RAM after) |
| `page_to_virt(page)`, `virt_to_page(va)` | compositions of the above; `virt_to_page` returns `NULL` for non-RAM |
| `phys_in_direct_map(pa)` | `pa < pmm_hhdm_limit` |
| `virt_is_direct_map(va)` | `va` lies in the direct map's RAM span (`pmm_hhdm_base <= va < pmm_hhdm_base + pmm_hhdm_limit`); what `dma_map` (Phase 6) requires of a buffer |

All inline, lock-free, no failure other than the documented `NULL`s.

## `kernel/pmm.h`

Constants: `PMM_MAX_ORDER` (11: orders 0..10, 4 KiB..4 MiB),
`PMM_ZONE_DMA` (< 16 MiB), `PMM_ZONE_DMA32` (< 4 GiB), `PMM_ZONE_NORMAL`,
`PMM_ZONE_DMA_LIMIT`, `PMM_ZONE_DMA32_LIMIT`.

Flags: `PMM_FLAGS_ZONE_NORMAL` (0, highest zone with downward fallback),
`PMM_FLAGS_ZONE_DMA32`, `PMM_FLAGS_ZONE_DMA`, `PMM_FLAGS_ZERO`.

Types: `struct pmm_free_area`, `struct pmm_zone`, `struct pmm_node`,
`struct pmm_stats` (`total_pages`, `free_pages`, `reserved_pages`,
`deferred_pages`, `zone_free[]`).

### `void pmm_init(void)`

- **Purpose:** build the page array and zones from the boot memory map and
  release free RAM below the bootstrap direct map into the buddy.
- **Inputs:** `bootinfo_get()`, `bootinfo_mem_map()`, `bootinfo_phys_limit()`.
- **Outputs:** sets `pmm_page_array`, `pmm_max_pfn`, `pmm_hhdm_base`,
  `pmm_hhdm_limit`; logs a summary line `pmm: ...`.
- **Ownership/lifetime:** the page array is a permanent bootmem allocation.
- **Concurrency:** must run once, before interrupts are enabled, before
  `kmalloc_init` and `vmm_init`.
- **Failure:** panics if the map has no RAM or no range can hold the page
  array (`bootmem_alloc` panics).

### `struct page *pmm_alloc_pages(unsigned order, unsigned flags)`

- **Purpose:** allocate `2^order` naturally aligned frames.
- **Inputs:** `order < PMM_MAX_ORDER`; zone flag selects the highest
  acceptable zone, then fallback goes downward; `PMM_FLAGS_ZERO` zeroes
  the block through the direct map.
- **Outputs:** head descriptor with `refcount == 1` and `page->order ==
  order`, or `NULL` when no zone has a block (or `order` is out of range).
- **Ownership:** caller owns the block until `pmm_free_pages` or the
  refcount reaches zero via `pmm_page_put`.
- **Concurrency:** `pmm_zone.lock` (irqsave) per zone tried.
- **Failure:** `NULL`. `KASSERT`s that a zeroed block is inside the direct
  map.

`pmm_alloc_page(flags)` is `pmm_alloc_pages(0, flags)`.

### `void pmm_free_pages(struct page *page, unsigned order)`

- **Purpose:** return a block allocated with the same `order`.
- **Inputs:** the head descriptor, `refcount == 1`, no owner flags
  (`PG_SLAB`, `PG_KMALLOC_LARGE`, `PG_PAGETABLE`), `page->order == order`.
- **Concurrency:** `pmm_zone.lock`.
- **Failure:** panics on reserved page, double free (`PG_BUDDY` set),
  owned page, refcount other than 1, or order mismatch.

`pmm_free_page(page)` is order 0.

### `void pmm_page_get(struct page *)`, `void pmm_page_put(struct page *)`

Atomic reference counting for shared frames. `get` panics on a free
frame. `put` frees with the stored order when the count drops from 1 to
0, else decrements. Lock-free except the free path.

### `void pmm_release_deferred(void)`

Releases every `PG_DEFERRED` run into the buddy. Called by `vmm_init`
after the full direct map is active. `KASSERT`s the direct map covers each
run; panics via `release_range` if a page is not deferred/reserved.

### `void pmm_free_reserved_range(paddr_t base, size_t size)`

Returns a page-aligned `PG_RESERVED` range (refcount 0) to the buddy. Used
for the loader's page tables after takeover. Panics if any page is not
reserved.

### `void pmm_get_stats(struct pmm_stats *)`, `void pmm_dump(void)`

Stats take each zone lock briefly. `pmm_dump` prints per-zone per-order
free counts and runs `buddy_zone_check`, printing `INCONSISTENT` on
failure. Both safe in any context.

### `pmm_zone_of(pa)`, `pmm_zone_name(id)`

Pure.

## `arch/mmu.h`

`vm_prot_t` (`VM_PROT_READ`, `VM_PROT_WRITE`, `VM_PROT_EXEC`, `VM_PROT_RW`,
`VM_PROT_RX`), `vm_cache_t` (`VM_CACHE_WB`, `VM_CACHE_WT`, `VM_CACHE_UC`),
map flags `ARCH_MMU_MAP_LARGE`, `ARCH_MMU_MAP_GLOBAL`,
`struct arch_mmu_context { paddr_t root; }`.

Implemented for x86-64 in `kernel/arch/x86_64/mmu.c`. The implementation
does not lock: the caller (the VMM under `vm_space.lock`) serializes
modification of one context. Table pages come from
`pmm_alloc_page(PMM_FLAGS_ZERO)` (so `pmm_zone.lock` may be taken) and are
flagged `PG_PAGETABLE`.

| Function | Contract |
|---|---|
| `arch_mmu_context_init(ctx)` | allocate an empty root; `0` or `-ENOMEM` |
| `arch_mmu_map(ctx, va, pa, len, prot, cache, flags)` | map page-aligned `[va, va+len)`; uses 1 GiB/2 MiB leaves when `ARCH_MMU_MAP_LARGE` and alignment allow; `-EINVAL` on misalignment or `VM_PROT_NONE`, `-ENOMEM` when a table page cannot be allocated, `-EEXIST` if a page is already mapped (the range may be partially mapped; callers unmap it) |
| `arch_mmu_unmap(ctx, va, len)` | clear leaves in the range, skipping unmapped pages; `-EINVAL` if the range would split a large page, decided before any change; invalidates |
| `arch_mmu_protect(ctx, va, len, prot)` | rewrite leaf permissions, same large-page rule; invalidates |
| `arch_mmu_query(ctx, va, pa, prot, cache, page_size)` | translate one address; `false` if unmapped; outputs may be NULL |
| `arch_mmu_activate(ctx)` | load CR3 |
| `arch_mmu_invalidate(ctx, va, len)` | `invlpg` per page up to 64 pages, else a full flush that also drops global entries (CR4.PGE toggle); current CPU only until Phase 3 |
| `arch_mmu_large_page_sizes()` | bitmask `PAGE_2M_SIZE | PAGE_1G_SIZE` (1 GiB only with `pdpe1gb`) |
| `arch_mmu_kernel_base()` | `0xFFFF800000000000` |

## `arch/trap.h` additions

| Function | Contract |
|---|---|
| `arch_trap_fault_address(frame)` | faulting address (CR2); `KASSERT`s the frame's vector is the page-fault vector |
| `arch_trap_fault_flags(frame)` | `ARCH_FAULT_PRESENT`, `ARCH_FAULT_WRITE`, `ARCH_FAULT_EXEC`, `ARCH_FAULT_USER`, `ARCH_FAULT_RESERVED` decoded from the error code |

`kernel/vmm.h` re-exports these bits as `VM_FAULT_*`.

## `kernel/vmm.h`

Types: `enum vm_region_kind` (`VM_REGION_PHYS`, `VM_REGION_ANON`),
region flags (`VM_REGION_GUARD_BELOW`, `VM_REGION_GUARD_ABOVE`,
`VM_REGION_POPULATED`), `struct vm_region`, `struct vm_space`,
`struct vm_stats` (`regions`, `anon_pages`, `faults_handled`), the global
`kernel_space`.

### `void vmm_init(void)`

- **Purpose:** take over paging from the loader.
- **Requires:** `pmm_init`, `kmalloc_init`, `interrupt_init`.
- **Effects, in order:** creates the `vm_region` cache and a fresh root;
  maps the image sections (`__text_*` RX, `__rodata_*` R,
  `__data_start`..`__bss_end` RW, 4 KiB, global); maps every RAM entry
  into the HHDM (RW, NX, WB, large pages, global); activates the root;
  sets `pmm_hhdm_limit` to the RAM end; frees every
  `COSMOBOOT_MEM_BOOT_PAGETABLES` entry; `pmm_release_deferred()`;
  registers the fault handler on `arch_trap_vector(ARCH_TRAP_PAGE_FAULT)`.
- **Failure:** panics on any mapping or allocation failure.

### `vaddr_t vm_kernel_alloc(size_t size, unsigned flags, vm_prot_t prot)`

- **Purpose:** kernel virtual memory backed by fresh zeroed frames.
- **Inputs:** `size` a non-zero page multiple; `VM_KALLOC_GUARD` adds one
  unmapped page below and above; `VM_KALLOC_POPULATE` maps frames now,
  otherwise the region populates on first touch through the fault path;
  `VM_KALLOC_NEAR_KERNEL` places the region in the near arena (the range
  `arch_mmu_near_arena` reports, above the image) instead of the main
  arena.
- **Outputs:** base address inside the chosen arena, or `0` (bad
  arguments, arena full, region record allocation failure, frame or
  table allocation failure with everything rolled back).
- **Ownership:** the region owns its frames; `vm_kernel_free` releases them.
- **Concurrency:** `vm_space.lock`, then `kmem_cache.lock` (region record),
  then `pmm_zone.lock`.

### `void vm_kernel_free(vaddr_t base)`

Frees every frame the region populated (found by `arch_mmu_query` per
page), unmaps, removes the record. Panics if `base` is not the base of a
live `VM_REGION_ANON` region. Works for both arenas.

### `int vm_kernel_protect(vaddr_t base, vm_prot_t prot)`

- **Purpose:** change the protection of a whole populated kernel
  allocation once its contents are final (module text RW → RX, module
  rodata RW → R).
- **Inputs:** `base` of a live `vm_kernel_alloc` region created with
  `VM_KALLOC_POPULATE`; `prot` without `VM_PROT_USER`, not
  `VM_PROT_NONE`, not both `WRITE` and `EXEC`.
- **Outputs:** `0`; `-EINVAL` for any input outside the above (including
  an unpopulated region); an `arch_mmu_protect` error otherwise.
- **Ownership:** unchanged.
- **Concurrency:** takes `kernel_space.lock` for the lookup and rewrite,
  releases it, then runs a TLB shootdown over the range. Needs
  interrupts enabled (shootdown) and must not be called with
  `kernel_space.lock` held. Thread context only.
- **Guarantee:** when it returns `0`, no CPU holds a stale translation
  with the old protection.

### `vaddr_t vm_map_phys(paddr_t pa, size_t size, vm_prot_t prot, vm_cache_t cache)`

Maps a page-aligned physical range into the arena as a `VM_REGION_PHYS`
region, large pages allowed. For MMIO use `VM_CACHE_UC`. Returns `0` on
failure with nothing left mapped. Owns no frames.

### `void vm_unmap_phys(vaddr_t base)`

Reverse of `vm_map_phys`. Panics if `base` is not a live arena `PHYS`
region.

### `const struct vm_region *vm_find_region(struct vm_space *, vaddr_t)`

Region containing the address, or NULL. Takes and releases
`vm_space.lock`; the pointer is only meaningful while no concurrent free
of that region can happen (single CPU today).

### `bool vm_query(vaddr_t va, paddr_t *pa, vm_prot_t *prot, vm_cache_t *cache, size_t *page_size)`

`arch_mmu_query` on `kernel_space`. Lock-free read of the live tables.

### `void vm_get_stats(struct vm_stats *)`, `void vm_dump(struct vm_space *)`

Take `vm_space.lock`. `vm_dump` prints one line per region.

### Page-fault behaviour (not a function callers invoke)

`vm_fault_handler` runs on the page-fault vector. If the address is in the
kernel half and lies in a `VM_REGION_ANON` region, the fault is
not-present, no reserved bit is set, and the access kind is within the
region's `prot`, it allocates a zeroed frame, maps it, and returns. Every
other case is `panic_frame` with the access kind, fault kind, and the
region description or `no region`. A fault while the calling CPU holds
`vm_space.lock` panics with a distinct message rather than deadlocking.

## `kernel/kmalloc.h`

Constants: `KMEM_ZERO`, `KMALLOC_MIN_ALIGN` (16), `KMALLOC_MAX_SLAB`
(8192), `KMALLOC_MAX_SIZE` (4 MiB). Type: `struct kmem_cache` (public
fields are readable for diagnostics; only the heap writes them).

### `struct kmem_cache *kmem_cache_create(const char *name, size_t object_size, size_t align)`

`align` 0 means 16; otherwise a power of two ≥ 16. Returns NULL if the
object does not fit an order-3 (32 KiB) slab or the record cannot be
allocated. The name must be immortal. Takes the global cache-list lock.

### `void kmem_cache_destroy(struct kmem_cache *)`

Releases retained empty slabs and the record. Panics if objects are live.

### `void *kmem_cache_alloc(cache, flags)`, `void kmem_cache_free(cache, obj)`

Alloc: `kmem_cache.lock`, then `pmm_zone.lock` when a slab is grown.
Returns NULL on frame exhaustion. `KMEM_ZERO` zeroes `object_size` bytes.
Free: panics if `obj` is not in this cache, not an object start, or
already free (bitmap). Empty slabs beyond two are returned to the buddy.

### `size_t kmem_cache_pages(const struct kmem_cache *)`

Frames currently held. Unlocked read.

### `void kmalloc_init(void)`

Bootstraps the cache-of-caches and creates the fifteen size classes
(16 .. 8192). Requires `pmm_init`. Panics on failure. Once only.

### `void *kmalloc(size_t size, unsigned flags)`, `void *kzalloc(size_t size)`

`size` 0 or above `KMALLOC_MAX_SIZE` returns NULL. Up to 8192 bytes: the
smallest size class. Above: whole frames from `pmm_alloc_pages` with
`PG_KMALLOC_LARGE` and the order in `page->order`. Results are 16-byte
aligned. Ownership passes to the caller until `kfree`.

### `void *krealloc(void *ptr, size_t new_size, unsigned flags)`

Resize. Copies `min(kmalloc_size(ptr), new_size)` bytes. With `KMEM_ZERO`
the bytes beyond the old *usable* size are zero; the heap does not record
requested sizes, so bytes between the old request and the old usable size
keep whatever the caller left there. NULL `ptr` behaves as `kmalloc`;
`new_size` 0 frees and returns NULL. On failure the original block is
untouched and NULL is returned. A shrink that stays in the same size class
returns `ptr` unchanged.

### `void kfree(void *ptr)`

NULL is a no-op. Panics if `ptr` is not heap memory, not the start of a
large allocation, or already free.

### `size_t kmalloc_size(const void *ptr)`

Usable size of a live allocation; 0 if `ptr` is not heap memory.

### `void kmalloc_get_stats(struct kmalloc_stats *)`, `void kmalloc_dump(void)`

`live_objects` across the size classes, `slab_pages` across all caches,
`large_pages`. Take the relevant locks briefly.

## Private headers

Not part of the kernel API; included only by `kernel/memory/*.c` and the
host tests in `tests/host/`.

| Header | Contents |
|---|---|
| `kernel/memory/buddy.h` | `buddy_zone_init`, `buddy_alloc_block`, `buddy_free_block`, `buddy_free_range`, `buddy_zone_check`; every function requires `zone->lock` held (asserted) |
| `kernel/memory/bootmem.h` | `bootmem_init`, `bootmem_alloc`, `bootmem_release_all`, `bootmem_seal`, `bootmem_allocated_bytes`; valid only inside `pmm_init` |
| `kernel/memory/slab.h` | `struct slab`, `slab_bootstrap`, `slab_of`, `slab_dump`, `slab_total_pages` |
