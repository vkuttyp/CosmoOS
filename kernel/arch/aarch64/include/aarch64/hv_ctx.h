/*
 * hv_ctx.h - The per-vCPU context the EL2 world switch reads and writes
 * (docs/kernel-services/virtualization/design.md, "The AArch64 EL2
 * backend").
 *
 * One page, addressed physically: the EL2 code runs with its MMU off, as
 * the loader's stub does. C and hv_el2_switch.S must agree on every offset, so
 * the offsets are named here once and checked by static assertions.
 */

#ifndef AARCH64_HV_CTX_H
#define AARCH64_HV_CTX_H

/* --- offsets (used by hv_el2.S; keep in step with struct hv_ctx) --- */

#define HV_CTX_GUEST_X      0x000   /* x0..x30, 31 * 8 */
#define HV_CTX_GUEST_SP_EL1 0x0F8
#define HV_CTX_GUEST_PC     0x100
#define HV_CTX_GUEST_PSTATE 0x108
#define HV_CTX_GUEST_SYS    0x110   /* the EL1 system registers, HV_CTX_SYS_COUNT of them */
#define HV_CTX_HOST_SYS     0x1B0   /* the host's, same order */
#define HV_CTX_HOST_X       0x250   /* x19..x30 and sp: 13 * 8 */
#define HV_CTX_HOST_ELR     0x2B8
#define HV_CTX_HOST_SPSR    0x2C0
#define HV_CTX_VTTBR        0x2C8
#define HV_CTX_VTCR         0x2D0
#define HV_CTX_HCR          0x2D8
#define HV_CTX_EXIT_ESR     0x2E0
#define HV_CTX_EXIT_FAR     0x2E8
#define HV_CTX_EXIT_HPFAR   0x2F0
#define HV_CTX_EXIT_KIND    0x2F8   /* 0 sync, 1 IRQ, 2 FIQ, 3 SError */
#define HV_CTX_HOST_SP_EL2  0x300   /* SP_EL2 at the HVC: the per-CPU EL2 stack, put back on exit */
#define HV_CTX_HOST_X18     0x308   /* the host's x18: reserved by the ABI, so the switch keeps it */

/*
 * The virtual GIC. `ICH_*_EL2` cannot be reached from EL1, so a guest's
 * interrupt state lives here and the switch moves it: written on entry,
 * read back on exit. `vgic_on` is what says whether to touch those
 * registers at all -- on a machine without a GICv3 virtual interface
 * they do not exist and the accesses are UNDEFINED.
 *
 * One list register. The generic layer offers one vector at a time, so
 * a second would carry nothing; `ICH_VTR_EL2`'s count is recorded at
 * probe and the occupancy is measured before more assembly is written
 * for them.
 */
#define HV_CTX_VGIC_ON      0x310   /* nonzero: the switch moves the fields below */
#define HV_CTX_VGIC_HCR     0x318   /* ICH_HCR_EL2 */
#define HV_CTX_VGIC_VMCR    0x320   /* ICH_VMCR_EL2: the guest's own PMR, BPR and group enables */
#define HV_CTX_VGIC_LR0     0x328   /* ICH_LR0_EL2 */
#define HV_CTX_VGIC_ELRSR   0x330   /* read back: which list registers are free */
#define HV_CTX_VGIC_MISR    0x338   /* read back: why maintenance would have fired */
/*
 * The active priorities. A guest that has acknowledged an interrupt and
 * not completed it is *running at* that priority, and the fact lives in
 * ICH_AP<n>R<m>_EL2 -- EL2 registers, shared by every guest on the CPU.
 * Left unsaved, a vCPU destroyed inside its handler leaves its priority
 * active and the next guest on that CPU is refused every interrupt that
 * does not outrank it. One register of each group, which covers an
 * implementation with five priority bits; probe warns above that.
 */
#define HV_CTX_VGIC_AP0R0   0x340
#define HV_CTX_VGIC_AP1R0   0x348

/* The EL1 system registers the switch moves, in this order. */
#define HV_CTX_SYS_COUNT 20

/* The HVC selectors are in arch/el2.h, next to the stub's: generic code
 * asks who owns EL2, and this header is private to the switch. */
#include <arch/el2.h>

#ifndef __ASSEMBLER__

#include <kernel/types.h>

/* The EL1 system registers a guest owns, saved and restored around every
 * entry. The order is the assembly's. */
struct hv_sysregs {
    uint64_t sctlr, ttbr0, ttbr1, tcr, mair, amair, vbar, esr, far, elr;
    uint64_t spsr, sp_el0, tpidr_el0, tpidrro_el0, tpidr_el1, contextidr;
    uint64_t cpacr, par, mdscr, cntkctl;
};

struct hv_ctx {
    uint64_t guest_x[31];
    uint64_t guest_sp_el1;
    uint64_t guest_pc;
    uint64_t guest_pstate;
    struct hv_sysregs guest;
    struct hv_sysregs host;
    uint64_t host_x[13];       /* x19..x30, then sp */
    uint64_t host_elr, host_spsr;
    uint64_t vttbr, vtcr, hcr;
    uint64_t exit_esr, exit_far, exit_hpfar, exit_kind;
    uint64_t host_sp_el2;
    uint64_t host_x18;
    uint64_t vgic_on;
    uint64_t vgic_hcr, vgic_vmcr, vgic_lr0;
    uint64_t vgic_elrsr, vgic_misr;
    uint64_t vgic_ap0r0, vgic_ap1r0;
};

_Static_assert(sizeof(struct hv_sysregs) == HV_CTX_SYS_COUNT * 8, "hv_sysregs order");
_Static_assert(__builtin_offsetof(struct hv_ctx, guest_x) == HV_CTX_GUEST_X, "ctx guest_x");
_Static_assert(__builtin_offsetof(struct hv_ctx, guest_sp_el1) == HV_CTX_GUEST_SP_EL1, "ctx sp_el1");
_Static_assert(__builtin_offsetof(struct hv_ctx, guest_pc) == HV_CTX_GUEST_PC, "ctx pc");
_Static_assert(__builtin_offsetof(struct hv_ctx, guest_pstate) == HV_CTX_GUEST_PSTATE, "ctx pstate");
_Static_assert(__builtin_offsetof(struct hv_ctx, guest) == HV_CTX_GUEST_SYS, "ctx guest sys");
_Static_assert(__builtin_offsetof(struct hv_ctx, host) == HV_CTX_HOST_SYS, "ctx host sys");
_Static_assert(__builtin_offsetof(struct hv_ctx, host_x) == HV_CTX_HOST_X, "ctx host x");
_Static_assert(__builtin_offsetof(struct hv_ctx, host_elr) == HV_CTX_HOST_ELR, "ctx host elr");
_Static_assert(__builtin_offsetof(struct hv_ctx, host_spsr) == HV_CTX_HOST_SPSR, "ctx host spsr");
_Static_assert(__builtin_offsetof(struct hv_ctx, vttbr) == HV_CTX_VTTBR, "ctx vttbr");
_Static_assert(__builtin_offsetof(struct hv_ctx, vtcr) == HV_CTX_VTCR, "ctx vtcr");
_Static_assert(__builtin_offsetof(struct hv_ctx, hcr) == HV_CTX_HCR, "ctx hcr");
_Static_assert(__builtin_offsetof(struct hv_ctx, exit_esr) == HV_CTX_EXIT_ESR, "ctx esr");
_Static_assert(__builtin_offsetof(struct hv_ctx, exit_far) == HV_CTX_EXIT_FAR, "ctx far");
_Static_assert(__builtin_offsetof(struct hv_ctx, exit_hpfar) == HV_CTX_EXIT_HPFAR, "ctx hpfar");
_Static_assert(__builtin_offsetof(struct hv_ctx, exit_kind) == HV_CTX_EXIT_KIND, "ctx kind");
_Static_assert(__builtin_offsetof(struct hv_ctx, host_sp_el2) == HV_CTX_HOST_SP_EL2, "ctx host sp_el2");
_Static_assert(__builtin_offsetof(struct hv_ctx, host_x18) == HV_CTX_HOST_X18, "ctx host x18");
_Static_assert(__builtin_offsetof(struct hv_ctx, vgic_on) == HV_CTX_VGIC_ON, "ctx vgic on");
_Static_assert(__builtin_offsetof(struct hv_ctx, vgic_hcr) == HV_CTX_VGIC_HCR, "ctx vgic hcr");
_Static_assert(__builtin_offsetof(struct hv_ctx, vgic_vmcr) == HV_CTX_VGIC_VMCR, "ctx vgic vmcr");
_Static_assert(__builtin_offsetof(struct hv_ctx, vgic_lr0) == HV_CTX_VGIC_LR0, "ctx vgic lr0");
_Static_assert(__builtin_offsetof(struct hv_ctx, vgic_elrsr) == HV_CTX_VGIC_ELRSR, "ctx vgic elrsr");
_Static_assert(__builtin_offsetof(struct hv_ctx, vgic_misr) == HV_CTX_VGIC_MISR, "ctx vgic misr");
_Static_assert(__builtin_offsetof(struct hv_ctx, vgic_ap0r0) == HV_CTX_VGIC_AP0R0, "ctx vgic ap0r0");
_Static_assert(__builtin_offsetof(struct hv_ctx, vgic_ap1r0) == HV_CTX_VGIC_AP1R0, "ctx vgic ap1r0");
_Static_assert(sizeof(struct hv_ctx) <= 4096, "the context is one page");

/* The EL2 vector table this backend installs through el2_set_vectors. */
extern const uint8_t hv_el2_vectors[];
extern const uint8_t hv_el2_vectors_end[];

#endif /* __ASSEMBLER__ */

#endif /* AARCH64_HV_CTX_H */
