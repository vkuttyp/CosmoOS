# Memory Subsystem: Design

## 1. Address types

`kernel/types.h` defines the address kinds the constitution requires be
kept apart:

| Type | Meaning |
|---|---|
| `paddr_t` | physical address (what the MMU emits, what devices see without an IOMMU) |
| `vaddr_t` | kernel virtual address |
| `pfn_t` | page frame number, `paddr >> 12` |

User virtual (`uaddr_t`) and DMA (`dma_addr_t`) arrive with their
subsystems. Conversions are explicit functions, never casts in callers.

## 2. Physical memory

### 2.1 Bootstrap allocator (`bootmem.c`)

Runs once, inside `pmm_init`, before the buddy exists.

- Builds a table of free ranges from memory-map entries of type USABLE and
  LOADER_RECLAIMABLE, clipped to `[1 MiB, hhdm_size)`. Below 1 MiB is
  reserved for legacy uses (the future SMP trampoline). Above the
  bootstrap direct map is unreachable until the VMM runs.
- `bootmem_alloc(size, align)` takes from the top of the highest range that
  fits, so low memory (ZONE_DMA) stays untouched. Allocations are recorded
  so they can be marked reserved in the page array.
- After the page array exists, every remaining bootmem range is released
  into the buddy and bootmem is sealed; later calls panic.

### 2.2 Page frame array (`page.h`, `pmm.c`)

One `struct page` per frame in `[0, phys_limit)` where `phys_limit` is the
end of the highest RAM entry (`bootinfo_phys_limit`). Holes get reserved
pages so `phys_to_page` is total on that range.

```c
struct page {
    uint32_t flags;        /* PG_* */
    uint32_t refcount;     /* 0 when free or reserved */
    uint8_t  order;        /* buddy order (free) or allocation order (allocated) */
    uint8_t  zone;         /* PMM_ZONE_* */
    uint16_t reserved0;
    uint32_t reserved1;
    union {
        struct list_node buddy;   /* PG_BUDDY: link in zone free list */
        struct slab *slab;        /* PG_SLAB: owning slab */
    };
};                                /* 32 bytes */
```

Flags: `PG_RESERVED` (never allocatable), `PG_BUDDY` (head of a free
block), `PG_SLAB`, `PG_KMALLOC_LARGE` (page-backed kmalloc; order in
`order`), `PG_PAGETABLE`, `PG_DEFERRED` (RAM the bootstrap map cannot
reach; released by the VMM).

Keeping the free-list link inside `struct page` rather than inside the
free frame means the PMM never touches a frame's contents to manage it.
That is what makes deferred (unmapped) frames representable.

### 2.3 Zones and node

```c
enum pmm_zone_id { PMM_ZONE_DMA, PMM_ZONE_DMA32, PMM_ZONE_NORMAL, PMM_ZONE_COUNT };

struct pmm_free_area { struct list_node list; uint64_t nr_free; };

struct pmm_zone {
    const char *name;
    pfn_t start_pfn, end_pfn;          /* [start, end) */
    struct pmm_free_area free_area[PMM_MAX_ORDER];
    uint64_t nr_pages_total, nr_pages_free;
    spinlock_t lock;
};

struct pmm_node { struct pmm_zone zones[PMM_ZONE_COUNT]; /* NUMA unit */ };
```

Zone boundaries: DMA `[0, 16 MiB)`, DMA32 `[16 MiB, 4 GiB)`, NORMAL the
rest. Allocation flags select the highest acceptable zone; the allocator
falls back downward (NORMAL → DMA32 → DMA). A block never spans zones
because coalescing checks the buddy's zone.

### 2.4 Buddy allocator (`buddy.c`)

`PMM_MAX_ORDER = 11` (orders 0..10, 4 KiB..4 MiB). Pure functions over a
zone, page array, and lock; no I/O, no logging, so they compile on the
host for unit tests.

Free (`buddy_free`):
```text
while order < MAX-1:
    buddy_pfn = pfn ^ (1 << order)
    if buddy outside zone or !PG_BUDDY or buddy.order != order: break
    remove buddy from free_area[order]; clear PG_BUDDY
    pfn = min(pfn, buddy_pfn); order++
page(pfn).order = order; set PG_BUDDY; insert into free_area[order]
```

Alloc (`buddy_alloc`):
```text
for o in order..MAX-1:
    if free_area[o] empty: continue
    page = pop; clear PG_BUDDY
    while o > order:
        o--; half = page + (1 << o); half.order = o; set PG_BUDDY; insert free_area[o]
    page.order = order; refcount = 1; return page
return NULL
```

Both run under `zone.lock` with interrupts disabled. Block addresses are
naturally aligned to their order by construction (`pfn ^ (1 << order)`).

### 2.5 Public PMM behaviour (`pmm.c`)

- `pmm_init()`:
  1. Validate the map; compute `phys_limit`, `max_pfn`.
  2. Build bootmem ranges.
  3. `bootmem_alloc` the page array (`max_pfn * 32` bytes); zero it; mark
     every page reserved with refcount 0.
  4. Initialise zones over their pfn ranges.
  5. For each RAM entry above `hhdm_size`: flag pages `PG_DEFERRED`.
  6. Release every remaining bootmem range into the buddy in maximal
     aligned power-of-two blocks (`pmm_free_range_core`).
  7. Log totals; seal bootmem.
- `pmm_alloc_pages(order, flags)`: pick starting zone from flags, try each
  lower zone, optionally zero through the direct map. Returns
  `struct page *` or NULL. `pmm_free_pages(page, order)` asserts the
  page is allocated, refcount 1, `page->order == order`, then frees.
- `pmm_page_get/put`: atomic refcount; put to zero frees with the stored
  order.
- `pmm_release_deferred()`: called by `vmm_init` after the full direct map
  exists; releases `PG_DEFERRED` runs into the buddy.
- `pmm_free_reserved_range(paddr, size)`: for the boot page tables after
  takeover; pages must be `PG_RESERVED` and inside a
  `COSMOBOOT_MEM_BOOT_PAGETABLES` entry.

## 3. Virtual memory

### 3.1 Arch MMU interface (`arch/mmu.h`)

```c
struct arch_mmu_context { paddr_t root; };   /* CR3 value on x86-64 */

int  arch_mmu_context_init(struct arch_mmu_context *ctx);       /* fresh root */
int  arch_mmu_map(ctx, vaddr_t va, paddr_t pa, size_t len, vm_prot_t prot, vm_cache_t cache, unsigned flags);
int  arch_mmu_unmap(ctx, vaddr_t va, size_t len);
int  arch_mmu_protect(ctx, vaddr_t va, size_t len, vm_prot_t prot);
bool arch_mmu_query(ctx, vaddr_t va, paddr_t *pa, vm_prot_t *prot, vm_cache_t *cache, size_t *page_size);
void arch_mmu_activate(ctx);
void arch_mmu_invalidate(ctx, vaddr_t va, size_t len);
size_t arch_mmu_large_page_sizes(void);   /* bitmask of supported sizes */
```

x86-64 implementation (`kernel/arch/x86_64/mmu.c`):
- Table pages come from `pmm_alloc_pages(0, PMM_FLAGS_ZERO)` and are
  flagged `PG_PAGETABLE`; accessed through the direct map.
- `map` walks per page, choosing 1 GiB (if `pdpe1gb` and
  `ARCH_MMU_MAP_LARGE` and both addresses and remaining length are
  1 GiB aligned), else 2 MiB by the same rule, else 4 KiB. Mapping over an
  existing present entry is a bug (`-EEXIST` to the caller after a
  `WARN`); callers unmap first.
- Protection: P from any prot; RW from `VM_PROT_WRITE`; NX unless
  `VM_PROT_EXEC`; G for kernel spaces; US never (no user yet).
- Cache: WB = 0, WT = PWT, UC = PCD|PWT (PAT left at reset defaults).
- `unmap` clears leaf entries and invalidates; a range that crosses a
  large page it does not fully cover is refused (`-EINVAL`). Empty
  intermediate tables are not freed yet.
- `invalidate`: `invlpg` per 4 KiB up to 64 pages, otherwise reload CR3.

### 3.2 Spaces and regions (`vmm.c`)

```c
enum vm_region_kind { VM_REGION_PHYS, VM_REGION_ANON };

struct vm_region {
    struct list_node link;         /* sorted by base in the space */
    vaddr_t base; size_t size;     /* page multiples; size excludes guards */
    vm_prot_t prot; vm_cache_t cache;
    enum vm_region_kind kind;
    unsigned flags;                /* VM_REGION_GUARD_BELOW/ABOVE, VM_REGION_POPULATED */
    paddr_t phys;                  /* PHYS: mapped physical base */
    const char *name;
};

struct vm_space {
    struct arch_mmu_context mmu;
    struct list_node regions;
    spinlock_t lock;
    vaddr_t arena_lo, arena_hi;    /* kernel VA arena for vm_kernel_alloc */
    vaddr_t near_lo, near_hi;      /* near arena: top 2 GiB above the image (modules) */
    bool user;                     /* a process address space (Phase 4) */
    uint64_t anon_pages;
};
```

Kernel layout (x86-64, 48-bit):

| Range | Use |
|---|---|
| `0xFFFF800000000000` + 64 TiB | HHDM: direct map of RAM, RW NX WB, large pages |
| `0xFFFFC00000000000` + 32 TiB | kernel VA arena (vm_kernel_alloc, MMIO windows) |
| `0xFFFFFFFF80000000` + 128 MiB | kernel image (link address); `vmm_init` panics if `__kernel_end` passes the near arena's low bound |
| `arch_mmu_near_arena()` | near arena (`VM_KALLOC_NEAR_KERNEL`): kernel modules. x86-64: `0xFFFFFFFF88000000` – `0xFFFFFFFFFF000000`, because modules are built with `-mcmodel=kernel` and must sit in the top 2 GiB so `R_X86_64_32S`/`PC32` relocations reach both themselves and the image (Phase 5). AArch64: from the 2 MiB-aligned end of the image to image base + 120 MiB so `R_AARCH64_CALL26` (±128 MiB) reaches every export (Phase 13). `vmm_init` copies the bounds into `kernel_space.near_lo/near_hi` |

`vmm_init()` sequence:
1. `arch_mmu_context_init(&kernel_space.mmu)`.
2. Map the image: `[__text_start, __text_end)` RX, `[__rodata_start, __rodata_end)` R, `[__data_start, __bss_end)` RW, 4 KiB pages, from `kernel_phys_base + offset`. Record three PHYS regions.
3. For each RAM entry in the memory map, map `hhdm_base + base` → `base` RW NX WB with large pages allowed. Record one PHYS region covering `[hhdm_base, hhdm_base + phys_limit)`.
4. `arch_mmu_activate`. From here the loader's tables are dead.
5. `pmm_free_reserved_range` on every `COSMOBOOT_MEM_BOOT_PAGETABLES` entry.
6. `pmm_release_deferred()`.
7. Register the page-fault handler on `arch_trap_vector(ARCH_TRAP_PAGE_FAULT)`.

Region operations hold `space.lock`. Insertion keeps the list sorted and
rejects overlap. Free-range search is first-fit over the gaps between
regions inside a range (`range_find_free`), counting guard pages as part
of the footprint; the range is `[arena_lo, arena_hi)` by default and
`[near_lo, near_hi)` for `VM_KALLOC_NEAR_KERNEL`. Both arenas share one
region list and one PML4 entry set: the near arena lives under the
image's PML4 slot, whose PDPT exists from `vmm_init`, so invariant P9
(no new kernel-half PML4 entries after init) holds for it without
pre-allocation.

### 3.3 Kernel allocations

`vm_kernel_alloc(size, flags, prot)`:
- `VM_KALLOC_GUARD`: one unmapped page below and above.
- `VM_KALLOC_POPULATE`: allocate and map zeroed frames now; otherwise the
  region is `VM_REGION_ANON` and populates on first touch.
- `VM_KALLOC_NEAR_KERNEL`: search the near arena instead of the main one.
- Returns the base virtual address (after the lower guard) or 0.

`vm_kernel_protect(base, prot)`: changes the protection of one whole
populated `vm_kernel_alloc` region (the module loader's RW → RX and
RW → R flip after relocation). Under `kernel_space.lock` it finds the
region by base, refuses anything that is not a `VM_REGION_POPULATED`
ANON region, refuses `VM_PROT_NONE`, W+X, and `VM_PROT_USER`, rewrites
the leaf entries with `arch_mmu_protect`, and records the new `prot`;
then, outside the lock, it runs `arch_mmu_shootdown` over the range so
no CPU keeps a writable translation of pages that are now executable.
Unpopulated regions are refused because their unmapped pages have no
leaf entries to rewrite; the fault path would use the recorded `prot`,
but a half-mapped region with two protections is not a state worth
supporting.

`vm_kernel_free(base)`: finds the region by base, unmaps, frees every
frame the region populated (found by `arch_mmu_query` per page), removes
the region, frees the region struct.

`vm_map_phys(paddr, size, prot, cache)`: PHYS region in the arena; used
for MMIO. `vm_unmap_phys(vaddr)` reverses it.

### 3.4 Page faults

`arch/trap.h` gains `arch_trap_fault_address(frame)` and
`arch_trap_fault_flags(frame)` returning `VM_FAULT_WRITE`,
`VM_FAULT_EXEC`, `VM_FAULT_USER`, `VM_FAULT_PRESENT`, `VM_FAULT_RESERVED`.

`vm_fault_handler(vector, frame, arg)`:
1. Decode address and flags.
2. If the address is in the kernel half, take `kernel_space.lock` and find
   the region.
3. Region is ANON, fault is not-present, and access is within `prot`:
   allocate a zeroed frame, map it, invalidate, return.
4. Anything else: release the lock and `panic_frame` with a report naming
   the region (or "no region"), the protection, and the fault kind. User
   faults are impossible until Phase 4 and are treated the same way.

The demand-zero path is the one that will later grow anonymous private
memory and CoW; its structure (lookup → kind switch → populate) is the
skeleton for that.

## 4. Kernel heap

### 4.1 Slab caches (`slab.c`)

```c
struct kmem_cache {
    const char *name;
    size_t object_size;      /* caller's size */
    size_t align;            /* ≥ 16 */
    size_t slot_size;        /* object_size rounded to align */
    unsigned slab_order;     /* pages per slab = 1 << order */
    unsigned objects_per_slab;
    size_t header_size;      /* struct slab + bitmap, rounded to align */
    struct list_node partial, full, empty;
    unsigned nr_slabs, nr_empty;
    uint64_t nr_allocated;   /* live objects */
    spinlock_t lock;
    struct list_node link;   /* global cache list for diagnostics */
};

struct slab {
    struct list_node link;
    struct kmem_cache *cache;
    void *objects;           /* first slot */
    unsigned free_count;
    unsigned next_hint;      /* bitmap scan start */
    uint64_t bitmap[];       /* 1 = free */
};
```

The header sits at the start of the slab's frames (on-slab). Every frame of
the slab has `PG_SLAB` and `page->slab` pointing at the header so
`kfree(ptr)` resolves ptr → page → slab → cache without knowing the size.

Slab order: the smallest order such that at least 8 objects fit, capped at
order 3 (32 KiB); if even one object does not fit in order 3 the cache
creation fails (larger objects go through the page path in kmalloc).

Allocation: lock; take from `partial`, else `empty`, else grow (allocate
frames from the PMM, build header, bitmap all-free); find the first set
bitmap bit from `next_hint`; clear it; move slab to `full` if now
exhausted. Free: locate slab; assert the bit is clear (double free →
panic); set it; move `full → partial`, `partial → empty` when fully free.
Empty slabs beyond `SLAB_KEEP_EMPTY` (2) are returned to the PMM.

### 4.2 kmalloc (`kmalloc.c`)

Size classes: 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
2048, 4096, 8192 — one `kmem_cache` each, created by `kmalloc_init`.
Requests above 8192 bytes get `pmm_alloc_pages(order)` with
`PG_KMALLOC_LARGE` and the order stored in `page->order`; the maximum is
`4 MiB`. `kfree` inspects the page flags to choose the path.
`krealloc` allocates, copies `min(old, new)`, frees; the old size comes
from the cache's `object_size` or the page order. All kmalloc results are
16-byte aligned.

## 5. Failure modes

| Condition | Behaviour |
|---|---|
| no RAM range fits the page array | panic in `pmm_init` |
| buddy exhaustion | `pmm_alloc_pages` returns NULL; callers propagate `-ENOMEM` |
| free of reserved/free/wrong-order page | panic (bug) |
| refcount underflow | panic (bug) |
| arena exhausted | `vm_kernel_alloc` returns 0 |
| map over existing mapping | `-EEXIST` |
| unmap crossing a partial large page | `-EINVAL` |
| kernel fault outside any region | panic with VMM report |
| kernel fault in PHYS region (permission) | panic with region name and prot |
| slab double free | panic with object address |
| kfree of non-heap pointer | panic |
| kmalloc size > 4 MiB | NULL |

## 6. User memory: fixups, regions, shootdown (audit milestone 5)

Milestone 5 of the post-roadmap plan (`docs/audit/2026-09-post-roadmap-audit.md`
§19; findings #11, #18, #19 and the region part of #30). Before it, four
assumptions were load-bearing and unwritten: a process has one thread
(so check-then-copy user access cannot race an unmap), a user region is
only ever unmapped or re-protected whole, `PROT_NONE` means readable, and
the kernel-half PML4 never grows. This section makes each one either
true by construction or unnecessary.

### 6.1 Exception fixups

`copy_from_user`, `copy_to_user` and `strncpy_from_user` no longer walk
the region list before touching user memory. They check the address
range (`user_range_ok`: inside `[USER_LO, USER_HI)`, no overflow) and
then copy through an architecture primitive whose faulting instructions
are listed in an **exception table**:

```c
struct ex_entry { int32_t insn; int32_t fixup; };   /* offsets from the entry itself */
```

The entries live in `.ex_table` (`KEEP`, between `.rodata` and
`.ksymtab`, bounded by `__ex_table_start/end`). The copy primitive is
`arch_copy_user_raw(dst, src, n)`: it returns the number of bytes **not**
copied, 0 on success. On x86-64 it is `rep movsb` with one table entry:
the fixup returns the remaining count from `rcx`. On AArch64 it is an
8-byte loop and a byte tail with one entry per load and store; the fixup
computes the remainder from the destination cursor. Both run inside the
`arch_user_access_begin/end` window (STAC/CLAC, PAN).

`vm_fault_handler` gains one rule. A **kernel-mode** fault at a user
address is first offered the demand-zero path as before (a lazily
mapped ANON region the copy is the first to touch). If that path does
not apply (no process, no region, a protection violation, `PROT_NONE`)
**or it fails for lack of memory**, the handler looks the faulting PC up
in the exception table; a hit rewrites the frame's PC to the fixup and
returns, and the copy reports `-EFAULT`. A miss is a kernel bug and
panics as before. So a sibling thread's `munmap` between the check and
the copy, a `PROT_NONE` page, a read-only destination, or an exhausted
allocator all become `-EFAULT` from the system call, never a panic and
never a process kill (finding #11, #19).

Two hardening rules ride along. A **user-mode** fault at a kernel
address goes straight to the fatal hook: it used to select
`kernel_space` and could populate a lazily mapped kernel region on an
unprivileged process's behalf (audit 5.2). And `strncpy_from_user`
copies page-bounded pieces with the raw primitive and scans for the NUL,
so a string that ends before an unmapped page is accepted and one that
runs into it is `-EFAULT`, as before, without the region walk.

What the fixup does not cover: direct dereferences of user pointers
outside `uaccess.c` (there are none, invariant P14), and faults at kernel
addresses (no entry ever names one; a kernel-address fault with a
fixup-table PC still panics).

### 6.2 `PROT_NONE`

A region may now have `prot == VM_PROT_NONE`, at creation (`mmap` with
`PROT_NONE`: a reservation) or by `mprotect`. A `PROT_NONE` page keeps
its frame: the leaf entry is rewritten with the hardware valid bit clear
and a software bit set (x86-64 `PTE_SW_NONE`, bit 9; AArch64
`DESC_SW_NONE`, bit 55) while the frame address stays. The table walker
treats such an entry as a present leaf, so `arch_mmu_query` reports the
frame with `VM_PROT_NONE` (plus `VM_PROT_USER`), `arch_mmu_unmap` frees
it like any leaf, `arch_mmu_map` over it is `-EEXIST`, and
`arch_mmu_protect` from `NONE` to anything restores the valid bit with
the new permissions. The hardware sees an invalid entry: every access
faults as not-present; the fault handler finds the region, sees that no
access is allowed by `prot == NONE`, and the process dies (user mode) or
the copy gets `-EFAULT` (kernel mode). Guard pages and `mprotect(NONE)`
trap, as they must.

### 6.3 Regions split and merge

`vm_user_unmap` and `vm_user_protect` take any page-aligned range inside
the user window. Under `space->lock`:

- a region that lies wholly inside the range is unlinked (unmap) or
  re-protected (protect);
- a region that straddles a range end is **split** at that end: the head
  keeps `GUARD_BELOW`, the tail keeps `GUARD_ABOVE`, both keep kind,
  cache, name and `POPULATED`. At most two splits happen per call (the
  first region's head and the last region's tail), so the two spare
  region structs are allocated before the lock is taken and freed if
  unused;
- after protect (and after every `vm_user_map_anon`), a region is
  **merged** with a neighbour that is adjacent, has the same kind, prot,
  cache, flags and name pointer, and has no guard page between them. The
  brk heap therefore stays one region as it grows.

The frames of an unmapped range are released after the lock is dropped,
by address range (`user_range_teardown(space, va, len)`: query the frame
of each page, unmap the chunk, shoot down, free), with the regions
already unlinked or shrunk. A fault in the range during that window
finds no region and is fatal or `-EFAULT`; it cannot repopulate a page
that is about to be freed (audit 5.2 MEDIUM, closed).

Semantics by caller:

| Caller | Range rule |
|---|---|
| native `munmap` | strict: every page of the range must be mapped, else `-EINVAL` and nothing changes |
| Linux `munmap`, `brk` shrink, Linux `MAP_FIXED` | lenient: unmapped pages in the range are skipped; 0 |
| `mprotect` | every page must be mapped, else `-ENOMEM` (Linux) and nothing changes |
| native `MAP_FIXED` | still `-EEXIST` over an existing mapping (the native ABI does not replace) |
| Linux `MAP_FIXED` | replaces: lenient unmap of the range, then map |

`brk` shrink checks the unmap result (it cannot fail on a well-formed
heap; a failure leaves the break unchanged) and growth merges into the
existing heap region, so a shrink followed by a growth no longer fails
with `-EEXIST` for the life of the process (finding #30, region part).

### 6.4 Shootdown by the CPUs that run the space

`struct vm_space` gains `active_cpus`, the set of CPUs whose translation
root is this space right now. `arch_thread_switch_prepare` maintains it
through `vm_space_switch(prev, next)`: set the CPU's bit in `next`
(an atomic OR, a full barrier), write CR3 / TTBR0, then clear the bit
in `prev`. Without PCID or ASIDs a CPU that leaves a space holds none of
its translations, so the bit can be cleared at once.

`arch_mmu_shootdown_cpus(ctx, va, len, cpus)` invalidates on exactly the
CPUs in `cpus`; `arch_mmu_shootdown` remains the all-online form for
the kernel space. The VMM calls the mask form for user spaces after every
PTE change that can leave a stale translation (unmap, protect), reading
`active_cpus` after a full fence that orders the PTE write before the
mask read. A CPU switching into the space either has its bit visible to
the initiator (and is sent the IPI) or writes CR3 after the PTE change
(and loads the fresh table). On x86-64 the IPIs are `ipi_send` per target
and the acknowledgement count is the target count; on AArch64 the
hardware broadcast covers every CPU regardless, and the statistics count
the same targets so the two architectures report alike.

The consequence for process exit: `vm_space_destroy` runs on the reaper
with no CPU running the space, so its shootdowns are local invalidates
and the exit of a process no longer interrupts every other CPU once per
32-page chunk (audit 6.2).

### 6.5 Kernel-half tables are fixed before the first user space

`vmm_init` calls `arch_mmu_prepopulate(&kernel_space.mmu, KERNEL_ARENA_LO,
KERNEL_ARENA_HI - KERNEL_ARENA_LO)`, which creates the 64 PDPTs of the
arena's PML4 slots (256 KiB). The direct map and the image slots exist
from the mapping itself. From then on every user root copies a complete
kernel half. In debug builds `descend` panics if it would create a
kernel-half PML4 entry after the first user context exists
(`arch_mmu_context_init_user` sets the flag): invariant P9 is checked,
not asserted by comment (finding #18). AArch64 is unaffected: the kernel
half lives in TTBR1 and is shared by construction.

### 6.6 What stays as it was

Populated mappings (ELF segments) still allocate under `space->lock`
with interrupts off; a region tree, per-CPU frame caches and ASIDs are
scalability work (audit 5.4) outside this milestone. The native ABI does
not gain `mprotect` or `brk`; both are Linux-personality calls, and the
native `munmap` keeps its strict contract.
