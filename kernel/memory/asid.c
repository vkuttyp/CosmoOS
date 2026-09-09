/*
 * asid.c - Address-space tags: allocation, generations, rollover
 * (docs/kernel/memory/design.md §2.6).
 *
 * One global bitmap under one spinlock. A global allocator is the right
 * shape here and a per-CPU one is not: this kernel caps at 64 CPUs and
 * the narrowest tag width it will meet is eight bits, so 255 tags are
 * shared by at most 64 CPUs -- the lock is taken once per *first* switch
 * into a space, not once per switch, and it protects a bitmap scan.
 *
 * Tag 0 is never handed out. It belongs to the kernel's own root (the
 * empty TTBR0 on AArch64, CR3 with PCID 0 on x86-64), so a kernel thread
 * runs under a tag no user space can be given, and no user translation
 * can alias the kernel's view of the user half.
 *
 * The rollover rule, which is the whole of the correctness argument:
 *
 *   A tag is only ever reused after every CPU that could hold
 *   translations under it has flushed them.
 *
 * The generation counter is how that is enforced without visiting every
 * space or sending an IPI. When the bitmap fills, the generation
 * advances and the bitmap is cleared: every tag issued earlier is now
 * stale, because a space carries the generation it was tagged in.
 * Every CPU carries the generation it last flushed everything at; a CPU
 * whose stamp is behind flushes everything before it uses any tag, and
 * only then stamps itself current. So the moment a tag is handed to its
 * next owner, the only CPUs that can use it are ones that have already
 * dropped what the previous owner left.
 */

#include <kernel/asid.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#include <arch/cpu.h>
#include <arch/mmu.h>

#define ASID_MAX_BITS 16u
#define ASID_MAX      (1u << ASID_MAX_BITS)
#define ASID_WORDS    (ASID_MAX / 64u)

static spinlock_t g_lock = SPINLOCK_INIT("asid");
static uint64_t g_bitmap[ASID_WORDS];
static unsigned g_bits;              /* 0 when the machine has no usable tag */
static uint32_t g_count;             /* 1 << g_bits */
static uint32_t g_next = 1;          /* round-robin cursor: tag 0 is the kernel's */
static uint64_t g_generation;        /* asid_init makes it 1; a context at 0 is always stale */
static struct asid_stats g_stats;

/* The generation each CPU last flushed everything at. Behind the global
 * generation means "this CPU may still hold tags from before the
 * rollover". Written only by the CPU itself, at a point where it is
 * about to flush; read by that CPU alone. */
static uint64_t g_cpu_generation[CONFIG_MAX_CPUS];

/* Paranoid mode: tags are still allocated and written, but every switch
 * is told to flush, so no translation survives one. An isolation failure
 * that appears with tags trusted and disappears here is a stale
 * translation by construction. */
static bool g_paranoid;

void asid_set_paranoid(bool on)
{
    __atomic_store_n(&g_paranoid, on, __ATOMIC_RELEASE);
    kinfo("asid: paranoid mode %s%s", on ? "on" : "off",
          on ? ": every switch flushes, tags still allocated and written" : "");
}

bool asid_paranoid(void)
{
    return __atomic_load_n(&g_paranoid, __ATOMIC_ACQUIRE);
}

void asid_init(unsigned bits)
{
    KASSERT(bits == 0 || (bits >= 8 && bits <= ASID_MAX_BITS));
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    g_bits = bits;
    g_count = bits ? (1u << bits) : 0;
    g_next = 1;
    /*
     * Advance rather than reset. Re-initialising at a different width
     * (the self-test does) must not hand a generation number back to a
     * space that already carries it, or a tag issued under the old
     * regime would read as current under the new one.
     */
    g_generation++;
    memset(g_bitmap, 0, sizeof(g_bitmap));
    memset(&g_stats, 0, sizeof(g_stats));
    for (unsigned c = 0; c < CONFIG_MAX_CPUS; c++)
        g_cpu_generation[c] = 0;   /* every CPU is behind, so every CPU flushes next */
    spin_unlock_irqrestore(&g_lock, s);
    if (bits)
        kinfo("asid: %u-bit tags, %u usable", bits, g_count - 1);
    else
        kinfo("asid: no address-space tags on this machine; every switch flushes");
}

static bool bitmap_take(uint32_t tag)
{
    uint64_t mask = 1ull << (tag % 64u);
    if (g_bitmap[tag / 64u] & mask)
        return false;
    g_bitmap[tag / 64u] |= mask;
    return true;
}

/* Lock held. The next free tag at or after the cursor, wrapping once;
 * 0 when the bitmap is full. */
static uint32_t bitmap_next_free(void)
{
    for (uint32_t i = 0; i < g_count - 1; i++) {
        uint32_t tag = g_next + i;
        if (tag >= g_count)
            tag -= g_count - 1;   /* wrap past 0, which is never free */
        if (bitmap_take(tag)) {
            g_next = tag + 1 >= g_count ? 1 : tag + 1;
            return tag;
        }
    }
    return 0;
}

/* Lock held. Advance the generation and empty the bitmap: every tag
 * issued before now is stale, and every CPU will flush before it uses a
 * tag of the new generation. */
static void rollover(void)
{
    g_generation++;
    memset(g_bitmap, 0, sizeof(g_bitmap));
    g_next = 1;
    g_stats.rollovers++;
    kdebug("asid: rollover to generation %llu", (unsigned long long)g_generation);
}

bool asid_switch_prepare(struct arch_mmu_context *ctx)
{
    if (g_bits == 0)
        return true;   /* no tags: every switch flushes, as it always did */

    unsigned cpu = arch_cpu_id();
    uint64_t gen = __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
    bool flush = false;

    if (ctx != NULL && ctx->asid_gen != gen) {
        arch_irq_state_t s = spin_lock_irqsave(&g_lock);
        /* Re-read under the lock: another CPU may have rolled over
         * between the load above and here, which would make a tag
         * allocated from the old generation stale on arrival. */
        gen = g_generation;
        uint32_t tag = bitmap_next_free();
        if (tag == 0) {
            rollover();
            gen = g_generation;
            tag = bitmap_next_free();
            KASSERT(tag != 0);   /* an empty bitmap always has one */
        }
        g_stats.allocs++;
        spin_unlock_irqrestore(&g_lock, s);
        ctx->asid = tag;
        ctx->asid_gen = gen;
    }

    /* Whether this CPU may still hold tags from before a rollover. The
     * stamp is this CPU's own, and the caller has interrupts off, so
     * nothing can advance the generation and be missed here without
     * also being seen by the next switch on this CPU. */
    if (g_cpu_generation[cpu] != gen) {
        g_cpu_generation[cpu] = gen;
        __atomic_fetch_add(&g_stats.flushes, 1u, __ATOMIC_RELAXED);
        flush = true;
    }
    return flush || __atomic_load_n(&g_paranoid, __ATOMIC_ACQUIRE);
}

void asid_release(struct arch_mmu_context *ctx)
{
    if (g_bits == 0 || ctx->asid == 0)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    /* Only if the tag is still this space's: after a rollover the
     * bitmap belongs to a later generation and this bit is someone
     * else's, or free. */
    if (ctx->asid_gen == g_generation) {
        g_bitmap[ctx->asid / 64u] &= ~(1ull << (ctx->asid % 64u));
        g_stats.releases++;
    }
    spin_unlock_irqrestore(&g_lock, s);
    ctx->asid = 0;
    ctx->asid_gen = 0;
}

uint64_t asid_generation(void)
{
    return __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
}

void asid_get_stats(struct asid_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    *out = g_stats;
    spin_unlock_irqrestore(&g_lock, s);
}

bool asid_test_force_rollover(void)
{
#if CONFIG_DEBUG
    if (g_bits == 0)
        return false;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    rollover();
    spin_unlock_irqrestore(&g_lock, s);
    return true;
#else
    return false;
#endif
}

bool asid_test_set_bits(unsigned bits)
{
#if CONFIG_DEBUG
    if (bits < 8 || bits > ASID_MAX_BITS)
        return false;
    if (arch_mmu_asid_bits() == 0)
        return false;   /* the machine has no tags to narrow */
    asid_init(bits);
    return true;
#else
    (void)bits;
    return false;
#endif
}
