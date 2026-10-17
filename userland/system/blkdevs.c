/* blkdevs.c - The block devices through /dev/blkctl (blkdevs.h). */
#include "blkdevs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct cosmo_blkctl_dev g_blkdevs[BLKDEVS_MAX];
unsigned g_nblkdevs;

static int g_ctl = -1;
static uint8_t g_io[sizeof(struct cosmo_blkctl) + COSMO_BLKCTL_LABEL];
static uint8_t g_res[64 * 1024];

/* One command, its data after the struct; its result into g_res. The
 * result's length or -errno. */
static long ctl(unsigned op, const char *name, uint32_t count, const char *fstype, const void *data, size_t len)
{
    struct cosmo_blkctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_BLKCTL_VERSION;
    cmd.op = (uint16_t)op;
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
    long n = ctl(COSMO_BLKCTL_LIST, NULL, 0, NULL, NULL, 0);
    if (n < (long)sizeof(struct cosmo_blkctl_list))
        return n < 0 ? (int)n : -EIO;
    struct cosmo_blkctl_list hdr;
    memcpy(&hdr, g_res, sizeof(hdr));
    g_nblkdevs = hdr.count < BLKDEVS_MAX ? hdr.count : BLKDEVS_MAX;
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

int blkdevs_read_head(const struct cosmo_blkctl_dev *d, void *buf, size_t bytes)
{
    if (d->sector_size == 0 || bytes % d->sector_size != 0 || bytes / d->sector_size > d->sectors)
        return -EINVAL;
    long rc = ctl(COSMO_BLKCTL_READ, d->name, (uint32_t)(bytes / d->sector_size), NULL, NULL, 0);
    if (rc < 0)
        return (int)rc;
    if ((size_t)rc != bytes)
        return -EIO;
    memcpy(buf, g_res, bytes);
    return 0;
}

int blkdevs_format(const char *name, const char *fstype, const char *label)
{
    if (strncmp(name, "/dev/", 5) == 0)
        name += 5;
    long rc = ctl(COSMO_BLKCTL_FORMAT, name, 0, fstype, label, strlen(label));
    return rc < 0 ? (int)rc : 0;
}

int blkdevs_mountpoint(const char *name, char *out, size_t len)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (f == NULL)
        return 0;
    char line[1200];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        /* "DEVICE DIR TYPE OPTIONS 0 0": the first two fields. */
        char *dir = strchr(line, ' ');
        if (dir == NULL)
            continue;
        *dir++ = '\0';
        char *end = strchr(dir, ' ');
        if (end == NULL || strncmp(line, "/dev/", 5) != 0 || strcmp(line + 5, name) != 0)
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
