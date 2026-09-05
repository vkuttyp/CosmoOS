#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>
#define SIGHUP COSMO_SIGHUP
#define SIGINT COSMO_SIGINT
#define SIGKILL COSMO_SIGKILL
#define SIGSEGV COSMO_SIGSEGV
#define SIGTERM COSMO_SIGTERM
#define NSIG COSMO_NSIG
int kill(pid_t pid, int sig);
#endif
