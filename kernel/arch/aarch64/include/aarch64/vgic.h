/*
 * vgic.h - Whether this machine can give a guest an interrupt
 * (kernel/arch/aarch64/hv_el2.c).
 *
 * A guest is interrupted by a *virtual* interrupt placed in one of the
 * GIC's list registers; the guest's own `ICC_*_EL1` accesses are then
 * redirected by hardware to the virtual CPU interface, so it
 * acknowledges and completes exactly as the host does on the physical
 * one. Two facts decide whether any of that is possible, and both are
 * EL2's to answer:
 *
 *   - the machine has a GICv3 CPU interface (a GICv2 virtualises
 *     through MMIO frames this kernel does not drive), and
 *   - EL2 belongs to this kernel, so the switch can program the
 *     registers -- `ICH_*_EL2` cannot be touched from EL1 at all.
 *
 * Implemented next to the world switch rather than next to the GIC
 * driver, because the answer comes back from an HVC and the state it
 * describes is per-vCPU.
 */

#ifndef AARCH64_VGIC_H
#define AARCH64_VGIC_H

#include <kernel/compiler.h>

/* True when a virtual interrupt can be delivered at all. */
bool aarch64_vgic_available(void);

/* How many can be in flight at once (ICH_VTR_EL2), 0 when none can. */
unsigned aarch64_vgic_lr_count(void);

#endif /* AARCH64_VGIC_H */
