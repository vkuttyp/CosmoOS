/*
 * x86/paging.h - 4-level page-table format. Private to x86-64.
 */

#ifndef X86_PAGING_H
#define X86_PAGING_H

#include <stdint.h>

#define PTE_P    (1ULL << 0)
#define PTE_RW   (1ULL << 1)
#define PTE_US   (1ULL << 2)
#define PTE_PWT  (1ULL << 3)
#define PTE_PCD  (1ULL << 4)
#define PTE_A    (1ULL << 5)
#define PTE_D    (1ULL << 6)
#define PTE_PS   (1ULL << 7)   /* large page in PDPT/PD entries */
#define PTE_PAT4K (1ULL << 7)  /* PAT bit in 4 KiB PTEs (unused: PAT at defaults) */
#define PTE_G    (1ULL << 8)
#define PTE_NX   (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_FLAGS_MASK (~PTE_ADDR_MASK)

#define PT_ENTRIES 512u

#define PML4_SHIFT 39u
#define PDPT_SHIFT 30u
#define PD_SHIFT   21u
#define PT_SHIFT   12u

#define PML4_INDEX(va) (((va) >> PML4_SHIFT) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> PDPT_SHIFT) & 0x1FF)
#define PD_INDEX(va)   (((va) >> PD_SHIFT) & 0x1FF)
#define PT_INDEX(va)   (((va) >> PT_SHIFT) & 0x1FF)

/* Kernel half starts at PML4 index 256. */
#define X86_KERNEL_BASE 0xFFFF800000000000ULL

typedef uint64_t pte_t;

#endif /* X86_PAGING_H */
