/*
 * interrupt.h - Architecture-neutral interrupt dispatch.
 *
 * Model: the architecture layer owns entry/exit (stubs, frames, EOI) and
 * calls interrupt_dispatch() with a vector number and an opaque trap
 * frame. This layer maps vectors to registered handlers. Vector numbering
 * is architecture-defined; generic code obtains vectors symbolically via
 * arch_trap_vector() and never hard-codes numbers.
 *
 * Handlers run in interrupt context on the interrupted stack with
 * interrupts disabled. They must not sleep, allocate, or take sleeping
 * locks. Long work is deferred (the deferred-work mechanism arrives with
 * the scheduler in Phase 3).
 *
 * Concurrency: the handler table is modified with interrupts disabled on
 * the calling CPU. Before SMP exists that is sufficient; the Phase 3 SMP
 * work must add cross-CPU synchronization (an RCU-style publish is the
 * intended design so dispatch stays lock-free).
 *
 * Ownership: the table holds a pointer to the handler and its argument;
 * it does not own `arg`. The registrant must keep `arg` alive until
 * interrupt_unregister() returns.
 */

#ifndef KERNEL_INTERRUPT_H
#define KERNEL_INTERRUPT_H

#include <stdint.h>

struct arch_trap_frame;

typedef void (*interrupt_handler_fn)(unsigned vector, struct arch_trap_frame *frame, void *arg);

/* Set up the dispatch table. Called once by kernel_main before enabling
 * interrupts. Does not allocate. */
void interrupt_init(void);

/* Install `fn` for `vector`. `name` is for diagnostics and must be a
 * string literal or otherwise immortal.
 * Returns 0, -EINVAL (bad vector or fn), or -EBUSY (already registered). */
int interrupt_register(unsigned vector, interrupt_handler_fn fn, void *arg, const char *name);

/* Remove `fn` from `vector`. Returns 0, -EINVAL, or -ENOENT if `fn` is not
 * the registered handler. */
int interrupt_unregister(unsigned vector, interrupt_handler_fn fn);

/* Remove whatever handler `vector` has. For owners of the vector (the
 * IRQ layer) that do not track the function pointer. */
int interrupt_unregister_vector(unsigned vector);

/* Called by the architecture layer for every interrupt and exception. If
 * no handler is registered it calls arch_trap_unhandled(). */
void interrupt_dispatch(unsigned vector, struct arch_trap_frame *frame);

/* Diagnostics: number of times `vector` was dispatched, or 0 if invalid. */
uint64_t interrupt_count(unsigned vector);

/* Diagnostics: registered handler name, or NULL. */
const char *interrupt_handler_name(unsigned vector);

#endif /* KERNEL_INTERRUPT_H */
