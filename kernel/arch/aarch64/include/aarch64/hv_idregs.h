/*
 * hv_idregs.h - The feature registers a guest is told it has
 * (docs/kernel/arch/aarch64/design.md, "The features a guest is told").
 *
 * HCR_EL2.TID3 traps every ID-register read a guest makes to EL2. A
 * hand-written fixture reads one and the owner answers it; a real kernel
 * reads dozens before it finishes setting itself up, and each is a trap
 * the owner cannot answer with nothing. This model answers them in the
 * kernel: the value is the host's own, masked to what a guest may safely
 * see -- deny by default, so a field is present to the guest only when
 * the policy names it. A guest must never be told it has a feature the
 * hypervisor does not isolate (its own EL2, the host's pointer-auth keys,
 * a debug or SME unit the switch does not save).
 *
 * The whole ID space (Op0=3, Op1=0, CRn=0, CRm 1..7) is answered: the
 * registers below by policy, everything else as zero -- which is what the
 * architecture already requires of an unallocated ID register, and is
 * what makes this robust to a register a future kernel reads that this
 * list does not name.
 */
#ifndef AARCH64_HV_IDREGS_H
#define AARCH64_HV_IDREGS_H

#include <kernel/types.h>

/*
 * The value a guest reads for the ID register whose ISS encoding (Rt and
 * direction stripped, as SYSREG_ENC builds it) is `enc`. `*handled` is
 * true when `enc` is an ID register this answers -- the whole CRn=0
 * CRm 1..7 space -- and false otherwise, when the access is the owner's
 * as before.
 */
uint64_t hv_idreg_read(uint32_t enc, bool *handled);

/* The masks the policy keeps, exposed so a test can assert the model
 * applied them rather than trust a constant. A field named here is kept
 * from the host; everything else in the register is forced to zero. */
#define IDREG_PFR0_KEEP   0x0F0F00FFull   /* EL0, EL1, FP, AdvSIMD, GIC; EL2/EL3/RAS/SVE/MPAM zeroed */
#define IDREG_ISAR1_DROP  0xFF000FF0ull   /* GPI[31:28], GPA[27:24], API[11:8], APA[7:4]: pointer authentication */
#define IDREG_ISAR2_DROP  0x0000F000ull   /* APA3 */
#define IDREG_MMFR1_DROP  0x00000F00ull   /* VH: a guest does not run at EL2 */
#define IDREG_DFR0_VALUE  0x0000000000000006ull   /* DebugVer 6 (the architectural minimum), nothing else */

#endif /* AARCH64_HV_IDREGS_H */
