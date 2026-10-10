/*
 * cosmo-install - put the running system on a disk (roadmap M2;
 * docs/userland/design.md, "The installer").
 *
 *   cosmo-install --list            the block devices, as /dev/blkctl sees them
 *   cosmo-install [--force] DEVICE  install on DEVICE (a whole disk)
 *
 * The disk becomes: a GPT, partition 1 a copy of the EFI System Partition
 * the loader was read from (the same loader, kernel and boot archive),
 * partition 2 a cosmofs filesystem holding the running system's /bin,
 * /sbin, /etc and package database, and the ESP's command-line slot
 * rewritten to root=PARTUUID=<partition 2>. The installer never writes
 * FAT: the slot is one sector found by its marker (docs/boot/design.md,
 * "The command line").
 *
 * Non-interactive. A disk that already has a partition table is refused
 * unless --force. Exit 0 installed, 1 failed (nothing left mounted, the
 * new table wiped), 2 usage, 3 refused.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cosmo/auxv.h>
#include <kernel/crypto.h>
#include <kernel/gpt.h>
#include <uapi/cosmo/blkctl.h>

#define MNT "/mnt/cosmo-install"
#define CHUNK COSMO_BLKCTL_IO_MAX
#define ALIGN_BYTES (1024u * 1024u)          /* partitions start on 1 MiB */
#define MIN_ROOT_BYTES (32u * 1024u * 1024u)

static int g_ctl = -1;
static uint8_t g_io[sizeof(struct cosmo_blkctl) + CHUNK];
static uint8_t g_res[64 * 1024];

/* --- the channel ----------------------------------------------------------- */

/* One command; its result (if any) into g_res. Returns the result's
 * length or -errno. */
static long ctl(unsigned op, const char *name, uint64_t sector, uint32_t count, const void *data, size_t len,
                const char *fstype)
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
    memcpy(g_io, &cmd, sizeof(cmd));
    if (len)
        memcpy(g_io + sizeof(cmd), data, len);
    if (write(g_ctl, g_io, sizeof(cmd) + len) < 0)
        return -errno;
    long n = read(g_ctl, g_res, sizeof(g_res));
    return n < 0 ? -errno : n;
}

static struct cosmo_blkctl_dev g_devs[128];
static unsigned g_ndevs;

static int list_devices(void)
{
    long n = ctl(COSMO_BLKCTL_LIST, NULL, 0, 0, NULL, 0, NULL);
    if (n < (long)sizeof(struct cosmo_blkctl_list))
        return n < 0 ? (int)n : -EIO;
    struct cosmo_blkctl_list hdr;
    memcpy(&hdr, g_res, sizeof(hdr));
    g_ndevs = hdr.count < 128 ? hdr.count : 128;
    memcpy(g_devs, g_res + sizeof(hdr), g_ndevs * sizeof(g_devs[0]));
    return 0;
}

static const struct cosmo_blkctl_dev *find_dev(const char *name)
{
    for (unsigned i = 0; i < g_ndevs; i++)
        if (strcmp(g_devs[i].name, name) == 0)
            return &g_devs[i];
    return NULL;
}

static const struct cosmo_blkctl_dev *find_part(const char *disk, uint32_t index)
{
    for (unsigned i = 0; i < g_ndevs; i++)
        if ((g_devs[i].flags & COSMO_BLKCTL_PART) && g_devs[i].index == index && strcmp(g_devs[i].disk, disk) == 0)
            return &g_devs[i];
    return NULL;
}

/* Write `bytes` (a multiple of the sector size) at byte `off`. */
static int dev_write(const struct cosmo_blkctl_dev *d, uint64_t off, const void *buf, size_t bytes)
{
    const uint8_t *p = buf;
    while (bytes > 0) {
        size_t n = bytes < CHUNK ? bytes : CHUNK;
        long rc = ctl(COSMO_BLKCTL_WRITE, d->name, off / d->sector_size, (uint32_t)(n / d->sector_size), p, n, NULL);
        if (rc < 0)
            return (int)rc;
        off += n;
        p += n;
        bytes -= n;
    }
    return 0;
}

static int dev_read(const struct cosmo_blkctl_dev *d, uint64_t off, void *buf, size_t bytes)
{
    long rc = ctl(COSMO_BLKCTL_READ, d->name, off / d->sector_size, (uint32_t)(bytes / d->sector_size), NULL, 0, NULL);
    if (rc < 0)
        return (int)rc;
    if ((size_t)rc != bytes)
        return -EIO;
    memcpy(buf, g_res, bytes);
    return 0;
}

/* --- GUIDs ------------------------------------------------------------------ */

/* Random version-4 GUIDs from the 16 bytes the kernel put on this
 * process's stack (COSMO_AT_RANDOM), expanded with SHA-512. */
static int new_guid(uint8_t out[16])
{
    static unsigned counter;
    const uint8_t *seed = (const uint8_t *)cosmo_getauxval(COSMO_AT_RANDOM);
    if (seed == NULL)
        return -1;
    struct sha512_ctx c;
    uint8_t h[SHA512_DIGEST_SIZE];
    sha512_init(&c);
    sha512_update(&c, seed, 16);
    sha512_update(&c, "cosmo-install guid", 18);
    sha512_update(&c, &counter, sizeof(counter));
    sha512_final(&c, h);
    counter++;
    memcpy(out, h, 16);
    out[7] = (uint8_t)((out[7] & 0x0F) | 0x40);   /* version 4: the third group is stored little-endian */
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);   /* RFC 4122 variant */
    return 0;
}

/* --- copying the system ------------------------------------------------------- */

static uint8_t g_buf[16384];

static int copy_file(const char *src, const char *dst, uint32_t mode)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cosmo-install: %s: %s\n", src, strerror(errno));
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777);
    if (out < 0) {
        fprintf(stderr, "cosmo-install: %s: %s\n", dst, strerror(errno));
        close(in);
        return -1;
    }
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, g_buf, sizeof(g_buf));
        if (n <= 0) {
            if (n < 0)
                rc = -1;
            break;
        }
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, g_buf + done, (size_t)(n - done));
            if (w <= 0) {
                rc = -1;
                break;
            }
            done += w;
        }
        if (rc)
            break;
    }
    if (rc)
        fprintf(stderr, "cosmo-install: copying %s: %s\n", src, strerror(errno));
    close(in);
    close(out);
    return rc;
}

static unsigned g_files, g_dirs;

static int copy_tree(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) < 0) {
        fprintf(stderr, "cosmo-install: %s: %s\n", src, strerror(errno));
        return -1;
    }
    if (S_ISLNK(st.st_type)) {
        char target[1024];
        long n = readlink(src, target, sizeof(target) - 1);
        if (n < 0)
            return -1;
        target[n] = '\0';
        return symlink(target, dst) < 0 ? -1 : 0;
    }
    if (S_ISREG(st.st_type)) {
        g_files++;
        return copy_file(src, dst, st.st_mode);
    }
    if (!S_ISDIR(st.st_type))
        return 0;   /* device nodes, FIFOs and sockets belong to a running system, not an installed one */
    if (mkdir(dst, st.st_mode & 07777) < 0 && errno != EEXIST) {
        fprintf(stderr, "cosmo-install: %s: %s\n", dst, strerror(errno));
        return -1;
    }
    g_dirs++;
    DIR *d = opendir(src);
    if (d == NULL)
        return -1;
    int rc = 0;
    struct dirent *e;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char s[512], t[512];
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
        rc = copy_tree(s, t);
    }
    closedir(d);
    return rc;
}

/* What the installed system is made of: the running system's own files.
 * /boot is not copied -- the boot archive is on the ESP -- and the
 * filesystems the kernel provides get empty mountpoints. */
static int populate(void)
{
    static const char *const trees[] = { "/bin", "/sbin", "/etc", "/usr", "/var/db" };
    static const struct { const char *path; uint32_t mode; } dirs[] = {
        { "/dev", 0755 }, { "/proc", 0555 }, { "/tmp", 01777 }, { "/mnt", 0755 },
        { "/var", 0755 }, { "/var/log", 0755 }, { "/boot", 0755 },
    };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char t[256];
        snprintf(t, sizeof(t), MNT "%s", dirs[i].path);
        if (mkdir(t, dirs[i].mode) < 0 && errno != EEXIST) {
            fprintf(stderr, "cosmo-install: %s: %s\n", t, strerror(errno));
            return -1;
        }
    }
    for (unsigned i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
        struct stat st;
        if (stat(trees[i], &st) < 0)
            continue;   /* /usr and /var/db exist once a package is installed */
        char t[256];
        snprintf(t, sizeof(t), MNT "%s", trees[i]);
        if (copy_tree(trees[i], t) < 0)
            return -1;
    }
    return 0;
}

/* --- the command-line slot ------------------------------------------------------ */

/* The marker, assembled at run time: the installer's own image is inside
 * the boot archive on the ESP it scans, and must not carry a copy of the
 * marker that could land at the start of a sector. */
static size_t slot_marker(char *m)
{
    strcpy(m, "#cosmo");
    strcat(m, "-cmdline v1\n");
    return strlen(m);
}

/* --- the install ---------------------------------------------------------------- */

static bool g_mounted;
static bool g_table_written;
static const struct cosmo_blkctl_dev *g_target;
static uint8_t g_head[GPT_BUILD_HEAD_SECTORS(512) * 512 > GPT_BUILD_HEAD_SECTORS(4096) * 4096
                     ? GPT_BUILD_HEAD_SECTORS(512) * 512 : GPT_BUILD_HEAD_SECTORS(4096) * 4096];
static uint8_t g_tail[sizeof(g_head)];

static int fail(const char *what, int err)
{
    fprintf(stderr, "cosmo-install: %s: %s\n", what, err ? strerror(err < 0 ? -err : err) : "failed");
    if (g_mounted && umount2(MNT, MNT_FORCE) < 0)
        fprintf(stderr, "cosmo-install: cannot unmount %s: %s\n", MNT, strerror(errno));
    rmdir(MNT);
    if (g_table_written && g_target != NULL) {
        /* No half-installed disk that looks like a system: the table goes. */
        uint32_t ss = g_target->sector_size;
        memset(g_head, 0, sizeof(g_head));
        dev_write(g_target, 0, g_head, (size_t)GPT_BUILD_HEAD_SECTORS(ss) * ss);
        dev_write(g_target, (g_target->sectors - GPT_BUILD_TAIL_SECTORS(ss)) * ss, g_head,
                  (size_t)GPT_BUILD_TAIL_SECTORS(ss) * ss);
        ctl(COSMO_BLKCTL_FLUSH, g_target->name, 0, 0, NULL, 0, NULL);
        ctl(COSMO_BLKCTL_RESCAN, g_target->name, 0, 0, NULL, 0, NULL);
    }
    return 1;
}

/* gpt_parse over the table about to be written, from memory. */
struct mem_disk {
    uint32_t ss;
    uint64_t n;
};

static int mem_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    const struct mem_disk *d = ctx;
    uint64_t head = GPT_BUILD_HEAD_SECTORS(d->ss), tail0 = d->n - GPT_BUILD_TAIL_SECTORS(d->ss);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *out = (uint8_t *)buf + (size_t)i * d->ss;
        uint64_t s = lba + i;
        if (s < head)
            memcpy(out, g_head + s * d->ss, d->ss);
        else if (s >= tail0 && s < d->n)
            memcpy(out, g_tail + (s - tail0) * d->ss, d->ss);
        else
            memset(out, 0, d->ss);
    }
    return 0;
}

static bool has_table(const struct cosmo_blkctl_dev *d, uint8_t *sec)
{
    if (dev_read(d, d->sector_size, sec, d->sector_size) == 0 && memcmp(sec, "EFI PART", 8) == 0)
        return true;
    return dev_read(d, (d->sectors - 1) * d->sector_size, sec, d->sector_size) == 0 && memcmp(sec, "EFI PART", 8) == 0;
}

static void print_list(void)
{
    printf("%-12s %-10s %12s %6s  %s\n", "NAME", "DISK", "SECTORS", "SECTOR", "FLAGS");
    for (unsigned i = 0; i < g_ndevs; i++) {
        const struct cosmo_blkctl_dev *d = &g_devs[i];
        char uuid[GPT_GUID_TEXT] = "";
        if (d->flags & COSMO_BLKCTL_PART)
            gpt_guid_format(d->uuid, uuid);
        printf("%-12s %-10s %12llu %6u  %s%s%s%s%s\n", d->name, d->disk[0] ? d->disk : "-",
               (unsigned long long)d->sectors, d->sector_size,
               (d->flags & COSMO_BLKCTL_RDONLY) ? "ro " : "", (d->flags & COSMO_BLKCTL_BOOT) ? "boot " : "",
               (d->flags & COSMO_BLKCTL_MOUNTED) ? "mounted " : "", uuid[0] ? "partuuid=" : "", uuid);
    }
}

static int install(const char *target_name, bool force)
{
    if (list_devices() < 0)
        return fail("cannot list the block devices", errno);
    const struct cosmo_blkctl_dev *boot = NULL;
    for (unsigned i = 0; i < g_ndevs; i++)
        if (g_devs[i].flags & COSMO_BLKCTL_BOOT)
            boot = &g_devs[i];
    if (boot == NULL) {
        fprintf(stderr, "cosmo-install: the loader was not read from a GPT partition; there is no ESP to copy\n");
        return 3;
    }
    const struct cosmo_blkctl_dev *t = find_dev(target_name);
    if (t == NULL) {
        fprintf(stderr, "cosmo-install: %s: no such block device\n", target_name);
        return 3;
    }
    const char *refuse = NULL;
    if (t->flags & COSMO_BLKCTL_PART)
        refuse = "is a partition; name the whole disk";
    else if (t->flags & COSMO_BLKCTL_RDONLY)
        refuse = "is read-only";
    else if (t->flags & COSMO_BLKCTL_MOUNTED)
        refuse = "is mounted";
    else if (strcmp(t->name, boot->disk) == 0)
        refuse = "is the disk this system booted from";
    else if (t->sector_size != 512 && t->sector_size != 4096)
        refuse = "has a sector size other than 512 or 4096";
    uint32_t ss = t->sector_size;
    uint64_t esp_bytes = boot->sectors * boot->sector_size;
    uint64_t align = ALIGN_BYTES / ss;
    uint64_t esp_first = align;
    uint64_t esp_last = esp_first + (esp_bytes + ss - 1) / ss - 1;
    uint64_t root_first = (esp_last + 1 + align - 1) / align * align;
    uint64_t root_last = t->sectors > GPT_BUILD_TAIL_SECTORS(ss) + 1 ? gpt_build_last_usable(ss, t->sectors) : 0;
    if (refuse == NULL && (root_last <= root_first || (root_last - root_first + 1) * ss < MIN_ROOT_BYTES))
        refuse = "is too small";
    if (refuse) {
        fprintf(stderr, "cosmo-install: %s %s\n", t->name, refuse);
        return 3;
    }
    if (has_table(t, g_buf) && !force) {
        fprintf(stderr, "cosmo-install: %s already has a partition table; --force replaces it\n", t->name);
        return 3;
    }
    g_target = t;

    /* The table, checked by the parser the kernel will use before it
     * touches the disk. */
    uint8_t disk_guid[16];
    struct gpt_build_part parts[2];
    memset(parts, 0, sizeof(parts));
    if (new_guid(disk_guid) || new_guid(parts[0].uuid) || new_guid(parts[1].uuid)) {
        fprintf(stderr, "cosmo-install: no random bytes from the kernel (COSMO_AT_RANDOM)\n");
        return 1;
    }
    parts[0].index = 1;
    parts[0].first_lba = esp_first;
    parts[0].last_lba = esp_last;
    memcpy(parts[0].type, gpt_type_esp, 16);
    parts[1].index = 2;
    parts[1].first_lba = root_first;
    parts[1].last_lba = root_last;
    memcpy(parts[1].type, gpt_type_cosmo_root, 16);
    if (!gpt_build(ss, t->sectors, disk_guid, parts, 2, g_head, g_tail))
        return fail("cannot lay out the partition table", 0);
    static uint8_t scratch[GPT_SCRATCH_BYTES];
    static struct gpt_table check;
    const char *why = "";
    struct mem_disk md = { ss, t->sectors };
    if (gpt_parse(ss, t->sectors, mem_read, &md, scratch, &check, &why) != GPT_OK || check.nparts != 2) {
        fprintf(stderr, "cosmo-install: the table built does not parse: %s\n", why);
        return 1;
    }
    char root_uuid[GPT_GUID_TEXT];
    gpt_guid_format(parts[1].uuid, root_uuid);
    printf("cosmo-install: %s: esp sectors %llu-%llu, root sectors %llu-%llu\n", t->name,
           (unsigned long long)esp_first, (unsigned long long)esp_last, (unsigned long long)root_first,
           (unsigned long long)root_last);
    fflush(stdout);

    /* Backup first, primary last: until the primary lands, the disk has
     * no table the kernel accepts. */
    g_table_written = true;
    int rc = dev_write(t, (t->sectors - GPT_BUILD_TAIL_SECTORS(ss)) * ss, g_tail, (size_t)GPT_BUILD_TAIL_SECTORS(ss) * ss);
    if (rc == 0)
        rc = dev_write(t, 0, g_head, (size_t)GPT_BUILD_HEAD_SECTORS(ss) * ss);
    if (rc == 0)
        rc = (int)ctl(COSMO_BLKCTL_FLUSH, t->name, 0, 0, NULL, 0, NULL);
    if (rc < 0)
        return fail("writing the partition table", rc);
    long n = ctl(COSMO_BLKCTL_RESCAN, t->name, 0, 0, NULL, 0, NULL);
    struct cosmo_blkctl_rescan rs;
    if (n != (long)sizeof(rs))
        return fail("rescanning the partitions", n < 0 ? (int)n : EIO);
    memcpy(&rs, g_res, sizeof(rs));
    char tname[COSMO_BLKCTL_NAME];
    strcpy(tname, t->name);
    if (rs.partitions != 2 || list_devices() < 0)
        return fail("the new partitions did not appear", 0);
    g_target = find_dev(tname);   /* the list was re-read */
    const struct cosmo_blkctl_dev *esp = find_part(tname, 1), *root = find_part(tname, 2);
    boot = NULL;
    for (unsigned i = 0; i < g_ndevs; i++)
        if (g_devs[i].flags & COSMO_BLKCTL_BOOT)
            boot = &g_devs[i];
    if (g_target == NULL || esp == NULL || root == NULL || boot == NULL)
        return fail("the new partitions did not appear", 0);

    /* Partition 1: the boot ESP, byte for byte, but for the slot. */
    char marker[32];
    size_t mlen = slot_marker(marker);
    uint64_t slot = 0;
    unsigned slots = 0;
    for (uint64_t off = 0; off < esp_bytes; off += CHUNK) {
        size_t len = esp_bytes - off < CHUNK ? (size_t)(esp_bytes - off) : CHUNK;
        static uint8_t chunk[CHUNK];
        rc = dev_read(boot, off, chunk, len);
        if (rc == 0)
            rc = dev_write(esp, off, chunk, len);
        if (rc < 0)
            return fail("copying the EFI system partition", rc);
        for (size_t s = 0; s < len; s += 512) {
            if (memcmp(chunk + s, marker, mlen) == 0) {
                slot = off + s;
                slots++;
            }
        }
    }
    if (slots != 1) {
        fprintf(stderr, "cosmo-install: the ESP has %u command-line slots, not one\n", slots);
        return fail("finding the command-line slot", 0);
    }
    static uint8_t sec[512];
    memset(sec, 0, sizeof(sec));
    snprintf((char *)sec, sizeof(sec), "%sroot=PARTUUID=%s\n", marker, root_uuid);
    /* The slot is one 512-byte sector of the FAT image; on a disk with
     * larger sectors, rewrite the sector that holds it. */
    uint64_t at = slot / ss * ss;
    static uint8_t big[4096];
    rc = dev_read(esp, at, big, ss);
    if (rc == 0) {
        memcpy(big + (slot - at), sec, sizeof(sec));
        rc = dev_write(esp, at, big, ss);
    }
    if (rc < 0)
        return fail("writing the command line", rc);

    /* Partition 2: an empty cosmofs, then the system. */
    rc = (int)ctl(COSMO_BLKCTL_FORMAT, root->name, 0, 0, NULL, 0, "cosmofs");
    if (rc < 0)
        return fail("making the cosmofs root", rc);
    if (mkdir(MNT, 0755) < 0 && errno != EEXIST)
        return fail("creating " MNT, errno);
    if (mount(root->name, MNT, "cosmofs", 0) < 0)
        return fail("mounting the new root", errno);
    g_mounted = true;
    if (populate() < 0)
        return fail("copying the system", 0);
    sync();
    if (umount(MNT) < 0)
        return fail("unmounting the new root", errno);
    g_mounted = false;
    rmdir(MNT);
    rc = (int)ctl(COSMO_BLKCTL_FLUSH, tname, 0, 0, NULL, 0, NULL);
    if (rc < 0)
        return fail("flushing the disk", rc);
    printf("cosmo-install: copied %u files in %u directories\n", g_files, g_dirs);
    printf("cosmo-install: installed on %s: esp %s, root %s, root=PARTUUID=%s\n", tname, esp->name, root->name,
           root_uuid);
    return 0;
}

int main(int argc, char **argv)
{
    bool force = false, list = false;
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0)
            force = true;
        else if (strcmp(argv[i], "--list") == 0)
            list = true;
        else if (argv[i][0] != '-' && target == NULL)
            target = argv[i];
        else
            target = NULL, list = false, i = argc;
    }
    if (!list && target == NULL) {
        fprintf(stderr, "usage: cosmo-install --list | cosmo-install [--force] DEVICE\n");
        return 2;
    }
    g_ctl = open("/dev/blkctl", O_RDWR);
    if (g_ctl < 0) {
        fprintf(stderr, "cosmo-install: /dev/blkctl: %s\n", strerror(errno));
        return 1;
    }
    if (list) {
        if (list_devices() < 0) {
            fprintf(stderr, "cosmo-install: cannot list the block devices\n");
            return 1;
        }
        print_list();
        return 0;
    }
    int rc = install(target, force);
    close(g_ctl);
    return rc;
}
