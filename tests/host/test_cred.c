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
    c.privs = PRIV_ALL;   /* as init starts */
    return c;
}

static struct credentials user(uint32_t uid, uint32_t gid)
{
    struct credentials c = root();
    c.ruid = c.euid = c.suid = uid;
    c.rgid = c.egid = c.sgid = gid;
    c.privs = 0;
    return c;
}

/* Roadmap M5: each operation names its privilege; uid 0 alone grants
 * nothing; privileges are only ever cleared. */
static void test_privilege(void)
{
    struct credentials r = root(), u = user(1000, 1000);
    static const uint32_t each[] = { PRIV_MOUNT, PRIV_SYS_ADMIN, PRIV_NET, PRIV_PROC, PRIV_CRED, PRIV_DEVICE };
    for (unsigned i = 0; i < sizeof(each) / sizeof(each[0]); i++) {
        EXPECT(cred_has(&r, each[i]));
        EXPECT(cred_has(&cred_kernel, each[i]));
        EXPECT(!cred_has(&u, each[i]));
    }
    EXPECT(!cred_has(&r, 0));
    EXPECT(PRIV_ALL == 0x3fu && COSMO_PRIV_COUNT <= 16);
    /* Effective uid 0 without privileges is nothing, and privileges with
     * another effective uid are dormant. */
    struct credentials z = root();
    z.privs = 0;
    EXPECT(!cred_has(&z, PRIV_MOUNT));
    struct credentials d = root();
    d.euid = 1000;
    EXPECT(!cred_has(&d, PRIV_MOUNT) && d.privs == PRIV_ALL);
    /* Dropping one leaves the others. */
    cred_drop(&r, PRIV_MOUNT);
    EXPECT(!cred_has(&r, PRIV_MOUNT) && cred_has(&r, PRIV_SYS_ADMIN));
    cred_drop(&r, PRIV_ALL);
    EXPECT(r.privs == 0);
}

static void test_setresuid_root(void)
{
    struct credentials c = root();
    EXPECT(cred_setresuid(&c, 1000, 1001, 1002) == 0);
    EXPECT(c.ruid == 1000 && c.euid == 1001 && c.suid == 1002);
    /* No id 0 is left: every privilege went with it. */
    EXPECT(c.privs == 0);
    /* With a 0 still among the ids, nothing is cleared -- but the bits act
     * only while the effective uid is 0 (Linux's effective set). */
    c = root();
    EXPECT(cred_setresuid(&c, -1, 1000, -1) == 0 && c.ruid == 0 && c.euid == 1000 && c.suid == 0);
    EXPECT(c.privs == PRIV_ALL);
    EXPECT(!cred_has(&c, PRIV_CRED) && !cred_has(&c, PRIV_SYS_ADMIN));   /* acting as uid 1000 */
    EXPECT(cred_setresuid(&c, 5, -1, -1) == -EPERM);                      /* so no foreign ids */
    EXPECT(cred_setresuid(&c, -1, 0, -1) == 0 && cred_has(&c, PRIV_CRED)); /* back: the bits act again */
    EXPECT(cred_setresuid(&c, 5, -1, -1) == 0 && c.ruid == 5);           /* PRIV_CRED: any ids */
    EXPECT(c.privs == PRIV_ALL);                                          /* the saved id is still 0 */
    EXPECT(cred_setresuid(&c, -1, 7, 5) == 0 && c.privs == 0);            /* and now no id is */
    /* Without PRIV_CRED a uid-0 process may only shuffle its own ids. */
    c = root();
    cred_drop(&c, PRIV_CRED);
    EXPECT(cred_setresuid(&c, 8, -1, -1) == -EPERM);
    EXPECT(cred_setresuid(&c, 0, 0, 0) == 0);
    EXPECT(c.ruid == 0 && c.euid == 0 && c.suid == 0 && c.privs == (PRIV_ALL & ~PRIV_CRED));
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
    /* A saved id lets a process switch back and forth, and an id change
     * never adds a privilege. */
    struct credentials s = user(1000, 1000);
    s.euid = 0;
    s.suid = 0;
    EXPECT(cred_setresuid(&s, -1, 1000, -1) == 0 && s.euid == 1000);
    EXPECT(cred_setresuid(&s, -1, 0, -1) == 0 && s.euid == 0 && s.privs == 0);
    /* Out-of-range values. */
    EXPECT(cred_setresuid(&c, -2, -1, -1) == -EINVAL);
    EXPECT(cred_setresuid(&c, 1ll << 32, -1, -1) == -EINVAL);
}

static void test_ids_changed(void)
{
    struct credentials c = root();
    c.ruid = c.euid = c.suid = 1000;
    cred_ids_changed(&c, 0, 0, 0);
    EXPECT(c.privs == 0);
    c = root();
    c.euid = 1000;
    cred_ids_changed(&c, 0, 0, 0);   /* real and saved are still 0 */
    EXPECT(c.privs == PRIV_ALL);
    c = root();
    c.ruid = c.euid = c.suid = 1000;
    cred_ids_changed(&c, 1000, 1000, 1000);   /* never held 0: nothing to clear, nothing added */
    EXPECT(c.privs == PRIV_ALL);
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
    /* Leaving uid 0 keeps the supplementary groups and drops the privileges. */
    EXPECT(cred_setgroups(&r, groups, 2) == 0);
    EXPECT(cred_setresuid(&r, 1000, 1000, 1000) == 0);
    EXPECT(cred_in_group(&r, 4) && !cred_has(&r, PRIV_CRED));
    /* uid 0 without PRIV_CRED changes neither groups nor gids freely. */
    struct credentials z = root();
    cred_drop(&z, PRIV_CRED);
    EXPECT(cred_setgroups(&z, groups, 1) == -EPERM);
    EXPECT(cred_setresgid(&z, 50, -1, -1) == -EPERM);
}

static void test_may_signal(void)
{
    struct credentials r = root(), a = user(1000, 1000), b = user(1001, 1001);
    EXPECT(cred_may_signal(&r, &a));
    EXPECT(cred_may_signal(&a, &a));
    EXPECT(!cred_may_signal(&a, &r));
    EXPECT(!cred_may_signal(&a, &b));
    /* uid 0 without PRIV_PROC signals only by the id rule. */
    struct credentials np = root();
    cred_drop(&np, PRIV_PROC);
    EXPECT(!cred_may_signal(&np, &a));
    EXPECT(cred_may_signal(&np, &r));
    /* A process of a's with effective id 0 may still signal a. */
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
    { "cred-ids-changed", test_ids_changed },
    { "cred-setresgid-groups", test_setresgid_and_groups },
    { "cred-may-signal", test_may_signal },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
