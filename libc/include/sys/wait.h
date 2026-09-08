#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>
#define WNOHANG COSMO_WNOHANG
#define WUNTRACED COSMO_WUNTRACED
#define WCONTINUED COSMO_WCONTINUED
/*
 * The status is the exit status itself: 0..255 from exit, 128 + sig from
 * a kill, 139 for a fault. Job control needed two more outcomes and they
 * sit *above* that byte rather than replacing the encoding, so every
 * status that was meaningful before still means the same thing -- which
 * is why the two tests below have an upper bound as well as a lower one.
 */
#define WIFSTOPPED(s) COSMO_STATUS_IS_STOPPED(s)
#define WSTOPSIG(s) COSMO_STATUS_STOPSIG(s)
#define WIFCONTINUED(s) COSMO_STATUS_IS_CONTINUED(s)
#define WEXITSTATUS(s) (s)
#define WIFEXITED(s) ((s) < 128)
#define WIFSIGNALED(s) ((s) > 128 && (s) < 256 && (s) != 139)
#define WTERMSIG(s) ((s) - 128)
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);
#endif
