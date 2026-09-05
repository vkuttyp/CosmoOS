/*
 * arch/aarch64/cpu.c - CPU state for the AArch64 loader
 * (docs/kernel/arch/aarch64/design.md, "The loader").
 *
 * The firmware hands over at EL1 with the MMU on (QEMU `virt` without
 * the virtualization option runs EDK2 at EL1). Switching translation
 * tables happens with the MMU on: the new TTBR0 identity-maps the RAM
 * the loader runs from, so the PC and stack stay valid across the switch.
 */

#include "loader.h"

#define SCTLR_M   (1ull << 0)
#define SCTLR_A   (1ull << 1)
#define SCTLR_C   (1ull << 2)
#define SCTLR_I   (1ull << 12)
#define SCTLR_WXN (1ull << 19)

#define MAIR_VALUE 0x0000000000440000FFull   /* idx0 normal WB, idx1 device nGnRnE, idx2 normal NC */

#define TCR_T0SZ(x)    ((uint64_t)(x) << 0)
#define TCR_T1SZ(x)    ((uint64_t)(x) << 16)
#define TCR_IRGN0_WBWA (1ull << 8)
#define TCR_ORGN0_WBWA (1ull << 10)
#define TCR_SH0_INNER  (3ull << 12)
#define TCR_IRGN1_WBWA (1ull << 24)
#define TCR_ORGN1_WBWA (1ull << 26)
#define TCR_SH1_INNER  (3ull << 28)
#define TCR_TG1_4K     (2ull << 30)
#define TCR_IPS(x)     ((uint64_t)(x) << 32)

static inline uint64_t read_sysreg_current_el(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 3;
}

bool cpu_prepare(void)
{
    uint64_t el = read_sysreg_current_el();
    if (el == 1)
        return true;
    lprintf("cosmoboot: started at EL%u; this loader requires EL1 (boot the virt machine with "
            "virtualization=off)\n", (unsigned)el);
    return false;
}

void cpu_finish(void)
{
    /* Nothing: W^X is the kernel's business (WXN stays off, the direct map is RW). */
}

void cpu_halt(void)
{
    for (;;)
        __asm__ volatile("msr daifset, #0xF\n\twfi" ::: "memory");
}

static uint64_t tcr_value(void)
{
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t parange = mmfr0 & 0xF;
    if (parange > 6)
        parange = 6;
    return TCR_T0SZ(16) | TCR_T1SZ(16) | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_INNER | TCR_IRGN1_WBWA |
           TCR_ORGN1_WBWA | TCR_SH1_INNER | TCR_TG1_4K | TCR_IPS(parange);
}

void cpu_jump_to_kernel(const struct paging_ctx *pg, uint64_t stack_top, uint64_t info, uint64_t entry)
{
    uint64_t tcr = tcr_value();
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    sctlr &= ~(SCTLR_WXN | SCTLR_A);
    __asm__ volatile(
        "msr daifset, #0xF\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        "msr mair_el1, %[mair]\n\t"
        "msr tcr_el1, %[tcr]\n\t"
        "msr ttbr0_el1, %[t0]\n\t"
        "msr ttbr1_el1, %[t1]\n\t"
        "isb\n\t"
        "tlbi vmalle1\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        "msr sctlr_el1, %[sctlr]\n\t"
        "isb\n\t"
        "mov sp, %[sp]\n\t"
        "mov x0, %[info]\n\t"
        "mov x29, xzr\n\t"
        "mov x30, xzr\n\t"
        "br %[entry]\n\t"
        :
        : [mair] "r"(MAIR_VALUE), [tcr] "r"(tcr), [t0] "r"(pg->root_user), [t1] "r"(pg->root),
          [sctlr] "r"(sctlr), [sp] "r"(stack_top), [info] "r"(info), [entry] "r"(entry)
        : "memory", "x0");
    __builtin_unreachable();
}
