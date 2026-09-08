/*
 * fpu.h - AArch64 FP/SIMD state, private to the architecture layer
 * (kernel/arch/aarch64/fpu.c, fpu.S).
 *
 * The area is the 32 vector registers followed by FPSR and FPCR, which
 * is also the body of the Linux `fpsimd_context` a signal frame carries,
 * so one layout serves the switch and the ABI.
 */

#ifndef AARCH64_FPU_H
#define AARCH64_FPU_H

#include <kernel/types.h>

struct thread;

struct aarch64_fpu_area {
    uint8_t vregs[32][16];   /* Q0..Q31, little-endian as the registers are */
    uint32_t fpsr;
    uint32_t fpcr;
} __attribute__((aligned(16)));

/* FPCR bits a program may set; everything else is RES0 and refused when
 * a signal frame or a debugger hands one in. */
#define AARCH64_FPCR_MASK 0x07ff9f00u

/*
 * The body of a Linux `fpsimd_context`, which is the same registers in
 * the order the ABI puts them: the two status words first, then Q0-Q31.
 * The kernel keeps its own area in the order the load and store pairs
 * want (vectors first, sixteen-byte aligned), so the two are converted
 * rather than aliased -- 520 bytes either way.
 */
struct aarch64_fpsimd_image {
    uint32_t fpsr;
    uint32_t fpcr;
    uint8_t vregs[32][16];
} __attribute__((packed));

/* Set CPACR_EL1.FPEN so FP/SIMD is usable at EL0 and EL1. Every CPU. */
void aarch64_fpu_init_cpu(void);

/* The switch hook: save `prev`'s registers, restore `next`'s. */
void aarch64_fpu_switch(struct thread *prev, struct thread *next);

/* The running thread's registers into its own area and back, for a guest
 * entry that borrows them. False when the caller owns no state. */
bool aarch64_fpu_save_current(void);
bool aarch64_fpu_restore_current(void);

/* The running thread's state, saved from the registers first / restored
 * into them after. False when the caller owns no state. */
bool aarch64_fpu_get_current(struct aarch64_fpu_area *out);
bool aarch64_fpu_set_current(const struct aarch64_fpu_area *in);

/* fpuregs.S: the register moves themselves. These, and the probe helpers
 * below, are the only functions in the kernel allowed to name a vector
 * register (scripts/check-fpregs.sh). */
void aarch64_fpu_area_save(struct aarch64_fpu_area *area);
void aarch64_fpu_area_restore(const struct aarch64_fpu_area *area);

/* Self-tests only: put a pattern in the registers and read it back. */
void aarch64_fpu_probe_load(const uint8_t pattern[16][16]);
void aarch64_fpu_probe_store(uint8_t out[16][16]);
void aarch64_fpu_probe_set_q0(const uint8_t in[16]);
void aarch64_fpu_probe_get_q0(uint8_t out[16]);

#endif /* AARCH64_FPU_H */
