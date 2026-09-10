/*
 * gicv3_vdist.h - The virtual GICv3 distributor a guest finds
 * (docs/kernel/arch/aarch64/design.md, "The guest's distributor").
 *
 * One per VM, in the kernel, beside the virtual CPU interface it feeds:
 * the distributor's output is a list-register write, which is EL2 state
 * an owner in userland cannot reach. A guest's GICD/GICR access is a
 * stage-2 fault that the EL2 backend hands here instead of to the owner.
 *
 * The layout is the hypervisor's to define -- these are the virtual
 * machine's addresses, and have nothing to do with where the host's own
 * GIC is. They match what QEMU's `virt` presents, which is what a stock
 * guest kernel's device tree already expects; the decoder here and the
 * device tree a guest is eventually handed both read them from this one
 * place. Redistributor frame `i` belongs to vCPU `i`, whose MPIDR is
 * Aff0 = i (hv_el2.c sets VMPIDR_EL2 to say so), and GICR_TYPER agrees,
 * so a driver that walks the frames for its own affinity finds it.
 */
#ifndef AARCH64_GICV3_VDIST_H
#define AARCH64_GICV3_VDIST_H

#include <kernel/types.h>

#define VDIST_GICD_BASE    0x08000000ull
#define VDIST_GICD_SIZE    0x10000ull
#define VDIST_GICR_BASE    0x080A0000ull
#define VDIST_GICR_STRIDE  0x20000ull          /* RD_base then SGI_base, 64 KiB each */
#define VDIST_GICR_FRAMES  4u                  /* COSMO_HV_VCPUS_MAX */
#define VDIST_NR_LINES     288u                /* 32 private + 256 SPIs: what virt has */

struct gicv3_vdist;

struct gicv3_vdist *vdist_create(void);
void vdist_destroy(struct gicv3_vdist *d);

/* Frame `i` exists while vCPU `i` does; the last present frame carries
 * GICR_TYPER.Last, which is how a guest knows where the walk ends. */
void vdist_vcpu_present(struct gicv3_vdist *d, unsigned i, bool present);

/* The MPIDR_EL1 vCPU `i` reads, and the affinity the distributor routes
 * by; the same value from both, or a guest's SGI to "the CPU whose MPIDR
 * I read" would go nowhere. */
uint64_t vdist_mpidr(unsigned i);

/*
 * A guest's data access at `gpa`, `size` bytes (1, 2, 4 or 8). True when
 * the address is inside the distributor's or a redistributor's window --
 * it has then been emulated: on a read `*val` holds the result, on a
 * write it held the value. False when the address is not this device's
 * at all, and the caller treats it as it always did. Every offset inside
 * a window is answered; the unimplemented ones read as zero and swallow
 * writes, because a stock driver touches registers this model has no
 * state for and a fault there would end the boot.
 */
bool vdist_mmio(struct gicv3_vdist *d, unsigned vcpu, uint64_t gpa, unsigned size, bool write,
                uint64_t *val);

/*
 * Routing, for the EL2 backend. The distributor decides; the vCPU's own
 * run thread places, into the one list register it owns.
 *
 * vdist_pending_for: the INTID vCPU `i` should be given next -- the
 *   highest-priority interrupt that is pending, enabled, in group 1 and
 *   routed to it, with the distributor and its redistributor both on --
 *   and that interrupt's priority; -1 when there is none.
 * vdist_deliverable: whether one particular INTID still meets all of
 *   that, so an interrupt placed but not yet taken can be withdrawn when
 *   the guest has since disabled or cleared it.
 * vdist_ack: the guest acknowledged `intid`: its pending state leaves the
 *   distributor, as it does in hardware.
 * vdist_raise_private: a PPI fired for vCPU `i` (its timer); it becomes
 *   pending in that redistributor and is forwarded when, and only when,
 *   the guest has enabled it there.
 */
int vdist_pending_for(struct gicv3_vdist *d, unsigned i, uint8_t *prio);
bool vdist_deliverable(struct gicv3_vdist *d, unsigned i, unsigned intid);
void vdist_ack(struct gicv3_vdist *d, unsigned i, unsigned intid);
void vdist_raise_private(struct gicv3_vdist *d, unsigned i, unsigned intid);

#endif /* AARCH64_GICV3_VDIST_H */
