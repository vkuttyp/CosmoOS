/* irq.c - Local interrupt masking through PSTATE.DAIF (docs/kernel/arch/aarch64/design.md). */

#include <arch/irq.h>
#include <aarch64/sysreg.h>

arch_irq_state_t arch_irq_save(void)
{
    uint64_t daif = READ_SYSREG(daif);
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return (arch_irq_state_t)daif;
}

void arch_irq_restore(arch_irq_state_t state)
{
    WRITE_SYSREG(daif, state);
}

void arch_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

void arch_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

bool arch_irq_enabled(void)
{
    return (READ_SYSREG(daif) & DAIF_I) == 0;
}
