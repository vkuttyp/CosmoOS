#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H
#include <uapi/cosmo/syscall.h>
#define MS_RDONLY COSMO_MOUNT_RDONLY
#define MNT_FORCE COSMO_UMOUNT_FORCE
int mount(const char *source, const char *target, const char *fstype, unsigned flags);
int umount(const char *target);
int umount2(const char *target, unsigned flags);
#endif
