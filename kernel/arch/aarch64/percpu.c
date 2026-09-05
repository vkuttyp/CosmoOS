/* percpu.c - The per-CPU block lives in TPIDR_EL1 (docs/kernel/arch/aarch64/design.md). */

#include <kernel/percpu.h>
#include <arch/cpu.h>
#include <arch/percpu.h>
#include <aarch64/sysreg.h>

void arch_percpu_install(struct percpu *pc)
{
    pc->self = pc;
    WRITE_SYSREG(tpidr_el1, (uint64_t)(uintptr_t)pc);
    isb();
}

struct percpu *arch_percpu_get(void)
{
    return (struct percpu *)(uintptr_t)READ_SYSREG(tpidr_el1);
}

unsigned arch_cpu_id(void)
{
    return arch_percpu_get()->cpu_id;
}

/* Module ABI export: a multi-queue driver picks the queue of the CPU it runs on. */
#include <kernel/module.h>
EXPORT_SYMBOL(arch_cpu_id);
