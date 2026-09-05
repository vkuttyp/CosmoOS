/*
 * extable.h - The exception fixup table (docs/kernel/memory/design.md §6.1).
 *
 * An instruction that may fault on a user address and has a recovery
 * path is listed here by the assembly that contains it:
 *
 *     1:  <access>
 *     .pushsection .ex_table, "a"
 *     .balign 4
 *     .long 1b - ., 9b - .
 *     .popsection
 *
 * Both words are offsets from their own position, so the table is
 * position independent and 8 bytes per entry. The fault handler resolves
 * a kernel-mode fault's PC through extable_fixup; a hit moves execution
 * to the fixup, which reports the failure to the caller.
 */

#ifndef KERNEL_EXTABLE_H
#define KERNEL_EXTABLE_H

#include <kernel/types.h>

struct ex_entry {
    int32_t insn;    /* relative to &entry->insn */
    int32_t fixup;   /* relative to &entry->fixup */
};

/* The fixup address for a faulting PC, or 0 when the PC has none. */
uintptr_t extable_fixup(uintptr_t pc);

/* Enumeration for tests and diagnostics. */
unsigned extable_count(void);
bool extable_entry(unsigned i, uintptr_t *insn, uintptr_t *fixup);

#endif /* KERNEL_EXTABLE_H */
