/*
 * ksym.h - The kernel's exported symbol table.
 *
 * EXPORT_SYMBOL() records land in .ksymtab, which the linker script
 * keeps between __ksymtab_start and __ksymtab_end in the read-only
 * segment. ksym_init() builds a sorted index for binary search. Lookups
 * are lock-free and usable from the panic path.
 */

#ifndef KERNEL_KSYM_H
#define KERNEL_KSYM_H

#include <kernel/module.h>
#include <kernel/types.h>

void ksym_init(void);
uintptr_t ksym_lookup(const char *name);
size_t ksym_count(void);
const struct ksym *ksym_entry(size_t sorted_index);

/* Sort an array of ksym pointers by name (heap sort, in place). Shared
 * with the module loader for module export tables. */
void ksym_sort(const struct ksym **v, size_t n);
uintptr_t ksym_search(const struct ksym *const *v, size_t n, const char *name);

#endif /* KERNEL_KSYM_H */
