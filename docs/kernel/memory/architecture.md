# Memory Subsystem: Architecture

Phase 2 of the roadmap. This document is the specification the code was
written against; `design.md` records the data structures and algorithms,
`api.md` the contracts, `invariants.md` the rules, `testing.md` the proof.

## 1. Purpose

Own every byte of RAM after boot. Turn the loader's memory map into a
physical page allocator, replace the loader's bootstrap page tables with
kernel-owned tables that map all RAM, provide a kernel virtual-address
allocator with guard pages, handle page faults, and provide a general
kernel heap built on slab caches.

## 2. Where it sits

```text
            kernel core (scheduler, processes, drivers ...)
                      │  kmalloc / kmem_cache / vm_kernel_alloc / vm_map_phys
                      ▼
   ┌───────────────────────────────────────────────────────────┐
   │  kernel heap      kernel/memory/slab.c, kmalloc.c          │
   │       │ pages                                             │
   │  virtual memory   kernel/memory/vmm.c  (spaces, regions,  │
   │       │           faults, kernel VA arena)                │
   │       │ arch_mmu_*          ▲ arch_trap_fault_*           │
   │  physical memory  kernel/memory/pmm.c, buddy.c, bootmem.c │
   │       │ struct page, zones, buddy free lists              │
   └───────┼───────────────────────────────────────────────────┘
           │ cosmoboot memory map (kernel/core/bootinfo.c)
           ▼
   arch page tables   kernel/arch/x86_64/mmu.c   (4-level, 4K/2M/1G)
```

Dependency direction is strictly downward: heap → VMM → PMM → bootinfo.
The arch MMU layer may call the PMM (to get page-table pages); the PMM
never calls the VMM or heap. Generic code reaches the MMU only through
`kernel/include/arch/mmu.h`.

## 3. Responsibilities

**Physical memory manager (PMM)**
- Build the page-frame array (`struct page`, one per 4 KiB frame of RAM)
  from the boot memory map, placing it with a one-shot bootstrap
  allocator (`bootmem`).
- Classify frames into zones (DMA < 16 MiB, DMA32 < 4 GiB, NORMAL) inside
  a single node; the node abstraction is where NUMA attaches later.
- Buddy allocation of power-of-two page blocks, orders 0 to 10 (4 KiB to
  4 MiB), with coalescing on free.
- Reference counts on pages; reserved-page bookkeeping for the kernel
  image, boot info, boot page tables, firmware, and legacy low memory.
- Defer frames the bootstrap page tables cannot reach (above 4 GiB) until
  the VMM has mapped them.

**Virtual memory manager (VMM)**
- The kernel address space object and its regions (image sections,
  direct map, kernel VA arena allocations, MMIO windows).
- Take over paging: build kernel-owned page tables mapping the image
  W^X, all RAM in the higher-half direct map (HHDM) with large pages, and
  activate them; free the loader's tables.
- Kernel virtual allocator (`vm_kernel_alloc`) with optional guard pages
  and eager or lazy population.
- MMIO mapping with cache attributes (`vm_map_phys`).
- Page-fault dispatch: demand-zero population for lazily populated
  regions; a precise panic report for everything else.

**Kernel heap**
- `kmem_cache` slab caches for fixed-size objects with double-free
  detection.
- `kmalloc`/`kzalloc`/`krealloc`/`kfree` on size-class caches, with
  page-allocator backing for large requests.

## 4. Non-responsibilities (deliberately later)

- User address spaces, anonymous private memory, copy-on-write, file
  backed mappings (Phase 4, needs processes and VFS). The region and
  fault machinery is shaped for them but only kernel regions exist.
- Swapping, compression, deduplication, huge-page promotion policy.
- NUMA placement policy (the node structure exists; there is one node).
- IOMMU and DMA address translation (Phase 6 DMA API sits on the PMM).
- TLB shootdown across CPUs (Phase 3 SMP; single-CPU invalidation now).
- Freeing empty intermediate page-table pages on unmap.
- Write-combining mappings (needs PAT reprogramming).

## 5. Interfaces (summary; contracts in api.md)

| Header | Provides |
|---|---|
| `kernel/types.h` | `paddr_t`, `vaddr_t`, `pfn_t`, page constants |
| `kernel/list.h` | intrusive doubly linked list |
| `kernel/spinlock.h` | `spinlock_t`, irq-saving lock/unlock |
| `kernel/page.h` | `struct page`, flags, phys/page/virt conversions |
| `kernel/pmm.h` | `pmm_init`, `pmm_alloc_pages`, `pmm_free_pages`, refcounts, stats |
| `kernel/vmm.h` | `vmm_init`, `vm_kernel_alloc/free`, `vm_map_phys/unmap_phys`, `vm_query`, fault entry |
| `kernel/kmalloc.h` | `kmalloc`, `kzalloc`, `krealloc`, `kfree`, `kmem_cache_*` |
| `arch/mmu.h` | page-table create/map/unmap/protect/query/activate/invalidate |
| `arch/trap.h` (extended) | fault address and fault flags from a trap frame |

## 6. Data structures (detail in design.md)

- `struct page` (32 bytes): flags, refcount, order, zone, and a union of
  the buddy list link or the owning slab. Free-list links live here, not
  in the free memory, so a frame can be managed before it is mapped.
- `struct pmm_zone`: pfn range, per-order free lists with counts, lock.
- `struct pmm_node`: the zones; the future NUMA unit.
- `struct vm_space`: MMU context, region list, lock, VA bounds.
- `struct vm_region`: base, size, protection, cache mode, kind (PHYS or
  ANON), flags (GUARD, POPULATED), name.
- `struct kmem_cache`, `struct slab`: cache parameters and per-slab
  header with a free bitmap.

## 7. Concurrency model

Every shared structure has one spinlock taken with interrupts saved and
disabled. Lock order, outermost first:

```text
vm_space.lock  →  kmem_cache.lock  →  pmm_zone.lock
```

Fault handlers run in exception context with interrupts disabled and take
`vm_space.lock` then `pmm_zone.lock`; they never take a slab lock (region
lookup does not allocate). A fault while `vm_space.lock` is held by the
same CPU is a kernel bug and the spinlock's owner check turns it into a
panic rather than a hang. All operations are non-blocking; nothing in
this subsystem sleeps, because there is nothing to sleep on yet.

## 8. Memory ownership

- The PMM owns all frames. A frame is either reserved (never handed out),
  free (in a buddy list), or allocated (refcount ≥ 1, owned by whoever
  holds the `struct page *`).
- A slab owns its frames until the slab is destroyed; objects handed out
  by `kmem_cache_alloc`/`kmalloc` are owned by the caller until freed.
- A VM region owns the frames it populated (ANON regions) and frees them
  on `vm_kernel_free`. PHYS regions own nothing physical.
- Page-table pages are owned by the MMU context that references them.
- Bootmem allocations (page array, zone tables) are permanent and are
  marked reserved.

## 9. Error handling

Allocation failure returns NULL/-ENOMEM; it is never silent and never a
panic in the allocator itself. Invariant violations (double free, freeing
a reserved page, wrong order, unmapping an unmapped range) are bugs and
panic with the offending address. Kernel page faults on non-lazy regions
panic with the full VMM diagnosis (region, protection, fault kind).

## 10. Performance considerations

Buddy alloc/free are O(orders). Slab alloc/free are O(1) with a bitmap
scan bounded by objects-per-slab. Direct map uses 2 MiB pages (1 GiB
where the CPU supports it) so TLB pressure for kernel memory access is
low. None of this is measured yet; coding rule 9 applies.

## 11. Security considerations

- W^X holds in the kernel tables: the image is mapped from the ELF flags,
  the direct map and every arena allocation are NX, MMIO is NX.
- Guard pages surround kernel stacks and any arena allocation that asks.
- Freed pages are not zeroed by default; `PMM_FLAGS_ZERO` and
  `kzalloc` exist for callers handing memory to less trusted consumers.
- The boot memory map is validated before use; unknown types are
  reserved, never freed.
- Legacy low memory (< 1 MiB) stays reserved.

## 12. Testing strategy

Boot-time self-tests for each layer, host-side unit tests with ASan and
UBSan for the buddy and slab algorithms, the existing crash test for the
fault-report path, and stats assertions that free-page counts return to
baseline after every test. See `testing.md`.

## 13. Future extensibility

- NUMA: multiple `pmm_node`s, zone lists per node, allocation policy.
- User spaces: `vm_space` per process, ANON private regions with CoW,
  file regions with a pager callback; the region kind enum and fault
  dispatch are already structured per kind.
- SMP: per-CPU page caches in front of zones; TLB shootdown in
  `arch_mmu_invalidate`.
- AArch64: `arch/mmu.h` maps to stage-1 translation tables with the same
  4K/2M/1G granules.
