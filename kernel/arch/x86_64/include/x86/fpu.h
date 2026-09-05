/*
 * x86/fpu.h - x87/SSE/AVX state management. Private to x86-64.
 *
 * XSAVE when the CPU has it (XCR0 = every supported user state component
 * this kernel knows how to hold: x87, SSE, AVX, AVX-512), FXSAVE
 * otherwise. Both forms are stored in the same per-thread area whose
 * size comes from CPUID.(EAX=0DH,ECX=0):EBX for the enabled XCR0.
 */

#ifndef X86_FPU_H
#define X86_FPU_H

#include <kernel/types.h>

/* XCR0 bits. */
#define XCR0_X87        (1ull << 0)
#define XCR0_SSE        (1ull << 1)
#define XCR0_AVX        (1ull << 2)
#define XCR0_OPMASK     (1ull << 5)
#define XCR0_ZMM_HI256  (1ull << 6)
#define XCR0_HI16_ZMM   (1ull << 7)
#define XCR0_SUPPORTED_BY_KERNEL (XCR0_X87 | XCR0_SSE | XCR0_AVX | XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM)

#define X86_FXSAVE_SIZE   512u
#define X86_FPU_AREA_MAX  4096u   /* AVX-512 needs 2688; anything larger is not enabled */

struct x86_fpu_info {
    bool xsave;                 /* CPUID.1:ECX.XSAVE; else FXSAVE/FXRSTOR */
    bool xsaveopt;              /* CPUID.(0DH,1):EAX.XSAVEOPT */
    uint64_t xcr0;              /* what the kernel enables on every CPU */
    uint64_t xcr0_supported;    /* what the CPU offers (CPUID.(0DH,0):EDX:EAX) */
    unsigned area_size;         /* bytes of one thread's state, multiple of 64 */
};

/* Every CPU, from x86_cpu_enable_features: CR0/CR4/XCR0 policy and, on the
 * boot CPU, feature discovery and the reset image. */
void x86_fpu_init_cpu(void);
const struct x86_fpu_info *x86_fpu_info(void);

/* Whole-state moves between the registers and a 64-byte-aligned area of
 * area_size bytes (all enabled components; XCR0 must be the kernel's). */
void x86_fpu_area_save(void *area);
void x86_fpu_area_restore(const void *area);
/* Fill `area` with the architectural reset state. */
void x86_fpu_area_init(void *area);

/* The architectural reset state, area_size bytes, 64-byte aligned. */
const void *x86_fpu_reset_image(void);

/* The calling thread's own state, if it owns one (user threads, and test
 * threads that asked for one): save the live registers into it / load it
 * back. Used around a guest run, whose registers replace the thread's.
 * Return whether the thread owns state. */
bool x86_fpu_save_current(void);
bool x86_fpu_restore_current(void);

/* The switch hook's half (context.c): prev's registers are live, next
 * runs next; prev may be NULL. Interrupts disabled, run-queue lock held. */
struct thread;
void x86_fpu_switch(struct thread *prev, struct thread *next);

static inline uint64_t xgetbv(uint32_t index)
{
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(index));
    return ((uint64_t)hi << 32) | lo;
}

static inline void xsetbv(uint32_t index, uint64_t value)
{
    __asm__ volatile("xsetbv" : : "c"(index), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory");
}

#endif /* X86_FPU_H */
