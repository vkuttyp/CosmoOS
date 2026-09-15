/*
 * hv_s2_core.h - The stage-2 walk's starting level, from the physical
 * address range (docs/kernel-services/virtualization/design.md, "The
 * AArch64 EL2 backend"). Pure: the host test (tests/host/test_hv_s2.c)
 * includes it as is, and the SMMU driver asks it the same question.
 *
 * With the 4 KiB granule the architecture allows a stage-2 walk to
 * start at level 0 (VTCR_EL2.SL0 = 2) only when the physical address
 * size exceeds 42 bits; below that the walk starts at level 1
 * (SL0 = 1), and an input range wider than the 39 bits one level-1
 * table covers is served by concatenated level-1 tables: 2, 4 or 8
 * contiguous pages, aligned to their own size, indexed as one table.
 * A cortex-a72 reports 44 bits and starts at level 0; a cortex-a76
 * reports 40 and must start at level 1 from two pages, or every walk
 * faults at its first step (ESR 0x82000004 on the guest's first
 * instruction: the fault the hardening unit's second boot found).
 */

#ifndef ARCH_HV_S2_CORE_H
#define ARCH_HV_S2_CORE_H

struct hv_s2_layout {
    unsigned start_level;   /* the walk's own numbering: 3 = a level-0 start, 2 = level-1 */
    unsigned root_order;    /* log2 of the root's page count: 0..3 */
    unsigned sl0;           /* the VTCR_EL2.SL0 / STE.S2SL0 encoding: 2 (level 0) or 1 (level 1) */
};

static inline void hv_s2_layout(unsigned pa_bits, struct hv_s2_layout *out)
{
    if (pa_bits > 42) {
        out->start_level = 3;
        out->root_order = 0;
        out->sl0 = 2;
        return;
    }
    out->start_level = 2;
    out->sl0 = 1;
    out->root_order = pa_bits > 39 ? pa_bits - 39 : 0;
}

#endif /* ARCH_HV_S2_CORE_H */
