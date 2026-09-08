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

/* Every spawn goes through this; the variants differ only in the flags
 * and the one field each of them adds. */
static pid_t spawn_req(const char *path, const char *const argv[], const char *const envp[],
                       const struct spawn_handle *h, size_t nh, unsigned extra, pid_t pgid)
{
    struct cosmo_spawn req = {
        .path = path,
        .argv = argv,
        .envp = envp,
        .handles = (const struct cosmo_spawn_handle *)h,
        .nr_handles = nh,
        .cwd = NULL,
        .flags = COSMO_SPAWN_HANDLE_RIGHTS | extra,   /* struct spawn_handle carries rights */
        .pgid = (uint32_t)pgid,
    };
    return (pid_t)__syscall_ret(cosmo_spawn(&req));
}

pid_t spawnve(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
              size_t nh)
{
    return spawn_req(path, argv, envp, h, nh, 0, 0);
}

pid_t spawnve_pgrp(const char *path, const char *const argv[], const char *const envp[], const struct spawn_handle *h,
                   size_t nh, pid_t pgid)
{
    return spawn_req(path, argv, envp, h, nh, COSMO_SPAWN_SETPGID, pgid);
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

pid_t spawnve_mountns(const char *path, const char *const argv[], const char *const envp[],
                      const struct spawn_handle *h, size_t nh)
{
    struct cosmo_spawn req = {
        .path = path,
        .argv = argv,
        .envp = envp,
        .handles = (const struct cosmo_spawn_handle *)h,
        .nr_handles = nh,
        .cwd = NULL,
        .flags = COSMO_SPAWN_HANDLE_RIGHTS | COSMO_SPAWN_NEWMOUNTNS,
    };
    return (pid_t)__syscall_ret(cosmo_spawn(&req));
}

pid_t spawnve_utsns(const char *path, const char *const argv[], const char *const envp[],
                    const struct spawn_handle *h, size_t nh)
{
    struct cosmo_spawn req = {
        .path = path,
        .argv = argv,
        .envp = envp,
        .handles = (const struct cosmo_spawn_handle *)h,
        .nr_handles = nh,
        .cwd = NULL,
        .flags = COSMO_SPAWN_HANDLE_RIGHTS | COSMO_SPAWN_NEWUTSNS,
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

static pid_t spawnvp_flags(const char *file, const char *const argv[], const struct spawn_handle *h, size_t nh,
                           unsigned extra, pid_t pgid)
{
    if (strchr(file, '/'))
        return spawn_req(file, argv, (const char *const *)environ, h, nh, extra, pgid);
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
                pid_t pid = spawn_req(cand, argv, (const char *const *)environ, h, nh, extra, pgid);
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

pid_t spawnvp(const char *file, const char *const argv[], const struct spawn_handle *h, size_t nh)
{
    return spawnvp_flags(file, argv, h, nh, 0, 0);
}

pid_t spawnvp_pgrp(const char *file, const char *const argv[], const struct spawn_handle *h, size_t nh, pid_t pgid)
{
    return spawnvp_flags(file, argv, h, nh, COSMO_SPAWN_SETPGID, pgid);
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

int setpgid(pid_t pid, pid_t pgid)
{
    return (int)__syscall_ret(cosmo_setpgid(pid, pgid));
}

pid_t getpgid(pid_t pid)
{
    return (pid_t)__syscall_ret(cosmo_getpgid(pid));
}

pid_t getpgrp(void)
{
    return getpgid(0);
}

pid_t setsid(void)
{
    return (pid_t)__syscall_ret(cosmo_setsid());
}

pid_t getsid(pid_t pid)
{
    return (pid_t)__syscall_ret(cosmo_getsid(pid));
}

pid_t tcgetpgrp(int fd)
{
    return (pid_t)__syscall_ret(cosmo_tcgetpgrp(fd));
}

int tcsetpgrp(int fd, pid_t pgid)
{
    return (int)__syscall_ret(cosmo_tcsetpgrp(fd, pgid));
}
