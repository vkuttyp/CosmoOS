/* testhooks.c - Self-test aids with no AArch64 implementation (docs/kernel/arch/aarch64/testing.md). */

#include <arch/testhooks.h>

/*
 * AArch64 has no vector that can arrive with a foreign per-CPU pointer:
 * TPIDR_EL1 is never swapped on the user boundary and every exception
 * from EL0 lands on SP_EL1, which is the thread's own kernel stack (the
 * SPSel=1 rule in vectors.S). The periodic IRQ hook lives in timer.c.
 */
bool arch_test_paranoid_entry(const char **why)
{
    *why = NULL;
    return true;
}
