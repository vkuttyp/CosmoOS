/*
 * test_fsctl.c - /dev/fsctl (kernel-services/vfs/fsctl.c) on the host: roadmap 2.0 SA7. A digest recorded from the C
 * over random sessions: files opened with and without the privilege and while the allocator fails; commands of every
 * op, well formed and not -- short, long, of the wrong version, with flags an op does not take, names without their
 * NUL, key commands of the wrong length -- against a mount table of filesystems with every pass and with none,
 * mounts that cannot be acquired, devices mounted, unmounted and absent, and passes that fail now and then or report
 * more than they were given room for; each result read back into buffers of every size; and /proc/mounts rendered
 * into buffers of every size from namespaces with paths that need escaping. Every call into the kernel with its
 * arguments, every result, every byte read back and the key command's copy as it is freed (its key wiped) are in
 * the digest; every mount and device reference is given back.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mountns.h>
#include <kernel/mutex.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/fsctl.h>

#include "vfs_internal.h"

/* The C library's headers are not included: their pid_t is not the kernel's. */
void *malloc(size_t size);
void free(void *p);
int printf(const char *fmt, ...);

/* The digest of the C (fsctl.c, 2026-11-07), which the CeeSafe port gives. */
#define FSCTL_DIGEST 0x9be41a29127374e0ull
#define ROUNDS 50000u
#define SEED 0xF5C7C0DE20261107ull
#define NMNT 6u
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

static void klog_hook(int level, const char *line)
{
    note(1, (uint64_t)level);
    note_str(2, line);
}

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

static void *block(size_t size, bool zero)
{
    note(3, size);
    note(4, zero);
    if (chance(14)) {
        note(5, 0);
        return NULL;
    }
    size_t *p = malloc(sizeof(size_t) * 2 + size);
    EXPECT(p != NULL);
    p[0] = size;
    memset(p + 2, zero ? 0 : 0xA5, size);
    g_live++;
    return p + 2;
}

void *kmalloc(size_t size, unsigned flags) { return block(size, (flags & KMEM_ZERO) != 0); }
void *kzalloc(size_t size) { return block(size, true); }

void kfree(void *q)
{
    if (q == NULL)
        return;
    size_t *p = (size_t *)q - 2;
    note(6, p[0]);
    if (p[0] == sizeof(struct cosmo_fsctl_key))   /* the command's copy: its key must be wiped */
        mix(q, p[0]);
    g_live--;
    free(p);
}

/* ---- locks ---- */

static unsigned g_mutex_held;

void mutex_init(struct mutex *m, const char *name)
{
    (void)m;
    note_str(10, name);
}

void mutex_lock(struct mutex *m)
{
    (void)m;
    g_mutex_held++;
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_mutex_held > 0);
    g_mutex_held--;
}

/* ---- credentials ---- */

static struct credentials g_cred;

const struct credentials *cred_current(void) { return &g_cred; }

/* ---- devices ---- */

static struct blkdev g_dev[3];   /* vda, vdb, vdc */
static bool g_dev_mounted[3];
static int g_dev_refs;

struct blkdev *blk_find(const char *name)
{
    note_str(20, name);
    for (unsigned i = 0; i < 3; i++)
        if (strcmp(g_dev[i].name, name) == 0) {
            g_dev_refs++;
            return &g_dev[i];
        }
    return NULL;
}

void kobject_put(struct kobject *obj)   /* blkdev_put: the devices are the only kobjects put */
{
    for (unsigned i = 0; i < 3; i++)
        if (obj == &g_dev[i].obj) {
            note(21, i);
            g_dev_refs--;
            return;
        }
    EXPECT(false);
}

bool vfs_bdev_mounted(const struct blkdev *bd)
{
    bool m = g_dev_mounted[bd - g_dev];
    note(22, m);
    return m;
}

/* ---- the passes: filled at random, failing now and then ---- */

static int pass_rc(void)
{
    unsigned k = pick(10);
    return k == 0 ? -EIO : k == 1 ? -EBUSY : 0;
}

static void fill(void *p, size_t n)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)next();
}

static void name_into(char *dst, size_t n)
{
    size_t k = pick((unsigned)n);
    for (size_t i = 0; i < k; i++)
        dst[i] = (char)('a' + pick(26));
    dst[k] = '\0';
}

static unsigned mount_index(const struct mount *mnt);

static int fs_check(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags)
{
    note(30, mount_index(mnt));
    note(31, flags);
    fill(out, sizeof(*out));
    out->partial = pick(2);
    out->clean = pick(2);
    out->repair_refused = pick(2);
    out->boot_refused = pick(2);
    return pass_rc();
}

static int fs_scrub(struct mount *mnt, struct cosmofs_scrub_stats *out, unsigned flags)
{
    note(32, mount_index(mnt));
    note(33, flags);
    fill(out, sizeof(*out));
    return pass_rc();
}

static int fs_check_dev(struct blkdev *bdev, unsigned flags, struct cosmofs_check_report *out,
                        struct cosmofs_dev_report *dev)
{
    note(34, (uint64_t)(bdev - g_dev));
    note(35, flags);
    fill(out, sizeof(*out));
    out->partial = pick(2);
    out->clean = pick(2);
    out->repair_refused = pick(2);
    out->boot_refused = pick(2);
    fill(dev, sizeof(*dev));
    name_into(dev->rollback_target, sizeof(dev->rollback_target));
    return pass_rc();
}

static int fs_snapctl(struct mount *mnt, struct cosmofs_snap_req *req)
{
    note(36, mount_index(mnt));
    note(37, req->op);
    note_str(38, req->name);
    note_str(39, req->name2);
    note(40, req->max);
    note(41, req->no_sizes);
    note(42, req->after);
    int rc = pass_rc();
    if (rc)
        return rc;
    req->count = req->max == 1 ? pick(2) : pick(5) == 0 ? req->max : pick(6);
    req->total = req->count + pick(3);
    req->format = (unsigned)next();
    req->pool_flags = pick(4);
    req->free_bytes = next();
    req->total_bytes = next();
    req->rollback_error = -(int)pick(30);
    for (unsigned i = 0; i < req->count; i++) {
        struct cosmofs_snap_info *s = &req->out[i];
        fill(s, sizeof(*s));
        s->flags = pick(64);
        name_into(s->name, sizeof(s->name));
    }
    return 0;
}

static int fs_poolctl(struct mount *mnt, struct cosmofs_pool_req *req)
{
    note(43, mount_index(mnt));
    note(44, req->op);
    note_str(45, req->name);
    note_str(46, req->name2);
    note(47, req->max);
    note(48, req->out != NULL);
    int rc = pass_rc();
    if (rc)
        return rc;
    unsigned k = pick(6);
    req->count = k == 0 ? req->max + 1 + pick(3) : k == 1 ? req->max : pick(5);   /* more than room: none taken */
    if (req->max != 0 && req->count > req->max)
        req->count = k == 0 ? req->count : req->max;
    req->total = pick(2) == 0 ? req->count + pick(4) : pick(3);
    req->generation = next();
    req->members = pick(255);
    req->format = pick(20);
    req->rate = (unsigned)next();
    req->degraded = pick(2);
    req->resilvering = pick(2);
    fill(req->label, sizeof(req->label));
    for (unsigned i = 0; req->out != NULL && i < req->count && i < req->max; i++) {
        struct cosmofs_copy_info *c = &req->out[i];
        fill(c, sizeof(*c));
        c->flags = pick(4);
        name_into(c->dev, sizeof(c->dev));
    }
    return 0;
}

static int fs_keyctl(struct blkdev *bdev, struct mount *mnt, struct cosmofs_key_req *req)
{
    note(49, bdev ? (uint64_t)(bdev - g_dev) : 99);
    note(50, mnt ? mount_index(mnt) : 99);
    note(51, req->op);
    note(52, req->slot);
    mix(req->key, COSMO_KEY_SIZE);
    mix(req->rec, sizeof(*req->rec));
    note(53, req->info != NULL);
    int rc = pass_rc();
    if (rc == 0 && req->info != NULL)
        fill(req->info, sizeof(*req->info));
    return rc;
}

static struct fs_type g_cosmofs = {
    .name = "cosmofs", .check = fs_check, .scrub = fs_scrub, .check_dev = fs_check_dev, .snapctl = fs_snapctl,
    .poolctl = fs_poolctl, .keyctl = fs_keyctl,
};
static struct fs_type g_plainfs = { .name = "ramfs" };
static struct fs_type g_devonly = { .name = "devfs", .check = fs_check };   /* a check but no scrub */

struct fs_type *vfs_find_fs(const char *name)
{
    note_str(54, name);
    if (strcmp(name, "cosmofs") == 0)
        return &g_cosmofs;
    if (strcmp(name, "ramfs") == 0)
        return &g_plainfs;
    return NULL;
}

/* ---- the mount table and the caller's namespace ---- */

struct mutex g_mounts_lock;
struct list_node g_mounts;
spinlock_t g_mount_list_lock = SPINLOCK_INIT("test-mount-list");
struct mount *g_root_mount;

static struct mount g_mnt[NMNT];
static struct mount_ns_ref g_ref[NMNT];
static struct mount_ns g_ns;
static int g_mnt_refs;
static unsigned g_count_lie;

static unsigned mount_index(const struct mount *mnt) { return (unsigned)(mnt - g_mnt); }

struct mount_ns *mountns_current(void) { return &g_ns; }

unsigned vfs_mount_count(void)
{
    unsigned n = 0;
    struct mount_ns_ref *r;
    list_for_each_entry(r, &g_ns.mounts, ns_link)
        n++;
    return n > g_count_lie ? n - g_count_lie : 0;
}

int vfs_mount_acquire(uint64_t id, struct mount **out)
{
    note(60, id);
    for (unsigned i = 0; i < NMNT; i++) {
        if (g_mnt[i].id == id) {
            if (chance(10)) {
                note(61, 0);
                return -EBUSY;
            }
            g_mnt_refs++;
            *out = &g_mnt[i];
            return 0;
        }
    }
    return -ENOENT;
}

void vfs_mount_release(struct mount *mnt)
{
    note(62, mount_index(mnt));
    g_mnt_refs--;
}

static const char *const g_paths[NMNT] = {
    "/", "/tmp", "/mnt/my disk", "/mnt/tab\there", "/mnt/back\\slash\nnl", NULL,
};
static char g_long_path[1000];

static void build_mounts(void)
{
    struct fs_type *const fs[NMNT] = { &g_cosmofs, &g_plainfs, &g_cosmofs, &g_devonly, &g_cosmofs, &g_plainfs };
    for (unsigned i = 0; i < NMNT; i++) {
        struct mount *m = &g_mnt[i];
        m->id = 100 + i * 7;
        m->fs = fs[i];
        m->bdev = i == 0 ? &g_dev[0] : i == 2 ? &g_dev[1] : i == 4 ? &g_dev[2] : NULL;
        m->flags = i == 2 ? MOUNT_RDONLY : 0;
        m->state = i == 4 ? MOUNT_STATE_DEGRADED : 0;
        g_ref[i].mnt = m;
        g_ref[i].ns = &g_ns;
        strlcpy(g_ref[i].path, g_paths[i] ? g_paths[i] : g_long_path, sizeof(g_ref[i].path));
    }
}

/* A new view: the root or not, a random subset of the other mounts, the root's ref now and then. */
static void reshape(void)
{
    list_init(&g_ns.mounts);
    g_ns.id = pick(5);
    g_root_mount = pick(8) == 0 ? NULL : &g_mnt[0];
    for (unsigned i = 0; i < NMNT; i++)
        if (pick(3) != 0 && (i != 0 || pick(4) == 0))
            list_push_back(&g_ns.mounts, &g_ref[i].ns_link);
    g_count_lie = pick(4) == 0 ? 1 + pick(2) : 0;
    note(63, g_ns.id);
    note(64, g_root_mount != NULL);
}

/* ---- the device node ---- */

static const struct chrdev_ops *g_ops;
static unsigned g_mkchr_calls;

int ramfs_mkchr(const char *path, uint32_t mode, const struct chrdev_ops *ops, void *priv, struct vnode **out)
{
    note_str(70, path);
    note(71, mode);
    note(72, priv != NULL);
    if (g_mkchr_calls++ == 0)
        return -ENOMEM;
    g_ops = ops;
    *out = NULL;
    return 0;
}

void fsctl_dev_init(void);

/* ---- a session ---- */

static struct file g_file[NFILES];
static bool g_open[NFILES];

static void open_file(unsigned i)
{
    g_cred.privs = pick(5) == 0 ? 0 : PRIV_SYS_ADMIN;
    g_cred.euid = pick(8) == 0 ? 1000 : 0;
    memset(&g_file[i], 0, sizeof(g_file[i]));
    int rc = g_ops->open(NULL, &g_file[i]);
    note(80, (uint64_t)(int64_t)rc);
    g_open[i] = rc == 0;
}

static void close_file(unsigned i)
{
    g_ops->release(NULL, &g_file[i]);
    EXPECT(g_file[i].priv == NULL);
    g_open[i] = false;
}

static uint16_t some_op(void)
{
    unsigned k = pick(30);
    return k < 26 ? (uint16_t)k : k == 26 ? 0 : (uint16_t)(25 + pick(1000));
}

/* The flags an op takes, as fsctl.h defines them. */
static uint32_t takes(uint16_t op)
{
    return op == COSMO_FSCTL_CHECK || op == COSMO_FSCTL_SCRUB_EX ? COSMO_FSCTL_F_REPAIR
         : op == COSMO_FSCTL_CHECK_DEV ? COSMO_FSCTL_F_REPAIR | COSMO_FSCTL_F_BOOT
         : op == COSMO_FSCTL_SNAP_ROLLBACK ? COSMO_FSCTL_F_SAFETY_NAME
         : op == COSMO_FSCTL_SNAP_LIST ? COSMO_FSCTL_F_NO_SIZES : 0u;
}

static uint32_t some_flags(uint16_t op)
{
    unsigned k = pick(10);
    if (k < 7)
        return takes(op) & (uint32_t)next();
    return k == 7 ? COSMO_FSCTL_F_BOOT : k == 8 ? 1u << pick(4) : 1u << pick(32);
}

static void some_name(char *dst, size_t size, bool device)
{
    static const char *const devs[] = { "vda", "vdb", "vdc", "vdz", "" };
    static const char *const names[] = { "", "snap1", "cosmofs", "ramfs", "nofs", "a-b" };
    unsigned k = pick(24);
    memset(dst, 0, size);
    if (k == 0)
        memset(dst, 'x', size);   /* no NUL */
    else if (k == 1)
        name_into(dst, size);
    else if (device)
        strlcpy(dst, devs[pick(5)], size);
    else
        strlcpy(dst, names[pick(6)], size);
}

static void write_cmd(unsigned i)
{
    struct cosmo_fsctl_key k;
    memset(&k, 0, sizeof(k));
    struct cosmo_fsctl *cmd = &k.cmd;
    cmd->version = pick(12) == 0 ? (uint16_t)pick(5) : COSMO_FSCTL_VERSION;
    cmd->op = some_op();
    cmd->flags = some_flags(cmd->op);
    unsigned target = pick(NMNT + 2);
    cmd->mount_id = target < NMNT ? g_mnt[target].id : target == NMNT ? 0 : 999;
    bool device = cmd->op == COSMO_FSCTL_CHECK_DEV || (cmd->op >= COSMO_FSCTL_KEY_INFO && pick(2) == 0);
    some_name(cmd->name, sizeof(cmd->name), device);
    some_name(cmd->name2, sizeof(cmd->name2), false);
    cmd->cursor = pick(4) == 0 ? next() : pick(10);
    k.slot = pick(10);
    fill(k.key, sizeof(k.key));
    fill(&k.rec, sizeof(k.rec));
    bool key_op = cmd->op >= COSMO_FSCTL_KEY_INFO && cmd->op <= COSMO_FSCTL_KEY_SLOT;
    unsigned lk = pick(24);
    size_t len = lk == 0 ? sizeof(struct cosmo_fsctl) - 1 - pick(4) : lk == 1 ? sizeof(k) + 1
               : lk == 2 ? (key_op ? sizeof(struct cosmo_fsctl) : sizeof(k)) : key_op ? sizeof(k) : sizeof(*cmd);
    if (pick(10) == 0)
        g_cred.privs = 0;   /* the fd outlived the privilege */
    else
        g_cred.privs = PRIV_SYS_ADMIN;
    g_cred.euid = 0;
    note(81, cmd->op);
    note(82, cmd->flags);
    note(83, len);
    int64_t rc = g_ops->write_file(NULL, &g_file[i], 0, &k, len);
    note(84, (uint64_t)rc);
}

static void read_result(unsigned i)
{
    static uint8_t buf[300000];
    size_t lens[] = { 0, 1, 24, 64, 1080, 4096, 70000, sizeof(buf) };
    size_t len = lens[pick(8)];
    memset(buf, 0xEE, len < 4096 ? len : 4096);
    int64_t rc = g_ops->read_file(NULL, &g_file[i], 0, buf, len);
    note(85, len);
    note(86, (uint64_t)rc);
    if (rc > 0)
        mix(buf, (size_t)rc);
}

static void render(void)
{
    static char out[8192];
    size_t sizes[] = { 0, 1, 5, 40, 90, 200, 1100, sizeof(out) };
    size_t n = sizes[pick(8)];
    memset(out, 0x7F, sizeof(out));
    int len = vfs_mounts_render(out, n);
    note(87, n);
    note(88, (uint64_t)(int64_t)len);
    mix(out, n > 0 ? (size_t)len + 1 : 0);   /* the text and its NUL */
}

static void test_fsctl_sessions(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = klog_hook;
    harness_klog_min = KLOG_ERROR + 1;
    for (unsigned i = 0; i < 3; i++) {
        strlcpy(g_dev[i].name, (const char *[]){ "vda", "vdb", "vdc" }[i], sizeof(g_dev[i].name));
        g_dev_mounted[i] = i == 0;
    }
    memset(g_long_path, 'L', sizeof(g_long_path) - 1);
    build_mounts();
    list_init(&g_ns.mounts);

    fsctl_dev_init();   /* the node cannot be made */
    EXPECT(g_ops == NULL);
    fsctl_dev_init();
    EXPECT(g_ops != NULL);
    g_faults = true;

    for (unsigned r = 0; r < ROUNDS; r++) {
        unsigned i = pick(NFILES);
        unsigned op = pick(14);
        note(90, op);
        if (op == 0)
            reshape();
        else if (op == 1)
            render();
        else if (op == 2)
            g_dev_mounted[pick(3)] = pick(2);
        else if (!g_open[i])
            open_file(i);
        else if (op == 3)
            close_file(i);
        else if (op < 9)
            write_cmd(i);
        else
            read_result(i);
        EXPECT(g_mutex_held == 0);
        EXPECT(g_mnt_refs == 0 && g_dev_refs == 0);
    }
    /* A file whose open was refused: no state, reads nothing, writes nothing. */
    struct file none = { 0 };
    note(91, (uint64_t)g_ops->read_file(NULL, &none, 0, NULL, 0));
    note(92, (uint64_t)g_ops->write_file(NULL, &none, 0, NULL, 0));
    g_ops->release(NULL, &none);
    for (unsigned i = 0; i < NFILES; i++)
        if (g_open[i])
            close_file(i);
    harness_klog_hook = NULL;
    EXPECT(g_live == 0);
    printf("  fsctl digest 0x%016llx\n", (unsigned long long)g_digest);
    EXPECT(g_digest == FSCTL_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "fsctl_sessions", test_fsctl_sessions },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
