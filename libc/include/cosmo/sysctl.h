#ifndef COSMO_SYSCTL_H
#define COSMO_SYSCTL_H
#include <stddef.h>
/* Read-only value as a string; returns its length (NUL written when it fits) or -1. */
int sysctl_get(const char *name, char *buf, size_t len);
/* Write one (kernel.hostname; PRIV_SYS_ADMIN). 0 or -1 with errno. */
int sysctl_set(const char *name, const char *value, size_t len);
#endif
