/*
 * hvseg.h - Translation between the UAPI's neutral segment attributes
 * and each backend's control-block packing
 * (docs/kernel-services/virtualization/design.md, "Neutral segment
 * attributes"). Pure functions: the host test covers them.
 */

#ifndef X86_HVSEG_H
#define X86_HVSEG_H

#include <kernel/types.h>
#include <uapi/cosmo/syscall.h>

/* The VMCB packs AVL/L/DB/G at bits 8-11 and has no unusable bit: a
 * segment nothing is loaded into is written as not present. */
static inline uint16_t hv_seg_to_svm(uint16_t neutral)
{
    uint16_t v = (uint16_t)((neutral & 0x00FFu) | ((neutral >> 4) & 0x0F00u));
    if (neutral & COSMO_SEG_UNUSABLE)
        v &= (uint16_t)~COSMO_SEG_P;
    return v;
}

/* The inverse. A not-present segment with a null selector is how the
 * VMCB spells "unusable", which is what the guest was given. */
static inline uint16_t hv_seg_from_svm(uint16_t vmcb, uint16_t selector)
{
    uint16_t n = (uint16_t)((vmcb & 0x00FFu) | ((vmcb & 0x0F00u) << 4));
    if (!(vmcb & COSMO_SEG_P) && selector == 0)
        n |= COSMO_SEG_UNUSABLE;
    return n;
}

/* VMX access rights are the descriptor layout with unusable at bit 16
 * and bits 8-11 reserved. */
static inline uint32_t hv_seg_to_vmx(uint16_t neutral)
{
    uint32_t ar = (uint32_t)(neutral & 0xF0FFu);
    if (neutral & COSMO_SEG_UNUSABLE)
        ar |= 1u << 16;
    return ar;
}

static inline uint16_t hv_seg_from_vmx(uint32_t ar)
{
    uint16_t n = (uint16_t)(ar & 0xF0FFu);
    if (ar & (1u << 16))
        n |= COSMO_SEG_UNUSABLE;
    return n;
}

/* Reserved bits must be zero; an unusable segment may not also claim to
 * be present. */
static inline bool hv_seg_attrib_valid(uint16_t neutral)
{
    if (neutral & COSMO_SEG_RESERVED)
        return false;
    if ((neutral & COSMO_SEG_UNUSABLE) && (neutral & COSMO_SEG_P))
        return false;
    return true;
}

#endif /* X86_HVSEG_H */
