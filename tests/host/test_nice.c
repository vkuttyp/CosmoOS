/*
 * test_nice.c - Nice values' rules (kernel/process/nice.c, generated from
 * CeeSafe) on the host: the range, the permission rules, and how setpriority
 * and getpriority report a group. The inputs that matter are few enough
 * to try them all against Linux's rules written out here
 * (kernel/sys.c: set_one_prio_perm, can_nice, set_one_prio, getpriority),
 * so this replaces a fuzzer.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/nice.h>


static void test_clamp(void)
{
    for (int32_t v = -100; v <= 100; v++)
        EXPECT(nice_clamp(v) == (v < NICE_MIN ? NICE_MIN : v > NICE_MAX ? NICE_MAX : v));
    EXPECT(nice_clamp(INT32_MIN) == NICE_MIN && nice_clamp(INT32_MAX) == NICE_MAX);
}

/* Linux's rules, as kernel/sys.c and kernel/sched/core.c have them. */
static int linux_may_set(const struct nice_caller *c, const struct nice_target *t, int32_t nice)
{
    if (t->ruid != c->euid && t->euid != c->euid && !c->privileged)
        return -EPERM;
    if (nice < t->nice && !(20 - nice <= (int64_t)(t->rlimit > 64 ? 64 : t->rlimit) || c->privileged))
        return -EACCES;
    return 0;
}

static void test_may_set_exhaustive(void)
{
    static const uint64_t rlimits[] = { 0, 1, 19, 20, 21, 25, 39, 40, 41, 1000, UINT64_MAX };
    static const uint32_t uids[] = { 0, 7, 1000 };
    unsigned tried = 0;
    for (int priv = 0; priv <= 1; priv++)
        for (unsigned ce = 0; ce < 3; ce++)
            for (unsigned tr = 0; tr < 3; tr++)
                for (unsigned te = 0; te < 3; te++)
                    for (unsigned r = 0; r < sizeof(rlimits) / sizeof(rlimits[0]); r++)
                        for (int32_t cur = NICE_MIN; cur <= NICE_MAX; cur++)
                            for (int32_t want = NICE_MIN; want <= NICE_MAX; want++) {
                                struct nice_caller c = { uids[ce], priv != 0 };
                                struct nice_target t = { uids[tr], uids[te], cur, rlimits[r] };
                                EXPECT(nice_may_set(&c, &t, want) == linux_may_set(&c, &t, want));
                                tried++;
                            }
    EXPECT(tried == 2 * 27 * 11 * 40 * 40);
}

static void test_may_set_cases(void)
{
    struct nice_caller user = { 1000, false }, root = { 0, true }, rootless = { 0, false };
    struct nice_target own = { 1000, 1000, 0, 0 }, other = { 0, 0, 0, 0 };
    EXPECT(nice_may_set(&user, &own, 19) == 0);            /* raising is free */
    EXPECT(nice_may_set(&user, &own, 0) == 0);             /* so is keeping it */
    EXPECT(nice_may_set(&user, &own, -1) == -EACCES);      /* the default limit allows no lowering */
    own.nice = 10;
    EXPECT(nice_may_set(&user, &own, 5) == -EACCES);       /* nor back down after raising */
    own.rlimit = 25;                                       /* down to 20 - 25 = -5 */
    EXPECT(nice_may_set(&user, &own, -5) == 0 && nice_may_set(&user, &own, -6) == -EACCES);
    own.rlimit = UINT64_MAX;                               /* RLIM_INFINITY */
    EXPECT(nice_may_set(&user, &own, NICE_MIN) == 0);
    EXPECT(nice_may_set(&user, &other, 5) == -EPERM);      /* another user's, even upwards */
    struct nice_target setuid_like = { 0, 1000, 0, 0 };    /* its effective uid is the caller's */
    EXPECT(nice_may_set(&user, &setuid_like, 5) == 0);
    struct nice_target dropped = { 1000, 0, 0, 0 };       /* its real uid is */
    EXPECT(nice_may_set(&user, &dropped, 5) == 0);
    EXPECT(nice_may_set(&root, &own, NICE_MIN) == 0 && nice_may_set(&root, &other, NICE_MIN) == 0);
    /* Uid 0 without the privilege is a user like another: its own
     * processes, and no lowering past the limit. */
    EXPECT(nice_may_set(&rootless, &other, 5) == 0 && nice_may_set(&rootless, &other, -1) == -EACCES);
    EXPECT(nice_may_set(&rootless, &own, 5) == -EPERM);
}

static void test_set_result(void)
{
    /* None named: -ESRCH. One: its outcome. */
    EXPECT(nice_set_result(-ESRCH, 0) == 0);
    EXPECT(nice_set_result(-ESRCH, -EPERM) == -EPERM);
    /* Several: the last refusal stands, a later success does not clear it,
     * and a success after none keeps 0 (Linux's set_one_prio). */
    int r = -ESRCH;
    r = nice_set_result(r, 0);
    r = nice_set_result(r, -EACCES);
    r = nice_set_result(r, 0);
    EXPECT(r == -EACCES);
    r = nice_set_result(r, -EPERM);
    EXPECT(r == -EPERM);
    EXPECT(nice_set_result(nice_set_result(-ESRCH, 0), 0) == 0);
}

static void test_get_result(void)
{
    EXPECT(nice_get_result(-ESRCH, 0) == 20);
    EXPECT(nice_get_result(-ESRCH, NICE_MAX) == 1 && nice_get_result(-ESRCH, NICE_MIN) == 40);
    /* A group reports its lowest value: the highest 20 - nice. */
    int r = -ESRCH;
    r = nice_get_result(r, 8);
    r = nice_get_result(r, 2);
    r = nice_get_result(r, 11);
    EXPECT(r == 18);
    for (int32_t a = NICE_MIN; a <= NICE_MAX; a++)
        for (int32_t b = NICE_MIN; b <= NICE_MAX; b++)
            EXPECT(nice_get_result(nice_get_result(-ESRCH, a), b) == 20 - (a < b ? a : b));
}

static const struct host_test tests[] = {
    { "clamp", test_clamp },
    { "permission, every input", test_may_set_exhaustive },
    { "permission, named cases", test_may_set_cases },
    { "setpriority's result", test_set_result },
    { "getpriority's result", test_get_result },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
