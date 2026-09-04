/*
 * vmm.h - Virtual memory: address spaces, regions, kernel VA arena,
 * page faults.
 *
 * Only the kernel address space exists in this phase. The region model is
 * built so user spaces (Phase 4) add region kinds and a per-process
 * vm_space without changing these signatures.
 *
 * All functions are non-blocking. They take vm_space.lock (irqsave) and
 * may allocate from the heap (region structs) and the PMM (frames, table
 * pages). Lock order: vm_space.lock -> kmem_cache.lock -> pmm_zone.lock.
 */

#ifndef KERNEL_VMM_H
#define KERNEL_VMM_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

#include <arch/mmu.h>
#include <arch/trap.h>

enum vm_region_kind {
    VM_REGION_PHYS,  /* fixed physical backing: image, direct map, MMIO */
    VM_REGION_ANON,  /* frames owned by the region; demand-zero unless populated */
};

/* Region flags. */
#define VM_REGION_GUARD_BELOW (1u << 0)
#define VM_REGION_GUARD_ABOVE (1u << 1)
#define VM_REGION_POPULATED   (1u << 2)  /* ANON: fully populated at creation */
#define VM_REGION_USER        (1u << 3)  /* accessible from user mode (U/S) */

struct vm_region {
    struct list_node link;   /* in vm_space.regions, sorted by base */
    vaddr_t base;            /* first mapped byte (guards excluded) */
    size_t size;             /* page multiple, guards excluded */
    vm_prot_t prot;
    vm_cache_t cache;
    enum vm_region_kind kind;
    unsigned flags;
    paddr_t phys;            /* PHYS: physical base */
    const char *name;        /* immortal string */
};

struct vm_space {
    struct arch_mmu_context mmu;
    struct list_node regions;
    spinlock_t lock;
    vaddr_t arena_lo;        /* kernel VA arena for dynamic allocations */
    vaddr_t arena_hi;
    bool user;               /* a process address space (lower half) */
    uint64_t anon_pages;     /* frames populated for this space's ANON regions */
};

extern struct vm_space kernel_space;

/* --- user address spaces (Phase 4) --- */

/* The user window: canonical lower half minus the first 4 MiB (null
 * page and legacy space) and the last page. */
#define VM_USER_LO 0x0000000000400000ULL
#define VM_USER_HI 0x00007FFFFFFFF000ULL

/* The process layer tells the fault handler which user space the
 * current thread runs in and how to terminate it on a fatal fault. */
struct arch_trap_frame;
struct vm_user_hooks {
    struct vm_space *(*current_space)(void);              /* NULL for kernel threads */
    void (*fatal)(uint64_t addr, unsigned fault_flags, struct arch_trap_frame *frame) __noreturn;
};
void vm_set_user_hooks(const struct vm_user_hooks *hooks);

/* Fresh user space whose kernel half mirrors the kernel tables.
 * Returns 0 or -ENOMEM. */
int vm_space_create_user(struct vm_space **out);

/* Tear down every region, free frames, free lower-half tables, free the
 * struct. Must not be the space active on the calling CPU. */
void vm_space_destroy(struct vm_space *space);

/* Map an anonymous user region at exactly [base, base+size). `prot`
 * must not be W+X. flags: VM_REGION_POPULATED for eager zeroed frames,
 * VM_REGION_GUARD_BELOW for a guard page below. Returns 0, -EEXIST if it
 * overlaps, -EINVAL, -ENOMEM. */
int vm_user_map_anon(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot, unsigned flags,
                     const char *name);

/* Unmap a region that starts at `base` with exactly `size` bytes. */
int vm_user_unmap(struct vm_space *space, uint64_t base, size_t size);

/* Change the protection of a whole region (exact base and size).
 * -EINVAL for W+X or a partial range. */
int vm_user_protect(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot);

/* Lowest free range of `size` bytes at or above `from` inside
 * [USER_LO, USER_HI) with a guard gap; 0 if none. */
uint64_t vm_user_find_free(struct vm_space *space, uint64_t from, size_t size);

/* True if every page of [addr, addr+len) is inside one or more regions
 * of `space` that all carry `prot`. */
bool vm_user_range_mapped(struct vm_space *space, uint64_t addr, size_t len, vm_prot_t prot);

/* Take over paging from the loader. Requires pmm_init and kmalloc_init.
 * After return: kernel tables active, all RAM in the direct map, boot
 * tables freed, deferred frames released, fault handler installed. */
void vmm_init(void);

/* vm_kernel_alloc flags */
#define VM_KALLOC_GUARD    (1u << 0)  /* unmapped page below and above */
#define VM_KALLOC_POPULATE (1u << 1)  /* map zeroed frames now instead of on fault */

/* Allocate `size` bytes (page multiple) of kernel virtual memory backed by
 * fresh zeroed frames. Returns the base or 0 on failure. */
vaddr_t vm_kernel_alloc(size_t size, unsigned flags, vm_prot_t prot);

/* Free a vm_kernel_alloc result, unmapping and releasing every frame it
 * populated. Panics if `base` is not a live allocation. */
void vm_kernel_free(vaddr_t base);

/* Map physical [pa, pa+size) (page aligned) into the arena, for MMIO.
 * Returns the virtual base or 0. */
vaddr_t vm_map_phys(paddr_t pa, size_t size, vm_prot_t prot, vm_cache_t cache);
void vm_unmap_phys(vaddr_t base);

/* Look up the region containing va in the kernel space. Returns NULL if
 * none. The pointer is valid only while the caller holds no expectation
 * of it surviving a vm_kernel_free of that region. */
const struct vm_region *vm_find_region(struct vm_space *space, vaddr_t va);

/* Translate a kernel virtual address through the active tables. */
static inline bool vm_query(vaddr_t va, paddr_t *pa, vm_prot_t *prot, vm_cache_t *cache,
                            size_t *page_size)
{
    return arch_mmu_query(&kernel_space.mmu, va, pa, prot, cache, page_size);
}

/* Fault kind bits: the arch layer's decoding, re-exported under the VM
 * name so callers of the fault path never include arch/trap.h. */
#define VM_FAULT_PRESENT  ARCH_FAULT_PRESENT
#define VM_FAULT_WRITE    ARCH_FAULT_WRITE
#define VM_FAULT_EXEC     ARCH_FAULT_EXEC
#define VM_FAULT_USER     ARCH_FAULT_USER
#define VM_FAULT_RESERVED ARCH_FAULT_RESERVED

struct vm_stats {
    uint64_t regions;
    uint64_t anon_pages;       /* frames populated for ANON regions */
    uint64_t faults_handled;   /* demand-zero populations */
};

void vm_get_stats(struct vm_stats *out);
void vm_dump(struct vm_space *space);

#endif /* KERNEL_VMM_H */
