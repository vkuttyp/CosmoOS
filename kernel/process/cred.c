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

const struct credentials cred_kernel = { .privs = PRIV_ALL };

void cred_ids_changed(struct credentials *c, uint32_t old_ruid, uint32_t old_euid, uint32_t old_suid)
{
    bool had_root = old_ruid == CRED_ROOT_UID || old_euid == CRED_ROOT_UID || old_suid == CRED_ROOT_UID;
    bool has_root = c->ruid == CRED_ROOT_UID || c->euid == CRED_ROOT_UID || c->suid == CRED_ROOT_UID;
    if (had_root && !has_root)
        c->privs = 0;
}

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
    /* Privilege is judged before the change: a PRIV_CRED process may move
     * to any ids in one call; one without it may only shuffle its own. */
    uint32_t r = c->ruid, e = c->euid, s = c->suid;
    int rc = setres_common(&c->ruid, &c->euid, &c->suid, ruid, euid, suid, cred_has(c, PRIV_CRED));
    if (rc == 0)
        cred_ids_changed(c, r, e, s);
    return rc;
}

int cred_setresgid(struct credentials *c, int64_t rgid, int64_t egid, int64_t sgid)
{
    return setres_common(&c->rgid, &c->egid, &c->sgid, rgid, egid, sgid, cred_has(c, PRIV_CRED));
}

int cred_setgroups(struct credentials *c, const uint32_t *groups, unsigned n)
{
    if (n > CRED_NGROUPS_MAX)
        return -EINVAL;
    if (!cred_has(c, PRIV_CRED))
        return -EPERM;
    c->ngroups = n;
    for (unsigned i = 0; i < n; i++)
        c->groups[i] = groups[i];
    return 0;
}

bool cred_may_signal(const struct credentials *sender, const struct credentials *target)
{
    if (cred_has(sender, PRIV_PROC))
        return true;
    return sender->ruid == target->ruid || sender->ruid == target->suid || sender->euid == target->ruid ||
           sender->euid == target->suid;
}
