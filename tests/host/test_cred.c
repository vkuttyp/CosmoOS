/*
 * test_cred.c - Host test of the credential rules (kernel/process/cred.c,
 * docs/kernel/process/testing.md). ASan/UBSan.
 */

#include "harness.h"

#include <kernel/cred.h>
#include <kernel/errno.h>

#include <string.h>

static struct credentials root(void)
{
    struct credentials c;
    memset(&c, 0, sizeof(c));
    return c;
}

static struct credentials user(uint32_t uid, uint32_t gid)
{
    struct credentials c = root();
    c.ruid = c.euid = c.suid = uid;
    c.rgid = c.egid = c.sgid = gid;
    return c;
}

static void test_privilege(void)
{
    struct credentials r = root(), u = user(1000, 1000);
    EXPECT(cred_privileged(&r));
    EXPECT(!cred_privileged(&u));
    EXPECT(cred_privileged(&cred_kernel));
    /* Only the effective id decides. */
    u.euid = 0;
    EXPECT(cred_privileged(&u));
    u.euid = 1000;
    u.ruid = 0;
    EXPECT(!cred_privileged(&u));
}

static void test_setresuid_root(void)
{
    struct credentials c = root();
    EXPECT(cred_setresuid(&c, 1000, 1001, 1002) == 0);
    EXPECT(c.ruid == 1000 && c.euid == 1001 && c.suid == 1002);
    /* Root may set anything, including in one call that drops privilege. */
    c = root();
    EXPECT(cred_setresuid(&c, -1, 1000, -1) == 0 && c.ruid == 0 && c.euid == 1000 && c.suid == 0);
    /* Now unprivileged: it can only shuffle its own three ids. */
    EXPECT(cred_setresuid(&c, 5, -1, -1) == -EPERM);
    EXPECT(cred_setresuid(&c, -1, 0, -1) == 0 && c.euid == 0);     /* saved/real is 0: allowed */
    EXPECT(cred_setresuid(&c, 7, 7, 7) == 0);                      /* root again */
    EXPECT(cred_setresuid(&c, 8, -1, -1) == -EPERM);
    EXPECT(c.ruid == 7 && c.euid == 7 && c.suid == 7);
}

static void test_setresuid_user(void)
{
    struct credentials c = user(1000, 1000);
    EXPECT(cred_setresuid(&c, -1, -1, -1) == 0);
    EXPECT(cred_setresuid(&c, 1000, 1000, 1000) == 0);
    EXPECT(cred_setresuid(&c, 0, -1, -1) == -EPERM);
    EXPECT(cred_setresuid(&c, -1, 0, -1) == -EPERM);
    EXPECT(cred_setresuid(&c, -1, -1, 0) == -EPERM);
    EXPECT(cred_setresuid(&c, 1000, 1000, 2000) == -EPERM);
    /* All or nothing: a refused call changes no id. */
    EXPECT(c.ruid == 1000 && c.euid == 1000 && c.suid == 1000);
    /* A saved id lets a setuid program switch back and forth. */
    struct credentials s = user(1000, 1000);
    s.euid = 0;
    s.suid = 0;
    EXPECT(!cred_privileged(&s) == false);
    EXPECT(cred_setresuid(&s, -1, 1000, -1) == 0 && !cred_privileged(&s));
    EXPECT(cred_setresuid(&s, -1, 0, -1) == 0 && cred_privileged(&s));
    /* Out-of-range values. */
    EXPECT(cred_setresuid(&c, -2, -1, -1) == -EINVAL);
    EXPECT(cred_setresuid(&c, 1ll << 32, -1, -1) == -EINVAL);
}

static void test_setresgid_and_groups(void)
{
    struct credentials c = user(1000, 1000);
    EXPECT(cred_setresgid(&c, 0, -1, -1) == -EPERM);
    EXPECT(cred_setresgid(&c, 1000, 1000, 1000) == 0);
    uint32_t groups[3] = { 4, 20, 27 };
    EXPECT(cred_setgroups(&c, groups, 3) == -EPERM);
    EXPECT(c.ngroups == 0);
    EXPECT(cred_in_group(&c, 1000));
    EXPECT(!cred_in_group(&c, 20));

    struct credentials r = root();
    EXPECT(cred_setgroups(&r, groups, 3) == 0 && r.ngroups == 3);
    EXPECT(cred_in_group(&r, 20) && cred_in_group(&r, 27) && !cred_in_group(&r, 5));
    EXPECT(cred_setgroups(&r, groups, CRED_NGROUPS_MAX + 1) == -EINVAL);
    EXPECT(cred_setgroups(&r, groups, 0) == 0 && r.ngroups == 0);
    /* Dropping privilege keeps the supplementary groups. */
    EXPECT(cred_setgroups(&r, groups, 2) == 0);
    EXPECT(cred_setresuid(&r, 1000, 1000, 1000) == 0);
    EXPECT(cred_in_group(&r, 4) && !cred_privileged(&r));
}

static void test_may_signal(void)
{
    struct credentials r = root(), a = user(1000, 1000), b = user(1001, 1001);
    EXPECT(cred_may_signal(&r, &a));
    EXPECT(cred_may_signal(&a, &a));
    EXPECT(!cred_may_signal(&a, &r));
    EXPECT(!cred_may_signal(&a, &b));
    /* A setuid program of a's (real 1000, effective 0) may still signal a. */
    struct credentials su = a;
    su.euid = 0;
    EXPECT(cred_may_signal(&su, &a));
    /* Sender's effective id matches the target's saved id. */
    struct credentials t = b;
    t.suid = 1000;
    EXPECT(cred_may_signal(&a, &t));
    /* Matching only the target's effective id is not enough. */
    struct credentials t2 = b;
    t2.euid = 1000;
    EXPECT(!cred_may_signal(&a, &t2));
}

static const struct host_test tests[] = {
    { "cred-privilege", test_privilege },
    { "cred-setresuid-root", test_setresuid_root },
    { "cred-setresuid-user", test_setresuid_user },
    { "cred-setresgid-groups", test_setresgid_and_groups },
    { "cred-may-signal", test_may_signal },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
