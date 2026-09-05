#ifndef COSMO_SYSCTL_H
#define COSMO_SYSCTL_H
#include <stddef.h>
/* Read-only value as a string; returns its length (NUL written when it fits) or -1. */
int sysctl_get(const char *name, char *buf, size_t len);
#endif
