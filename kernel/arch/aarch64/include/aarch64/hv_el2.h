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
bool el2_vcpu_timer_state(struct arch_hv_vcpu *v, uint64_t *ctl, uint64_t *cntvoff);
/* The host's CNTV_CTL as the last exit left it, read with interrupts off. */
uint64_t el2_vcpu_host_cntv_after(struct arch_hv_vcpu *v);
/* The INTID a guest's virtual timer raises, 0 when the backend could not bind it. */
unsigned el2_guest_timer_intid(void);
/* Whether the guest's own distributor holds an interrupt this vCPU can take. */
bool el2_vcpu_irq_waiting(struct arch_hv_vcpu *v);

#endif /* AARCH64_HV_EL2_H */
