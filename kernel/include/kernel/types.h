/*
 * types.h - Address kinds and page constants.
 *
 * Physical and virtual addresses are different things and the type system
 * says so. Conversions go through the named functions in page.h, never
 * through casts in callers. `pfn_t` is a physical page frame number.
 */

#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t  paddr_t;  /* physical address */
typedef uintptr_t vaddr_t;  /* kernel virtual address */
typedef uint64_t  pfn_t;    /* page frame number: paddr >> PAGE_SHIFT */

#define PAGE_SHIFT 12u
#define PAGE_SIZE  ((size_t)1 << PAGE_SHIFT)
#define PAGE_MASK  (~((paddr_t)PAGE_SIZE - 1))

#define PAGE_2M_SHIFT 21u
#define PAGE_2M_SIZE  ((size_t)1 << PAGE_2M_SHIFT)
#define PAGE_1G_SHIFT 30u
#define PAGE_1G_SIZE  ((size_t)1 << PAGE_1G_SHIFT)

static inline pfn_t phys_to_pfn(paddr_t pa)
{
    return pa >> PAGE_SHIFT;
}

static inline paddr_t pfn_to_phys(pfn_t pfn)
{
    return pfn << PAGE_SHIFT;
}

static inline bool is_page_aligned(uint64_t x)
{
    return (x & (PAGE_SIZE - 1)) == 0;
}

static inline uint64_t page_align_down(uint64_t x)
{
    return x & ~((uint64_t)PAGE_SIZE - 1);
}

static inline uint64_t page_align_up(uint64_t x)
{
    return (x + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
}

#endif /* KERNEL_TYPES_H */
