/*
 * arch/user.h - User-mode transitions, implemented per architecture.
 */

#ifndef ARCH_USER_H
#define ARCH_USER_H

#include <kernel/compiler.h>

/* Calling CPU: enable the system-call instruction path (MSRs on x86-64). */
void arch_syscall_init_cpu(void);

/* Switch the calling thread to user mode at `entry` with stack `sp`.
 * All general registers are zeroed. Never returns; the thread comes back
 * into the kernel only through system calls, traps, and interrupts. */
void arch_user_enter(uintptr_t entry, uintptr_t sp) __noreturn;
/* Set the calling user thread's thread-pointer base (x86-64: FS base) now
 * and for every later switch to it. */
void arch_set_tls_base(uintptr_t base);

/* Bracket direct kernel access to user memory (STAC/CLAC with SMAP). */
void arch_user_access_begin(void);
void arch_user_access_end(void);

/* True if the trap frame was captured while executing user code. */
struct arch_trap_frame;
bool arch_trap_frame_is_user(const struct arch_trap_frame *frame);

/* Copy n bytes between kernel memory and user memory (either direction:
 * the caller knows which side is which) with every access listed in the
 * exception table. Returns the number of bytes NOT copied: 0 on success,
 * else the copy stopped at the first faulting page and the fault handler
 * resumed here. Call inside arch_user_access_begin/end. */
size_t arch_copy_user_raw(void *dst, const void *src, size_t n);

/* Kernel-mode fault at a fixable PC: move the frame's PC to the fixup
 * and return true; false when the PC has no table entry. */
bool arch_trap_fixup(struct arch_trap_frame *frame);

#endif /* ARCH_USER_H */
