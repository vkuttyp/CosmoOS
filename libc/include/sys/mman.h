#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#include <stddef.h>
#include <uapi/cosmo/syscall.h>
#define PROT_NONE COSMO_PROT_NONE
#define PROT_READ COSMO_PROT_READ
#define PROT_WRITE COSMO_PROT_WRITE
#define PROT_EXEC COSMO_PROT_EXEC
#define MAP_ANONYMOUS COSMO_MAP_ANONYMOUS
#define MAP_FIXED COSMO_MAP_FIXED
#define MAP_PRIVATE 0
#define MAP_FAILED ((void *)-1)
void *mmap(void *hint, size_t len, int prot, int flags, int fd, long off);
int munmap(void *addr, size_t len);
#endif
