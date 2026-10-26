/*
 * modcycle.c - Load and unload one module many times through the real
 * loader (module_load/module_unload) and check that nothing is left
 * behind (invariant M27). Debug builds only: a test seam
 * (docs/kernel/module/design.md, "Load and unload in cycles").
 *
 * Two callers: the self-test module-cycle, on the cosmotest fixture, and
 * module_cycle_boot, which takes `module.cycle=<archive entry>:<count>
 * [:<hold ms>]` from the kernel command line so that a module built
 * outside this tree (Nova's C8 module, for one) can be cycled by the
 * kernel itself. The loader API stays unexported.
 */

#include <kernel/bootarchive.h>
#include <kernel/cmdline.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <kernel/vmm.h>

#if CONFIG_DEBUG

static void snapshot(struct module_cycle_count *c)
{
    struct vm_stats vm;
    struct kmalloc_stats heap;
    struct lockdep_stats locks;
    vm_get_stats(&vm);
    kmalloc_get_stats(&heap);
    lockdep_get_stats(&locks);
    c->modules = module_count();
    c->heap_objects = heap.live_objects;
    c->large_pages = heap.large_pages;
    c->vm_regions = vm.regions;
    c->anon_pages = vm.anon_pages;
    c->lock_classes = locks.classes;
}

static bool counts_equal(const struct module_cycle_count *a, const struct module_cycle_count *b)
{
    return a->modules == b->modules && a->heap_objects == b->heap_objects && a->large_pages == b->large_pages &&
           a->vm_regions == b->vm_regions && a->anon_pages == b->anon_pages;
}

#define SETTLE_STEP_NS 10000000ULL   /* 10 ms between readings */

/* The counts are machine-wide, and the network's workers (a DHCP
 * exchange, a neighbour entry) allocate while a run goes on. The baseline
 * is two equal readings 10 ms apart; the last reading is taken again until
 * it equals the baseline, for MODULE_CYCLE_SETTLE_MS at most. A buffer
 * held for a moment is gone by then; a leak never is (design.md, "Load
 * and unload in cycles"). */
/* Both bounded by the clock, not by a count of sleeps: a 10 ms sleep
 * lasts longer under TCG. Each returns the milliseconds it waited. */
static unsigned snapshot_quiet(struct module_cycle_count *c)
{
    uint64_t t0 = clock_now_ns();
    snapshot(c);
    while (clock_since_ns(t0) < MODULE_CYCLE_SETTLE_MS * 1000000ULL) {
        struct module_cycle_count again;
        thread_sleep_ns(SETTLE_STEP_NS);
        snapshot(&again);
        if (counts_equal(&again, c))
            break;
        *c = again;
    }
    return (unsigned)(clock_since_ns(t0) / 1000000ULL);
}

static unsigned snapshot_until(struct module_cycle_count *c, const struct module_cycle_count *want)
{
    uint64_t t0 = clock_now_ns();
    snapshot(c);
    while (!counts_equal(c, want) && clock_since_ns(t0) < MODULE_CYCLE_SETTLE_MS * 1000000ULL) {
        thread_sleep_ns(SETTLE_STEP_NS);
        snapshot(c);
    }
    return (unsigned)(clock_since_ns(t0) / 1000000ULL);
}

bool module_cycle(const void *file, size_t size, const char *origin, unsigned count, unsigned hold_ms,
                  module_cycle_live_fn live, void *arg, struct module_cycle_result *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < count; i++) {
        /* The baseline is taken after one cycle: what the first load makes
         * once and every later one reuses is not a leak. */
        if (i == 1)
            out->settle_before_ms = snapshot_quiet(&out->before);
        struct module *m = NULL;
        int rc = module_load(file, size, origin, &m);
        if (rc != 0) {
            out->load_failures++;
            if (out->first_error == 0)
                out->first_error = rc;
            continue;
        }
        /* `m` is borrowed until the unload: keep the name, not the pointer. */
        char name[MODULE_NAME_MAX];
        strlcpy(name, m->name, sizeof(name));
        if (live != NULL)
            live(m, i, arg);
        if (hold_ms != 0)
            thread_sleep_ns((uint64_t)hold_ms * 1000000ULL);
        rc = module_unload(name);
        if (rc != 0) {
            out->unload_failures++;   /* -EBUSY: a zombie, or a dependant */
            if (out->first_error == 0)
                out->first_error = rc;
        }
        out->cycles++;
    }
    if (count < 2)
        out->settle_before_ms = snapshot_quiet(&out->before);
    out->settle_after_ms = snapshot_until(&out->after, &out->before);
    /* lockdep's classes are reported, not judged: a callback's class is
     * named after its address, so a module whose callback lands at a new
     * address adds a class that stays (backlog LOCK-10). */
    return out->load_failures == 0 && out->unload_failures == 0 && counts_equal(&out->after, &out->before);
}

/* Digits to a number no larger than `max`; false for none, anything
 * else, or more. */
static bool parse_number(const char *p, unsigned max, unsigned *out)
{
    uint64_t n = 0;
    if (*p == '\0')
        return false;
    for (; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
        n = n * 10 + (uint64_t)(*p - '0');
        if (n > max)
            return false;
    }
    *out = (unsigned)n;
    return true;
}

bool module_cycle_parse(char *spec, unsigned *count, unsigned *hold_ms)
{
    char *fields[3] = { spec, NULL, NULL };
    unsigned n = 1;
    for (char *p = spec; *p; p++) {
        if (*p != ':')
            continue;
        if (n == 3)
            return false;
        *p = '\0';
        fields[n++] = p + 1;
    }
    *hold_ms = 0;
    return n >= 2 && fields[0][0] != '\0' && parse_number(fields[1], MODULE_CYCLE_MAX, count) && *count > 0 &&
           (n == 2 || parse_number(fields[2], MODULE_CYCLE_HOLD_MAX_MS, hold_ms));
}

unsigned module_cycle_boot(void)
{
    char spec[128];
    int len = cmdline_get("module.cycle", spec, sizeof(spec));
    if (len == -1)
        return 0;   /* not asked */
    unsigned count, hold_ms;
    if (len < 0 || !module_cycle_parse(spec, &count, &hold_ms)) {
        kerror("modcycle: module.cycle wants <archive entry>:<count>[:<hold ms>] (count 1..%u, hold 0..%u)",
               MODULE_CYCLE_MAX, MODULE_CYCLE_HOLD_MAX_MS);
        return 1;
    }
    const void *file;
    size_t size;
    if (!bootarchive_find(spec, &file, &size)) {
        kerror("modcycle: %s: not in the boot archive", spec);
        return 1;
    }
    struct module_cycle_result r;
    uint64_t t0 = clock_now_ns();
    bool ok = module_cycle(file, size, spec, count, hold_ms, NULL, NULL, &r);
    uint64_t took_ms = clock_since_ns(t0) / 1000000ULL;
    kinfo("modcycle: %s: %u cycles (hold %u ms): %s in %llu ms (load failures %u, unload failures %u, "
          "first error %d; modules %u -> %u; kmalloc live %llu -> %llu, large pages %llu -> %llu; "
          "vm regions %llu -> %llu, anon pages %llu -> %llu; lockdep classes %u -> %u; waited %u + %u ms for quiet)",
          spec, r.cycles, hold_ms, ok ? "PASS" : "FAIL", (unsigned long long)took_ms, r.load_failures,
          r.unload_failures, r.first_error, r.before.modules, r.after.modules,
          (unsigned long long)r.before.heap_objects,
          (unsigned long long)r.after.heap_objects, (unsigned long long)r.before.large_pages,
          (unsigned long long)r.after.large_pages, (unsigned long long)r.before.vm_regions,
          (unsigned long long)r.after.vm_regions, (unsigned long long)r.before.anon_pages,
          (unsigned long long)r.after.anon_pages, r.before.lock_classes, r.after.lock_classes, r.settle_before_ms,
          r.settle_after_ms);
    return ok ? 0 : 1;
}

#else

unsigned module_cycle_boot(void)
{
    return 0;
}

#endif
