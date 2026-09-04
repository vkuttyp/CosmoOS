/*
 * arch/trap.h - Trap frames and vector naming, architecture-neutral view.
 *
 * `struct arch_trap_frame` is opaque here; only the architecture layer
 * knows its layout. Generic code reads it through the accessors below.
 *
 * Vector numbers are architecture-defined. Generic code that must handle
 * a particular kind of trap (the VMM for page faults, a debugger for
 * breakpoints) asks for the vector by kind via arch_trap_vector() and
 * registers on that number with the interrupt subsystem.
 */

#ifndef ARCH_TRAP_H
#define ARCH_TRAP_H

#include <kernel/compiler.h>

struct arch_trap_frame;

enum arch_trap_kind {
    ARCH_TRAP_BREAKPOINT,
    ARCH_TRAP_DEBUG,
    ARCH_TRAP_DIVIDE_ERROR,
    ARCH_TRAP_INVALID_OPCODE,
    ARCH_TRAP_GENERAL_PROTECTION,
    ARCH_TRAP_PAGE_FAULT,
    ARCH_TRAP_KIND_COUNT
};

/* Total number of vectors the architecture can dispatch. */
unsigned arch_trap_vector_count(void);

/* Vector for a trap kind, or -1 if the architecture has no such trap. */
int arch_trap_vector(enum arch_trap_kind kind);

/* True for CPU exceptions (as opposed to external or software interrupts). */
bool arch_trap_is_exception(unsigned vector);

/* Short name for logs, e.g. "#PF". Never NULL. */
const char *arch_trap_name(unsigned vector);

uintptr_t arch_trap_frame_pc(const struct arch_trap_frame *frame);
uintptr_t arch_trap_frame_sp(const struct arch_trap_frame *frame);
uintptr_t arch_trap_frame_fp(const struct arch_trap_frame *frame);

/* Log the full register state. Interrupt and panic safe. */
void arch_trap_frame_dump(const struct arch_trap_frame *frame);

/* Called by interrupt_dispatch() when no handler is registered. Panics
 * for exceptions; counts and logs spurious external interrupts. */
void arch_trap_unhandled(unsigned vector, struct arch_trap_frame *frame);

/* Raise the breakpoint trap synchronously from kernel code. */
void arch_debug_break(void);

/* Page-fault decoding. Valid only for a frame whose vector is the
 * ARCH_TRAP_PAGE_FAULT vector. */
#define ARCH_FAULT_PRESENT  (1u << 0)  /* protection violation on a present page */
#define ARCH_FAULT_WRITE    (1u << 1)  /* access was a write */
#define ARCH_FAULT_EXEC     (1u << 2)  /* access was an instruction fetch */
#define ARCH_FAULT_USER     (1u << 3)  /* access came from user mode */
#define ARCH_FAULT_RESERVED (1u << 4)  /* reserved bit set in a table entry */

uintptr_t arch_trap_fault_address(const struct arch_trap_frame *frame);
unsigned arch_trap_fault_flags(const struct arch_trap_frame *frame);

#endif /* ARCH_TRAP_H */
