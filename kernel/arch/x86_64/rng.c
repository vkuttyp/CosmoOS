/*
 * rng.c - RDSEED and RDRAND as entropy sources (arch/rng.h).
 *
 * CPUID.(7,0):EBX bit 18 is RDSEED and CPUID.1:ECX bit 30 is RDRAND
 * (Intel SDM vol. 2). Either instruction sets CF when the value is
 * valid and clears it (and the destination) when the CPU had none.
 */

#include <arch/rng.h>

#include <x86/cpu.h>

static bool g_has_seed, g_has_random;

void arch_rng_init(void)
{
    struct cpuid_regs r;
    cpuid(0, 0, &r);
    uint32_t max_leaf = r.eax;
    if (max_leaf >= 1) {
        cpuid(1, 0, &r);
        g_has_random = (r.ecx & (1u << 30)) != 0;
    }
    if (max_leaf >= 7) {
        cpuid(7, 0, &r);
        g_has_seed = (r.ebx & (1u << 18)) != 0;
    }
}

bool arch_rng_has_seed(void) { return g_has_seed; }
bool arch_rng_has_random(void) { return g_has_random; }
const char *arch_rng_seed_name(void) { return "rdseed"; }
const char *arch_rng_random_name(void) { return "rdrand"; }

bool arch_rng_seed64(uint64_t *v)
{
    if (!g_has_seed)
        return false;
    uint64_t x;
    uint8_t ok;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(x), "=qm"(ok) : : "cc");
    if (ok)
        *v = x;
    return ok != 0;
}

bool arch_rng_random64(uint64_t *v)
{
    if (!g_has_random)
        return false;
    uint64_t x;
    uint8_t ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(x), "=qm"(ok) : : "cc");
    if (ok)
        *v = x;
    return ok != 0;
}
