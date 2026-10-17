/*
 * blkdevs.h - The block devices through /dev/blkctl, for lsblk, blkid and
 * mkfs.cosmofs (shell completeness S6; uapi/cosmo/blkctl.h).
 */
#ifndef USERLAND_BLKDEVS_H
#define USERLAND_BLKDEVS_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/cosmo/blkctl.h>

#define BLKDEVS_MAX 128

extern struct cosmo_blkctl_dev g_blkdevs[BLKDEVS_MAX];
extern unsigned g_nblkdevs;

/* Open /dev/blkctl and list the devices into g_blkdevs. 0 or -errno. */
int blkdevs_list(void);
/* The device named `name`, with or without a leading "/dev/"; NULL if
 * there is none. */
const struct cosmo_blkctl_dev *blkdevs_find(const char *name);
/* `bytes` (a multiple of the sector size, at most COSMO_BLKCTL_IO_MAX)
 * from the device's first sector. 0 or -errno. */
int blkdevs_read_head(const struct cosmo_blkctl_dev *d, void *buf, size_t bytes);
/* A FORMAT of `fstype` named `label` ("" for none). 0 or -errno. */
int blkdevs_format(const char *name, const char *fstype, const char *label);
/* Where `name` is mounted, from /proc/mounts ("/dev/NAME DIR ..."),
 * unescaped into `out`; false when it is not. */
int blkdevs_mountpoint(const char *name, char *out, size_t len);

#endif /* USERLAND_BLKDEVS_H */
