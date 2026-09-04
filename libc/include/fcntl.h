#ifndef _FCNTL_H
#define _FCNTL_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>
#define O_RDONLY COSMO_O_RDONLY
#define O_WRONLY COSMO_O_WRONLY
#define O_RDWR COSMO_O_RDWR
#define O_ACCMODE COSMO_O_ACCMODE
#define O_CREAT COSMO_O_CREAT
#define O_EXCL COSMO_O_EXCL
#define O_TRUNC COSMO_O_TRUNC
#define O_APPEND COSMO_O_APPEND
#define O_DIRECTORY COSMO_O_DIRECTORY
int open(const char *path, int flags, ...);
#endif
