/*
 * blkdevs.h - The block devices through /dev/blkctl, for lsblk, blkid,
 * mkfs.cosmofs (shell completeness S6) and cosmo-upgrade (S7;
 * uapi/cosmo/blkctl.h).
 */
#ifndef USERLAND_BLKDEVS_H
#define USERLAND_BLKDEVS_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/cosmo/blkctl.h>

#define BLKDEVS_MAX 128

extern struct cosmo_blkctl_dev g_blkdevs[BLKDEVS_MAX];
extern unsigned g_nblkdevs;

/* Open /dev/blkctl and list the devices into g_blkdevs. 0 or -errno:
 * -E2BIG when more than BLKDEVS_MAX are registered, rather than a list
 * with some left out. */
int blkdevs_list(void);
/* The device named `name`, with or without a leading "/dev/"; NULL if
 * there is none. */
const struct cosmo_blkctl_dev *blkdevs_find(const char *name);
/* `count` sectors from `lba` into `buf`, in reads of at most
 * COSMO_BLKCTL_IO_MAX. 0 or -errno (-EINVAL past the device's end). */
int blkdevs_read(const struct cosmo_blkctl_dev *d, uint64_t lba, uint64_t count, void *buf);
/* `count` sectors from `buf` at `lba`, in writes of at most
 * COSMO_BLKCTL_IO_MAX. 0 or -errno. */
int blkdevs_write(const struct cosmo_blkctl_dev *d, uint64_t lba, uint64_t count, const void *buf);
/* The device's volatile cache to stable media. 0 or -errno. */
int blkdevs_flush(const struct cosmo_blkctl_dev *d);
/* A FORMAT of `fstype` named `label` ("" for none). 0 or -errno. */
int blkdevs_format(const char *name, const char *fstype, const char *label);
/* A two-way mirror over devices `a` and `b` (roadmap 1.2 B2). */
int blkdevs_format_mirror(const char *a, const char *b, const char *fstype, const char *label);
/* An encrypted pool (roadmap 1.2 B5) on `name`, or a mirror of it and
 * `mirror` (NULL for none), with the uuid the slots were sealed for, the
 * master key and the slots; the kernel keeps the key, so the pool mounts at once. */
int blkdevs_format_keyed(const char *name, const char *mirror, const char *fstype, const char *label,
                         const uint8_t uuid[16], const uint8_t master[COSMO_KEY_SIZE], const struct cosmo_keyslot slots[COSMO_KEY_SLOTS]);
/* The directories `name` is mounted on, from /proc/mounts ("/dev/NAME
 * DIR ..."), unescaped, in order: the `which`th (from 0) into `out`.
 * False when there is no such one. */
int blkdevs_mountpoint(const char *name, unsigned which, char *out, size_t len);

#endif /* USERLAND_BLKDEVS_H */
