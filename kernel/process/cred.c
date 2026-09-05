/*
 * cred.c - Credential rules (docs/kernel/process/design.md, "Credentials").
 *
 * Pure functions over struct credentials: no locking, no process access,
 * so the rules are host-testable (tests/host/test_cred.c). The current
 * process's credentials are reached through cred_current() in process.c.
 */

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/string.h>

const struct credentials cred_kernel = { 0 };

bool cred_in_group(const struct credentials *c, uint32_t gid)
{
    if (c->egid == gid)
        return true;
    for (unsigned i = 0; i < c->ngroups && i < CRED_NGROUPS_MAX; i++) {
        if (c->groups[i] == gid)
            return true;
    }
    return false;
}

/* An unprivileged process may pick, for each of the three ids, only a
 * value it already holds in one of them. */
static bool one_of_own(uint32_t want, uint32_t r, uint32_t e, uint32_t s)
{
    return want == r || want == e || want == s;
}

static int setres_common(uint32_t *r, uint32_t *e, uint32_t *s, int64_t nr, int64_t ne, int64_t ns,
                         bool privileged)
{
    if (nr < -1 || nr > 0xFFFFFFFFll || ne < -1 || ne > 0xFFFFFFFFll || ns < -1 || ns > 0xFFFFFFFFll)
        return -EINVAL;
    uint32_t wr = nr == -1 ? *r : (uint32_t)nr;
    uint32_t we = ne == -1 ? *e : (uint32_t)ne;
    uint32_t ws = ns == -1 ? *s : (uint32_t)ns;
    if (!privileged) {
        if (!one_of_own(wr, *r, *e, *s) || !one_of_own(we, *r, *e, *s) || !one_of_own(ws, *r, *e, *s))
            return -EPERM;
    }
    *r = wr;
    *e = we;
    *s = ws;
    return 0;
}

int cred_setresuid(struct credentials *c, int64_t ruid, int64_t euid, int64_t suid)
{
    /* Privilege is judged before the change: a root process may drop to
     * any ids in one call; a process that is not root may only shuffle. */
    return setres_common(&c->ruid, &c->euid, &c->suid, ruid, euid, suid, cred_privileged(c));
}

int cred_setresgid(struct credentials *c, int64_t rgid, int64_t egid, int64_t sgid)
{
    return setres_common(&c->rgid, &c->egid, &c->sgid, rgid, egid, sgid, cred_privileged(c));
}

int cred_setgroups(struct credentials *c, const uint32_t *groups, unsigned n)
{
    if (n > CRED_NGROUPS_MAX)
        return -EINVAL;
    if (!cred_privileged(c))
        return -EPERM;
    c->ngroups = n;
    for (unsigned i = 0; i < n; i++)
        c->groups[i] = groups[i];
    return 0;
}

bool cred_may_signal(const struct credentials *sender, const struct credentials *target)
{
    if (cred_privileged(sender))
        return true;
    return sender->ruid == target->ruid || sender->ruid == target->suid || sender->euid == target->ruid ||
           sender->euid == target->suid;
}
