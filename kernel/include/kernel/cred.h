/*
 * cred.h - Process credentials and the privilege predicate
 * (docs/kernel/process/design.md, "Credentials").
 *
 * The security boundary is one predicate: cred_privileged(). A process
 * whose effective user id is 0 is root-equivalent; every privileged
 * operation in the kernel (mount, umount, signalling another user's
 * process, reading the kernel log, binding a reserved port, changing
 * credentials arbitrarily) asks this and nothing else, so the policy
 * lives in one place and a future capability set replaces the body of
 * one function. Discretionary access to files uses the effective ids and
 * the supplementary groups (vfs_permission).
 *
 * The struct is POSIX-shaped (real, effective, saved ids; supplementary
 * groups) so setresuid semantics are exactly the standard ones and the
 * Linux personality maps onto it without translation tables.
 *
 * Ownership: a process's credentials are written only by that process
 * (its own setres* calls, while it is single-threaded; under
 * process->lock once threads exist) and inherited by copy at spawn.
 * Other processes read them (kill, procinfo) without the lock: every
 * field is a naturally aligned 32-bit word and a torn view is at worst
 * an old id, never an invalid one.
 */

#ifndef KERNEL_CRED_H
#define KERNEL_CRED_H

#include <kernel/compiler.h>

#define CRED_NGROUPS_MAX 16u
#define CRED_ROOT_UID    0u

struct credentials {
    uint32_t ruid, euid, suid;
    uint32_t rgid, egid, sgid;
    uint32_t ngroups;
    uint32_t groups[CRED_NGROUPS_MAX];
};

/* The kernel's own identity: root, no supplementary groups. Kernel
 * threads and boot-time work run with these. */
extern const struct credentials cred_kernel;

/* Root-equivalent: may perform every privileged operation. */
static inline bool cred_privileged(const struct credentials *c)
{
    return c->euid == CRED_ROOT_UID;
}

/* The calling context's credentials: the current process's, or
 * cred_kernel on a kernel thread. Never NULL. Read-only for callers. */
const struct credentials *cred_current(void);

/* `gid` is the effective group or one of the supplementary groups. */
bool cred_in_group(const struct credentials *c, uint32_t gid);

/* POSIX setresuid/setresgid: each of r, e, s is a new id or -1 to keep
 * the current value. Unprivileged callers may set each id only to one of
 * their current real, effective or saved ids. Applies all or nothing;
 * -EPERM on refusal. */
int cred_setresuid(struct credentials *c, int64_t ruid, int64_t euid, int64_t suid);
int cred_setresgid(struct credentials *c, int64_t rgid, int64_t egid, int64_t sgid);

/* Replace the supplementary groups (privileged only; n <= CRED_NGROUPS_MAX). */
int cred_setgroups(struct credentials *c, const uint32_t *groups, unsigned n);

/* POSIX kill permission: privileged, or the sender's real or effective
 * uid equals the target's real or saved uid. */
bool cred_may_signal(const struct credentials *sender, const struct credentials *target);

#endif /* KERNEL_CRED_H */
