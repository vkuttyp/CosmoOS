/* process.c - spawn, wait, kill (docs/libc/design.md). */

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libc.h"

pid_t spawnve(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
              size_t nh)
{
    struct cosmo_spawn req = {
        .path = path,
        .argv = argv,
        .envp = envp,
        .handles = (const struct cosmo_spawn_handle *)h,
        .nr_handles = nh,
        .cwd = NULL,
        .flags = COSMO_SPAWN_HANDLE_RIGHTS,   /* struct spawn_handle carries rights */
    };
    return (pid_t)__syscall_ret(cosmo_spawn(&req));
}

pid_t spawnve_in(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
                 size_t nh, const char *root)
{
    struct cosmo_spawn req = {
        .path = path,
        .argv = argv,
        .envp = envp,
        .handles = (const struct cosmo_spawn_handle *)h,
        .nr_handles = nh,
        .cwd = NULL,
        .flags = COSMO_SPAWN_HANDLE_RIGHTS | COSMO_SPAWN_SETROOT,
        .root = root,
    };
    return (pid_t)__syscall_ret(cosmo_spawn(&req));
}

pid_t spawnve_domain(const char *path, const char *const argv[], const char *const envp[],
                     const struct spawn_handle *h, size_t nh)
{
    struct cosmo_spawn req = {
        .path = path,
        .argv = argv,
        .envp = envp,
        .handles = (const struct cosmo_spawn_handle *)h,
        .nr_handles = nh,
        .cwd = NULL,
        .flags = COSMO_SPAWN_HANDLE_RIGHTS | COSMO_SPAWN_NEWDOMAIN,
    };
    return (pid_t)__syscall_ret(cosmo_spawn(&req));
}

pid_t spawnve_as(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
                 size_t nh, uid_t uid, gid_t gid)
{
    struct cosmo_spawn req = {
        .path = path,
        .argv = argv,
        .envp = envp,
        .handles = (const struct cosmo_spawn_handle *)h,
        .nr_handles = nh,
        .cwd = NULL,
        .flags = COSMO_SPAWN_SETCRED | COSMO_SPAWN_HANDLE_RIGHTS,
        .uid = uid,
        .gid = gid,
    };
    return (pid_t)__syscall_ret(cosmo_spawn(&req));
}

pid_t spawnvp(const char *file, const char *const argv[], const struct spawn_handle *h, size_t nh)
{
    if (strchr(file, '/'))
        return spawnve(file, argv, (const char *const *)environ, h, nh);
    const char *path = getenv("PATH");
    if (path == NULL)
        path = "/bin:/sbin:/usr/bin:/usr/sbin";
    char cand[1024];
    int last = ENOENT;
    while (*path) {
        const char *end = strchr(path, ':');
        size_t dl = end ? (size_t)(end - path) : strlen(path);
        if (dl + 1 + strlen(file) + 1 <= sizeof(cand)) {
            memcpy(cand, path, dl);
            cand[dl] = '/';
            strcpy(cand + dl + 1, file);
            struct stat st;
            if (stat(cand, &st) == 0 && S_ISREG(st.st_type)) {
                pid_t pid = spawnve(cand, argv, (const char *const *)environ, h, nh);
                if (pid >= 0)
                    return pid;
                last = errno;
                if (errno != ENOENT)
                    break;
            }
        }
        if (end == NULL)
            break;
        path = end + 1;
    }
    errno = last;
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    int st = 0;
    long r = __syscall_ret(cosmo_wait(pid, &st, (unsigned)options));
    if (r > 0 && status)
        *status = st;
    return (pid_t)r;
}

pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}

int kill(pid_t pid, int sig)
{
    return (int)__syscall_ret(cosmo_kill(pid, sig));
}
