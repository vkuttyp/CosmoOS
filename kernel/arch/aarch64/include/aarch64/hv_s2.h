/*
 * hv_s2.h - Stage-2 translation tables for an EL2 guest
 * (docs/kernel-services/virtualization/design.md, "The AArch64 EL2
 * backend"). The same interface as npt_* and ept_*, in this
 * architecture's descriptors.
 */

#ifndef AARCH64_HV_S2_H
#define AARCH64_HV_S2_H

#include <kernel/types.h>

/* Bits 47:12 of a descriptor address a 4 KiB-aligned frame. */
#define S2_ADDR_MASK 0x0000FFFFFFFFF000ull

paddr_t hv_s2_create(void);
void hv_s2_destroy(paddr_t root);
/* 4 KiB granular with 2 MiB blocks where the addresses and the length
 * allow; `prot` is HV_MAP_* and must be nonzero. -EINVAL/-ENOMEM/-EEXIST.
 * The caller invalidates: these tables cache in every CPU's TLB. */
int hv_s2_map(paddr_t root, uint64_t ipa, paddr_t pa, size_t len, unsigned prot);
int hv_s2_unmap(paddr_t root, uint64_t ipa, size_t len);
bool hv_s2_query(paddr_t root, uint64_t ipa, paddr_t *pa);
unsigned hv_s2_table_pages(paddr_t root);

/* VTCR_EL2 for a four-level walk over `pa_bits` of output address,
 * with `ps_field` the PS encoding for that size. */
uint64_t hv_s2_vtcr(unsigned pa_bits, unsigned ps_field);

#endif /* AARCH64_HV_S2_H */
