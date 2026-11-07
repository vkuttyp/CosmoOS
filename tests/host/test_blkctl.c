/*
 * test_blkctl.c - /dev/blkctl (kernel-services/vfs/blkctl.c) on the host: roadmap 2.0 SA7. A digest recorded from
 * the C over random sessions: files opened with and without the privilege, and while the allocator fails; commands
 * of every op, well formed and not -- short, of the wrong version, with data an op does not take, names and types
 * without their NUL, labels of every length and byte, transfers of every size, mirrors and keyed pools named and
 * not -- against devices that are mounted, partitioned, read-only, test disks or gone, whose reads, writes, flushes,
 * rescans and formats fail now and then; each result read back into buffers of every size. Every call into the
 * kernel with its arguments, every result, every byte read back and every allocation's contents when it is freed
 * (so the keyed format's wipe of the master key is in it) are in the digest; every device reference is given back.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/bootinfo.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/part.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/blkctl.h>

/* The C library's headers are not included: their pid_t is not the kernel's. */
void *malloc(size_t size);
void free(void *p);
int printf(const char *fmt, ...);

void blkctl_dev_init(void);

/* The digest of the C (blkctl.c, 2026-11-07), which the CeeSafe port gives. */
#define BLKCTL_DIGEST 0xa8eae3202115e31full
#define ROUNDS 20000u
#define SEED 0xB1C7C0DE20261107ull
#define NDEV 6u
#define NFILES 3u

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void note(uint32_t which, uint64_t value)
{
    mix(&which, sizeof(which));
    mix(&value, sizeof(value));
}

static void note_str(uint32_t which, const char *s)
{
    size_t n = s == NULL ? 0 : strlen(s);
    note(which, s == NULL ? ~0ull : n);
    mix(s, n);
}

static bool g_faults;

static bool chance(unsigned n) { return g_faults && pick(n) == 0; }

size_t (strlcpy)(char *dst, const char *src, size_t size)   /* parenthesised: a fortified libc makes strlcpy a macro */
{
    size_t n = strlen(src);
    if (size > 0) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
}

/* ---- the allocator: sized, filled with a pattern, failing now and then ---- */

static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    note(1, size);
    note(2, flags);
    if (chance(12)) {
        note(3, 0);
        return NULL;
    }
    size_t *p = malloc(sizeof(size_t) * 2 + size);
    EXPECT(p != NULL);
    p[0] = size;
    memset(p + 2, (flags & KMEM_ZERO) ? 0 : 0xA5, size);
    g_live++;
    return p + 2;
}

void kfree(void *q)
{
    if (q == NULL)
        return;
    size_t *p = (size_t *)q - 2;
    note(4, p[0]);
    if (p[0] == sizeof(struct cosmo_blkctl_keyed))   /* the command's copy: its key must be wiped. Not every block --
                                                      * one may hold a stale pointer, an address */
        mix(q, p[0]);
    g_live--;
    free(p);
}

/* ---- locks and credentials ---- */

static unsigned g_mutexes;

void mutex_init(struct mutex *m, const char *name)
{
    (void)m;
    note_str(5, name);
    g_mutexes++;
}

static bool g_locked;

void mutex_lock(struct mutex *m)
{
    (void)m;
    EXPECT(!g_locked);
    g_locked = true;
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked);
    g_locked = false;
}

static struct credentials g_cred;

const struct credentials *cred_current(void) { return &g_cred; }

/* ---- the devices ---- */

static struct blkdev g_dev[NDEV];
static const char *const g_names[NDEV] = { "vda", "vda1", "vda2", "vdb", "ram0", "vdc" };
static bool g_mounted[NDEV], g_test[NDEV], g_parts[NDEV];
static long g_refs;
static unsigned g_count, g_vanish_at;
static uint8_t g_boot[16];
static bool g_have_boot;

static unsigned dev_index(const struct blkdev *bd)
{
    EXPECT(bd >= g_dev && bd < g_dev + NDEV);
    return (unsigned)(bd - g_dev);
}

static struct blkdev *take(unsigned i)
{
    g_dev[i].obj.refcount++;
    g_refs++;
    return &g_dev[i];
}

void kobject_put(struct kobject *obj)
{
    struct blkdev *bd = (struct blkdev *)obj;
    note(6, dev_index(bd));
    EXPECT(obj->refcount > 1);
    obj->refcount--;
    g_refs--;
}

unsigned blk_count(void)
{
    note(7, g_count);
    return g_count;
}

struct blkdev *blk_nth(unsigned i)
{
    note(8, i);
    if (i >= g_count || i >= g_vanish_at)
        return NULL;
    return take(i);
}

struct blkdev *blk_find(const char *name)
{
    note_str(9, name);
    for (unsigned i = 0; i < g_count; i++)
        if (strcmp(name, g_names[i]) == 0)
            return take(i);
    return NULL;
}

bool vfs_bdev_mounted(const struct blkdev *bd)
{
    unsigned i = dev_index(bd);
    note(10, i);
    return g_mounted[i];
}

bool blk_is_test_disk(const struct blkdev *bd)
{
    unsigned i = dev_index(bd);
    note(11, i);
    return g_test[i];
}

bool blk_part_any(const struct blkdev *disk)
{
    unsigned i = dev_index(disk);
    note(12, i);
    return g_parts[i];
}

/* vda1 and vda2 are vda's partitions. */
bool blk_part_info(const struct blkdev *bd, struct blk_part_info *out)
{
    unsigned i = dev_index(bd);
    note(13, i);
    if (i != 1 && i != 2)
        return false;
    memset(out, 0x5A, sizeof(*out));
    strlcpy(out->disk, "vda", sizeof(out->disk));
    out->index = i;
    out->start = 2048ull * i;
    for (unsigned k = 0; k < 16; k++) {
        out->uuid[k] = (uint8_t)(0x10 * i + k);
        out->type[k] = (uint8_t)(0xC0 + k);
    }
    return true;
}

bool bootinfo_boot_partuuid(uint8_t out[16])
{
    if (!g_have_boot)
        return false;
    memcpy(out, g_boot, 16);
    return true;
}

int blk_read(struct blkdev *bd, uint64_t sector, uint32_t nsectors, void *buf)
{
    note(14, dev_index(bd));
    note(15, sector);
    note(16, nsectors);
    if (chance(6))
        return -EIO;
    uint8_t *b = buf;
    for (size_t k = 0; k < (size_t)nsectors * bd->sector_size; k++)
        b[k] = (uint8_t)(sector * 7 + k * 13 + dev_index(bd));
    return 0;
}

int blk_write(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf)
{
    note(17, dev_index(bd));
    note(18, sector);
    note(19, nsectors);
    mix(buf, (size_t)nsectors * bd->sector_size);
    return chance(6) ? -EIO : 0;
}

int blk_flush(struct blkdev *bd)
{
    note(20, dev_index(bd));
    return chance(6) ? -EIO : 0;
}

int blk_part_scan(struct blkdev *disk, unsigned *nparts)
{
    note(21, dev_index(disk));
    if (chance(5))
        return -EBUSY;
    *nparts = pick(5);
    return 0;
}

int vfs_format(const char *fsname, struct blkdev *bd, const char *label)
{
    note_str(22, fsname);
    note(23, dev_index(bd));
    note_str(24, label);
    return chance(5) ? -EBUSY : 0;
}

int vfs_format_mirror(const char *fsname, struct blkdev **bd, unsigned n, const char *label)
{
    note_str(25, fsname);
    note(26, n);
    for (unsigned i = 0; i < n; i++)
        note(27, dev_index(bd[i]));
    note_str(28, label);
    return chance(5) ? -EBUSY : 0;
}

int vfs_format_keyed(const char *fsname, struct blkdev **bd, unsigned n, const char *label, const uint8_t uuid[16],
                     const uint8_t master[32], const struct cosmo_keyslot *slots)
{
    note_str(29, fsname);
    note(30, n);
    for (unsigned i = 0; i < n; i++)
        note(31, dev_index(bd[i]));
    note_str(32, label);
    mix(uuid, 16);
    mix(master, 32);
    mix(slots, sizeof(struct cosmo_keyslot) * COSMO_KEY_SLOTS);
    return chance(5) ? -EIO : 0;
}

/* ---- the device node and the log ---- */

static const struct chrdev_ops *g_ops;
static struct vnode g_node;

int ramfs_mkchr(const char *path, uint32_t mode, const struct chrdev_ops *ops, void *priv, struct vnode **out)
{
    note_str(33, path);
    note(34, mode);
    note(35, priv != NULL);
    static unsigned calls;
    if (calls++ == 0)   /* the first fails, as when the node exists */
        return -EEXIST;
    g_ops = ops;
    *out = &g_node;
    return 0;
}

static void on_klog(int level, const char *line)
{
    note(36, (uint64_t)level);
    note_str(37, line);
}

/* ---- commands ---- */

static void rand_bytes(void *p, size_t n)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)next();
}

/* A name: a device's, one that is none, or 16 bytes without a NUL. */
static void rand_name(char *out, size_t size)
{
    memset(out, 0, size);
    unsigned w = pick(8);
    if (w == 0)
        memset(out, 'x', size);
    else if (w == 1)
        strlcpy(out, "nope", size);
    else
        strlcpy(out, g_names[pick(NDEV)], size);
}

/* A label: of any length up to one past the limit, printable or with a byte FORMAT refuses. */
static size_t rand_label(char *out, size_t room)
{
    size_t n = pick(COSMO_BLKCTL_LABEL + 2);
    if (n > room)
        n = room;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)(0x21 + pick(0x5E));
    if (n > 0 && pick(4) == 0) {
        static const char bad[] = { ' ', '"', '\\', 0x7F, 0x10, (char)0x80, (char)0xFF, 0x20 };
        size_t at = pick((unsigned)n);
        out[at] = bad[pick(sizeof(bad))];
    }
    return n;
}

static uint8_t g_wbuf[sizeof(struct cosmo_blkctl) + COSMO_BLKCTL_IO_MAX + 64];
static uint8_t g_rbuf[COSMO_BLKCTL_IO_MAX + 4096];

static size_t build(void)
{
    struct cosmo_blkctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = pick(16) == 0 ? (uint16_t)pick(3) : COSMO_BLKCTL_VERSION;
    cmd.op = (uint16_t)pick(10);
    unsigned c = pick(8);
    cmd.count = c == 0 ? 0 : c == 1 ? (uint32_t)next() : c == 2 ? 64 + pick(8) : 1 + pick(8);
    cmd.sector = next();   /* each draw its own statement: the order is the same under every compiler */
    cmd.sector >>= pick(64);
    rand_name(cmd.name, sizeof(cmd.name));
    if (pick(8) == 0)
        memset(cmd.fstype, 'f', sizeof(cmd.fstype));
    else
        strlcpy(cmd.fstype, pick(4) ? "cosmofs" : "nofs", sizeof(cmd.fstype));
    memcpy(g_wbuf, &cmd, sizeof(cmd));
    size_t extra = 0;
    uint8_t *x = g_wbuf + sizeof(cmd);
    switch (cmd.op) {
    case COSMO_BLKCTL_WRITE: {
        unsigned i = pick(NDEV);
        size_t bytes = (size_t)(cmd.count & 0xFFFF) * g_dev[i].sector_size;
        if (bytes > COSMO_BLKCTL_IO_MAX)
            bytes = COSMO_BLKCTL_IO_MAX;
        extra = bytes;
        if (pick(5) == 0) {   /* one short or one over */
            if (pick(2) && bytes > 0)
                extra = bytes - 1;
            else
                extra = bytes + 1;
        }
        rand_bytes(x, extra);
        break;
    }
    case COSMO_BLKCTL_FORMAT:
        extra = rand_label((char *)x, 64);
        break;
    case COSMO_BLKCTL_FORMAT_MIRROR:
        if (pick(6) == 0) {
            extra = pick(COSMO_BLKCTL_NAME + COSMO_BLKCTL_LABEL + 4);
            rand_bytes(x, extra);
        } else {
            rand_name((char *)x, COSMO_BLKCTL_NAME);
            extra = COSMO_BLKCTL_NAME + rand_label((char *)x + COSMO_BLKCTL_NAME, 64);
        }
        break;
    case COSMO_BLKCTL_FORMAT_KEYED: {
        struct cosmo_blkctl_keyed k;
        rand_bytes(&k, sizeof(k));
        if (pick(3) == 0)
            memset(k.mirror, 0, sizeof(k.mirror));
        else
            rand_name(k.mirror, sizeof(k.mirror));
        memset(k.label, 0, sizeof(k.label));
        rand_label(k.label, sizeof(k.label));
        memcpy(x, &k, sizeof(k));
        extra = sizeof(k);
        if (pick(8) == 0)
            extra = sizeof(k) + 1 - pick(3);
        break;
    }
    default:
        if (pick(8) == 0) {
            extra = 1 + pick(32);
            rand_bytes(x, extra);
        }
        break;
    }
    size_t len = sizeof(cmd) + extra;
    if (pick(24) == 0)
        len = pick((unsigned)sizeof(cmd));
    note(38, cmd.op);
    note(39, len);
    return len;
}

static void read_back(struct file *f)
{
    unsigned w = pick(4);
    size_t len = w == 0 ? pick(64) : w == 1 ? pick(4096) : sizeof(g_rbuf);
    memset(g_rbuf, 0xEE, sizeof(g_rbuf));
    int64_t n = g_ops->read_file(&g_node, f, next(), g_rbuf, len);
    note(40, (uint64_t)n);
    if (n > 0)
        mix(g_rbuf, (size_t)n);
}

static void reshape(void)
{
    g_count = pick(NDEV + 1);
    g_vanish_at = pick(4) == 0 ? pick(NDEV) : NDEV;
    for (unsigned i = 0; i < NDEV; i++) {
        g_mounted[i] = pick(4) == 0;
        g_test[i] = pick(5) == 0;
        g_parts[i] = pick(4) == 0;
        g_dev[i].read_only = pick(6) == 0;
    }
    g_have_boot = pick(2);
    unsigned which = 1 + pick(2);   /* vda1's or vda2's */
    for (unsigned k = 0; k < 16; k++)
        g_boot[k] = (uint8_t)(0x10 * which + k);
    if (pick(4) == 0)
        g_boot[pick(16)] ^= 1;
}

static void test_sessions(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    int min = harness_klog_min;
    harness_klog_min = KLOG_ERROR + 1;

    for (unsigned i = 0; i < NDEV; i++) {
        strlcpy(g_dev[i].name, g_names[i], sizeof(g_dev[i].name));
        g_dev[i].sector_size = i == 3 ? 4096 : 512;
        g_dev[i].capacity = 100000ull * (i + 1);
        g_dev[i].obj.refcount = 1;
    }
    blkctl_dev_init();
    EXPECT(g_ops == NULL);
    blkctl_dev_init();
    EXPECT(g_ops != NULL);

    static struct file files[NFILES];
    for (unsigned round = 0; round < ROUNDS; round++) {
        g_faults = round % 3 != 0;
        if (round % 40 == 0)
            reshape();
        g_cred.euid = pick(12) == 0 ? 1000 : CRED_ROOT_UID;
        g_cred.privs = pick(12) == 0 ? 0 : PRIV_SYS_ADMIN;
        struct file *f = &files[pick(NFILES)];
        unsigned op = pick(16);
        if (op == 0) {
            if (f->priv != NULL) {
                g_ops->release(&g_node, f);
                note(41, f->priv == NULL);
            }
            int rc = g_ops->open(&g_node, f);
            note(42, (uint64_t)(int64_t)rc);
            note(43, f->priv != NULL);
        } else if (op == 1) {
            g_ops->release(&g_node, f);
            note(44, f->priv == NULL);
        } else if (op <= 4) {
            read_back(f);
        } else {
            size_t len = build();
            int64_t rc = g_ops->write_file(&g_node, f, next(), g_wbuf, len);
            note(45, (uint64_t)rc);
            EXPECT(!g_locked);
            if (pick(2))
                read_back(f);
        }
        note(46, (uint64_t)g_refs);
        EXPECT(g_refs == 0);
    }
    for (unsigned i = 0; i < NFILES; i++)
        g_ops->release(&g_node, &files[i]);
    note(47, g_mutexes);

    harness_klog_min = min;
    harness_klog_hook = NULL;
    printf("blkctl digest 0x%016llx\n", (unsigned long long)g_digest);
    EXPECT(g_live == 0);
    EXPECT(g_digest == BLKCTL_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "blkctl_sessions", test_sessions },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
