/* cosmo.c - Native introspection wrappers: procinfo, klog, sysctl. */

#include <cosmo/klog.h>
#include <cosmo/procinfo.h>
#include <cosmo/sysctl.h>

#include "libc.h"

int procinfo(struct cosmo_procinfo *buf, size_t count)
{
    return (int)__syscall_ret(cosmo_procinfo(buf, count));
}

ssize_t klog_read(char *buf, size_t len)
{
    return __syscall_ret(cosmo_klog(buf, len));
}

int sysctl_get(const char *name, char *buf, size_t len)
{
    return (int)__syscall_ret(cosmo_sysctl(name, buf, len));
}
