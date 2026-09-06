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

/* The stub, assembled into this image (el2_stub.S) and copied into a
 * page of its own before the drop to EL1. */
extern const uint8_t el2_stub_begin[];
extern const uint8_t el2_stub_end[];

static uint64_t g_el2_stub;   /* physical address of the copy, 0 when we booted at EL1 */
static bool g_el2_type_fallback;   /* the firmware refused the loader's memory type */

bool cpu_el2_type_fallback(void)
{
    return g_el2_type_fallback;
}

uint64_t cpu_el2_stub(void)
{
    return g_el2_stub;
}

bool cpu_prepare(void)
{
    uint64_t el = read_sysreg_current_el();
    if (el == 1)
        return true;
    if (el != 2) {
        lprintf("cosmoboot: started at EL%u; the kernel needs EL1 or EL2\n", (unsigned)el);
        return false;
    }
    /* EL2: keep it. One page, never freed by the kernel, holding a stub
     * that answers HVC so EL1 has a way back up
     * (docs/kernel/arch/aarch64/design.md, "Exception level 2"). */
    /* Refusing here is the only safe answer. Dropping to EL1 without a
     * stub would leave VBAR_EL2 pointing at firmware vectors that
     * ExitBootServices makes meaningless, so the first exception to EL2
     * -- an HVC from EL1, which is how PSCI is routed on some machines
     * -- would jump into reclaimed memory. Better to say so and stop
     * (docs/boot/invariants.md BT13). */
    size_t len = (size_t)(el2_stub_end - el2_stub_begin);
    if (len == 0 || len > PAGE_SIZE) {
        lprintf("cosmoboot: the EL2 stub is %u bytes, which does not fit its page\n", (unsigned)len);
        return false;
    }
    EFI_PHYSICAL_ADDRESS addr = 0;
    bool fallback = false;
    EFI_STATUS st = alloc_pages_low(1, EFI_MEMORY_TYPE_COSMO_EL2, &addr, &fallback);
    if (EFI_ERROR(st)) {
        lprintf("cosmoboot: no page for the EL2 stub (0x%llx); cannot hand over from EL2\n",
                (unsigned long long)st);
        return false;
    }
    g_el2_type_fallback = fallback;
    memcpy((void *)(uintptr_t)addr, el2_stub_begin, len);
    g_el2_stub = (uint64_t)addr;
    lprintf("cosmoboot: EL2 stub at 0x%llx (%u bytes)\n", (unsigned long long)addr, (unsigned)len);
    return true;
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

/* HCR_EL2.RW: EL1 is AArch64. CPTR_EL2's RES1 bits with TFP clear: EL1
 * and EL0 may use FP/SIMD. CNTHCTL_EL2 bits 0-1: EL1 may read the
 * counters and program the timers. */
#define HCR_EL2_RW      (1ull << 31)
#define CPTR_EL2_RES1   0x33FFull
#define CPTR_EL2_TFP    (1ull << 10)
#define CNTHCTL_EL1_ACCESS 0x3ull
/* EL1h, DAIF masked: what the kernel entry expects. */
#define SPSR_EL2_TO_EL1H 0x3C5ull

/* Program EL2 and leave through it. The EL1 translation registers are
 * already set by the caller, so the ERET lands on the kernel entry with
 * the loader's tables active; the EL2 MMU goes off first, so the stub
 * never depends on firmware page tables the kernel will reclaim
 * (docs/kernel/arch/aarch64/design.md, "Exception level 2"). */
static void jump_from_el2(uint64_t stack_top, uint64_t info, uint64_t entry, uint64_t stub)
{
    /* cpu_prepare refused to continue without one. */
    if (stub == 0)
        cpu_halt();
    uint64_t midr, mpidr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile(
        "msr hcr_el2, %[hcr]\n\t"
        "msr cptr_el2, %[cptr]\n\t"
        "msr cnthctl_el2, %[cnthctl]\n\t"
        "msr cntvoff_el2, xzr\n\t"
        "msr vttbr_el2, xzr\n\t"
        "msr vpidr_el2, %[midr]\n\t"
        "msr vmpidr_el2, %[mpidr]\n\t"
        "msr vbar_el2, %[stub]\n\t"
        "isb\n\t"
        /* EL2 runs unmapped from here: this code is identity mapped by
         * firmware, and after the ERET nothing executes at EL2 until an
         * HVC reaches the stub. */
        "msr sctlr_el2, %[sctlr2]\n\t"
        "isb\n\t"
        "msr spsr_el2, %[spsr]\n\t"
        "msr elr_el2, %[entry]\n\t"
        "mov x0, %[info]\n\t"
        "msr sp_el1, %[sp]\n\t"
        "mov x29, xzr\n\t"
        "mov x30, xzr\n\t"
        "eret\n\t"
        :
        : [hcr] "r"(HCR_EL2_RW), [cptr] "r"(CPTR_EL2_RES1 & ~CPTR_EL2_TFP),
          [cnthctl] "r"(CNTHCTL_EL1_ACCESS), [midr] "r"(midr), [mpidr] "r"(mpidr), [stub] "r"(stub),
          [sctlr2] "r"((uint64_t)0x30C50830ull), [spsr] "r"(SPSR_EL2_TO_EL1H), [entry] "r"(entry),
          [info] "r"(info), [sp] "r"(stack_top)
        : "memory", "x0");
    __builtin_unreachable();
}

void cpu_jump_to_kernel(const struct paging_ctx *pg, uint64_t stack_top, uint64_t info, uint64_t entry)
{
    uint64_t tcr = tcr_value();
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    sctlr &= ~(SCTLR_WXN | SCTLR_A);
    if (read_sysreg_current_el() == 2) {
        /* The EL1 registers below take effect at the ERET, not here. */
        __asm__ volatile(
            "msr daifset, #0xF\n\t"
            "dsb sy\n\t"
            "isb\n\t"
            "msr mair_el1, %[mair]\n\t"
            "msr tcr_el1, %[tcr]\n\t"
            "msr ttbr0_el1, %[t0]\n\t"
            "msr ttbr1_el1, %[t1]\n\t"
            "msr sctlr_el1, %[sctlr]\n\t"
            "isb\n\t"
            "tlbi vmalle1\n\t"
            "dsb sy\n\t"
            "isb\n\t"
            :
            : [mair] "r"(MAIR_VALUE), [tcr] "r"(tcr), [t0] "r"(pg->root_user), [t1] "r"(pg->root),
              [sctlr] "r"(sctlr)
            : "memory");
        jump_from_el2(stack_top, info, entry, g_el2_stub);
    }
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
