/*
 * random.c - SHA-512 hash pool.
 */

#include <kernel/crypto.h>
#include <kernel/random.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#include <arch/cpu.h>

static uint8_t g_state[SHA512_DIGEST_SIZE];
static uint64_t g_counter;
static unsigned g_entropy_bits;
static uint64_t g_source_bytes;
static spinlock_t g_lock = SPINLOCK_INIT("random");

static void mix_locked(const void *buf, size_t len)
{
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, g_state, sizeof(g_state));
    sha512_update(&ctx, buf, len);
    sha512_final(&ctx, g_state);
}

void random_init(void)
{
    /* Seed with whatever varies between boots on this platform: the
     * clock. This is not entropy and is not credited as such. */
    uint64_t seed[2] = { clock_now_ns(), (uint64_t)(uintptr_t)&seed };
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    mix_locked(seed, sizeof(seed));
    spin_unlock_irqrestore(&g_lock, s);
}

void random_add_entropy(const void *buf, size_t len, unsigned bits)
{
    if (len == 0)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    mix_locked(buf, len);
    g_source_bytes += len;
    unsigned total = g_entropy_bits + bits;
    g_entropy_bits = total > 512 ? 512 : total;
    spin_unlock_irqrestore(&g_lock, s);
}

void random_get_bytes(void *buf, size_t len)
{
    uint8_t *out = buf;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    while (len > 0) {
        uint8_t block[SHA512_DIGEST_SIZE];
        struct sha512_ctx ctx;
        sha512_init(&ctx);
        sha512_update(&ctx, g_state, sizeof(g_state));
        sha512_update(&ctx, &g_counter, sizeof(g_counter));
        sha512_final(&ctx, block);
        g_counter++;
        size_t n = len < sizeof(block) ? len : sizeof(block);
        memcpy(out, block, n);
        out += n;
        len -= n;
    }
    /* Backtracking resistance: move the state forward. */
    static const char reseed[] = "reseed";
    mix_locked(reseed, sizeof(reseed));
    spin_unlock_irqrestore(&g_lock, s);
}

uint64_t random_u64(void)
{
    uint64_t v;
    random_get_bytes(&v, sizeof(v));
    return v;
}

unsigned random_entropy_bits(void)
{
    return __atomic_load_n(&g_entropy_bits, __ATOMIC_RELAXED);
}

uint64_t random_source_bytes(void)
{
    return __atomic_load_n(&g_source_bytes, __ATOMIC_RELAXED);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(random_add_entropy);
EXPORT_SYMBOL(random_get_bytes);
EXPORT_SYMBOL(random_u64);
EXPORT_SYMBOL(random_entropy_bits);
