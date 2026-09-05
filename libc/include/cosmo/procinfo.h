#ifndef COSMO_PROCINFO_H
#define COSMO_PROCINFO_H
#include <stddef.h>
#include <uapi/cosmo/syscall.h>
/* Fill up to `count` records; returns the total number of processes or -1. */
int procinfo(struct cosmo_procinfo *buf, size_t count);
#endif
