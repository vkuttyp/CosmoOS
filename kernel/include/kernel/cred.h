/*
 * cred.h - Process credentials and operation-specific privileges
 * (docs/kernel/security/design.md §1, roadmap M5).
 *
 * Every privileged operation names the one privilege it needs and asks
 * cred_has(). A process's privileges are a small fixed bitmask (PRIV_*,
 * COSMO_PRIV_* in the ABI): init starts with all of them; they are
 * inherited by fork, spawn and exec and can only ever be dropped -- spawn
 * takes a mask to drop, and an id change that leaves a process with no id
 * 0 drops them all, as Linux clears capabilities on setuid. No call adds
 * one, and effective uid 0 by itself grants nothing; a privilege acts only
 * while the effective uid is 0 (cred_has). Discretionary access
 * to files uses the effective ids and the supplementary groups
 * (vfs_permission); PRIV_SYS_ADMIN overrides it.
 *
 * The struct is POSIX-shaped (real, effective, saved ids; supplementary
 * groups) so setresuid semantics are exactly the standard ones and the
 * Linux personality maps onto it without translation tables.
 *
 * Ownership: a process's credentials are written only by that process
 * (its own setres* calls, under process->lock -- which is what serialises
 * them now that a process can have several threads) and inherited by copy
 * at spawn. `privs` is written only to clear bits.
 * Other processes read them (kill, procinfo) without the lock: every
 * field is a naturally aligned 32-bit word and a torn view is at worst
 * an old id, never an invalid one.
 */

#ifndef KERNEL_CRED_H
#define KERNEL_CRED_H

#include <kernel/compiler.h>
#include <uapi/cosmo/syscall.h>

#define CRED_NGROUPS_MAX 16u
#define CRED_ROOT_UID    0u

/* The privileges (uapi/cosmo/syscall.h, COSMO_PRIV_*); at most 16 in 1.0. */
#define PRIV_MOUNT     COSMO_PRIV_MOUNT       /* mount, umount, switch_root, mount namespaces, a child's root */
#define PRIV_SYS_ADMIN COSMO_PRIV_SYS_ADMIN   /* sysctl writes, blkctl, fsctl, klog, domains, uts namespaces,
                                               * overriding file permissions and ownership */
#define PRIV_NET       COSMO_PRIV_NET         /* netctl configuration, binding ports below 1024 */
#define PRIV_PROC      COSMO_PRIV_PROC        /* signalling, seeing and inspecting any process; raising rlimits */
#define PRIV_CRED      COSMO_PRIV_CRED        /* spawning with other ids, changing uid, gid and groups */
#define PRIV_DEVICE    COSMO_PRIV_DEVICE      /* opening raw device nodes (/dev/net/tap, /dev/vmm) */
#define PRIV_ALL       COSMO_PRIV_ALL

struct credentials {
    uint32_t ruid, euid, suid;
    uint32_t rgid, egid, sgid;
    uint32_t ngroups;
    uint32_t groups[CRED_NGROUPS_MAX];
    uint32_t privs;                   /* PRIV_*: only ever cleared */
};

/* The kernel's own identity: root, no supplementary groups, every
 * privilege. Kernel threads, boot-time work and init start with these. */
extern const struct credentials cred_kernel;

/* Whether `c` may use privilege `priv` (one PRIV_* bit): it holds the bit
 * and its effective uid is 0. Uid 0 without the bit is nothing; the bit
 * without uid 0 is dormant -- a process that moved only its effective
 * uid away from 0 (seteuid to act as a user) acts as that user, and gets
 * its privileges back by moving it back, as Linux's effective capability
 * set does (PR #370 review). Leaving every uid 0 clears the bits for good
 * (cred_ids_changed). */
static inline bool cred_has(const struct credentials *c, uint32_t priv)
{
    return (c->privs & priv) == priv && priv != 0 && c->euid == CRED_ROOT_UID;
}

/* Clear `drop` (any PRIV_* bits): the only change privileges admit. */
static inline void cred_drop(struct credentials *c, uint32_t drop)
{
    c->privs &= ~drop;
}

/* The id-change rule: credentials that held a 0 among their real,
 * effective and saved uids and hold none after a change lose every
 * privilege (Linux clears capabilities the same way), so a server that
 * switches to a user's ids leaves that user nothing it held. Called by
 * every uid change: setres*uid and spawn's SETCRED. */
void cred_ids_changed(struct credentials *c, uint32_t old_ruid, uint32_t old_euid, uint32_t old_suid);

/* The calling context's credentials: the current process's, or
 * cred_kernel on a kernel thread. Never NULL. Read-only for callers. */
const struct credentials *cred_current(void);

/* `gid` is the effective group or one of the supplementary groups. */
bool cred_in_group(const struct credentials *c, uint32_t gid);

/* POSIX setresuid/setresgid: each of r, e, s is a new id or -1 to keep
 * the current value. Callers without PRIV_CRED may set each id only to
 * one of their current real, effective or saved ids. Applies all or
 * nothing; -EPERM on refusal. setresuid applies cred_ids_changed. */
int cred_setresuid(struct credentials *c, int64_t ruid, int64_t euid, int64_t suid);
int cred_setresgid(struct credentials *c, int64_t rgid, int64_t egid, int64_t sgid);

/* Replace the supplementary groups (PRIV_CRED; n <= CRED_NGROUPS_MAX). */
int cred_setgroups(struct credentials *c, const uint32_t *groups, unsigned n);

/* POSIX kill permission: PRIV_PROC, or the sender's real or effective
 * uid equals the target's real or saved uid. */
bool cred_may_signal(const struct credentials *sender, const struct credentials *target);

#endif /* KERNEL_CRED_H */
