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
#include <kernel/percpu.h>

typedef unsigned vm_prot_t;
#define VM_PROT_NONE  0u
#define VM_PROT_READ  (1u << 0)
#define VM_PROT_WRITE (1u << 1)
#define VM_PROT_EXEC  (1u << 2)
#define VM_PROT_RW    (VM_PROT_READ | VM_PROT_WRITE)
#define VM_PROT_RX    (VM_PROT_READ | VM_PROT_EXEC)
#define VM_PROT_USER  (1u << 3)   /* accessible from user mode; reported by query */

typedef enum vm_cache {
    VM_CACHE_WB,   /* write-back: normal memory */
    VM_CACHE_WT,   /* write-through */
    VM_CACHE_UC,   /* uncached: device MMIO */
} vm_cache_t;

/* arch_mmu_map flags */
#define ARCH_MMU_MAP_LARGE  (1u << 0)  /* may use 2 MiB / 1 GiB entries where aligned */
#define ARCH_MMU_MAP_GLOBAL (1u << 1)  /* kernel mapping shared by all spaces */
#define ARCH_MMU_MAP_USER   (1u << 2)  /* user-accessible leaf and intermediate entries */

struct arch_mmu_context {
    paddr_t root;  /* physical address of the top-level table */
};

/* Allocate an empty top-level table for the kernel space. Returns 0 or
 * -ENOMEM. */
int arch_mmu_context_init(struct arch_mmu_context *ctx);

/* Allocate a top-level table for a user space whose kernel half mirrors
 * the kernel context's entries (which never change after vmm_init). */
int arch_mmu_context_init_user(struct arch_mmu_context *ctx, const struct arch_mmu_context *kernel);

/* Free every lower-half table page and the root of a user context. The
 * caller has unmapped all leaves and ensured no CPU has it active. */
void arch_mmu_context_destroy(struct arch_mmu_context *ctx);

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
 * skipped. -EINVAL if the range splits a large page. VM_PROT_NONE keeps
 * the frame but makes the entry invalid to the hardware (a software
 * "none" bit marks it, docs/kernel/memory/design.md §6.2); a later
 * protect to any permission makes it valid again. */
int arch_mmu_protect(struct arch_mmu_context *ctx, vaddr_t va, size_t len, vm_prot_t prot);

/* Look up one virtual address. Returns false if not mapped. A PROT_NONE
 * page is mapped: true with *prot == VM_PROT_NONE (plus VM_PROT_USER).
 * Output pointers may be NULL. page_size is 4K/2M/1G of the leaf. */
bool arch_mmu_query(const struct arch_mmu_context *ctx, vaddr_t va, paddr_t *pa,
                    vm_prot_t *prot, vm_cache_t *cache, size_t *page_size);

/* Make ctx the active translation on the calling CPU. */
void arch_mmu_activate(const struct arch_mmu_context *ctx);

/* Invalidate cached translations for [va, va+len) on the calling CPU
 * only. */
void arch_mmu_invalidate(const struct arch_mmu_context *ctx, vaddr_t va, size_t len);

/* Invalidate [va, va+len) on every online CPU and wait for each to
 * acknowledge. Must be called with interrupts enabled and without any
 * spinlock held that an interrupt handler on another CPU could be
 * waiting for (the VMM releases its lock first). Panics if a CPU does
 * not answer within a generous bound. */
void arch_mmu_shootdown(const struct arch_mmu_context *ctx, vaddr_t va, size_t len);

/* The same for the CPUs in `cpus` only (the calling CPU is always
 * included; CPUs not online are ignored). The VMM passes the set of CPUs
 * whose active root is `ctx` (docs/kernel/memory/design.md §6.4). */
void arch_mmu_shootdown_cpus(const struct arch_mmu_context *ctx, vaddr_t va, size_t len, cpumask_t cpus);

/* Create the intermediate tables below the top level for every top-level
 * slot [va, va+len) touches, without mapping anything, so that later maps
 * in the range never add a top-level entry. Used by vmm_init on the
 * kernel arena before the first user space copies the kernel half
 * (x86-64); a no-op where the kernel half is shared by construction
 * (AArch64 TTBR1). Returns 0 or -ENOMEM. */
int arch_mmu_prepopulate(struct arch_mmu_context *ctx, vaddr_t va, size_t len);

struct arch_mmu_shootdown_stats {
    uint64_t initiated;       /* shootdowns started on this CPU */
    uint64_t handled;         /* flush IPIs handled on this CPU */
    uint64_t acks_received;   /* acknowledgements collected by this CPU */
};

void arch_mmu_shootdown_stats(struct arch_mmu_shootdown_stats *out);

/* Body of the IPI_TLB_FLUSH handler; called by the generic IPI layer on
 * the target CPU in interrupt context. */
void arch_mmu_shootdown_ipi_handler(void);

/* Bitmask of supported leaf sizes beyond 4 KiB: PAGE_2M_SIZE | PAGE_1G_SIZE. */
size_t arch_mmu_large_page_sizes(void);

/* Lowest kernel-half virtual address; below it is user space. */
vaddr_t arch_mmu_kernel_base(void);
/* The near arena: kernel virtual addresses modules are loaded at, chosen
 * so the architecture's direct branches and code model reach the kernel
 * image from there (x86-64: the top 2 GiB above the image; AArch64: within
 * +-128 MiB of the image for CALL26). Page aligned, lo < hi. */
void arch_mmu_near_arena(vaddr_t *lo, vaddr_t *hi);

#endif /* ARCH_MMU_H */
