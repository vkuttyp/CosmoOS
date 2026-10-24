/*
 * blkid - name what is on the block devices (shell completeness S6;
 * docs/userland/design.md, "Mounting").
 *
 *   blkid [DEVICE...]
 *
 * util-linux's output, one line a device: `/dev/NAME: LABEL="..."
 * UUID="..." TYPE="cosmofs" PARTUUID="..."`, read through /dev/blkctl.
 * It knows cosmofs -- of the two superblock slots, the valid one (magic,
 * a version this tree knows, CRC32C) with the higher generation, whose
 * label and pool uuid it takes from cosmofs_format.h -- and a disk's GUID Partition Table, accepted by the kernel's own
 * parser (gpt_parse: PTUUID, PTTYPE="gpt"). A partition holding anything
 * else shows its PARTUUID alone, and a device with nothing to show is
 * left out. With DEVICEs (NAME or /dev/NAME), only those. Exit 0, or 2
 * when no device had anything to show, as util-linux's. BusyBox's blkid
 * knows no cosmofs and prints no PARTUUID. Root's: /dev/blkctl.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <kernel/crc32c.h>
#include <kernel/gpt.h>

#include "blkdevs.h"

#include "../../kernel-services/filesystem/cosmofs/cosmofs_format.h"

/* The superblock is read raw, by cosmofs_format.h's offsets. */
#define CFS_OFF_VERSION offsetof(struct cfs_super, version)
#define CFS_OFF_GENERATION offsetof(struct cfs_super, generation)
#define CFS_OFF_UUID offsetof(struct cfs_super, uuid)
#define CFS_OFF_LABEL offsetof(struct cfs_super, label)
#define CFS_OFF_CRC offsetof(struct cfs_super, crc)

static uint8_t g_slots[2 * CFS_BLOCK];
static uint8_t g_scratch[GPT_SCRATCH_BYTES];
static struct gpt_table g_table;

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

/* cosmofs's superblock check (cfs_super_ok): the magic, a known version,
 * the CRC32C over the block with the crc field as zero. A version-14
 * encrypted pool's carries a MAC there instead, which needs the key: its
 * slot is taken unverified, as the kernel's KEY_INFO takes it, since what
 * blkid prints (a type, a label, a uuid) helps nobody forge anything. */
static bool slot_ok(const uint8_t *b, uint64_t *gen)
{
    uint32_t version, crc;
    memcpy(&version, b + CFS_OFF_VERSION, 4);
    memcpy(&crc, b + CFS_OFF_CRC, 4);
    if (memcmp(b, CFS_MAGIC, 8) != 0 || version < CFS_VERSION_MIN || version > CFS_VERSION)
        return false;
    if (cfs_super_is_mac((const struct cfs_super *)b)) {
        memcpy(gen, b + CFS_OFF_GENERATION, 8);
        return *gen != 0;
    }
    static const uint8_t zero4[4] = { 0 };
    uint32_t c = crc32c(b, CFS_OFF_CRC);
    c = crc32c_update(c, zero4, 4);
    c = crc32c_update(c, b + CFS_OFF_CRC + 4, CFS_BLOCK - CFS_OFF_CRC - 4);
    memcpy(gen, b + CFS_OFF_GENERATION, 8);
    return c == crc && *gen != 0;
}

/* The superblock a mount would take, or NULL: both slots are read, since
 * commits alternate between them and a torn one fails its CRC. */
static const uint8_t *cosmofs_super(const struct cosmo_blkctl_dev *d)
{
    if (d->sector_size == 0 || d->sector_size > CFS_BLOCK || CFS_BLOCK % d->sector_size != 0 ||
        blkdevs_read(d, 0, 2 * CFS_BLOCK / d->sector_size, g_slots) != 0)
        return NULL;
    uint64_t ga = 0, gb = 0;
    bool a = slot_ok(g_slots, &ga), b = slot_ok(g_slots + CFS_BLOCK, &gb);
    if (a && (!b || ga >= gb))
        return g_slots;
    return b ? g_slots + CFS_BLOCK : NULL;
}

static int gpt_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    return blkdevs_read(ctx, lba, count, buf);
}

/* Print the device's line; false when it has nothing to show. */
static bool probe(const struct cosmo_blkctl_dev *d)
{
    char line[256];
    int n = 0;
    const uint8_t *sb = cosmofs_super(d);
    if (sb != NULL) {
        uint32_t version;
        memcpy(&version, sb + CFS_OFF_VERSION, 4);
        char label[CFS_LABEL_MAX + 1] = "";
        memcpy(label, sb + CFS_OFF_LABEL, CFS_LABEL_MAX);
        if (label[0] != '\0')
            n += snprintf(line + n, sizeof(line) - (size_t)n, " LABEL=\"%s\"", label);
        if (version >= 4 && !zero(sb + CFS_OFF_UUID, 16)) {   /* the pool uuid came with version 4 */
            char u[37];
            uuid_text(sb + CFS_OFF_UUID, u);
            n += snprintf(line + n, sizeof(line) - (size_t)n, " UUID=\"%s\"", u);
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " TYPE=\"cosmofs\"");
        if (cfs_super_is_mac((const struct cfs_super *)sb))
            n += snprintf(line + n, sizeof(line) - (size_t)n, " ENCRYPTED=\"yes\"");
    } else if (!(d->flags & COSMO_BLKCTL_PART)) {
        const char *why = NULL;
        static struct cosmo_blkctl_dev disk;   /* the read callback's context, which is not const */
        disk = *d;
        if (gpt_parse(d->sector_size, d->sectors, gpt_read, &disk, g_scratch, &g_table, &why) == GPT_OK) {
            char g[GPT_GUID_TEXT];
            gpt_guid_format(g_table.disk_guid, g);
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
    int rc = blkdevs_list();
    if (rc < 0) {
        fprintf(stderr, "blkid: /dev/blkctl: %s\n", strerror(-rc));
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
