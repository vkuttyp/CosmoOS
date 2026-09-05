#ifndef COSMO_KLOG_H
#define COSMO_KLOG_H
#include <sys/types.h>
/* The newest whole kernel log lines that fit; returns the byte count or -1. */
ssize_t klog_read(char *buf, size_t len);
#endif
