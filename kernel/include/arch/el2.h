/*
 * arch/el2.h - The EL2 stub the loader leaves behind, from the kernel's
 * side (docs/kernel/arch/aarch64/design.md, "Exception level 2").
 *
 * AArch64 only; the header exists everywhere so generic code can guard
 * with ARCH_AARCH64 rather than with an include.
 */

#ifndef ARCH_EL2_H
#define ARCH_EL2_H

/* The constants are usable from assembly (hv_el2_switch.S includes this
 * through hv_ctx.h); the declarations are not. */
#ifndef __ASSEMBLER__
#include <kernel/types.h>

struct cosmoboot_info;
#endif

/* The stub's HVC selectors, matching boot/uefi/arch/aarch64/el2_stub.S. */
#define EL2_STUB_VERSION_CALL 0u
#define EL2_STUB_SET_VECTORS  1u
#define EL2_STUB_RESTORE      2u
#define EL2_STUB_SET_STACK    3u
#define EL2_STUB_VERSION      2

/* Once, early in boot: ask the stub for its version and remember whether
 * this machine has an EL2 the kernel can use. Never fails; a machine
 * without EL2 simply reports none. */
#ifndef __ASSEMBLER__
void el2_init(const struct cosmoboot_info *info);

bool el2_available(void);
/* The stub's physical address, 0 when there is no EL2. */
uint64_t el2_stub_phys(void);

/* Point VBAR_EL2 at `vbar_phys` (physical: the EL2 MMU is off), which is
 * how a hypervisor backend takes EL2 over; 0 on success, -1 without EL2.
 * The restore call puts the stub's own vectors back. */
int el2_set_vectors(uint64_t vbar_phys);
int el2_restore_stub_vectors(void);
/* EL2 needs a stack of its own before a vector table that uses one:
 * `sp_phys` is the top of it, physical like everything else at EL2. */
int el2_set_stack(uint64_t sp_phys);
#endif

/* The selectors the EL2 world switch answers once a hypervisor backend
 * has taken the vectors over (kernel/arch/aarch64/hv_el2_switch.S). They
 * live here rather than with the switch's private layout because a test
 * asks who owns EL2. */
#define HV_EL2_CALL_RUN      0x10
#define HV_EL2_CALL_VERSION  0x11
/* x1 = VTTBR value (stage-2 root | VMID): invalidate everything cached
 * for that VMID, inner-shareable, and return. Only EL2 can name a VMID,
 * which is why this is a call and not an instruction the host runs. */
#define HV_EL2_CALL_TLBI     0x12
/* Prepare the GIC's virtual interface and report it: sets
 * ICC_SRE_EL2.{SRE,Enable} so EL1 -- the host, and a guest under it --
 * may use the system-register interface at all, then returns
 * ICH_VTR_EL2. Both are EL2-only registers, which is the whole reason
 * the virtual GIC needs a call here rather than a driver at EL1. Must
 * not be issued on a machine without a GICv3 CPU interface: the
 * registers do not exist there and the read is UNDEFINED. */
#define HV_EL2_CALL_VGIC     0x13
/* x1 = the physical base of the loader's stub vectors: install them as
 * VBAR_EL2 and return 0. The switch owns EL2 from the moment it is
 * installed, so only it can give EL2 back; the stub's own set-vectors
 * call is gone by then. The disable path (arch_hv_disable) uses it,
 * after which the stub's ABI answers again and a later probe installs
 * the switch afresh. */
#define HV_EL2_CALL_HANDBACK 0x14
/* x1 = the syndrome to deliver, or 0 for none: set HCR_EL2.VSE so a
 * virtual SError is taken at EL1, and when x1 is non-zero put it in
 * VSESR_EL2 first. Only EL2 can do either, which is why a test that
 * wants a deterministic asynchronous abort needs a call here.
 *
 * x1 MUST be 0 on a CPU without FEAT_RAS: VSESR_EL2 does not exist
 * there and writing it is UNDEFINED. The caller checks
 * ID_AA64PFR0_EL1.RAS, because EL2 cannot refuse what it cannot read
 * (invariant I-ARCH-16, docs/audit/next-subsystem-async-error.md). */
#define HV_EL2_CALL_VSE      0x15
/* Clear HCR_EL2.{VSE,AMO} again. VSE is not self-clearing: while it is
 * set the virtual SError is pending and is re-taken every time EL1
 * returns with it unmasked, so the injector must take it back. */
#define HV_EL2_CALL_VSE_CLEAR 0x16
#define HV_EL2_VERSION       4

#ifndef __ASSEMBLER__
/* For tests: the raw call, including selectors the stub refuses. */
int64_t el2_call_raw(uint64_t selector, uint64_t arg);
/* The switch's version from EL2 on the calling CPU, its vectors installed
 * there first (kernel/arch/aarch64/hv_el2.c): the answer a test may check
 * from any CPU, where a bare `el2_call_raw` reaches the stub on a CPU the
 * run loop has not readied. -1 when EL2 cannot be readied here. */
int64_t arch_hv_el2_version_here(void);
#endif

#endif /* ARCH_EL2_H */
