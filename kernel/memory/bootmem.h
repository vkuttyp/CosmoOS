/*
 * bootmem.h - Bootstrap allocator, private to kernel/memory.
 *
 * Lifetime: bootmem_init() at the start of pmm_init(), bootmem_alloc()
 * for the page array and zone tables, bootmem_release_all() to hand the
 * remainder to the buddy, bootmem_seal() to forbid further use.
 */

#ifndef KERNEL_MEMORY_BOOTMEM_H
#define KERNEL_MEMORY_BOOTMEM_H

#include <kernel/types.h>

typedef void (*bootmem_release_fn)(pfn_t start_pfn, pfn_t end_pfn);

void bootmem_init(void);

/* Allocate zeroed, permanently reserved memory from the top of the
 * highest free range. Panics on failure: nothing can proceed without it.
 * Returns a direct-map virtual address. */
void *bootmem_alloc(size_t size, size_t align);

/* Call `release` for every remaining free range, page aligned inward. */
void bootmem_release_all(bootmem_release_fn release);

void bootmem_seal(void);
uint64_t bootmem_allocated_bytes(void);

#endif /* KERNEL_MEMORY_BOOTMEM_H */
