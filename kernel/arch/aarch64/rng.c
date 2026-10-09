/*
 * rng.c - RNDRRS and RNDR as entropy sources (arch/rng.h).
 *
 * FEAT_RNG: ID_AA64ISAR0_EL1.RNDR, bits [63:60], is 1 when both
 * registers exist (ARM ARM D19.2). A read sets NZCV to 0000 when the
 * value is valid and to 0100 (Z) with a zero result when the generator
 * had none. RNDRRS reseeds the generator before producing its value;
 * RNDR may not. The registers are named by encoding (S3_3_C2_C4_1,
 * S3_3_C2_C4_0) so the assembler needs no +rng.
 */

#include <arch/rng.h>

#include <aarch64/sysreg.h>

static bool g_has_rng;

void arch_rng_init(void)
{
    uint64_t isar0 = READ_SYSREG(id_aa64isar0_el1);
    g_has_rng = ((isar0 >> 60) & 0xF) >= 1;
}

bool arch_rng_has_seed(void) { return g_has_rng; }
bool arch_rng_has_random(void) { return g_has_rng; }
const char *arch_rng_seed_name(void) { return "rndrrs"; }
const char *arch_rng_random_name(void) { return "rndr"; }

bool arch_rng_seed64(uint64_t *v)
{
    if (!g_has_rng)
        return false;
    uint64_t x, ok;
    __asm__ volatile("mrs %0, s3_3_c2_c4_1\n\tcset %1, ne" : "=r"(x), "=r"(ok) : : "cc");
    if (ok)
        *v = x;
    return ok != 0;
}

bool arch_rng_random64(uint64_t *v)
{
    if (!g_has_rng)
        return false;
    uint64_t x, ok;
    __asm__ volatile("mrs %0, s3_3_c2_c4_0\n\tcset %1, ne" : "=r"(x), "=r"(ok) : : "cc");
    if (ok)
        *v = x;
    return ok != 0;
}
