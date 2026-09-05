#ifndef _SPAWN_H
#define _SPAWN_H
#include <stddef.h>
#include <sys/types.h>
/* The child receives exactly the mapped handles (docs/libc/design.md);
 * h == NULL, nh == 0 inherits 0, 1, 2 as they are. */
struct spawn_handle {
    int child;
    int parent;
};
pid_t spawnve(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
              size_t nh);
pid_t spawnvp(const char *file, const char *const argv[], const struct spawn_handle *h, size_t nh);
/* spawnve with COSMO_SPAWN_SETCRED: the child starts as uid/gid with no
 * supplementary groups; unprivileged callers may name only ids they hold. */
pid_t spawnve_as(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
                 size_t nh, uid_t uid, gid_t gid);
#endif
