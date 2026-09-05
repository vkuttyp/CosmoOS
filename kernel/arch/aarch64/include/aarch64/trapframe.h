/*
 * aarch64/trapframe.h - The exception frame vectors.S saves
 * (docs/kernel/arch/aarch64/design.md, "Vector table and frame").
 * Offsets are assembly ABI: keep them in step with vectors.S.
 */

#ifndef AARCH64_TRAPFRAME_H
#define AARCH64_TRAPFRAME_H

#ifndef __ASSEMBLER__
#include <stdint.h>

struct arch_trap_frame {
    uint64_t x[31];      /* 0x000: x0..x30 */
    uint64_t sp;         /* 0x0F8: SP_EL0 for a user frame, the interrupted SP for a kernel frame */
    uint64_t elr;        /* 0x100 */
    uint64_t spsr;       /* 0x108 */
    uint64_t esr;        /* 0x110 */
    uint64_t far;        /* 0x118 */
    uint64_t vector;     /* 0x120: the generic vector trap.c assigned */
    uint64_t kind;       /* 0x128: AARCH64_ENTRY_* written by the vector slot */
};
_Static_assert(sizeof(struct arch_trap_frame) == 0x130, "trap frame layout");
#endif

#define FRAME_SIZE      0x130
#define FRAME_OFF_SP    0x0F8
#define FRAME_OFF_ELR   0x100
#define FRAME_OFF_SPSR  0x108
#define FRAME_OFF_ESR   0x110
#define FRAME_OFF_FAR   0x118
#define FRAME_OFF_KIND  0x128

/* Which vector slot delivered the frame. */
#define AARCH64_ENTRY_EL1_SYNC   0u
#define AARCH64_ENTRY_EL1_IRQ    1u
#define AARCH64_ENTRY_EL0_SYNC   2u
#define AARCH64_ENTRY_EL0_IRQ    3u
#define AARCH64_ENTRY_BAD_BASE   8u   /* + slot index 0..15 of the vector table */

#ifndef __ASSEMBLER__
void aarch64_trap_entry(struct arch_trap_frame *frame);
#endif

#endif /* AARCH64_TRAPFRAME_H */
