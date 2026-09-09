/* testhooks.c - Self-test aids with no AArch64 implementation (docs/kernel/arch/aarch64/testing.md). */

#include <arch/testhooks.h>
#include <aarch64/sysreg.h>

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

uint64_t arch_test_host_vtimer_ctl(void)
{
    return READ_SYSREG(cntv_ctl_el0);
}
