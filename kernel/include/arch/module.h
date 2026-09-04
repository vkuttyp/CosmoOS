/*
 * arch/module.h - Architecture-specific part of the module loader:
 * relocation application.
 */

#ifndef ARCH_MODULE_H
#define ARCH_MODULE_H

#include <kernel/elf64.h>
#include <kernel/types.h>

/* Apply `count` RELA entries to the section mapped read-write at
 * `target` (size `target_size`). sym_addr[i] is the resolved address of
 * symbol i (nr_syms entries). Returns 0, -EINVAL (offset outside the
 * section or symbol index out of range), -ERANGE (32-bit overflow), or
 * -ENOEXEC (unsupported type). `why` gets an immortal string. */
int arch_module_reloc(vaddr_t target, size_t target_size, const struct elf64_rela *rela, size_t count,
                      const uintptr_t *sym_addr, size_t nr_syms, const char **why);

#endif /* ARCH_MODULE_H */
