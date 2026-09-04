/*
 * arch/mmu.h - Page-table interface every architecture implements.
 *
 * A context is one translation tree (an address space). The generic VMM
 * owns policy (which regions exist, what they mean); this layer owns the
 * table format, the TLB, and nothing else. Table pages come from the PMM
 * and are flagged PG_PAGETABLE.
 *
 * All functions are non-blocking. map/unmap/protect may allocate from the
 * PMM (zone spinlock). Callers serialize concurrent modification of one
 * context (the VMM holds vm_space.lock); the implementation does not.
 */

#ifndef ARCH_MMU_H
#define ARCH_MMU_H

#include <kernel/types.h>

typedef unsigned vm_prot_t;
#define VM_PROT_NONE  0u
#define VM_PROT_READ  (1u << 0)
#define VM_PROT_WRITE (1u << 1)
#define VM_PROT_EXEC  (1u << 2)
#define VM_PROT_RW    (VM_PROT_READ | VM_PROT_WRITE)
#define VM_PROT_RX    (VM_PROT_READ | VM_PROT_EXEC)

typedef enum vm_cache {
    VM_CACHE_WB,   /* write-back: normal memory */
    VM_CACHE_WT,   /* write-through */
    VM_CACHE_UC,   /* uncached: device MMIO */
} vm_cache_t;

/* arch_mmu_map flags */
#define ARCH_MMU_MAP_LARGE  (1u << 0)  /* may use 2 MiB / 1 GiB entries where aligned */
#define ARCH_MMU_MAP_GLOBAL (1u << 1)  /* kernel mapping shared by all spaces */

struct arch_mmu_context {
    paddr_t root;  /* physical address of the top-level table */
};

/* Allocate an empty top-level table. Returns 0 or -ENOMEM. */
int arch_mmu_context_init(struct arch_mmu_context *ctx);

/* Map [va, va+len) -> [pa, pa+len). Both page aligned. Fails with -EEXIST
 * if any page in the range is already mapped (nothing is changed in that
 * case only if the conflict is on the first page; callers should treat
 * the range as partially mapped and unmap it), -ENOMEM if a table page
 * cannot be allocated, -EINVAL on bad alignment. */
int arch_mmu_map(struct arch_mmu_context *ctx, vaddr_t va, paddr_t pa, size_t len,
                 vm_prot_t prot, vm_cache_t cache, unsigned flags);

/* Unmap [va, va+len). Pages that are not mapped are skipped. Refuses with
 * -EINVAL a range that would split a large page. Invalidates the TLB. */
int arch_mmu_unmap(struct arch_mmu_context *ctx, vaddr_t va, size_t len);

/* Change protection of mapped pages in [va, va+len); unmapped pages are
 * skipped. -EINVAL if the range splits a large page. */
int arch_mmu_protect(struct arch_mmu_context *ctx, vaddr_t va, size_t len, vm_prot_t prot);

/* Look up one virtual address. Returns false if not mapped. Output
 * pointers may be NULL. page_size is 4K/2M/1G of the leaf. */
bool arch_mmu_query(const struct arch_mmu_context *ctx, vaddr_t va, paddr_t *pa,
                    vm_prot_t *prot, vm_cache_t *cache, size_t *page_size);

/* Make ctx the active translation on the calling CPU. */
void arch_mmu_activate(const struct arch_mmu_context *ctx);

/* Invalidate cached translations for [va, va+len) on the calling CPU.
 * Cross-CPU shootdown is a Phase 3 addition. */
void arch_mmu_invalidate(const struct arch_mmu_context *ctx, vaddr_t va, size_t len);

/* Bitmask of supported leaf sizes beyond 4 KiB: PAGE_2M_SIZE | PAGE_1G_SIZE. */
size_t arch_mmu_large_page_sizes(void);

/* Lowest kernel-half virtual address; below it is user space. */
vaddr_t arch_mmu_kernel_base(void);

#endif /* ARCH_MMU_H */
