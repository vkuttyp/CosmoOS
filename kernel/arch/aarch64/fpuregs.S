/*
 * fpuregs.S - Saving and restoring the FP/SIMD registers (the name
 * differs from fpu.c's only because the build derives object names from
 * the stem).
 *
 * These are the only functions in the kernel that may name a vector
 * register (with the probe helpers below, which exist so a self-test can
 * put a known pattern in them). scripts/check-fpregs.sh disassembles the
 * built kernel and fails the build on any other user of Q0-Q31, V0-V31,
 * D0-D31 or S0-S31: on this architecture the hardware cannot enforce the
 * kernel's abstention, because EL1 is exactly where the state is saved.
 *
 * The area layout is struct aarch64_fpu_area: 32 * 16 bytes of vector
 * registers, then FPSR and FPCR as two words.
 */

/* The kernel is built -mgeneral-regs-only, which tells the assembler to
 * refuse these instructions too. This one file asks for them back, which
 * is the same statement the check makes from the other side: here and
 * nowhere else. */
    .arch armv8-a+fp+simd

    .text

/* void aarch64_fpu_area_save(struct aarch64_fpu_area *area) */
    .globl aarch64_fpu_area_save
    .type aarch64_fpu_area_save, @function
aarch64_fpu_area_save:
    stp     q0, q1,   [x0, #0]
    stp     q2, q3,   [x0, #32]
    stp     q4, q5,   [x0, #64]
    stp     q6, q7,   [x0, #96]
    stp     q8, q9,   [x0, #128]
    stp     q10, q11, [x0, #160]
    stp     q12, q13, [x0, #192]
    stp     q14, q15, [x0, #224]
    stp     q16, q17, [x0, #256]
    stp     q18, q19, [x0, #288]
    stp     q20, q21, [x0, #320]
    stp     q22, q23, [x0, #352]
    stp     q24, q25, [x0, #384]
    stp     q26, q27, [x0, #416]
    stp     q28, q29, [x0, #448]
    stp     q30, q31, [x0, #480]
    mrs     x1, fpsr
    mrs     x2, fpcr
    str     w1, [x0, #512]
    str     w2, [x0, #516]
    ret
    .size aarch64_fpu_area_save, . - aarch64_fpu_area_save

/* void aarch64_fpu_area_restore(const struct aarch64_fpu_area *area) */
    .globl aarch64_fpu_area_restore
    .type aarch64_fpu_area_restore, @function
aarch64_fpu_area_restore:
    ldp     q0, q1,   [x0, #0]
    ldp     q2, q3,   [x0, #32]
    ldp     q4, q5,   [x0, #64]
    ldp     q6, q7,   [x0, #96]
    ldp     q8, q9,   [x0, #128]
    ldp     q10, q11, [x0, #160]
    ldp     q12, q13, [x0, #192]
    ldp     q14, q15, [x0, #224]
    ldp     q16, q17, [x0, #256]
    ldp     q18, q19, [x0, #288]
    ldp     q20, q21, [x0, #320]
    ldp     q22, q23, [x0, #352]
    ldp     q24, q25, [x0, #384]
    ldp     q26, q27, [x0, #416]
    ldp     q28, q29, [x0, #448]
    ldp     q30, q31, [x0, #480]
    ldr     w1, [x0, #512]
    ldr     w2, [x0, #516]
    msr     fpsr, x1
    msr     fpcr, x2
    isb
    ret
    .size aarch64_fpu_area_restore, . - aarch64_fpu_area_restore

/* void aarch64_fpu_probe_load(const uint8_t pattern[16][16]) -- self-tests:
 * fill Q0-Q15 from a 256-byte pattern. */
    .globl aarch64_fpu_probe_load
    .type aarch64_fpu_probe_load, @function
aarch64_fpu_probe_load:
    ldp     q0, q1,   [x0, #0]
    ldp     q2, q3,   [x0, #32]
    ldp     q4, q5,   [x0, #64]
    ldp     q6, q7,   [x0, #96]
    ldp     q8, q9,   [x0, #128]
    ldp     q10, q11, [x0, #160]
    ldp     q12, q13, [x0, #192]
    ldp     q14, q15, [x0, #224]
    ret
    .size aarch64_fpu_probe_load, . - aarch64_fpu_probe_load

/* void aarch64_fpu_probe_store(uint8_t out[16][16]) */
    .globl aarch64_fpu_probe_store
    .type aarch64_fpu_probe_store, @function
aarch64_fpu_probe_store:
    stp     q0, q1,   [x0, #0]
    stp     q2, q3,   [x0, #32]
    stp     q4, q5,   [x0, #64]
    stp     q6, q7,   [x0, #96]
    stp     q8, q9,   [x0, #128]
    stp     q10, q11, [x0, #160]
    stp     q12, q13, [x0, #192]
    stp     q14, q15, [x0, #224]
    ret
    .size aarch64_fpu_probe_store, . - aarch64_fpu_probe_store

/* void aarch64_fpu_probe_set_q0(const uint8_t in[16]) */
    .globl aarch64_fpu_probe_set_q0
    .type aarch64_fpu_probe_set_q0, @function
aarch64_fpu_probe_set_q0:
    ldr     q0, [x0]
    ret
    .size aarch64_fpu_probe_set_q0, . - aarch64_fpu_probe_set_q0

/* void aarch64_fpu_probe_get_q0(uint8_t out[16]) */
    .globl aarch64_fpu_probe_get_q0
    .type aarch64_fpu_probe_get_q0, @function
aarch64_fpu_probe_get_q0:
    str     q0, [x0]
    ret
    .size aarch64_fpu_probe_get_q0, . - aarch64_fpu_probe_get_q0

    .section .note.GNU-stack, "", @progbits
