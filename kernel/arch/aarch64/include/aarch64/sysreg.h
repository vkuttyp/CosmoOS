/*
 * aarch64/sysreg.h - System registers, barriers, PSTATE and the
 * architectural constants the backend uses (docs/kernel/arch/aarch64/).
 * Private to kernel/arch/aarch64/.
 */

#ifndef AARCH64_SYSREG_H
#define AARCH64_SYSREG_H

#include <stdint.h>

#define READ_SYSREG(name)                                        \
    ({                                                           \
        uint64_t v_;                                             \
        __asm__ volatile("mrs %0, " #name : "=r"(v_));           \
        v_;                                                      \
    })
#define WRITE_SYSREG(name, v)                                    \
    do {                                                         \
        uint64_t v_ = (uint64_t)(v);                             \
        __asm__ volatile("msr " #name ", %0" : : "r"(v_) : "memory"); \
    } while (0)

static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void dsb_ish(void) { __asm__ volatile("dsb ish" ::: "memory"); }
static inline void dsb_ishst(void) { __asm__ volatile("dsb ishst" ::: "memory"); }
static inline void dmb_sy(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void wfi(void) { __asm__ volatile("wfi" ::: "memory"); }
static inline void wfe(void) { __asm__ volatile("wfe" ::: "memory"); }
static inline void sev(void) { __asm__ volatile("sev" ::: "memory"); }
static inline void yield_hint(void) { __asm__ volatile("yield" ::: "memory"); }

/* PSTATE.DAIF */
#define DAIF_D (1ull << 9)
#define DAIF_A (1ull << 8)
#define DAIF_I (1ull << 7)
#define DAIF_F (1ull << 6)

/* SPSR_EL1 */
#define SPSR_M_EL0T 0x0ull
#define SPSR_M_EL1T 0x4ull
#define SPSR_M_EL1H 0x5ull
#define SPSR_M_MASK 0xFull
#define SPSR_M_AARCH32 (1ull << 4)

/* SCTLR_EL1 */
#define SCTLR_M   (1ull << 0)
#define SCTLR_A   (1ull << 1)
#define SCTLR_C   (1ull << 2)
#define SCTLR_SA  (1ull << 3)
#define SCTLR_SA0 (1ull << 4)
#define SCTLR_I   (1ull << 12)
#define SCTLR_WXN (1ull << 19)
#define SCTLR_SPAN (1ull << 23)   /* set: PAN is not set automatically on exception entry */
#define SCTLR_RES1 ((1ull << 11) | (1ull << 20) | (1ull << 22) | (1ull << 28) | (1ull << 29))

/* TCR_EL1 */
#define TCR_T0SZ(x) ((uint64_t)(x) << 0)
#define TCR_T1SZ(x) ((uint64_t)(x) << 16)
#define TCR_IRGN0_WBWA (1ull << 8)
#define TCR_ORGN0_WBWA (1ull << 10)
#define TCR_SH0_INNER  (3ull << 12)
#define TCR_TG0_4K     (0ull << 14)
#define TCR_IRGN1_WBWA (1ull << 24)
#define TCR_ORGN1_WBWA (1ull << 26)
#define TCR_SH1_INNER  (3ull << 28)
#define TCR_TG1_4K     (2ull << 30)
#define TCR_IPS(x)     ((uint64_t)(x) << 32)
#define TCR_AS         (1ull << 36)
#define TCR_TBI0       (1ull << 37)

/* MAIR_EL1 attribute indices used by mmu.c and the loader alike. */
#define MAIR_IDX_NORMAL_WB 0u
#define MAIR_IDX_DEVICE    1u
#define MAIR_IDX_NORMAL_NC 2u
#define MAIR_ATTR_NORMAL_WB 0xFFull   /* inner+outer write-back, read/write allocate */
#define MAIR_ATTR_DEVICE    0x00ull   /* device nGnRnE */
#define MAIR_ATTR_NORMAL_NC 0x44ull   /* inner+outer non-cacheable */
#define MAIR_VALUE ((MAIR_ATTR_NORMAL_WB << (8 * MAIR_IDX_NORMAL_WB)) | \
                    (MAIR_ATTR_DEVICE << (8 * MAIR_IDX_DEVICE)) |       \
                    (MAIR_ATTR_NORMAL_NC << (8 * MAIR_IDX_NORMAL_NC)))

/* Translation table descriptors (4 KiB granule, 48-bit). */
#define DESC_VALID      (1ull << 0)
#define DESC_TABLE      (1ull << 1)   /* levels 0-2: table (set) or block (clear) */
#define DESC_PAGE       (1ull << 1)   /* level 3: page descriptor must have bit 1 set */
#define DESC_ATTRIDX(i) ((uint64_t)(i) << 2)
#define DESC_ATTRIDX_MASK (7ull << 2)
#define DESC_NS         (1ull << 5)
#define DESC_AP_RW_EL1  (0ull << 6)   /* EL1 RW, EL0 none */
#define DESC_AP_RW_ALL  (1ull << 6)   /* EL1 RW, EL0 RW */
#define DESC_AP_RO_EL1  (2ull << 6)   /* EL1 RO, EL0 none */
#define DESC_AP_RO_ALL  (3ull << 6)   /* EL1 RO, EL0 RO */
#define DESC_AP_MASK    (3ull << 6)
#define DESC_AP_USER    (1ull << 6)
#define DESC_AP_RO      (1ull << 7)
#define DESC_SH_INNER   (3ull << 8)
#define DESC_AF         (1ull << 10)
#define DESC_NG         (1ull << 11)
#define DESC_CONTIG     (1ull << 52)
#define DESC_PXN        (1ull << 53)
#define DESC_UXN        (1ull << 54)
#define DESC_ADDR_MASK  0x0000FFFFFFFFF000ull

/* ESR_EL1 */
#define ESR_EC_SHIFT 26
#define ESR_EC(esr) ((unsigned)(((esr) >> ESR_EC_SHIFT) & 0x3F))
#define ESR_IL      (1ull << 25)
#define ESR_ISS(esr) ((unsigned)((esr) & 0x1FFFFFF))
#define ESR_EC_UNKNOWN      0x00u
#define ESR_EC_WFX          0x01u
#define ESR_EC_FP_ACCESS    0x07u
#define ESR_EC_ILLEGAL      0x0Eu
#define ESR_EC_SVC64        0x15u
#define ESR_EC_HVC64        0x16u
#define ESR_EC_SMC64        0x17u
#define ESR_EC_SYSREG       0x18u
#define ESR_EC_IABT_LOWER   0x20u
#define ESR_EC_IABT_CUR     0x21u
#define ESR_EC_PC_ALIGN     0x22u
#define ESR_EC_DABT_LOWER   0x24u
#define ESR_EC_DABT_CUR     0x25u
#define ESR_EC_SP_ALIGN     0x26u
#define ESR_EC_SERROR       0x2Fu
#define ESR_EC_BREAKPT_LOWER 0x30u
#define ESR_EC_BREAKPT_CUR  0x31u
#define ESR_EC_SSTEP_LOWER  0x32u
#define ESR_EC_SSTEP_CUR    0x33u
#define ESR_EC_WATCHPT_LOWER 0x34u
#define ESR_EC_WATCHPT_CUR  0x35u
#define ESR_EC_BRK64        0x3Cu
/* data/instruction abort ISS */
#define ESR_ISS_WNR   (1u << 6)
#define ESR_ISS_FSC(iss) ((iss) & 0x3F)
#define FSC_TRANSLATION(fsc) (((fsc) & 0x3C) == 0x04)   /* 0b0001LL */
#define FSC_ACCESS_FLAG(fsc) (((fsc) & 0x3C) == 0x08)   /* 0b0010LL */
#define FSC_PERMISSION(fsc)  (((fsc) & 0x3C) == 0x0C)   /* 0b0011LL */

/* CNTP/CNTV control */
#define CNT_CTL_ENABLE  (1ull << 0)
#define CNT_CTL_IMASK   (1ull << 1)
#define CNT_CTL_ISTATUS (1ull << 2)

/* ID registers */
#define ID_AA64MMFR1_PAN(v) (((v) >> 20) & 0xF)
#define ID_AA64MMFR0_PARANGE(v) ((v) & 0xF)
#define ID_AA64PFR0_GIC(v) (((v) >> 24) & 0xF)

/* MPIDR affinity fields (Aff0..Aff2 in bits 0-23, Aff3 in 32-39). */
#define MPIDR_AFFINITY(v) (((v) & 0xFFFFFFull) | (((v) >> 8) & 0xFF000000ull))

static inline unsigned current_el(void)
{
    return (unsigned)((READ_SYSREG(CurrentEL) >> 2) & 3);
}

/* TLB maintenance, inner-shareable (broadcast). */
static inline void tlbi_vmalle1is(void)
{
    __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory");
}

static inline void tlbi_vaae1is(uint64_t va)
{
    __asm__ volatile("tlbi vaae1is, %0" : : "r"(va >> 12) : "memory");
}

#endif /* AARCH64_SYSREG_H */
