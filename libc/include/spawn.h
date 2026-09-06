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
/* spawnve with COSMO_SPAWN_SETCRED: the child starts as uid/gid with no
 * supplementary groups; unprivileged callers may name only ids they hold. */
pid_t spawnve_as(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
                 size_t nh, uid_t uid, gid_t gid);
#endif
