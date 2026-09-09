/*
 * hv_el2.h - What the EL2 backend exposes to the architecture's own
 * hypervisor glue (kernel/arch/aarch64/hv_el2.c).
 *
 * Everything the generic layer uses goes through `struct hv_backend`.
 * This is for the one thing that is not a backend operation because
 * only this architecture has it: the guest interrupt state the world
 * switch moves in and out of `struct hv_ctx`.
 */

#ifndef AARCH64_HV_EL2_H
#define AARCH64_HV_EL2_H

#include <kernel/compiler.h>

struct arch_hv_vcpu;

/* List register 0 and the free-register mask as the last run left them.
 * False when this machine has no virtual GIC. */
bool el2_vcpu_vgic_state(struct arch_hv_vcpu *v, uint64_t *lr0, uint64_t *elrsr);

#endif /* AARCH64_HV_EL2_H */
