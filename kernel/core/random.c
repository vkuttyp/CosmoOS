/*
 * random.c - The kernel's entropy pool and output generator
 * (docs/kernel/security/design.md §6, invariants S16-S19).
 *
 * Inputs are absorbed into an input pool; the output key changes only
 * by a reseed of at least 256 credited bits, by the ratchet at the end
 * of every request, and (while unseeded) by the uncredited clock. The
 * first reseed is the transition to seeded, which is sticky.
 */

#include <kernel/bootinfo.h>
#include <kernel/cmdline.h>
#include <kernel/crypto.h>
#include <kernel/jitter.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/random.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <arch/cpu.h>
#include <arch/rng.h>

#define SEED_BITS        256u   /* credited bits that make the pool seeded */
#define POOL_BITS_MAX    512u
#define RESEED_NS        (60ull * 1000000000ull)
#define HW_RETRIES       10u
#define HW_SEED_CREDIT   32u    /* RDSEED, RNDRRS: half of 64 */
#define HW_RANDOM_CREDIT 4u     /* RDRAND, RNDR: a sixteenth of 64 */
#define FW_SEED_CREDIT   4u     /* per byte of the firmware's seed: half */
#define RANDOM_BOOT_WAIT_NS (5ull * 1000000000ull)

static uint8_t g_pool[SHA512_DIGEST_SIZE];
static uint8_t g_key[SHA512_DIGEST_SIZE];
static uint64_t g_counter;
static unsigned g_pool_bits;      /* credited since the last reseed */
static unsigned g_entropy_bits;   /* credited in total, capped */
static unsigned g_cpu_bits, g_fw_bits, g_jitter_bits, g_device_bits;
static uint64_t g_source_bytes;
static uint64_t g_init_ns, g_last_reseed_ns;
static bool g_seeded;
static spinlock_t g_lock = SPINLOCK_INIT("random");
static struct waitqueue g_seed_wq = WAITQUEUE_INIT(g_seed_wq);

static void hash2(uint8_t out[SHA512_DIGEST_SIZE], const void *a, size_t alen, const void *b, size_t blen,
                  const char *label)
{
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, a, alen);
    if (blen)
        sha512_update(&ctx, b, blen);
    if (label)
        sha512_update(&ctx, label, strlen(label));
    sha512_final(&ctx, out);
}

static void absorb_locked(const void *buf, size_t len, unsigned bits)
{
    hash2(g_pool, g_pool, sizeof(g_pool), buf, len, NULL);
    unsigned p = g_pool_bits + bits;
    g_pool_bits = p > POOL_BITS_MAX ? POOL_BITS_MAX : p;
    unsigned t = g_entropy_bits + bits;
    g_entropy_bits = t > POOL_BITS_MAX ? POOL_BITS_MAX : t;
}

/* Move the input pool into the key. Returns true when this reseed made
 * the pool seeded: the caller logs and wakes after dropping the lock. */
static bool reseed_locked(uint64_t now)
{
    uint8_t next[SHA512_DIGEST_SIZE];
    hash2(next, g_key, sizeof(g_key), g_pool, sizeof(g_pool), "reseed");
    memcpy(g_key, next, sizeof(g_key));
    hash2(g_pool, g_pool, sizeof(g_pool), NULL, 0, "drained");
    memset(next, 0, sizeof(next));
    g_pool_bits = 0;
    g_last_reseed_ns = now;
    if (g_seeded)
        return false;
    __atomic_store_n(&g_seeded, true, __ATOMIC_RELEASE);
    return true;
}

static void announce_seeded(uint64_t now)
{
    kinfo("random: pool seeded after %llu ms: %u bits credited (cpu %u, firmware %u, jitter %u, devices %u)",
          (unsigned long long)((now - g_init_ns) / 1000000ull), random_entropy_bits(),
          __atomic_load_n(&g_cpu_bits, __ATOMIC_RELAXED), __atomic_load_n(&g_fw_bits, __ATOMIC_RELAXED),
          __atomic_load_n(&g_jitter_bits, __ATOMIC_RELAXED), __atomic_load_n(&g_device_bits, __ATOMIC_RELAXED));
    waitqueue_wake_all(&g_seed_wq);
}

/* One credited CPU read, retried; false when every attempt failed. */
static bool cpu_read(bool seed, uint64_t *v)
{
    for (unsigned i = 0; i < HW_RETRIES; i++)
        if (seed ? arch_rng_seed64(v) : arch_rng_random64(v))
            return true;
    return false;
}

/* Absorb up to `want` credited bits from the CPU. Under g_lock. */
static void cpu_collect_locked(unsigned want)
{
    unsigned got = 0;
    uint64_t v;
    if (arch_rng_has_seed())
        while (got < want && cpu_read(true, &v)) {
            absorb_locked(&v, sizeof(v), HW_SEED_CREDIT);
            got += HW_SEED_CREDIT;
        }
    if (arch_rng_has_random())
        for (unsigned n = 0; got < want && n < want / HW_RANDOM_CREDIT && cpu_read(false, &v); n++) {
            absorb_locked(&v, sizeof(v), HW_RANDOM_CREDIT);
            got += HW_RANDOM_CREDIT;
        }
    v = 0;
    g_cpu_bits += got;
}

/*
 * The source of last resort (jitter.c): only when the CPU has no random
 * instruction and the firmware gave no seed -- an entropy device may still
 * come, but nothing says it will. Batches until the pool can seed or
 * JITTER_MAX_BATCHES; each is health-tested and estimated, and credited
 * only what the estimate allows. `random.jitter=off` disables it (the
 * no-source acceptance boot uses that). True when the pool seeded.
 */
#define JITTER_MAX_BATCHES 32u

static uint64_t g_jdeltas[JITTER_BATCH];
static uint8_t g_jlow[JITTER_BATCH];

static bool jitter_collect(void)
{
    char opt[8];
    if (cmdline_get("random.jitter", opt, sizeof(opt)) >= 0 && strcmp(opt, "off") == 0) {
        kinfo("random: jitter source off (random.jitter=off)");
        return false;
    }
    uint64_t t0 = clock_now_ns();
    unsigned batches = 0, credited = 0, refused = 0;
    uint32_t h_min = UINT32_MAX;
    bool seeded = false;
    while (!seeded && batches < JITTER_MAX_BATCHES) {
        jitter_sample(g_jdeltas, g_jlow);
        batches++;
        uint32_t h = jitter_mcv_h_q16(g_jlow, JITTER_BATCH);
        if (h < h_min)
            h_min = h;
        unsigned credit = jitter_batch_credit(g_jlow, JITTER_BATCH);
        if (credit == 0)
            refused++;
        arch_irq_state_t s = spin_lock_irqsave(&g_lock);
        absorb_locked(g_jdeltas, sizeof(g_jdeltas), credit);   /* a refused batch is mixed, not credited */
        g_jitter_bits += credit;
        credited += credit;
        seeded = !g_seeded && g_pool_bits >= SEED_BITS && reseed_locked(clock_now_ns());
        spin_unlock_irqrestore(&g_lock, s);
    }
    for (volatile uint8_t *p = (volatile uint8_t *)g_jdeltas; p < (uint8_t *)g_jdeltas + sizeof(g_jdeltas); p++)
        *p = 0;
    for (volatile uint8_t *p = g_jlow; p < g_jlow + sizeof(g_jlow); p++)
        *p = 0;
    kinfo("random: jitter source: %u batches of %u samples in %llu ms, %u bits credited, %u batches refused, "
          "lowest estimate %u.%02u bits per sample",
          batches, JITTER_BATCH, (unsigned long long)((clock_now_ns() - t0) / 1000000ull), credited, refused,
          h_min >> 16, (unsigned)(((h_min & 0xffff) * 100u) >> 16));
    return seeded;
}

void random_init(void)
{
    /* The boot's own variation: the clock and a stack address. Mixed
     * into the key so unseeded output differs between boots; not
     * entropy, never credited. */
    uint64_t now = clock_now_ns();
    uint64_t seed[2] = { now, (uint64_t)(uintptr_t)&seed };
    arch_rng_init();
    if (arch_rng_has_seed() || arch_rng_has_random())
        kinfo("random: cpu source:%s%s%s%s", arch_rng_has_seed() ? " " : "",
              arch_rng_has_seed() ? arch_rng_seed_name() : "", arch_rng_has_random() ? " " : "",
              arch_rng_has_random() ? arch_rng_random_name() : "");
    else
        kinfo("random: cpu source: none");

    /* The firmware's seed (boot protocol v10, EFI_RNG_PROTOCOL), taken
     * out of the boot info and wiped there: the page outlives this. It is
     * the host's or the platform's generator across an interface the
     * kernel cannot inspect, like virtio-rng, and is credited the same,
     * half of what it delivers. */
    uint8_t fw[COSMOBOOT_FW_SEED_MAX];
    unsigned fw_len = bootinfo_take_fw_seed(fw);
    char opt[8];
    if (fw_len != 0 && cmdline_get("random.firmware", opt, sizeof(opt)) >= 0 && strcmp(opt, "off") == 0) {
        /* Not trusted by the command line: neither mixed nor credited. */
        kinfo("random: firmware seed: %u bytes, ignored (random.firmware=off)", fw_len);
        fw_len = 0;
    } else if (fw_len != 0) {
        kinfo("random: firmware seed: %u bytes", fw_len);
    }

    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    g_init_ns = now;
    hash2(g_key, g_key, sizeof(g_key), seed, sizeof(seed), "boot");
    if (fw_len != 0) {
        absorb_locked(fw, fw_len, fw_len * FW_SEED_CREDIT);
        g_fw_bits += fw_len * FW_SEED_CREDIT;
    }
    for (volatile uint8_t *p = fw; p < fw + sizeof(fw); p++)
        *p = 0;   /* the stack copy too: stores the compiler may not drop */
    cpu_collect_locked(POOL_BITS_MAX);
    bool seeded = g_pool_bits >= SEED_BITS && reseed_locked(now);
    spin_unlock_irqrestore(&g_lock, s);
    if (!seeded && !arch_rng_has_seed() && !arch_rng_has_random() && fw_len == 0)
        seeded = jitter_collect();
    if (seeded)
        announce_seeded(clock_now_ns());
}

void random_add_entropy(const void *buf, size_t len, unsigned bits)
{
    if (len == 0)
        return;
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    absorb_locked(buf, len, bits);
    g_source_bytes += len;
    g_device_bits += bits;
    bool seeded = !g_seeded && g_pool_bits >= SEED_BITS && reseed_locked(now);
    spin_unlock_irqrestore(&g_lock, s);
    if (seeded)
        announce_seeded(now);
}

void random_get_bytes(void *buf, size_t len)
{
    uint8_t *out = buf;
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    if (!g_seeded) {
        /* Unseeded output: distinct per request, predictable in
         * principle; only may-be-early callers get here (§6). */
        hash2(g_key, g_key, sizeof(g_key), &now, sizeof(now), "jitter");
    } else if (now - g_last_reseed_ns >= RESEED_NS) {
        cpu_collect_locked(SEED_BITS);
        if (g_pool_bits >= SEED_BITS)
            (void)reseed_locked(now);
    }
    uint8_t block[SHA512_DIGEST_SIZE];
    while (len > 0) {
        hash2(block, g_key, sizeof(g_key), &g_counter, sizeof(g_counter), NULL);
        g_counter++;
        size_t n = len < sizeof(block) ? len : sizeof(block);
        memcpy(out, block, n);
        out += n;
        len -= n;
    }
    /* Forward secrecy: the key that produced this output is gone. */
    hash2(block, g_key, sizeof(g_key), &g_counter, sizeof(g_counter), "ratchet");
    memcpy(g_key, block, sizeof(g_key));
    memset(block, 0, sizeof(block));
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

bool random_ready(void)
{
    return __atomic_load_n(&g_seeded, __ATOMIC_ACQUIRE);
}

int random_wait_ready(uint64_t timeout_ns)
{
    if (random_ready())
        return 0;
    if (timeout_ns == RANDOM_WAIT_FOREVER)
        return wait_event_killable(&g_seed_wq, random_ready());
    return wait_event_killable_timeout(&g_seed_wq, random_ready(), timeout_ns);
}

void random_boot_wait(void)
{
    if (random_wait_ready(RANDOM_BOOT_WAIT_NS) == 0)
        return;
    kwarn("random: pool not seeded %llu s after the boot modules loaded: getrandom blocks, "
          "GRND_NONBLOCK returns EAGAIN, encrypted pools cannot be created",
          (unsigned long long)(RANDOM_BOOT_WAIT_NS / 1000000000ull));
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(random_add_entropy);
EXPORT_SYMBOL(random_get_bytes);
EXPORT_SYMBOL(random_u64);
EXPORT_SYMBOL(random_entropy_bits);
