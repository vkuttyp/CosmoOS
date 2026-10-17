/*
 * lsblk - list the block devices (shell completeness S6;
 * docs/userland/design.md, "Mounting").
 *
 *   lsblk
 *
 * util-linux's default listing, from /dev/blkctl and /proc/mounts: each
 * disk, then its partitions in table order drawn as a tree, with the size
 * in powers of 1024, read-only (0/1), the type and where it is mounted.
 * Two of util-linux's columns are left out: MAJ:MIN (this kernel numbers
 * no devices) and RM (nothing here is removable media). BusyBox has no
 * lsblk. Needs /dev/blkctl, which is root's (mode 0600, PRIV_SYS_ADMIN to
 * write a command).
 */
#include <stdio.h>
#include <string.h>

#include "blkdevs.h"

/* util-linux's size: the largest unit the size reaches, one decimal
 * rounded, dropped when it is zero (64M, 31.5M, 512B). */
static void human(uint64_t bytes, char *out, size_t len)
{
    static const char units[] = "BKMGTPE";
    unsigned u = 0;
    uint64_t unit = 1;
    while (u + 1 < sizeof(units) - 1 && bytes >= unit * 1024) {
        unit *= 1024;
        u++;
    }
    uint64_t tenths = (bytes * 10 + unit / 2) / unit;
    if (u > 0 && tenths % 10 != 0 && tenths < 10240)
        snprintf(out, len, "%llu.%llu%c", (unsigned long long)(tenths / 10), (unsigned long long)(tenths % 10),
                 units[u]);
    else
        snprintf(out, len, "%llu%c", (unsigned long long)((tenths + 5) / 10), units[u]);
}

static void row(const struct cosmo_blkctl_dev *d, const char *prefix, int width)
{
    char name[64], size[16], mnt[1024] = "";
    snprintf(name, sizeof(name), "%s%s", prefix, d->name);
    human(d->sectors * d->sector_size, size, sizeof(size));
    blkdevs_mountpoint(d->name, mnt, sizeof(mnt));
    printf("%-*s %6s %2d %-4s %s\n", width, name, size, (d->flags & COSMO_BLKCTL_RDONLY) ? 1 : 0,
           (d->flags & COSMO_BLKCTL_PART) ? "part" : "disk", mnt);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: lsblk\n");
        return 2;
    }
    if (blkdevs_list() < 0) {
        perror("lsblk: /dev/blkctl");
        return 1;
    }
    int width = 4;
    for (unsigned i = 0; i < g_nblkdevs; i++) {
        int w = (int)strlen(g_blkdevs[i].name) + ((g_blkdevs[i].flags & COSMO_BLKCTL_PART) ? 2 : 0);
        if (w > width)
            width = w;
    }
    printf("%-*s %6s %2s %-4s %s\n", width, "NAME", "SIZE", "RO", "TYPE", "MOUNTPOINTS");
    for (unsigned i = 0; i < g_nblkdevs; i++) {
        const struct cosmo_blkctl_dev *d = &g_blkdevs[i];
        if (d->flags & COSMO_BLKCTL_PART)
            continue;
        row(d, "", width);
        /* Its partitions by entry number: the last one gets the corner. */
        uint32_t last = 0;
        for (unsigned j = 0; j < g_nblkdevs; j++)
            if ((g_blkdevs[j].flags & COSMO_BLKCTL_PART) && strcmp(g_blkdevs[j].disk, d->name) == 0 &&
                g_blkdevs[j].index > last)
                last = g_blkdevs[j].index;
        for (uint32_t idx = 1; idx <= last; idx++)
            for (unsigned j = 0; j < g_nblkdevs; j++)
                if ((g_blkdevs[j].flags & COSMO_BLKCTL_PART) && g_blkdevs[j].index == idx &&
                    strcmp(g_blkdevs[j].disk, d->name) == 0)
                    row(&g_blkdevs[j], idx == last ? "`-" : "|-", width);
    }
    return 0;
}
