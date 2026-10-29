/*
 * kcore_selftest.c - The self-test nova-kcore: Nova's kernel library
 * (lib/kernel) in code built into the kernel.
 *
 * The checks are Nova (kernel/nova/kcore_test.nova, generated into
 * kcore_test.c by `make nova`); this names the one that failed.
 */

#include <stddef.h>
#include <stdint.h>

#include "kernel/selftest.h"

uint32_t kcore_nova_selftest(void);

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
};

bool selftest_nova_kcore(const char **reason)
{
    uint32_t failed = kcore_nova_selftest();

    if (failed == 0)
        return true;
    *reason = failed < sizeof(kcore_failures) / sizeof(kcore_failures[0]) && kcore_failures[failed]
                  ? kcore_failures[failed]
                  : "an unknown check failed";
    return false;
}
