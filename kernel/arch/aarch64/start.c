/* start.c - From the loader's handoff to kernel_main (docs/kernel/arch/aarch64/design.md, "Kernel entry"). */

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/percpu.h>
#include <arch/console.h>
#include <arch/user.h>
#include <aarch64/platform.h>
#include <aarch64/sysreg.h>

#include <cosmoboot.h>

/* The direct-map base, for device access before the PMM publishes its own. */
uint64_t aarch64_hhdm_base;

void aarch64_start(const void *info_ptr)
{
    const struct cosmoboot_info *info = info_ptr;
    aarch64_hhdm_base = info->hhdm_base;
    arch_console_early_init();
    percpu_init_boot();
    aarch64_cpu_init();
    arch_syscall_init_cpu();
    const struct aarch64_cpu_info *c = aarch64_cpu_info();
    kdebug("aarch64: %s, EL%u, MPIDR 0x%llx, PAN %d, PARange %u, GIC sysregs %u", c->brand, current_el(),
           (unsigned long long)c->mpidr, c->has_pan, c->parange, c->gic_sysreg);
    kdebug("aarch64: SCTLR 0x%llx TCR 0x%llx MAIR 0x%llx TTBR0 0x%llx TTBR1 0x%llx",
           (unsigned long long)READ_SYSREG(sctlr_el1), (unsigned long long)READ_SYSREG(tcr_el1),
           (unsigned long long)READ_SYSREG(mair_el1), (unsigned long long)READ_SYSREG(ttbr0_el1),
           (unsigned long long)READ_SYSREG(ttbr1_el1));
    kernel_main(info);
}
