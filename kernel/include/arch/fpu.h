/*
 * arch/fpu.h - Floating-point and SIMD register state of a thread.
 *
 * The kernel itself never uses these registers (it is compiled with
 * -mgeneral-regs-only on every architecture), so the state belongs to
 * user threads and to virtual machine guests only. The rules
 * (docs/kernel/arch/design.md, "FPU and SIMD state"):
 *
 *   Kernel rule    kernel code never touches vector or x87 registers;
 *                  the compiler enforces it and the arch layer asserts
 *                  the hardware configuration that makes a violation a
 *                  fault rather than silent corruption where it can.
 *   Process rule   a thread observes only its own state: the arch layer
 *                  saves the outgoing thread's registers and restores the
 *                  incoming thread's on every switch between two threads
 *                  that own state (eager switching; no lazy ownership).
 *   Guest rule     a guest starts from the architectural reset state and
 *                  its registers are swapped with the owner thread's
 *                  around every entry (arch/hv.h backend).
 *
 * The state is opaque to generic code: struct thread carries a pointer
 * the arch layer allocates, restores and frees. A thread without state
 * (kernel threads, the idle threads) is never saved or restored and must
 * not execute such instructions. Architectures without a user
 * floating-point unit in use (AArch64 today: FP/SIMD is disabled at EL0)
 * implement these as no-ops that leave the pointer NULL.
 */

#ifndef ARCH_FPU_H
#define ARCH_FPU_H

#include <kernel/compiler.h>

struct thread;
struct arch_fpu_state;

/* Give `t` its own register state, initialised to the architectural
 * reset values (x87 control word 0x37F, MXCSR 0x1F80, everything else
 * zero on x86-64). Called before the thread first runs user code; 0 or
 * -ENOMEM. Idempotent for a thread that already has state. */
int arch_fpu_alloc(struct thread *t);

/* Release the state (no-op when there is none). Only for a thread that
 * is not running, or for the calling thread itself. */
void arch_fpu_free(struct thread *t);

/* Bytes one thread's state occupies (0 when the architecture keeps none). */
size_t arch_fpu_state_size(void);

#endif /* ARCH_FPU_H */
