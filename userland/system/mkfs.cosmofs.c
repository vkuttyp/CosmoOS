/*
 * mkfs.cosmofs - make an empty cosmofs on a block device (shell
 * completeness S6; docs/userland/design.md, "Mounting").
 *
 *   mkfs.cosmofs [-L LABEL] DEVICE
 *
 * DEVICE is NAME or /dev/NAME, a whole disk or a partition, not mounted;
 * a disk with registered partitions is refused (format a partition).
 * LABEL, up to 16 printable characters with no blank, quote or backslash,
 * is what blkid prints. A single-device pool, unencrypted: the filesystem
 * the installer makes for the root, through the same /dev/blkctl FORMAT
 * (root's: PRIV_SYS_ADMIN). Whatever was on DEVICE is lost. Exit 0 made,
 * 1 failed, 2 usage.
 */
#include <stdio.h>
#include <string.h>

#include "blkdevs.h"

int main(int argc, char **argv)
{
    const char *label = "";
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-L") == 0) {
        label = argv[i + 1];
        i += 2;
    }
    if (argc - i != 1 || strlen(label) > COSMO_BLKCTL_LABEL) {
        fprintf(stderr, "usage: mkfs.cosmofs [-L LABEL] DEVICE   (LABEL: at most %u characters)\n",
                COSMO_BLKCTL_LABEL);
        return 2;
    }
    int rc = blkdevs_list();
    const struct cosmo_blkctl_dev *d = rc == 0 ? blkdevs_find(argv[i]) : NULL;
    if (rc == 0 && d == NULL) {
        fprintf(stderr, "mkfs.cosmofs: %s: no such block device\n", argv[i]);
        return 1;
    }
    if (rc == 0 && !(d->flags & COSMO_BLKCTL_PART)) {
        for (unsigned j = 0; j < g_nblkdevs; j++)
            if ((g_blkdevs[j].flags & COSMO_BLKCTL_PART) && strcmp(g_blkdevs[j].disk, d->name) == 0) {
                fprintf(stderr, "mkfs.cosmofs: %s has partitions (%s, ...): format one of them\n", d->name,
                        g_blkdevs[j].name);
                return 1;
            }
    }
    if (rc == 0)
        rc = blkdevs_format(d->name, "cosmofs", label);
    if (rc < 0) {
        fprintf(stderr, "mkfs.cosmofs: %s: %s\n", argv[i], strerror(-rc));
        return 1;
    }
    printf("mkfs.cosmofs: made cosmofs on %s%s%s\n", d->name, label[0] ? ", label " : "", label);
    return 0;
}
