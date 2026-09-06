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
#define HV_EL2_VERSION       1

#ifndef __ASSEMBLER__
/* For tests: the raw call, including selectors the stub refuses. */
int64_t el2_call_raw(uint64_t selector, uint64_t arg);
#endif

#endif /* ARCH_EL2_H */
