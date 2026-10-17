/*
 * blkid - name what is on the block devices (shell completeness S6;
 * docs/userland/design.md, "Mounting").
 *
 *   blkid [DEVICE...]
 *
 * util-linux's output, one line a device: `/dev/NAME: LABEL="..."
 * UUID="..." TYPE="cosmofs" PARTUUID="..."`, from the device's first
 * block (read through /dev/blkctl) and the partition table the kernel
 * registered. It knows cosmofs -- the superblock's label and pool uuid
 * at the offsets tests/host/test_cosmofs.c pins -- and a disk's GUID
 * Partition Table (PTUUID, PTTYPE="gpt"); a partition holding anything
 * else shows its PARTUUID alone, and a device with nothing to show is
 * left out. With DEVICEs (NAME or /dev/NAME), only those. Exit 0, or 2
 * when no device had anything to show, as util-linux's. BusyBox's blkid
 * knows no cosmofs and prints no PARTUUID. Root's: /dev/blkctl.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <kernel/gpt.h>

#include "blkdevs.h"

#define HEAD 4096u   /* one cosmofs block: superblock A */

/* cosmofs_format.h's superblock, by offset: what is read is the raw block. */
#define CFS_MAGIC "COSMOFS1"
#define CFS_OFF_VERSION 8
#define CFS_OFF_UUID 96
#define CFS_OFF_LABEL 136
#define CFS_LABEL_MAX 16

static uint8_t g_head[HEAD];

/* A uuid as RFC 4122 writes it, the bytes in order: how util-linux prints
 * a filesystem's (a GPT GUID's first three fields are little-endian
 * instead: gpt_guid_format). */
static void uuid_text(const uint8_t u[16], char out[37])
{
    char *p = out;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            *p++ = '-';
        p += sprintf(p, "%02x", u[i]);
    }
}

static bool zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* Print the device's line; false when it has nothing to show. */
static bool probe(const struct cosmo_blkctl_dev *d)
{
    char line[256];
    int n = 0;
    size_t bytes = d->sector_size > HEAD ? d->sector_size : HEAD;
    if (bytes <= sizeof(g_head) && blkdevs_read_head(d, g_head, bytes) == 0) {
        uint32_t version;
        memcpy(&version, g_head + CFS_OFF_VERSION, 4);
        if (memcmp(g_head, CFS_MAGIC, 8) == 0) {
            char label[CFS_LABEL_MAX + 1] = "";
            memcpy(label, g_head + CFS_OFF_LABEL, CFS_LABEL_MAX);
            if (label[0] != '\0')
                n += snprintf(line + n, sizeof(line) - (size_t)n, " LABEL=\"%s\"", label);
            if (version >= 4 && !zero(g_head + CFS_OFF_UUID, 16)) {   /* the pool uuid came with version 4 */
                char u[37];
                uuid_text(g_head + CFS_OFF_UUID, u);
                n += snprintf(line + n, sizeof(line) - (size_t)n, " UUID=\"%s\"", u);
            }
            n += snprintf(line + n, sizeof(line) - (size_t)n, " TYPE=\"cosmofs\"");
        } else if (!(d->flags & COSMO_BLKCTL_PART) && d->sector_size >= 512 &&
                   bytes >= 2 * d->sector_size && memcmp(g_head + d->sector_size, "EFI PART", 8) == 0) {
            char g[GPT_GUID_TEXT];
            gpt_guid_format(g_head + d->sector_size + 56, g);   /* the header's disk GUID */
            n += snprintf(line + n, sizeof(line) - (size_t)n, " PTUUID=\"%s\" PTTYPE=\"gpt\"", g);
        }
    }
    if (d->flags & COSMO_BLKCTL_PART) {
        char g[GPT_GUID_TEXT];
        gpt_guid_format(d->uuid, g);
        n += snprintf(line + n, sizeof(line) - (size_t)n, " PARTUUID=\"%s\"", g);
    }
    if (n == 0)
        return false;
    printf("/dev/%s:%s\n", d->name, line);
    return true;
}

int main(int argc, char **argv)
{
    if (blkdevs_list() < 0) {
        perror("blkid: /dev/blkctl");
        return 2;
    }
    bool any = false;
    if (argc == 1) {
        for (unsigned i = 0; i < g_nblkdevs; i++)
            any |= probe(&g_blkdevs[i]);
    } else {
        for (int i = 1; i < argc; i++) {
            const struct cosmo_blkctl_dev *d = blkdevs_find(argv[i]);
            if (d != NULL)
                any |= probe(d);
        }
    }
    return any ? 0 : 2;
}
