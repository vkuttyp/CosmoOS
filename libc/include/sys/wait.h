#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>
#define WNOHANG COSMO_WNOHANG
/* The status is the exit status itself: 0..255 from exit, 128 + sig from a kill, 139 for a fault. */
#define WEXITSTATUS(s) (s)
#define WIFEXITED(s) ((s) < 128)
#define WIFSIGNALED(s) ((s) > 128 && (s) != 139)
#define WTERMSIG(s) ((s) - 128)
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);
#endif
