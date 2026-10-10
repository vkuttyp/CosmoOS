/*
 * bootfs.h - Boot composition of the filesystem namespace (roadmap M2;
 * docs/kernel-services/vfs/design.md, "Boot composition").
 *
 * Which filesystems exist and what the root is are decided here, not in
 * the VFS: the kernel registers ramfs, procfs and cosmofs, mounts a ramfs
 * root filled from the boot archive (always: the "live" system), and
 * mounts /proc. A disk root is named on the command line (root=) and is
 * mounted and switched to by init (docs/userland/design.md); the kernel
 * only resolves the name to a block device, after its partition scan.
 */

#ifndef KERNEL_BOOTFS_H
#define KERNEL_BOOTFS_H

#include <stddef.h>

/* After vfs_init and bootarchive_init: register the filesystems, mount
 * the ramfs root, populate it from the boot archive, mount /proc. */
void bootfs_init(void);

/* After the boot modules (the disk drivers) have loaded: scan every disk
 * for partitions, then resolve and log root= (once). */
void bootfs_disks_ready(void);

/* The root= value from the command line, "" when there is none. */
const char *bootfs_root_spec(void);

/*
 * The block device root= names, resolved now: its name into `out`, 0;
 * -ENOENT without root=; -ENODEV when nothing registered matches;
 * -EINVAL for a value that is neither PARTUUID=<guid> nor a device name.
 */
int bootfs_root_device(char *out, size_t len);

#endif /* KERNEL_BOOTFS_H */
