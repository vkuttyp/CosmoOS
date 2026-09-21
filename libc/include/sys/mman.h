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
#define MAP_FIXED_NOREPLACE COSMO_MAP_FIXED_NOREPLACE
/* A file mapping names exactly one of these; anonymous memory may say
 * MAP_PRIVATE and may not say MAP_SHARED (there is no fork, so nobody to
 * share it with: EINVAL rather than a yes that means nothing). A shared
 * mapping's writes reach the file and every other mapping of it; a
 * private one is copy-on-write. */
#define MAP_SHARED COSMO_MAP_SHARED
#define MAP_PRIVATE COSMO_MAP_PRIVATE
#define MAP_FAILED ((void *)-1)
#define MS_ASYNC COSMO_MS_ASYNC
#define MS_INVALIDATE COSMO_MS_INVALIDATE
#define MS_SYNC COSMO_MS_SYNC
/* POSIX, with the kernel's rules: len a page multiple (EINVAL, not
 * rounded), off page aligned, a regular file (ENODEV), a shared writable
 * mapping only of a file opened for writing (EACCES). */
void *mmap(void *hint, size_t len, int prot, int flags, int fd, long off);
int munmap(void *addr, size_t len);
/* POSIX, with two things POSIX does not say: len must be a page
 * multiple (EINVAL, not rounded), and EBUSY if a MAP_FIXED mapping is
 * replacing part of the range on another thread at that instant. A
 * shared mapping of a file opened read-only cannot be made writable
 * (EACCES). */
int mprotect(void *addr, size_t len, int prot);
/* MS_SYNC writes the dirty pages of every file mapped in the range and
 * waits; MS_ASYNC returns at once (the cache already owns them);
 * MS_INVALIDATE is nothing to do, the mapping being the cache. */
int msync(void *addr, size_t len, int flags);
#endif
