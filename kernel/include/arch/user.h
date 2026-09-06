/*
 * arch/user.h - User-mode transitions, implemented per architecture.
 */

#ifndef ARCH_USER_H
#define ARCH_USER_H

#include <kernel/compiler.h>
#include <kernel/types.h>

/* Calling CPU: enable the system-call instruction path (MSRs on x86-64). */
void arch_syscall_init_cpu(void);

/* Switch the calling thread to user mode at `entry` with stack `sp`.
 * All general registers are zeroed. Never returns; the thread comes back
 * into the kernel only through system calls, traps, and interrupts. */
void arch_user_enter(uintptr_t entry, uintptr_t sp) __noreturn;
/* Set the calling user thread's thread-pointer base (x86-64: FS base) now
 * and for every later switch to it. */
void arch_set_tls_base(uintptr_t base);

/*
 * The complete user register set (milestone 10: threads and signals).
 * `struct arch_user_regs` is defined by the architecture
 * (below); generic code moves it between
 * frames and threads and touches only the fields below.
 */
/* The register file of a user thread. Generic code moves it between a
 * system-call or trap frame and a signal frame; the layout is the
 * architecture's, defined here so that generic code (and modules) see it
 * without the architecture's private include path. */
#if defined(ARCH_X86_64)
struct arch_user_regs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
};
#define X86_RFLAGS_USER_MASK 0xcd5ull   /* CF PF AF ZF SF TF DF OF */
#define X86_RFLAGS_FIXED     0x202ull   /* IF and the reserved bit 1 */
#elif defined(ARCH_AARCH64)
struct arch_user_regs {
    uint64_t x[31];
    uint64_t sp, pc, pstate;
};
#define AARCH64_PSTATE_USER_MASK 0xf0000000ull   /* NZCV */
#else
struct arch_user_regs;
#endif
struct arch_trap_frame;
/* Read the registers out of a system-call frame (`frame` as passed to
 * syscall_dispatch) or a trap frame, and write them back. Writing marks
 * the frame for a full restore (every register reloaded on return). */
void arch_user_regs_from_syscall(const void *frame, struct arch_user_regs *r);
void arch_user_regs_to_syscall(void *frame, const struct arch_user_regs *r);
void arch_user_regs_from_trap(const struct arch_trap_frame *frame, struct arch_user_regs *r);
void arch_user_regs_to_trap(struct arch_trap_frame *frame, const struct arch_user_regs *r);
/* The fields generic code needs: program counter, stack pointer, the
 * system-call result register, and a pending call's number and first
 * argument (for restarting it: pc is moved back over the instruction). */
uintptr_t arch_user_regs_pc(const struct arch_user_regs *r);
uintptr_t arch_user_regs_sp(const struct arch_user_regs *r);
void arch_user_regs_set_pc(struct arch_user_regs *r, uintptr_t pc);
void arch_user_regs_set_sp(struct arch_user_regs *r, uintptr_t sp);
void arch_user_regs_set_result(struct arch_user_regs *r, int64_t v);
int64_t arch_user_regs_result(const struct arch_user_regs *r);
void arch_user_regs_restart_syscall(struct arch_user_regs *r, uint64_t nr, uint64_t arg0);
/* The calling user thread's floating-point and vector state as the
 * architecture's signal frame carries it (x86-64: the 512-byte FXSAVE
 * image; AArch64: nothing yet, user FP/SIMD is not enabled). `size` is 0
 * when there is none. `save` copies the live state into `buf`; `restore`
 * loads `buf` (sanitised: reserved control bits cleared) as the thread's
 * state. Both false when the thread owns no state. */
size_t arch_user_fpu_image_size(void);
bool arch_user_fpu_image_save(void *buf);
bool arch_user_fpu_image_restore(const void *buf);
/* The result register of a system-call frame, without a full copy. */
void arch_user_regs_set_result_in_frame(void *frame, int64_t v);
int64_t arch_user_regs_result_in_frame(const void *frame);
/* Switch the calling thread to user mode with every register loaded from
 * `r` (the thread pointer is the thread's tls_base). Never returns. */
void arch_user_enter_regs(const struct arch_user_regs *r) __noreturn;
/* Sanitise a register set that came from user memory (segment selectors,
 * privileged flag bits) so that loading it cannot escalate. */
void arch_user_regs_sanitize(struct arch_user_regs *r);

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
