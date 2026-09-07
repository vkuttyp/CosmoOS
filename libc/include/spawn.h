#ifndef _SPAWN_H
#define _SPAWN_H
#include <stddef.h>
#include <sys/types.h>
/* The child receives exactly the mapped handles (docs/libc/design.md);
 * h == NULL, nh == 0 inherits 0, 1, 2 as they are. */
/* The same shape as struct cosmo_spawn_handle: spawnve passes the array
 * straight to the kernel. */
struct spawn_handle {
    int child;
    int parent;
    /* What the child gets on this handle: 0 for the caller's own
     * rights, or a subset of COSMO_RIGHT_* to hand over less. */
    unsigned rights;
    unsigned pad;
};
pid_t spawnve(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
              size_t nh);
pid_t spawnvp(const char *file, const char *const argv[], const struct spawn_handle *h, size_t nh);
/* spawnve with COSMO_SPAWN_SETROOT: the child's root is `root`, resolved
 * in the caller's own namespace, and the child cannot name anything
 * outside it -- absolute paths start there and ".." stops there.
 * Privileged, and confinement only ever tightens: a caller already
 * confined can only name a directory inside its own root. */
pid_t spawnve_in(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
                 size_t nh, const char *root);
/* spawnve with COSMO_SPAWN_NEWDOMAIN: the child starts a process domain
 * of its own. It and its descendants see and may signal only each
 * other; the domain the system boots in still sees them. Entered only
 * here and never left. Privileged. */
pid_t spawnve_domain(const char *path, const char *const argv[], const char *const envp[],
                     const struct spawn_handle *h, size_t nh);
/* spawnve with COSMO_SPAWN_NEWMOUNTNS: the child starts a mount
 * namespace of its own -- a copy of what the caller can see now, which
 * then diverges. What the child mounts afterwards the caller does not
 * see, and what the caller mounts afterwards the child does not.
 * Privileged, and joined only by being spawned into it. */
pid_t spawnve_mountns(const char *path, const char *const argv[], const char *const envp[],
                      const struct spawn_handle *h, size_t nh);
/* spawnve with COSMO_SPAWN_NEWUTSNS: the child starts a uts namespace
 * of its own, holding a copy of the caller's hostname. Renaming there
 * is invisible outside. Privileged. */
pid_t spawnve_utsns(const char *path, const char *const argv[], const char *const envp[],
                    const struct spawn_handle *h, size_t nh);
/* spawnve with COSMO_SPAWN_SETCRED: the child starts as uid/gid with no
 * supplementary groups; unprivileged callers may name only ids they hold. */
pid_t spawnve_as(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
                 size_t nh, uid_t uid, gid_t gid);
#endif
