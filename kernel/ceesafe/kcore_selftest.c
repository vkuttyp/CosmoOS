/*
 * kcore_selftest.c - The self-test cee-kcore: CeeSafe's kernel library
 * (lib/kernel) in code built into the kernel.
 *
 * The checks are CeeSafe (kernel/ceesafe/kcore_test.cee, generated into
 * kcore_test.c by `make cee`); this names the one that failed. First it
 * checks, in C, that MUTEX_INIT_AT, which CeeSafe's static Mutex uses, leaves
 * a mutex as mutex_init does.
 */

#include <stddef.h>
#include <stdint.h>

#include "kernel/mutex.h"
#include "kernel/selftest.h"

uint32_t kcore_cee_selftest(void);

static const char *const kcore_failures[] = {
    [1] = "a static spin-locked table gave the wrong slots",
    [2] = "a static spin-locked table lost a value",
    [3] = "a static irqsave spin lock lost a value",
    [4] = "this CPU's per-CPU value did not count up by one",
    [5] = "a boxed spin lock was not made or lost its value",
    [6] = "a boxed mutex was not made or lost a change",
    [7] = "a kernel object's clone did not take a count",
    [8] = "a kernel object's last drop did not drop its data once",
    [9] = "a deferred free could not allocate",
    [10] = "a static mutex lost a change",
    [11] = "an object cache was not made, or its box lost the value",
    [12] = "an object cache box's drop did not drop its value once",
};

static struct mutex kcore_static_mutex = MUTEX_INIT_AT(&kcore_static_mutex, "cee.kcore.init-at");

static bool same_spinlock(const spinlock_t *a, const spinlock_t *b)
{
    return a->locked == b->locked && a->owner_cpu == b->owner_cpu && a->name == b->name && a->class == b->class;
}

/* Whether a list head of `a` and the same one of `b` both point to themselves. */
static bool both_empty(const struct list_node *a, const struct list_node *b)
{
    return a->next == a && a->prev == a && b->next == b && b->prev == b;
}

/* MUTEX_INIT_AT against mutex_init, member by member: the self-pointers each to its own mutex. Run before the static
 * mutex is first locked, which caches its lockdep class. */
static const char *mutex_init_at_matches(void)
{
    struct mutex made;

    mutex_init(&made, "cee.kcore.init-at");
    const struct mutex *s = &kcore_static_mutex;
    if (!same_spinlock(&s->lock, &made.lock) || s->owner != made.owner || s->name != made.name || s->lock_class != made.lock_class
        || s->pi_active != made.pi_active || s->handoff != made.handoff)
        return "MUTEX_INIT_AT and mutex_init leave different scalar members";
    if (!same_spinlock(&s->wq.lock, &made.wq.lock))
        return "MUTEX_INIT_AT and mutex_init leave different wait-queue locks";
    if (!both_empty(&s->wq.waiters, &made.wq.waiters) || !both_empty(&s->wq.callbacks, &made.wq.callbacks)
        || !both_empty(&s->pi_link, &made.pi_link))
        return "MUTEX_INIT_AT leaves a list head that does not point to itself";
    mutex_lock(&kcore_static_mutex);
    mutex_unlock(&kcore_static_mutex);
    return NULL;
}

bool selftest_cee_kcore(const char **reason)
{
    const char *init_at = mutex_init_at_matches();

    if (init_at) {
        *reason = init_at;
        return false;
    }
    uint32_t failed = kcore_cee_selftest();

    if (failed == 0)
        return true;
    *reason = failed < sizeof(kcore_failures) / sizeof(kcore_failures[0]) && kcore_failures[failed]
                  ? kcore_failures[failed]
                  : "an unknown check failed";
    return false;
}
