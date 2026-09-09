/* cpu.c - CPU identification and the arch/cpu.h primitives (docs/kernel/arch/aarch64/design.md). */

#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <arch/cpu.h>
#include <aarch64/platform.h>
#include <aarch64/fpu.h>
#include <aarch64/sysreg.h>

static struct aarch64_cpu_info g_cpu;

static const char *part_name(unsigned implementer, unsigned part)
{
    if (implementer == 0x41) {   /* Arm */
        switch (part) {
        case 0xD03: return "Cortex-A53";
        case 0xD04: return "Cortex-A35";
        case 0xD05: return "Cortex-A55";
        case 0xD07: return "Cortex-A57";
        case 0xD08: return "Cortex-A72";
        case 0xD09: return "Cortex-A73";
        case 0xD0A: return "Cortex-A75";
        case 0xD0B: return "Cortex-A76";
        case 0xD0C: return "Neoverse-N1";
        case 0xD0D: return "Cortex-A77";
        case 0xD40: return "Neoverse-V1";
        case 0xD41: return "Cortex-A78";
        case 0xD49: return "Neoverse-N2";
        case 0xD4F: return "Neoverse-V2";
        default: break;
        }
    }
    return NULL;
}

void aarch64_cpu_init(void)
{
    g_cpu.midr = READ_SYSREG(midr_el1);
    g_cpu.mpidr = READ_SYSREG(mpidr_el1);
    g_cpu.has_pan = ID_AA64MMFR1_PAN(READ_SYSREG(id_aa64mmfr1_el1)) != 0;
    g_cpu.parange = (unsigned)ID_AA64MMFR0_PARANGE(READ_SYSREG(id_aa64mmfr0_el1));
    /*
     * ASID width. The loader left TCR_EL1 with AS clear (8-bit ASIDs);
     * this is the kernel's only write to that register, and it happens
     * here -- before any space exists, while every TTBR0 in the machine
     * carries ASID 0, which reads the same at either width. Secondary
     * CPUs copy TCR_EL1 from this one through the SMP mailbox, so the
     * width is uniform without their doing anything.
     */
    g_cpu.asid_bits = ID_AA64MMFR0_ASIDBITS(READ_SYSREG(id_aa64mmfr0_el1)) == 2 ? 16 : 8;
    if (g_cpu.asid_bits == 16) {
        uint64_t tcr = READ_SYSREG(tcr_el1);
        if (!(tcr & TCR_AS)) {
            WRITE_SYSREG(tcr_el1, tcr | TCR_AS);
            isb();
            tlbi_vmalle1is();   /* a TCR change invalidates nothing by itself */
        }
    }
    g_cpu.gic_sysreg = (unsigned)ID_AA64PFR0_GIC(READ_SYSREG(id_aa64pfr0_el1));
    unsigned implementer = (unsigned)((g_cpu.midr >> 24) & 0xFF);
    unsigned part = (unsigned)((g_cpu.midr >> 4) & 0xFFF);
    unsigned variant = (unsigned)((g_cpu.midr >> 20) & 0xF);
    unsigned revision = (unsigned)(g_cpu.midr & 0xF);
    const char *name = part_name(implementer, part);
    if (name)
        ksnprintf(g_cpu.brand, sizeof(g_cpu.brand), "%s r%up%u", name, variant, revision);
    else
        ksnprintf(g_cpu.brand, sizeof(g_cpu.brand), "AArch64 implementer 0x%x part 0x%x r%up%u", implementer,
                  part, variant, revision);
    if (g_cpu.has_pan) {
        /* SPAN: do not set PAN automatically on exception entry; PAN stays set and
         * the copy routines clear it themselves around user-memory accesses. */
        uint64_t sctlr = READ_SYSREG(sctlr_el1);
        WRITE_SYSREG(sctlr_el1, sctlr | SCTLR_SPAN);
        __asm__ volatile(".inst 0xd500419f" ::: "memory");   /* msr pan, #1 */
        isb();
    }
    /* FP/SIMD, for user threads and for the switch that saves them
     * (fpu.c). Every CPU, because CPACR_EL1 is per-CPU state. */
    aarch64_fpu_init_cpu();
}

const struct aarch64_cpu_info *aarch64_cpu_info(void)
{
    return &g_cpu;
}

const char *arch_name(void)
{
    return "aarch64";
}

void arch_cpu_brand_string(char *buf, size_t len)
{
    strlcpy(buf, g_cpu.brand[0] ? g_cpu.brand : "AArch64", len);
}

void arch_cpu_relax(void)
{
    yield_hint();
}

void arch_cpu_wait_for_interrupt(void)
{
    /* Interrupts are enabled by the caller: WFI wakes on a pending IRQ and the handler runs. */
    wfi();
}

void arch_cpu_halt_forever(void)
{
    for (;;) {
        __asm__ volatile("msr daifset, #0xF" ::: "memory");
        wfi();
    }
}

void arch_dma_barrier(void)
{
    dsb_sy();
}
