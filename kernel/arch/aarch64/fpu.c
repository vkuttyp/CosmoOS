/*
 * fpu.c - FP/SIMD state on AArch64 (docs/kernel/arch/aarch64/design.md).
 *
 * Stage 1 leaves CPACR_EL1.FPEN at its reset value: every FP/SIMD
 * instruction at EL0 or EL1 traps, so no thread can hold vector state and
 * there is nothing to save. The generic interface is implemented as
 * no-ops that keep thread->fpu NULL; the process, guest and kernel rules
 * of arch/fpu.h hold trivially. Enabling FPEN for user mode means giving
 * this file a real Q0-Q31/FPCR/FPSR area and the switch hook in
 * context.c, exactly the shape of the x86-64 implementation.
 */

#include <arch/fpu.h>
#include <arch/testhooks.h>

int arch_fpu_alloc(struct thread *t)
{
    (void)t;
    return 0;
}

void arch_fpu_free(struct thread *t)
{
    (void)t;
}

size_t arch_fpu_state_size(void)
{
    return 0;
}

bool arch_test_fpu_switch(const char **why)
{
    *why = NULL;   /* no thread can own vector state: nothing to leak */
    return true;
}

bool arch_test_fpu_set(const uint8_t pattern[16])
{
    (void)pattern;
    return false;
}

bool arch_test_fpu_get(uint8_t out[16])
{
    (void)out;
    return false;
}
