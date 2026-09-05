/*
 * arch/context.h - Thread CPU context and switching.
 *
 * A context is the callee-saved state of a thread that is not running,
 * stored on its own stack; the struct holds only the stack pointer. The
 * switch saves the caller's callee-saved registers, swaps stack
 * pointers, and restores the target's. A freshly initialised context
 * "returns" into `entry` on its first switch-in.
 */

#ifndef ARCH_CONTEXT_H
#define ARCH_CONTEXT_H

#include <kernel/compiler.h>

struct arch_context {
    uintptr_t sp;
};

/* Prepare `ctx` so the first arch_context_switch into it starts
 * executing `entry` on the stack whose top is `stack_top`, with a
 * terminated frame chain for backtraces. */
void arch_context_init(struct arch_context *ctx, uintptr_t stack_top, void (*entry)(void));

/* Save into `from`, resume `to`. Returns when `from` is switched back
 * in. Must be called with interrupts disabled; the resumed thread
 * decides when to re-enable them. */
void arch_context_switch(struct arch_context *from, struct arch_context *to);

/* Bounds of the boot stack the initial context runs on (thread 0). */
void arch_boot_stack(uintptr_t *base, size_t *size);

/* Called by the scheduler with the run-queue lock held and interrupts
 * disabled, just before switching from `prev` (the thread whose registers
 * are live; NULL when a CPU's bootstrap context is being abandoned) to
 * `next`: save prev's vector/x87 state and load next's (arch/fpu.h),
 * publish next's kernel stack for traps and system calls (TSS rsp0,
 * per-CPU block) and activate its address space. */
struct thread;
void arch_thread_switch_prepare(struct thread *prev, struct thread *next);

#endif /* ARCH_CONTEXT_H */
