/*
 * test_lockup.c - Host test of the lockup unit's pure rule
 * (kernel/include/kernel/lockup_core.h, docs/kernel/diagnostics/
 * testing.md, "Lockups"): the hard-lockup watcher over online masks
 * with holes, which the machine itself cannot make (no CPU hotplug).
 */

#include "harness.h"

#include <kernel/lockup_core.h>

/* Every online CPU has exactly one watcher; nobody watches an offline
 * CPU; a lone CPU's target is itself. */
static void check_mask(uint64_t online)
{
    unsigned watchers[64] = { 0 };
    unsigned count = 0;
    for (unsigned c = 0; c < 64; c++)
        if (online & ((uint64_t)1 << c))
            count++;
    for (unsigned c = 0; c < 64; c++) {
        if (!(online & ((uint64_t)1 << c)))
            continue;
        unsigned t = lockup_watch_target(online, c);
        EXPECT(t < 64);
        EXPECT(online & ((uint64_t)1 << t));        /* never an offline target */
        if (count == 1)
            EXPECT(t == c);                          /* alone: itself */
        else
            EXPECT(t != c);                          /* otherwise never itself */
        watchers[t]++;
    }
    for (unsigned c = 0; c < 64; c++) {
        if (online & ((uint64_t)1 << c))
            EXPECT(watchers[c] == 1);
        else
            EXPECT(watchers[c] == 0);
    }
}

static void test_holes(void)
{
    check_mask(0x0Du);                 /* {0,2,3}: 0 -> 2, 2 -> 3, 3 -> 0 */
    EXPECT(lockup_watch_target(0x0Du, 0) == 2);
    EXPECT(lockup_watch_target(0x0Du, 2) == 3);
    EXPECT(lockup_watch_target(0x0Du, 3) == 0);
    check_mask(0x0Fu);                 /* {0,1,2,3} */
    EXPECT(lockup_watch_target(0x0Fu, 3) == 0);
    check_mask(0x02u);                 /* {1}: alone */
    EXPECT(lockup_watch_target(0x02u, 1) == 1);
    check_mask(0x8000000000000001ull); /* {0,63}: the top bit wraps */
    EXPECT(lockup_watch_target(0x8000000000000001ull, 63) == 0);
    EXPECT(lockup_watch_target(0x8000000000000001ull, 0) == 63);
    check_mask(0xFFFFFFFFFFFFFFFFull);
    EXPECT(lockup_watch_target(0xFFFFFFFFFFFFFFFFull, 63) == 0);
}

/* The bug the rule replaces: (k + 1) mod n leaves 2 unwatched in {0,2,3}. */
static void test_negative_model(void)
{
    uint64_t online = 0x0Du;
    unsigned n = 4, watchers[4] = { 0 };
    for (unsigned c = 0; c < n; c++) {
        if (!(online & ((uint64_t)1 << c)))
            continue;
        unsigned t = (c + 1) % n;
        if (online & ((uint64_t)1 << t))
            watchers[t]++;
    }
    EXPECT(watchers[2] == 0);          /* CPU 1 is offline, so 0's target is a hole */
    EXPECT(watchers[3] == 1 && watchers[0] == 1);
}

static const struct host_test tests[] = {
    { "holes", test_holes },
    { "negative-model", test_negative_model },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
