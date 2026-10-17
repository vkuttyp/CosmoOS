/* blkdevs.c - The block devices through /dev/blkctl (blkdevs.h). */
#include "blkdevs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct cosmo_blkctl_dev g_blkdevs[BLKDEVS_MAX];
unsigned g_nblkdevs;

static int g_ctl = -1;
static uint8_t g_io[sizeof(struct cosmo_blkctl) + COSMO_BLKCTL_IO_MAX];   /* a FORMAT's label, a WRITE's data */
static uint8_t g_res[64 * 1024];

/* One command, its data after the struct; its result into g_res. The
 * result's length or -errno. */
static long ctl(unsigned op, const char *name, uint64_t sector, uint32_t count, const char *fstype, const void *data,
                size_t len)
{
    struct cosmo_blkctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_BLKCTL_VERSION;
    cmd.op = (uint16_t)op;
    cmd.sector = sector;
    cmd.count = count;
    if (name)
        strncpy(cmd.name, name, sizeof(cmd.name) - 1);
    if (fstype)
        strncpy(cmd.fstype, fstype, sizeof(cmd.fstype) - 1);
    if (len > sizeof(g_io) - sizeof(cmd))
        return -EINVAL;
    memcpy(g_io, &cmd, sizeof(cmd));
    if (len)
        memcpy(g_io + sizeof(cmd), data, len);
    if (write(g_ctl, g_io, sizeof(cmd) + len) < 0)
        return -errno;
    long n = read(g_ctl, g_res, sizeof(g_res));
    return n < 0 ? -errno : n;
}

int blkdevs_list(void)
{
    if (g_ctl < 0) {
        g_ctl = open("/dev/blkctl", O_RDWR);
        if (g_ctl < 0)
            return -errno;
    }
    long n = ctl(COSMO_BLKCTL_LIST, NULL, 0, 0, NULL, NULL, 0);
    if (n < (long)sizeof(struct cosmo_blkctl_list))
        return n < 0 ? (int)n : -EIO;
    struct cosmo_blkctl_list hdr;
    memcpy(&hdr, g_res, sizeof(hdr));
    if (hdr.count > BLKDEVS_MAX)
        return -E2BIG;   /* not a list with devices silently left out (found in review) */
    if ((size_t)n < sizeof(hdr) + hdr.count * sizeof(g_blkdevs[0]))
        return -EIO;
    g_nblkdevs = hdr.count;
    memcpy(g_blkdevs, g_res + sizeof(hdr), g_nblkdevs * sizeof(g_blkdevs[0]));
    return 0;
}

const struct cosmo_blkctl_dev *blkdevs_find(const char *name)
{
    if (strncmp(name, "/dev/", 5) == 0)
        name += 5;
    for (unsigned i = 0; i < g_nblkdevs; i++)
        if (strcmp(g_blkdevs[i].name, name) == 0)
            return &g_blkdevs[i];
    return NULL;
}

int blkdevs_read(const struct cosmo_blkctl_dev *d, uint64_t lba, uint64_t count, void *buf)
{
    if (d->sector_size == 0 || d->sector_size > COSMO_BLKCTL_IO_MAX || lba > d->sectors || count > d->sectors - lba)
        return -EINVAL;
    uint8_t *p = buf;
    uint32_t per = COSMO_BLKCTL_IO_MAX / d->sector_size;   /* sectors a read may carry: fits the 32-bit count */
    while (count > 0) {
        uint32_t n = count < per ? (uint32_t)count : per;
        size_t bytes = (size_t)n * d->sector_size;
        long rc = ctl(COSMO_BLKCTL_READ, d->name, lba, n, NULL, NULL, 0);
        if (rc < 0)
            return (int)rc;
        if ((size_t)rc != bytes)
            return -EIO;
        memcpy(p, g_res, bytes);
        p += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

int blkdevs_write(const struct cosmo_blkctl_dev *d, uint64_t lba, uint64_t count, const void *buf)
{
    if (d->sector_size == 0 || d->sector_size > COSMO_BLKCTL_IO_MAX || lba > d->sectors || count > d->sectors - lba)
        return -EINVAL;
    const uint8_t *p = buf;
    uint32_t per = COSMO_BLKCTL_IO_MAX / d->sector_size;
    while (count > 0) {
        uint32_t n = count < per ? (uint32_t)count : per;
        size_t bytes = (size_t)n * d->sector_size;
        long rc = ctl(COSMO_BLKCTL_WRITE, d->name, lba, n, NULL, p, bytes);
        if (rc < 0)
            return (int)rc;
        p += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

int blkdevs_flush(const struct cosmo_blkctl_dev *d)
{
    long rc = ctl(COSMO_BLKCTL_FLUSH, d->name, 0, 0, NULL, NULL, 0);
    return rc < 0 ? (int)rc : 0;
}

int blkdevs_format(const char *name, const char *fstype, const char *label)
{
    if (strncmp(name, "/dev/", 5) == 0)
        name += 5;
    long rc = ctl(COSMO_BLKCTL_FORMAT, name, 0, 0, fstype, label, strlen(label));
    return rc < 0 ? (int)rc : 0;
}

/* A /proc/mounts line: two names of up to VFS_PATH_MAX (1024) bytes, each
 * byte written as at most four, and the rest. A longer line is skipped. */
static char g_line[2 * 4 * 1024 + 128];

int blkdevs_mountpoint(const char *name, unsigned which, char *out, size_t len)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (f == NULL)
        return 0;
    int found = 0;
    bool skipping = false;   /* the rest of a line too long for g_line */
    while (!found && fgets(g_line, sizeof(g_line), f) != NULL) {
        bool whole = strchr(g_line, '\n') != NULL || feof(f);
        if (skipping || !whole) {
            skipping = !whole;
            continue;
        }
        /* "DEVICE DIR TYPE OPTIONS 0 0": the first two fields. */
        char *dir = strchr(g_line, ' ');
        if (dir == NULL)
            continue;
        *dir++ = '\0';
        char *end = strchr(dir, ' ');
        if (end == NULL || strncmp(g_line, "/dev/", 5) != 0 || strcmp(g_line + 5, name) != 0)
            continue;
        if (which-- != 0)
            continue;
        *end = '\0';
        /* /proc/mounts writes a blank, tab, newline or backslash as \ooo. */
        size_t o = 0;
        for (const char *s = dir; *s != '\0' && o + 1 < len; o++) {
            if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' &&
                s[3] <= '7') {
                out[o] = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
                s += 4;
            } else {
                out[o] = *s++;
            }
        }
        out[o] = '\0';
        found = 1;
    }
    fclose(f);
    return found;
}
