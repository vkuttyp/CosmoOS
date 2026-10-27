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
    /* UCT and UCI, as Linux sets them: a user program may read the cache
     * line sizes and make the instruction cache see code it wrote (DC
     * CVAU, IC IVAU) -- what __clear_cache does, which a compiler running
     * code it just generated needs (tcc -run; roadmap 1.2 A1) and any JIT.
     * The APs start from this value (smp.c's mailbox). */
    uint64_t sctlr_u = READ_SYSREG(sctlr_el1);
    WRITE_SYSREG(sctlr_el1, sctlr_u | SCTLR_UCT | SCTLR_UCI);
    isb();
    /* FP/SIMD, for user threads and for the switch that saves them
     * (fpu.c). Every CPU, because CPACR_EL1 is per-CPU state. */
    aarch64_fpu_init_cpu();
    arch_user_hwcap_note_cpu();   /* the boot CPU; aarch64_ap_entry notes the rest */
}

void arch_hardening_report(void)
{
    /* SCTLR_EL1 as it is, not as intended: WXN is set by
     * arch_mmu_activate on the kernel's own tables (the loader's map
     * RAM writable and executable), and this line says whether it was. */
    bool wxn = (READ_SYSREG(sctlr_el1) & SCTLR_WXN) != 0;
    kinfo("hardening: aarch64:%s%s", g_cpu.has_pan ? " pan" : "", wxn ? " wxn" : "");
    /* Its own line: the hardening line above is a contract the guard
     * boot's harness matches whole, and RAS is not a hardening feature
     * anyway -- it decides what an asynchronous abort can be told apart
     * from. Without it ESR_EL1 carries no AET and every SError is
     * uncontained (invariant I-ARCH-16). */
    unsigned ras = (unsigned)ID_AA64PFR0_RAS(READ_SYSREG(id_aa64pfr0_el1));
    kinfo("async-error: aarch64: FEAT_RAS %s (ID_AA64PFR0_EL1.RAS %u)%s",
          ras ? "present" : "absent", ras,
          ras ? "" : " -- every SError classifies as uncontained");
    if (!g_cpu.has_pan || !wxn)
        kwarn("hardening: absent:%s%s%s", g_cpu.has_pan ? "" : " pan", wxn ? "" : " wxn",
              g_cpu.has_pan ? "" : " -- kernel access to user memory is unguarded");
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

/* Linux's arm64 names for the fields of MIDR_EL1. */
int arch_cpu_info(char *buf, size_t len)
{
    return ksnprintf(buf, len,
                     "CPU implementer\t: 0x%02x\n"
                     "CPU architecture: 8\n"
                     "CPU variant\t: 0x%x\n"
                     "CPU part\t: 0x%03x\n"
                     "CPU revision\t: %u\n",
                     (unsigned)((g_cpu.midr >> 24) & 0xFF), (unsigned)((g_cpu.midr >> 20) & 0xF),
                     (unsigned)((g_cpu.midr >> 4) & 0xFFF), (unsigned)(g_cpu.midr & 0xF));
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

/*
 * AT_HWCAP and AT_HWCAP2 from the boot CPU's ID registers, field by field
 * as Linux's arm64_elf_hwcaps decodes them (a field at or above the value
 * that names the feature). Left out: what this kernel would have to turn
 * on or emulate for EL0 -- SVE and SME (CPACR ZEN), pointer authentication
 * (no keys), BTI (no GP pages), MTE, SSBS, the MRS emulation (CPUID), the
 * event stream (EVTSTRM) and DC CVAP/CVADP (not advertised; UCI is set for
 * DC CVAU and IC IVAU since roadmap 1.2 A1).
 */
#define IDF(reg, lsb) ((unsigned)(((reg) >> (lsb)) & 0xF))

static uint64_t hwcap_this_cpu(void)
{
    uint64_t pfr0 = READ_SYSREG(id_aa64pfr0_el1), isar0 = READ_SYSREG(id_aa64isar0_el1);
    uint64_t isar1 = READ_SYSREG(S3_0_c0_c6_1) /* ID_AA64ISAR1_EL1 */, mmfr2 = READ_SYSREG(S3_0_c0_c7_2) /* ID_AA64MMFR2_EL1 */;
    uint64_t h = 0;
    unsigned fp = IDF(pfr0, 16), simd = IDF(pfr0, 20);   /* 0xF: not implemented */
    if (fp != 0xF) {
        h |= 1ull << 0;                         /* FP */
        if (fp >= 1)
            h |= 1ull << 9;                     /* FPHP */
    }
    if (simd != 0xF) {
        h |= 1ull << 1;                         /* ASIMD */
        if (simd >= 1)
            h |= 1ull << 10;                    /* ASIMDHP */
    }
    if (IDF(isar0, 4) >= 1)  h |= 1ull << 3;    /* AES */
    if (IDF(isar0, 4) >= 2)  h |= 1ull << 4;    /* PMULL */
    if (IDF(isar0, 8) >= 1)  h |= 1ull << 5;    /* SHA1 */
    if (IDF(isar0, 12) >= 1) h |= 1ull << 6;    /* SHA2 */
    if (IDF(isar0, 12) >= 2) h |= 1ull << 21;   /* SHA512 */
    if (IDF(isar0, 16) >= 1) h |= 1ull << 7;    /* CRC32 */
    if (IDF(isar0, 20) >= 2) h |= 1ull << 8;    /* ATOMICS */
    if (IDF(isar0, 28) >= 1) h |= 1ull << 12;   /* ASIMDRDM */
    if (IDF(isar0, 32) >= 1) h |= 1ull << 17;   /* SHA3 */
    if (IDF(isar0, 36) >= 1) h |= 1ull << 18;   /* SM3 */
    if (IDF(isar0, 40) >= 1) h |= 1ull << 19;   /* SM4 */
    if (IDF(isar0, 44) >= 1) h |= 1ull << 20;   /* ASIMDDP */
    if (IDF(isar0, 48) >= 1) h |= 1ull << 23;   /* ASIMDFHM */
    if (IDF(isar0, 52) >= 1) h |= 1ull << 27;   /* FLAGM */
    if (IDF(isar1, 12) >= 1) h |= 1ull << 13;   /* JSCVT */
    if (IDF(isar1, 16) >= 1) h |= 1ull << 14;   /* FCMA */
    if (IDF(isar1, 20) >= 1) h |= 1ull << 15;   /* LRCPC */
    if (IDF(isar1, 20) >= 2) h |= 1ull << 26;   /* ILRCPC */
    if (IDF(isar1, 36) >= 1) h |= 1ull << 29;   /* SB */
    if (IDF(pfr0, 48) >= 1)  h |= 1ull << 24;   /* DIT */
    if (IDF(mmfr2, 32) >= 1) h |= 1ull << 25;   /* USCAT */
    return h;
}

static uint64_t hwcap2_this_cpu(void)
{
    uint64_t isar0 = READ_SYSREG(id_aa64isar0_el1), isar1 = READ_SYSREG(S3_0_c0_c6_1);   /* ID_AA64ISAR1_EL1 */
    uint64_t h = 0;
    if (IDF(isar0, 52) >= 2) h |= 1ull << 7;    /* FLAGM2 */
    if (IDF(isar1, 32) >= 1) h |= 1ull << 8;    /* FRINT */
    if (IDF(isar1, 52) >= 1) h |= 1ull << 13;   /* I8MM */
    if (IDF(isar1, 44) >= 1) h |= 1ull << 14;   /* BF16 */
    if (IDF(isar0, 60) >= 1) h |= 1ull << 16;   /* RNG: RNDR and RNDRRS, which EL0 may read */
    return h;
}

static uint64_t g_user_hwcap = ~0ull, g_user_hwcap2 = ~0ull;   /* intersections over the CPUs noted; ~0: none yet */

void arch_user_hwcap_note_cpu(void)
{
    __atomic_fetch_and(&g_user_hwcap, hwcap_this_cpu(), __ATOMIC_RELAXED);
    __atomic_fetch_and(&g_user_hwcap2, hwcap2_this_cpu(), __ATOMIC_RELAXED);
}

uint64_t arch_user_hwcap(void)
{
    uint64_t h = __atomic_load_n(&g_user_hwcap, __ATOMIC_RELAXED);
    return h == ~0ull ? hwcap_this_cpu() : h;
}

uint64_t arch_user_hwcap2(void)
{
    uint64_t h = __atomic_load_n(&g_user_hwcap2, __ATOMIC_RELAXED);
    return h == ~0ull ? hwcap2_this_cpu() : h;
}
#undef IDF
