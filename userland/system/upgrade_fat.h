/*
 * upgrade_fat.h - FAT32 on a block device through /dev/blkctl, as much as
 * cosmo-upgrade needs to replace the boot set on the ESP (shell
 * completeness S7; docs/userland/design.md, "Updating").
 *
 * The FAT is held in memory and written back by fat_commit, to every copy
 * (or the active one when mirroring is off). Directory entries are changed
 * a sector at a time by the caller (fat_dir_sector, fat_write_sector): the
 * one-sector switch is cosmo-upgrade's, not this file's.
 */
#ifndef USERLAND_UPGRADE_FAT_H
#define USERLAND_UPGRADE_FAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blkdevs.h"

#define FAT_DIRENT 32u
#define FAT_ATTR_DIR 0x10u
#define FAT_ATTR_ARCHIVE 0x20u
#define FAT_ATTR_LFN 0x0Fu
#define FAT_ATTR_VOLUME 0x08u
#define FAT_NT_LOWER_BASE 0x08u
#define FAT_NT_LOWER_EXT 0x10u

struct fat {
    const struct cosmo_blkctl_dev *dev;
    uint32_t bps;           /* bytes per sector: the device's */
    uint32_t spc;           /* sectors per cluster */
    uint32_t rsvd, nfats, fatsz, root, fsinfo;
    uint32_t nclusters;     /* data clusters: valid numbers are 2 .. nclusters + 1 */
    uint64_t data_lba;      /* the first data sector */
    int active;             /* the one FAT written, or -1 for all (mirroring) */
    uint32_t *fat;          /* the FAT, in memory: fatsz * bps / 4 entries */
    uint8_t *dirty;         /* per FAT sector: changed since the last commit */
    uint32_t hint;          /* where the next allocation starts looking */
};

/* An entry found in a directory, and where it is. */
struct fat_ent {
    uint8_t raw[FAT_DIRENT];
    uint32_t cluster, size;
    uint64_t lba;           /* the sector holding it */
    unsigned slot;          /* its index in that sector */
};

int fat_open(struct fat *f, const struct cosmo_blkctl_dev *d, char *err, size_t errlen);
void fat_close(struct fat *f);

uint64_t fat_cluster_lba(const struct fat *f, uint32_t c);
uint32_t fat_cluster_bytes(const struct fat *f);
int fat_read_sector(const struct fat *f, uint64_t lba, void *buf);
int fat_write_sector(const struct fat *f, uint64_t lba, const void *buf);

/* "kernel.elf" -> "KERNEL  ELF" and the lowercase flags; false when the
 * name is not 8.3. */
bool fat_name83(const char *name, uint8_t out[11], uint8_t *nt);

/* Look `name83` up in the directory starting at `dir`: 1 found (*e), 0
 * not, -errno. */
int fat_lookup(struct fat *f, uint32_t dir, const uint8_t name83[11], struct fat_ent *e);
/* Every live entry of the directory at `dir`, '.' and '..' included,
 * up to `max`: the count, or -errno. */
int fat_list(struct fat *f, uint32_t dir, struct fat_ent *out, int max);
/* A file's bytes, malloc'd: 0, or -errno (-EIO for a broken chain). */
int fat_read_file(struct fat *f, uint32_t cluster, uint32_t size, uint8_t **out);

/* Allocate a chain for `len` bytes in the in-memory FAT and write `data`
 * into it (the rest of the last cluster zeroed). *first is 0 for len 0.
 * Nothing names the chain until the caller writes an entry. 0 or -errno. */
int fat_write_new(struct fat *f, const void *data, size_t len, uint32_t *first);
/* Free a chain in the in-memory FAT. */
void fat_free_chain(struct fat *f, uint32_t c);
/* Free every allocated cluster no directory reaches from the root. The
 * number freed, or -errno. */
int fat_sweep(struct fat *f);
/* The changed FAT sectors to every copy, FSInfo's free count and hint,
 * then FLUSH. 0 or -errno. */
int fat_commit(struct fat *f);

/* A directory entry for a file or directory, stamped with `now` (seconds
 * since 1970, UTC). */
void fat_make_ent(uint8_t raw[FAT_DIRENT], const uint8_t name83[11], uint8_t nt, uint8_t attr, uint32_t cluster,
                  uint32_t size, int64_t now);
void fat_ent_set(uint8_t raw[FAT_DIRENT], uint32_t cluster, uint32_t size, int64_t now);

#endif /* USERLAND_UPGRADE_FAT_H */
