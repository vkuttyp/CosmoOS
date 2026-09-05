/*
 * extable.c - Exception fixup table lookup (docs/kernel/memory/design.md §6.1).
 *
 * The table is small (one entry per faulting instruction in the user
 * copy primitives), so a linear scan on the fault path costs less than
 * keeping it sorted at link time would be worth.
 */

#include <kernel/extable.h>

extern const struct ex_entry __ex_table_start[];
extern const struct ex_entry __ex_table_end[];

static inline uintptr_t ex_insn(const struct ex_entry *e)
{
    return (uintptr_t)&e->insn + (uintptr_t)(intptr_t)e->insn;
}

static inline uintptr_t ex_fixup(const struct ex_entry *e)
{
    return (uintptr_t)&e->fixup + (uintptr_t)(intptr_t)e->fixup;
}

uintptr_t extable_fixup(uintptr_t pc)
{
    for (const struct ex_entry *e = __ex_table_start; e < __ex_table_end; e++)
        if (ex_insn(e) == pc)
            return ex_fixup(e);
    return 0;
}

unsigned extable_count(void)
{
    return (unsigned)(__ex_table_end - __ex_table_start);
}

bool extable_entry(unsigned i, uintptr_t *insn, uintptr_t *fixup)
{
    if (i >= extable_count())
        return false;
    *insn = ex_insn(&__ex_table_start[i]);
    *fixup = ex_fixup(&__ex_table_start[i]);
    return true;
}
