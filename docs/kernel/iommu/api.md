# IOMMU: API

Every interface the DMA layer, the PCI layer, a unit driver or a test
uses from the IOMMU layer. Each entry follows constitution section 52.
**ABI stability: kernel-internal and Module ABI v1.** The two functions
marked *exported* are visible to modules; changing one incompatibly
bumps `COSMO_MODULE_ABI_VERSION`.

Common properties unless stated: thread context, may allocate, and
negative errnos from `kernel/errno.h`. The mapping functions
(`iommu_map`, `iommu_unmap`, `iommu_lookup`, `iommu_dma_map`,
`iommu_dma_unmap`) run in **any** context: they take the domain's
IRQ-safe spinlock, because `dma_map` is called from completion handlers.

Drivers do not call this layer. They call the DMA API
(`docs/kernel/device/api.md`), which is unchanged; the bus address it
returns is an I/O virtual address when the device has a domain.

## Bring-up (`kernel/include/kernel/iommu.h`, `kernel/iommu/iommu.c`)

### `void iommu_init(void)`
Purpose: probe the architecture's unit driver and, when it finds
hardware, leave translation enabled. Called once from `kernel_main`
after `pci_init()` (requester ids exist) and before `module_load_boot()`
(the first `pci_enable_device`). Without hardware it logs
`iommu: none (devices use physical addresses)` and every device keeps
the identity path.
Never fails.

### `void iommu_register_unit(struct iommu_unit *u)`
Purpose: a unit driver's registration, from its probe. Inputs: an
immortal `u` with `ops`, `name` and `priv` set. The layer takes the
unit's list link and counts it. Never fails.

### `bool iommu_present(void)` *(exported)*
Purpose: is any unit registered — the predicate a test or a diagnostic
uses to decide whether translation is on. Any context, no lock.

### `struct iommu_unit *iommu_unit_first(void)`
Purpose: the first registered unit (tests and diagnostics; the machines
supported today have one). NULL when there is none.

## Devices

### `int iommu_attach_device(struct device *dev, uint32_t sid)`
Purpose: give `dev` an address space of its own. Inputs: a registered
device and its requester id (`bus << 8 | slot << 3 | func` for PCI).
Outputs: `dev->iommu` set to a fresh domain and `dev->iommu_sid` to
`sid`, or `dev->iommu` left NULL when no unit covers the device.
Returns 0 in **both** of those cases. `-ENOMEM`, `-ERANGE` (a stream id
the unit cannot table) or another driver error mean the unit published
nothing and the domain was destroyed: nothing changed. `-EIO` is
different — the unit's entry for the requester **is** live and names
this domain's tables, but the invalidation of the cached one was not
confirmed: `dev->iommu` is set and the domain is kept for good, because
freeing tables the hardware still points at is the failure this layer
exists to prevent. The device's DMA may fault until the unit catches
up. Called by `pci_enable_device` before it sets the bus-master bit.
Sleeps (allocation).

### `void iommu_detach_device(struct device *dev)`
Purpose: end the device's ability to DMA. Clears the unit's entry for
the requester id, invalidates it, warns about pages still mapped (they
cannot be freed on the driver's behalf) and destroys the domain.
Called by `device_unregister` after the driver's `remove`. No-op for a
device without a domain.

## Domains and mappings

### `struct iommu_domain *iommu_domain_create(struct iommu_unit *u)`
Purpose: an address space on `u`: a domain id, a root page table, an
IOVA allocator over `[IOMMU_IOVA_LO, IOMMU_IOVA_HI)`, and the unit's
reserved ranges applied (out of the allocator, identity-mapped where
the unit says so). NULL on failure. Sleeps.

### `void iommu_domain_destroy(struct iommu_domain *d)`
Purpose: free the tables, the id and the allocator. Asserts (debug
builds) that no device is still attached; warns if pages beyond the
reserved ranges are still mapped. NULL is a no-op.

### `int iommu_map(struct iommu_domain *d, uint64_t iova, paddr_t pa, size_t len, unsigned prot)`
Purpose: map `len` bytes at `iova` to `pa` with `IOMMU_PROT_READ`
and/or `IOMMU_PROT_WRITE`. Inputs: `iova`, `pa` and `len` page-aligned
and `len` nonzero, else `-EINVAL`. Failure: `-EEXIST` when any page of
the range is already mapped, `-ENOMEM` without memory for a table —
in both cases nothing of the call remains mapped. Counts the domain's
`maps` and `pages_mapped`; the global `iommu_stats.maps` counts the DMA
API's mappings (`iommu_dma_map`), which is what a driver produces. Any
context.

### `int iommu_unmap(struct iommu_domain *d, uint64_t iova, size_t len)`
Purpose: clear the range and invalidate the unit's IOTLB for it.
Returns 0; `-EINVAL` for an unaligned or empty range; **`-EIO` when the
unit did not confirm the invalidation**, which means the entries are
gone from the tables but the hardware may still translate the range —
the caller must treat the addresses and the memory behind them as still
reachable by the device. Pages that were not mapped are not an error,
and `pages_mapped` never goes below zero. Any context.

### `bool iommu_lookup(struct iommu_domain *d, uint64_t iova, paddr_t *pa)`
Purpose: the translation the hardware would make, for tests and
diagnostics. Outputs: `*pa` with the page offset of `iova` applied.
False when the page is not mapped. Any context.

### `uint64_t iommu_dma_map(struct iommu_domain *d, paddr_t pa, size_t len, unsigned prot)`
Purpose: what `dma_map`/`dma_alloc` need in one call: allocate IOVA for
the pages `[pa, pa + len)` touches and map them. Outputs: the bus
address (the allocated IOVA plus `pa`'s page offset), or 0 when the
window is full (counted in `iommu_stats.iova_failures`) or a table
could not be allocated. Any context.

### `int iommu_dma_unmap(struct iommu_domain *d, uint64_t dma, size_t len)`
Purpose: the inverse; takes the address `iommu_dma_map` returned and
the same length. Unmaps the pages and returns the IOVA to the
allocator; returns 0. On `-EIO` (the unit did not confirm the
invalidation) the addresses are **not** returned to the allocator —
they are retired for the life of the domain and counted in
`iommu_stats.retired` — and the caller must not reuse the memory
either: `dma_free` leaks the frames rather than handing a live
translation to the next allocation. Any context.

## Faults and statistics

### `void iommu_note_fault(struct iommu_unit *u, uint32_t sid, uint64_t addr, unsigned reason, bool write)`
Purpose: a unit driver reports a translation fault it decoded. Counts
it (`u->faults`, `iommu_stats.faults`) and logs requester, address and
reason at `WARN`, at most eight times per boot (a faulting device can
produce thousands). Interrupt context; takes no lock the caller holds.

### `void iommu_get_stats(struct iommu_stats *out)` *(exported)*
Purpose: `units`, `domains`, `maps`, `unmaps`, `faults`,
`iova_failures`, and `retired` (pages never handed out again because an
invalidation went unconfirmed) since boot. Any context.

## The IOVA allocator (`struct iova_space`)

A bitmap of page-granular ranges, used by the domain and directly by
tests. All four take no lock of their own: the caller's lock (the
domain's) serialises them.

- `int iova_init(struct iova_space *s, uint64_t lo, uint64_t hi)` —
  page-aligned `lo < hi`; allocates the bitmap. `-EINVAL`, `-ENOMEM`.
- `void iova_fini(struct iova_space *s)` — frees the bitmap.
- `uint64_t iova_alloc(struct iova_space *s, size_t pages)` — a run of
  `pages` free pages, first fit from the bottom, or 0.
- `void iova_free(struct iova_space *s, uint64_t iova, size_t pages)` —
  returns a run. Asserts the pages were allocated (debug builds).
- `void iova_reserve(struct iova_space *s, uint64_t base, size_t pages)`
  — takes the part of a range inside the window out of the allocator
  for good, counting it in `s->reserved`.

## What a unit driver implements (`struct iommu_ops`)

| Member | Context | Contract |
|---|---|---|
| `name` | — | the driver's name, e.g. `intel-vtd` |
| `covers(u, sid)` | thread | does this unit translate for that requester id |
| `reserved(u, out, max)` | thread | the ranges every domain reserves (`base`, `len`, `identity`); may be NULL |
| `domain_init(u, d)` | thread | assign `d->id` and `d->root`; `-ENOSPC` when ids are exhausted |
| `domain_fini(u, d)` | thread | free the tree and the id |
| `attach(u, d, sid)` | thread | point the unit's entry for `sid` at `d` and invalidate; `-EIO` **only** after the entry is published (the core then keeps the domain), any other error means nothing points at `d` |
| `detach(u, d, sid)` | thread | clear it and invalidate; `-EIO` when the invalidation was not confirmed, and the domain is then kept for good |
| `map(d, iova, pa, pages, prot)` | any, `d->lock` held | write leaves; `-EEXIST`/`-ENOMEM` leave nothing behind |
| `unmap(d, iova, pages)` | any, `d->lock` held | clear leaves **and** invalidate the IOTLB; `-EIO` when the unit did not confirm, and nothing it covered may be reused |
| `lookup(d, iova, pa)` | any, `d->lock` held | the page's translation |

## The page-table walker (`kernel/include/kernel/iommu_pt.h`)

Shared by both drivers: a 4-level, 4 KiB-granule tree of 512-entry
tables, the entry encoding supplied as a `struct iommu_pt_fmt`
(`make_table`, `make_leaf`, `present`, `addr_of`). Level `l` indexes
with `(iova >> (12 + 9 * (3 - l))) & 511`.

- `paddr_t iommu_pt_alloc_table(void)` — a zeroed page, 0 without
  memory.
- `int iommu_pt_map(paddr_t root, const struct iommu_pt_fmt *fmt, uint64_t iova, paddr_t pa, size_t pages, unsigned prot)`
  — `-EEXIST` or `-ENOMEM` undo the leaves this call wrote. Intermediate
  tables are allocated on demand and never freed before
  `iommu_pt_free`. Any context.
- `size_t iommu_pt_unmap(...)` — clears the leaves, returns how many
  were present. It does **not** invalidate: the driver's `unmap` does.
- `bool iommu_pt_lookup(...)`, `void iommu_pt_free(paddr_t root, const struct iommu_pt_fmt *fmt)`.

## Block layer addition (`kernel/include/kernel/blk.h`)

### `int (*debug_dma)(struct blkdev *bd, uint64_t addr)`
An optional `blkdev_ops` member, **tests only**: make the device DMA
into a bus address the caller chose (one no mapping covers) with a
harmless command, and return the errno of its status — which may be 0,
since a device is free not to notice that its DMA was dropped. NVMe
implements it as Identify Controller into `addr`. Thread context.
