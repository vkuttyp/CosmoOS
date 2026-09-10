/*
 * hv_idregs.c - The feature registers a guest is told it has
 * (kernel/arch/aarch64/include/aarch64/hv_idregs.h).
 */
#include <aarch64/hv_idregs.h>
#include <aarch64/sysreg.h>

/* The ID feature space: Op0=3, Op1=0, CRn=0, CRm 1..7. SYSREG_ENC packs
 * Op0 at 20, Op2 at 17, Op1 at 14, CRn at 10, CRm at 1. */
#define ENC(op0, op1, crn, crm, op2) \
    (((uint32_t)(op0) << 20) | ((uint32_t)(op2) << 17) | ((uint32_t)(op1) << 14) | \
     ((uint32_t)(crn) << 10) | ((uint32_t)(crm) << 1))

static bool in_id_space(uint32_t enc, unsigned *crm, unsigned *op2)
{
    unsigned op0 = (enc >> 20) & 3, op1 = (enc >> 14) & 7, crn = (enc >> 10) & 0xF;
    *op2 = (enc >> 17) & 7;
    *crm = (enc >> 1) & 0xF;
    return op0 == 3 && op1 == 0 && crn == 0 && *crm >= 1 && *crm <= 7;
}

/*
 * The host's value for a register in the ID space, read by its encoding
 * (the S-name syntax, so this does not depend on the assembler knowing a
 * register's mnemonic). Only the registers the policy shapes are read; the
 * rest of the space never touches hardware and is zero.
 */
static uint64_t host_idreg(unsigned crm, unsigned op2)
{
    switch ((crm << 3) | op2) {
    case (4 << 3) | 0: return READ_SYSREG(S3_0_c0_c4_0);   /* ID_AA64PFR0_EL1 */
    case (4 << 3) | 1: return READ_SYSREG(S3_0_c0_c4_1);   /* ID_AA64PFR1_EL1 */
    case (5 << 3) | 0: return READ_SYSREG(S3_0_c0_c5_0);   /* ID_AA64DFR0_EL1 */
    case (6 << 3) | 0: return READ_SYSREG(S3_0_c0_c6_0);   /* ID_AA64ISAR0_EL1 */
    case (6 << 3) | 1: return READ_SYSREG(S3_0_c0_c6_1);   /* ID_AA64ISAR1_EL1 */
    case (6 << 3) | 2: return READ_SYSREG(S3_0_c0_c6_2);   /* ID_AA64ISAR2_EL1 */
    case (7 << 3) | 0: return READ_SYSREG(S3_0_c0_c7_0);   /* ID_AA64MMFR0_EL1 */
    case (7 << 3) | 1: return READ_SYSREG(S3_0_c0_c7_1);   /* ID_AA64MMFR1_EL1 */
    case (7 << 3) | 2: return READ_SYSREG(S3_0_c0_c7_2);   /* ID_AA64MMFR2_EL1 */
    default: return 0;
    }
}

uint64_t hv_idreg_read(uint32_t enc, bool *handled)
{
    unsigned crm, op2;
    if (!in_id_space(enc, &crm, &op2)) {
        *handled = false;
        return 0;
    }
    *handled = true;
    uint64_t host = host_idreg(crm, op2);
    switch ((crm << 3) | op2) {
    case (4 << 3) | 0:   /* ID_AA64PFR0: keep EL0/EL1/FP/AdvSIMD/GIC; hide EL2, EL3, RAS, SVE, MPAM */
        return host & IDREG_PFR0_KEEP;
    case (4 << 3) | 1:   /* ID_AA64PFR1: BT, SSBS, MTE -- none this hypervisor virtualises */
        return 0;
    case (5 << 3) | 0:   /* ID_AA64DFR0: a minimal debug architecture, no PMU, no breakpoints modelled */
        return IDREG_DFR0_VALUE;
    case (6 << 3) | 0:   /* ID_AA64ISAR0: instruction attributes -- the CPU's own, safe to expose */
        return host;
    case (6 << 3) | 1:   /* ID_AA64ISAR1: minus pointer authentication (the guest would use the host's keys) */
        return host & ~IDREG_ISAR1_DROP;
    case (6 << 3) | 2:   /* ID_AA64ISAR2: minus its pointer-auth field */
        return host & ~IDREG_ISAR2_DROP;
    case (7 << 3) | 0:   /* ID_AA64MMFR0: PARange, ASIDBits, granules -- the guest's own MMU uses these; the truth */
        return host;
    case (7 << 3) | 1:   /* ID_AA64MMFR1: minus VH (a guest does not run at EL2) */
        return host & ~IDREG_MMFR1_DROP;
    case (7 << 3) | 2:   /* ID_AA64MMFR2: memory model features the guest's MMU uses */
        return host;
    default:
        return 0;         /* the rest of the ID space: RAZ, as the architecture requires */
    }
}
