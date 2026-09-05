/*
 * faultinject.c - Fault injection rules and their hook
 * (docs/verification/design.md, "Fault injection"). Debug builds only.
 */

#include <kernel/faultinject.h>

#if CONFIG_FAULTINJECT

#include <kernel/errno.h>
#include <kernel/fwcfg.h>
#include <kernel/log.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/thread.h>

struct fi_rule {
    unsigned every;          /* 0: disarmed */
    unsigned budget;         /* remaining failures; 0: unlimited */
    struct thread *only;     /* NULL: every thread */
    uint64_t seen, hits;
};

static struct fi_rule g_rules[FI_KIND_COUNT];

static const char *const g_names[FI_KIND_COUNT] = {
    [FI_KMALLOC] = "kmalloc",
    [FI_BLK_SUBMIT] = "blk-submit",
    [FI_BLK_COMPLETE] = "blk-complete",
};

const char *faultinject_kind_name(enum fi_kind kind)
{
    return (unsigned)kind < FI_KIND_COUNT ? g_names[kind] : "?";
}

void faultinject_set(enum fi_kind kind, unsigned every, unsigned budget, struct thread *only)
{
    struct fi_rule *r = &g_rules[kind];
    /* Disarm first so a reader never sees a half-written rule fire. */
    __atomic_store_n(&r->every, 0u, __ATOMIC_RELEASE);
    r->budget = budget;
    r->only = only;
    r->seen = r->hits = 0;
    __atomic_store_n(&r->every, every, __ATOMIC_RELEASE);
}

void faultinject_clear(enum fi_kind kind)
{
    __atomic_store_n(&g_rules[kind].every, 0u, __ATOMIC_RELEASE);
    g_rules[kind].only = NULL;
}

void faultinject_clear_all(void)
{
    for (unsigned k = 0; k < FI_KIND_COUNT; k++)
        faultinject_clear((enum fi_kind)k);
}

void faultinject_stats(enum fi_kind kind, struct fi_stats *out)
{
    const struct fi_rule *r = &g_rules[kind];
    out->every = r->every;
    out->budget = r->budget;
    out->only_thread = r->only != NULL;
    out->seen = r->seen;
    out->hits = r->hits;
}

/*
 * The hot path: one load when nothing is armed. Counters are relaxed
 * atomics; a rule targets one thread in practice, so a stale read only
 * moves a failure by an event. Never fails in interrupt context: the
 * rules describe thread work, and an allocation failure in a handler
 * would land in code that has no caller to report to.
 */
bool faultinject_should_fail(enum fi_kind kind)
{
    struct fi_rule *r = &g_rules[kind];
    unsigned every = __atomic_load_n(&r->every, __ATOMIC_ACQUIRE);
    if (every == 0)
        return false;
    struct percpu *pc = this_cpu();
    if (pc->irq_depth != 0)
        return false;
    if (r->only != NULL && r->only != pc->current)
        return false;
    uint64_t n = __atomic_add_fetch(&r->seen, 1u, __ATOMIC_RELAXED);
    if (n % every != 0)
        return false;
    if (r->budget != 0) {
        /* A budget of one failure left: take it and disarm. */
        unsigned b = __atomic_load_n(&r->budget, __ATOMIC_ACQUIRE);
        if (b == 0)
            return false;
        if (!__atomic_compare_exchange_n(&r->budget, &b, b - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return false;
        if (b == 1)
            __atomic_store_n(&r->every, 0u, __ATOMIC_RELEASE);
    }
    __atomic_fetch_add(&r->hits, 1u, __ATOMIC_RELAXED);
    return true;
}

/* "kmalloc:7", "blk-complete:2:10", comma separated. */
int faultinject_configure(const char *spec)
{
    const char *p = spec;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char item[64];
        if (len >= sizeof(item))
            return -EINVAL;
        memcpy(item, p, len);
        item[len] = '\0';
        char *colon = strchr(item, ':');
        if (colon == NULL)
            return -EINVAL;
        *colon = '\0';
        int kind = -1;
        for (unsigned k = 0; k < FI_KIND_COUNT; k++)
            if (strcmp(item, g_names[k]) == 0)
                kind = (int)k;
        if (kind < 0)
            return -EINVAL;
        unsigned every = 0, budget = 0;
        char *q = colon + 1;
        while (*q >= '0' && *q <= '9')
            every = every * 10 + (unsigned)(*q++ - '0');
        if (*q == ':') {
            q++;
            while (*q >= '0' && *q <= '9')
                budget = budget * 10 + (unsigned)(*q++ - '0');
        }
        if (*q != '\0' || every == 0)
            return -EINVAL;
        faultinject_set((enum fi_kind)kind, every, budget, NULL);
        kinfo("faultinject: %s fails every %u%s", g_names[kind], every, budget ? " (bounded)" : "");
        p = end ? end + 1 : p + len;
    }
    return 0;
}

void faultinject_init(void)
{
    char spec[128];
    if (fwcfg_get_string("faultinject", spec, sizeof(spec)) && faultinject_configure(spec) != 0)
        kwarn("faultinject: ignoring malformed opt/cosmo/faultinject '%s'", spec);
}

int faultinject_sysctl(char *out, size_t n)
{
    int len = 0;
    for (unsigned k = 0; k < FI_KIND_COUNT; k++) {
        const struct fi_rule *r = &g_rules[k];
        int m = ksnprintf(out + (len < (int)n ? len : (int)n), len < (int)n ? n - (size_t)len : 0,
                          "%s every=%u budget=%u thread=%s seen=%llu hits=%llu\n", g_names[k], r->every, r->budget,
                          r->only ? "one" : "all", (unsigned long long)r->seen, (unsigned long long)r->hits);
        len += m;
    }
    return len;
}

#endif /* CONFIG_FAULTINJECT */
