/*
 * test_ramfs.c - ramfs (kernel-services/vfs/ramfs.c) on the host: roadmap 2.0 SA7. A digest recorded from the C over
 * random histories of mounts: files, directories, links, sockets, named pipes and character devices made, looked up,
 * linked, unlinked, renamed over one another and across directories, listed a few entries at a time, truncated and
 * read; devices made by path and by directory, with tables of every shape, opened with and without the privilege
 * and torn down; anonymous files; the boot archive copied in; and every tree released by unmount -- while the vnode
 * and heap allocators fail now and then. Every call into the kernel with its arguments, every result, name, inode
 * number and log line are in the digest; every vnode, entry and allocation is given back.
 */

#include "harness.h"

#include <kernel/bootarchive.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/fifo.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/pagecache.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

/* The C library's headers are not included: their pid_t is not the kernel's. */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void free(void *p);
int printf(const char *fmt, ...);

extern struct fs_type ramfs_fs_type;

#define ANY_TYPE ((enum vnode_type)0)   /* some(): a vnode of any type */

/* The digest of the C (ramfs.c, 2026-11-07), which the CeeSafe port gives. */
#define RAMFS_DIGEST 0x34bf9feeddcfa426ull
#define ROUNDS 24000u
#define EPOCH 3000u
#define SEED 0x4A3F5C0DE2026110ull
#define NHELD 48u

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

static void note_str(uint32_t which, const char *s, size_t n)
{
    note(which, n);
    mix(s, n);
}

static bool g_faults = true;

static bool chance(unsigned n) { return g_faults && pick(n) == 0; }

static void klog_hook(int level, const char *line)
{
    note(1, (uint64_t)level);
    note_str(2, line, strlen(line));
}

/* ---- the heap ---- */

static long g_live;

static void *block(size_t size, bool zero)
{
    note(3, size);
    if (chance(30)) {
        note(4, 0);
        return NULL;
    }
    void *p = zero ? calloc(1, size) : malloc(size);
    EXPECT(p != NULL);
    g_live++;
    return p;
}

void *kmalloc(size_t size, unsigned flags) { return block(size, (flags & KMEM_ZERO) != 0); }
void *kzalloc(size_t size) { return block(size, true); }

void kfree(void *p)
{
    if (p == NULL)
        return;
    g_live--;
    free(p);
}

/* ---- the vnodes: allocated, referenced, evicted at the last put ---- */

static void forget_mutex(const struct mutex *m);

static long g_vnodes;
static uint64_t g_now = 1000;

uint64_t vfs_now_ns(void) { return g_now += 7; }

static bool g_fail_vnode;   /* the next vnode_alloc fails */

struct vnode *vnode_alloc(struct mount *mnt, uint64_t ino)
{
    note(10, ino);
    if (g_fail_vnode || chance(25)) {
        g_fail_vnode = false;
        note(11, 0);
        return NULL;
    }
    struct vnode *vn = calloc(1, sizeof(*vn));
    EXPECT(vn != NULL);
    vn->mnt = mnt;
    vn->ino = ino;
    vn->obj.refcount = 1;
    g_vnodes++;
    return vn;
}

void vnode_hash_insert(struct vnode *vn)
{
    note(12, vn->ino);
    note(13, vn->type);
    note(14, vn->mode);
    note(15, vn->nlink);
    note(16, vn->flags);
}

void kobject_get(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    obj->refcount++;
}

void vnode_put(struct vnode *vn)
{
    EXPECT(vn->obj.refcount > 0);
    if (--vn->obj.refcount != 0)
        return;
    note(17, vn->ino);
    EXPECT(!(vn->flags & VNODE_PINNED));
    if (vn->ops && vn->ops->evict)
        vn->ops->evict(vn);
    EXPECT(vn->fs_priv == NULL);
    forget_mutex(&vn->lock);
    free(vn);
    g_vnodes--;
}

void pagecache_truncate(struct vnode *vn, uint64_t size)
{
    note(18, vn->ino);
    note(19, size);
}

/* ---- locks ---- */

#define NMUTEX 512u
static const struct mutex *g_mutex[NMUTEX];
static unsigned g_nmutex;
static bool g_held[NMUTEX];

static unsigned mutex_slot(const struct mutex *m)
{
    for (unsigned i = 0; i < g_nmutex; i++)
        if (g_mutex[i] == m)
            return i;
    EXPECT(g_nmutex < NMUTEX);
    g_mutex[g_nmutex] = m;
    g_held[g_nmutex] = false;
    return g_nmutex++;
}

static void forget_mutex(const struct mutex *m)
{
    for (unsigned i = 0; i < g_nmutex; i++)
        if (g_mutex[i] == m) {
            EXPECT(!g_held[i]);
            g_mutex[i] = g_mutex[g_nmutex - 1];
            g_held[i] = g_held[g_nmutex - 1];
            g_nmutex--;
            return;
        }
}

void mutex_lock(struct mutex *m)
{
    unsigned i = mutex_slot(m);
    EXPECT(!g_held[i]);
    g_held[i] = true;
}

void mutex_lock_nested(struct mutex *m, unsigned subclass)
{
    note(20, subclass);
    mutex_lock(m);
}

void mutex_unlock(struct mutex *m)
{
    unsigned i = mutex_slot(m);
    EXPECT(g_held[i]);
    g_held[i] = false;
}

/* ---- credentials ---- */

static struct credentials g_cred;

const struct credentials *cred_current(void) { return &g_cred; }

/* ---- named pipes ---- */

struct fifo {
    unsigned id;
};
static unsigned g_fifos, g_fifo_next;

struct fifo *fifo_alloc(void)
{
    if (chance(12)) {
        note(30, 0);
        return NULL;
    }
    struct fifo *f = malloc(sizeof(*f));
    EXPECT(f != NULL);
    f->id = ++g_fifo_next;
    note(31, f->id);
    g_fifos++;
    return f;
}

void fifo_free(struct fifo *fifo)
{
    note(32, fifo->id);
    g_fifos--;
    free(fifo);
}

static struct waitqueue *const g_wq = (struct waitqueue *)(uintptr_t)0x1000;

int fifo_open(struct fifo *fifo, struct file *f) { (void)f; note(33, fifo->id); return -(int)pick(3); }
void fifo_release(struct fifo *fifo, struct file *f) { (void)f; note(34, fifo->id); }
int64_t fifo_read(struct fifo *fifo, struct file *f, void *buf, size_t len)
{
    (void)f;
    (void)buf;
    note(35, fifo->id);
    note(36, len);
    return (int64_t)(len / 2);
}
int64_t fifo_write(struct fifo *fifo, struct file *f, const void *buf, size_t len)
{
    (void)f;
    note(37, fifo->id);
    mix(buf, len);
    return (int64_t)len;
}
unsigned fifo_ready(struct fifo *fifo, struct file *f) { (void)f; note(38, fifo->id); return fifo->id & 3; }
struct waitqueue *fifo_poll_wq(struct fifo *fifo, struct file *f, unsigned events)
{
    (void)f;
    note(39, fifo->id);
    note(40, events);
    return g_wq;
}
int fifo_set_nonblock(struct fifo *fifo, struct file *f, int on)
{
    (void)f;
    note(41, fifo->id);
    note(42, (uint64_t)on);
    return 0;
}

/* ---- character devices: tables of every shape ---- */

static int64_t dev_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    (void)buf;
    note(50, vn->ino);
    note(51, off);
    return (int64_t)len;
}
static int64_t dev_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    note(52, vn->ino);
    note(53, off);
    mix(buf, len);
    return (int64_t)len - 1;
}
static int dev_open(struct vnode *vn, struct file *f)
{
    (void)f;
    note(54, vn->ino);
    return vn->ino % 3 == 0 ? -EBUSY : 0;
}
static void dev_release(struct vnode *vn, struct file *f) { (void)f; note(55, vn->ino); }
static int64_t dev_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)f;
    (void)buf;
    note(56, vn->ino);
    note(57, off);
    return (int64_t)(len / 3);
}
static int64_t dev_write_file(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    (void)f;
    note(58, vn->ino);
    note(59, off);
    mix(buf, len);
    return (int64_t)len;
}
static unsigned dev_ready(struct vnode *vn, struct file *f) { (void)f; note(60, vn->ino); return 2; }
static struct waitqueue *dev_poll_wq(struct vnode *vn, struct file *f, unsigned events)
{
    (void)f;
    note(61, vn->ino);
    note(62, events);
    return g_wq;
}
static int dev_set_nonblock(struct vnode *vn, struct file *f, int on)
{
    (void)f;
    note(63, vn->ino);
    return on ? -EAGAIN : 0;
}

static const struct chrdev_ops g_dev_full = {
    .read = dev_read, .write = dev_write, .open = dev_open, .release = dev_release, .read_file = dev_read_file,
    .write_file = dev_write_file, .ready = dev_ready, .poll_wq = dev_poll_wq, .set_nonblock = dev_set_nonblock,
};
static const struct chrdev_ops g_dev_plain = { .read = dev_read, .write = dev_write };
static const struct chrdev_ops g_dev_priv = {
    .read_file = dev_read_file, .open = dev_open, .open_priv = PRIV_SYS_ADMIN,
};
static const struct chrdev_ops g_dev_none = { 0 };
static const struct chrdev_ops *const g_devs[] = { &g_dev_full, &g_dev_plain, &g_dev_priv, &g_dev_none };

/* ---- the VFS's half of a path: walked through the mount's own lookups ---- */

static struct mount *g_root_mnt;   /* what vfs_lookup, vfs_mkdir, vfs_symlink and vfs_open see as / */

/* The directory holding `path`'s last component, referenced, and that component. */
static int walk_parent(const char *path, struct vnode **dir, const char **last, size_t *lastlen)
{
    if (g_root_mnt == NULL || path[0] != '/')
        return -ENOENT;
    struct vnode *cur = g_root_mnt->root;
    kobject_get(&cur->obj);
    const char *p = path + 1;
    for (;;) {
        const char *slash = strchr(p, '/');
        if (slash == NULL || slash[1] == '\0') {
            *dir = cur;
            *last = p;
            *lastlen = slash ? (size_t)(slash - p) : strlen(p);
            return 0;
        }
        struct vnode *nx = NULL;
        int rc = cur->type == VNODE_DIR ? cur->ops->lookup(cur, p, (size_t)(slash - p), &nx) : -ENOTDIR;
        vnode_put(cur);
        if (rc)
            return rc;
        cur = nx;
        p = slash + 1;
    }
}

int vfs_lookup(struct vnode *start, const char *path, struct vnode **out)
{
    EXPECT(start == NULL);
    note_str(70, path, strlen(path));
    if (chance(20)) {
        note(71, 0);
        return -EIO;
    }
    struct vnode *dir;
    const char *last;
    size_t n;
    int rc = walk_parent(path, &dir, &last, &n);
    if (rc)
        return rc;
    if (n == 0) {
        *out = dir;
        return 0;
    }
    rc = dir->type == VNODE_DIR ? dir->ops->lookup(dir, last, n, out) : -ENOTDIR;
    vnode_put(dir);
    return rc;
}

int vfs_mkdir(struct vnode *start, const char *path, uint32_t mode)
{
    EXPECT(start == NULL);
    note_str(72, path, strlen(path));
    note(73, mode);
    if (chance(8)) {
        note(69, 0);
        return -EIO;
    }
    struct vnode *dir;
    const char *last;
    size_t n;
    int rc = walk_parent(path, &dir, &last, &n);
    if (rc)
        return rc;
    struct vnode *vn = NULL;
    rc = dir->type != VNODE_DIR ? -ENOTDIR : dir->ops->lookup(dir, last, n, &vn) == 0 ? -EEXIST
         : dir->ops->mkdir(dir, last, n, mode, &vn);
    if (vn)
        vnode_put(vn);
    vnode_put(dir);
    return rc;
}

int vfs_symlink(struct vnode *start, const char *path, const char *target)
{
    EXPECT(start == NULL);
    note_str(74, path, strlen(path));
    note_str(75, target, strlen(target));
    struct vnode *dir;
    const char *last;
    size_t n;
    int rc = walk_parent(path, &dir, &last, &n);
    if (rc)
        return rc;
    struct vnode *vn = NULL;
    rc = dir->type == VNODE_DIR ? dir->ops->symlink(dir, last, n, target, &vn) : -ENOTDIR;
    if (rc == 0)
        vnode_put(vn);
    vnode_put(dir);
    return rc;
}

static long g_files;

int vfs_open(struct vnode *start, const char *path, unsigned flags, uint32_t mode, struct file **out)
{
    EXPECT(start == NULL);
    note_str(76, path, strlen(path));
    note(77, flags);
    note(78, mode);
    struct vnode *dir;
    const char *last;
    size_t n;
    int rc = walk_parent(path, &dir, &last, &n);
    if (rc)
        return rc;
    struct vnode *vn = NULL;
    rc = dir->type != VNODE_DIR ? -ENOTDIR : dir->ops->lookup(dir, last, n, &vn);
    if (rc == -ENOENT)
        rc = dir->ops->create(dir, last, n, mode, &vn);
    else if (rc == 0 && vn->ops->truncate)
        rc = vn->ops->truncate(vn, 0);
    vnode_put(dir);
    if (rc) {
        if (vn)
            vnode_put(vn);
        return rc;
    }
    struct file *f = calloc(1, sizeof(*f));
    EXPECT(f != NULL);
    f->obj.refcount = 1;
    f->vn = vn;
    g_files++;
    *out = f;
    return 0;
}

int64_t file_write(struct file *f, const void *buf, size_t len)
{
    note(79, f->vn->ino);
    mix(buf, len);
    unsigned k = pick(8);
    return k == 0 ? -ENOSPC : k == 1 ? (int64_t)(len / 2) : (int64_t)len;
}

void kobject_put(struct kobject *obj)   /* file_put: the test's files are the only kobjects put */
{
    struct file *f = (struct file *)obj;
    EXPECT(obj->refcount == 1);
    vnode_put(f->vn);
    free(f);
    g_files--;
}

/* ---- the boot archive ---- */

static const char g_data[] = "#!/bin/sh\necho archive\n";
static struct bootarchive_entry g_arch[] = {
    { .name = "init" }, { .name = "bin/sh" }, { .name = "sbin/fsck" }, { .name = "usr/bin/env" },
    { .name = "usr/sbin/sshd" }, { .name = "usr/libexec/sftp-server" }, { .name = "lib/libc.so" },
    { .name = "lib/ld-musl.so.1", .link = "libc.so" }, { .name = "usr/lib/libz.so" }, { .name = "etc/passwd" },
    { .name = "usr/share/doc/README" }, { .name = "tests/t1" }, { .name = "modules/m.ko" },
    { .name = "deep/a/b/c/file" }, { .name = "deep/a/link", .link = "/etc/passwd" }, { .name = "bin" },
};

unsigned bootarchive_count(void) { return (unsigned)(sizeof(g_arch) / sizeof(g_arch[0])); }

const struct bootarchive_entry *bootarchive_entry(unsigned index)
{
    note(80, index);
    return &g_arch[index];
}

/* ---- what the test holds ---- */

struct held {
    struct vnode *vn;
    bool dead;   /* a directory that was removed: nothing is made in it, as the VFS refuses */
};

static struct held g_held_vn[NHELD];
static unsigned g_nheld;
static struct mount g_mnt;
static bool g_mounted;

static void hold(struct vnode *vn)
{
    if (g_nheld < NHELD) {
        g_held_vn[g_nheld].vn = vn;
        g_held_vn[g_nheld].dead = false;
        g_nheld++;
    } else {
        vnode_put(vn);
    }
}

static void unhold(unsigned i)
{
    vnode_put(g_held_vn[i].vn);
    g_held_vn[i] = g_held_vn[--g_nheld];
}

static void mark_dead(struct vnode *vn)
{
    for (unsigned i = 0; i < g_nheld; i++)
        if (g_held_vn[i].vn == vn)
            g_held_vn[i].dead = true;
}

/* A held vnode of `type` (any type: ANY_TYPE), or the root for a directory now and then. */
static struct vnode *some(enum vnode_type type, bool live)
{
    unsigned start = pick(g_nheld + 1);
    if (type == VNODE_DIR && (start == g_nheld || g_nheld == 0))
        return g_mnt.root;
    for (unsigned k = 0; k < g_nheld; k++) {
        struct held *h = &g_held_vn[(start + k) % g_nheld];
        if ((type == ANY_TYPE || h->vn->type == type) && !(live && h->dead))
            return h->vn;
    }
    return type == VNODE_DIR ? g_mnt.root : NULL;
}

static char g_name[VFS_NAME_MAX + 2];

static size_t some_name(void)
{
    static const char *const names[] = { "a", "b", "c", "dd", "e.x", "..", ".", ".y" };
    unsigned k = pick(11);
    if (k < 8) {
        size_t n = strlen(names[k]);
        memcpy(g_name, names[k], n + 1);
        return n;
    }
    size_t n = k == 8 ? VFS_NAME_MAX : 1 + pick(40);
    for (size_t i = 0; i < n; i++)
        g_name[i] = (char)('f' + (i % 3));
    g_name[n] = '\0';
    return n;
}

static void note_rc(uint32_t which, int rc) { note(which, (uint64_t)(int64_t)rc); }

/* Whether `dir` is `vn` or below it: a rename may not put a directory inside itself (the VFS refuses). */
static bool below(struct vnode *dir, struct vnode *vn)
{
    struct vnode *cur = dir;
    kobject_get(&cur->obj);
    for (unsigned depth = 0; depth < 64; depth++) {
        if (cur == vn) {
            vnode_put(cur);
            return true;
        }
        struct vnode *up = NULL;
        EXPECT(cur->ops->lookup(cur, "..", 2, &up) == 0);
        bool top = up == cur;
        vnode_put(cur);
        cur = up;
        if (top)
            break;
    }
    vnode_put(cur);
    return false;
}

static void op_make(void)
{
    struct vnode *dir = some(VNODE_DIR, true);
    size_t n = some_name();
    if (n <= 2 && g_name[0] == '.')
        return;   /* the VFS answers . and .. itself */
    unsigned kind = pick(7);
    uint32_t mode = 0600 + pick(0200);
    struct vnode *vn = NULL;
    int rc;
    note(100, kind);
    note_str(101, g_name, n);
    if (kind == 0 || kind == 1)
        rc = dir->ops->create(dir, g_name, n, mode, &vn);
    else if (kind == 2)
        rc = dir->ops->mkdir(dir, g_name, n, mode, &vn);
    else if (kind == 3) {
        unsigned t = pick(4);
        enum vnode_type type = t == 0 ? VNODE_SOCK : t == 1 ? VNODE_FIFO : t == 2 ? VNODE_FIFO : VNODE_REG;
        rc = dir->ops->mknod(dir, g_name, n, mode, type, &vn);
    } else if (kind == 4) {
        char target[VFS_PATH_MAX + 8];
        unsigned t = pick(6);
        size_t tl = t == 0 ? 0 : t == 1 ? VFS_PATH_MAX : t == 2 ? VFS_PATH_MAX - 1 : 1 + pick(30);
        for (size_t i = 0; i < tl; i++)
            target[i] = (char)('A' + i % 26);
        target[tl] = '\0';
        note(102, tl);
        rc = dir->ops->symlink(dir, g_name, n, target, &vn);
    } else if (kind == 5) {
        unsigned d = pick(4);
        const struct chrdev_ops *ops = g_devs[d];
        note(103, d);
        unsigned bad = pick(10);
        const char *name = bad == 0 ? "a/b" : bad == 1 ? "" : g_name;
        if (bad == 2) {
            memset(g_name, 'l', VFS_NAME_MAX + 1);
            g_name[VFS_NAME_MAX + 1] = '\0';
        }
        if (bad == 3 && some(ANY_TYPE, true) != NULL)
            dir = some(ANY_TYPE, true);   /* not a directory, perhaps */
        uintptr_t priv = 8 * (1 + pick(4));
        bool keep = pick(3) != 0;
        rc = ramfs_mkchr_at(dir, name, 0600, ops, (void *)priv, keep ? &vn : NULL);
    } else {
        struct vnode *target = some(ANY_TYPE, false);
        if (target == NULL || target->type == VNODE_DIR || target->nlink == 0)
            return;   /* the VFS links no directory, and no file without a name (-ENOENT) */
        rc = dir->ops->link(dir, g_name, n, target);
        note_rc(104, rc);
        return;
    }
    note_rc(105, rc);
    if (rc == 0 && vn != NULL) {
        note(106, vn->ino);
        note(107, vn->size);
        note(108, vn->nlink);
        hold(vn);
    }
}

static void op_lookup(void)
{
    struct vnode *dir = some(VNODE_DIR, true);
    size_t n = some_name();
    struct vnode *vn = NULL;
    int rc = dir->ops->lookup(dir, g_name, n, &vn);
    note_rc(110, rc);
    if (rc == 0) {
        note(111, vn->ino);
        note(112, vn->type);
        hold(vn);
    }
}

static void op_remove(void)
{
    struct vnode *dir = some(VNODE_DIR, true);
    size_t n = some_name();
    if (n <= 2 && g_name[0] == '.')
        return;
    struct vnode *victim = NULL;
    if (dir->ops->lookup(dir, g_name, n, &victim) != 0)
        return;
    struct vnode *named = victim;
    if (pick(10) == 0)
        named = some(ANY_TYPE, false);   /* not the file of that name */
    if (named == NULL)
        named = victim;
    if (pick(12) == 0) {   /* a name that is not there */
        memcpy(g_name, "zz-none", 8);
        n = 7;
    }
    int rc;
    if (named->type == VNODE_DIR) {
        rc = dir->ops->rmdir(dir, g_name, n, named);
        if (rc == 0)
            mark_dead(named);
    } else {
        rc = dir->ops->unlink(dir, g_name, n, named);
    }
    note(113, named->type);
    note_rc(114, rc);
    vnode_put(victim);
}

static void op_rename(void)
{
    struct vnode *odir = some(VNODE_DIR, true);
    struct vnode *ndir = some(VNODE_DIR, true);
    size_t on = some_name();
    char oname[VFS_NAME_MAX + 2];
    memcpy(oname, g_name, on + 1);
    size_t nn = some_name();
    if ((on <= 2 && oname[0] == '.') || (nn <= 2 && g_name[0] == '.'))
        return;
    struct vnode *victim = NULL, *replaced = NULL;
    if (odir->ops->lookup(odir, oname, on, &victim) != 0)
        return;
    int rc = ndir->ops->lookup(ndir, g_name, nn, &replaced);
    bool ok = !(odir == ndir && on == nn && memcmp(oname, g_name, on) == 0) && victim != replaced &&
              !(victim->type == VNODE_DIR && below(ndir, victim)) &&
              (replaced == NULL || (replaced->type == VNODE_DIR) == (victim->type == VNODE_DIR));
    if (ok) {
        struct vnode *pass_victim = pick(12) == 0 ? ndir : victim;
        struct vnode *pass_replaced = replaced != NULL && pick(12) == 0 ? victim : replaced;
        if (replaced == NULL && pick(10) == 0)
            pass_replaced = victim;   /* a name to replace that is not there */
        if (pick(14) == 0) {
            memcpy(oname, "zz-none", 8);
            on = 7;
        }
        note(115, rc == 0);
        rc = odir->ops->rename(odir, oname, on, pass_victim, ndir, g_name, nn, pass_replaced);
        note_rc(116, rc);
        if (rc == 0 && replaced != NULL && replaced->type == VNODE_DIR)
            mark_dead(replaced);
    }
    if (replaced)
        vnode_put(replaced);
    vnode_put(victim);
}

struct listing {
    unsigned left;
};

static int list_cb(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type)
{
    struct listing *l = arg;
    note_str(120, name, len);
    note(121, ino);
    note(122, type);
    return l->left-- == 0;
}

static void op_readdir(void)
{
    struct vnode *dir = some(VNODE_DIR, true);
    uint64_t pos = pick(4) == 0 ? pick(6) : 0;
    for (unsigned k = 0; k < 4; k++) {
        struct listing l = { .left = pick(4) };
        note(123, pos);
        int rc = dir->ops->readdir(dir, &pos, list_cb, &l);
        note_rc(124, rc);
    }
    note(125, pos);
}

static void op_file(void)
{
    struct vnode *vn = some(ANY_TYPE, false);
    if (vn == NULL)
        return;
    const struct vnode_ops *ops = vn->ops;
    struct file f = { .vn = vn };
    char buf[64];
    note(130, vn->type);
    switch (vn->type) {
    case VNODE_REG: {
        unsigned k = pick(3);
        if (k == 0) {
            uint64_t size = pick(4) == 0 ? (64ull << 20) + pick(2) : pick(1u << 20);
            note_rc(131, ops->truncate(vn, size));
            note(132, vn->size);
        } else {
            static uint8_t page[PAGE_SIZE];
            memset(page, 0x77, sizeof(page));
            int rc = k == 1 ? ops->readpage(vn, pick(9), page) : ops->writepage(vn, pick(9), page);
            note_rc(133, rc);
            mix(page, sizeof(page));
        }
        break;
    }
    case VNODE_LNK: {
        size_t len = pick(40);
        memset(buf, '.', sizeof(buf));
        int rc = ops->readlink(vn, buf, len);
        note_rc(134, rc);
        mix(buf, sizeof(buf));
        break;
    }
    case VNODE_FIFO:
        note_rc(135, ops->open(vn, &f));
        note(136, (uint64_t)ops->read_file(vn, &f, 5, buf, 10));
        note(137, (uint64_t)ops->write_file(vn, &f, 5, "pipe", 4));
        note(138, ops->ready(vn, &f));
        note(139, ops->poll_wq(vn, &f, 3) == g_wq);
        note_rc(140, ops->set_nonblock(vn, &f, 1));
        ops->release(vn, &f);
        break;
    case VNODE_CHR: {
        const struct chrdev_ops *dev = ramfs_chr_ops_of(vn);
        note(141, dev == &g_dev_full ? 1 : dev == &g_dev_plain ? 2 : dev == &g_dev_priv ? 3 : dev == &g_dev_none);
        note(142, (uintptr_t)ramfs_chr_priv(vn));
        g_cred.privs = pick(2) == 0 ? PRIV_SYS_ADMIN : 0;
        note_rc(143, ops->open(vn, &f));
        note(144, (uint64_t)ops->read(vn, 3, buf, 9));
        note(145, (uint64_t)ops->write(vn, 3, "dev", 3));
        note(146, (uint64_t)ops->read_file(vn, &f, 4, buf, 9));
        note(147, (uint64_t)ops->write_file(vn, &f, 4, "file", 4));
        note(148, ops->ready(vn, &f));
        note(149, ops->poll_wq(vn, &f, 1) == g_wq);
        note_rc(150, ops->set_nonblock(vn, &f, (int)pick(2)));
        ops->release(vn, &f);
        if (pick(4) == 0)
            ramfs_chr_set_priv(vn, (void *)(uintptr_t)(64 + 8 * pick(4)));
        break;
    }
    default:
        note(151, ramfs_chr_ops_of(vn) == NULL);
        break;
    }
}

static void op_chr(void)
{
    static const char *const paths[] = {
        "/dev/null0", "/dev/zz", "noslash", "/", "/dev/", "/nodir/x", "/a/b", "/x",
    };
    unsigned k = pick(11);
    struct vnode *vn = NULL;
    if (k < 8) {
        const struct chrdev_ops *ops = g_devs[pick(4)];
        bool keep = pick(2) != 0;
        int rc = ramfs_mkchr(paths[k], 0666, ops, (void *)(uintptr_t)16, keep ? &vn : NULL);
        note_rc(160, rc);
    } else if (k == 8) {
        static char path[VFS_PATH_MAX + 64];
        size_t n = pick(2) == 0 ? VFS_PATH_MAX + 2 : VFS_PATH_MAX - 4;
        for (size_t i = 0; i < n; i++)
            path[i] = i % 8 == 0 ? '/' : 'q';
        memcpy(path + n, "/dev", 5);
        int rc = ramfs_mkchr(path, 0666, &g_dev_plain, NULL, NULL);
        note_rc(161, rc);
    } else if (k == 9) {
        static char path[VFS_NAME_MAX + 8];
        path[0] = '/';
        memset(path + 1, 'n', VFS_NAME_MAX + 1);
        path[VFS_NAME_MAX + 2] = '\0';
        note_rc(162, ramfs_mkchr(path, 0666, &g_dev_plain, NULL, NULL));
    } else {
        struct vnode *dir = some(VNODE_DIR, true);
        size_t n = some_name();
        if (pick(2) == 0) {   /* the devices made by path, in /dev */
            struct vnode *dev = NULL;
            if (g_mnt.root->ops->lookup(g_mnt.root, "dev", 3, &dev) != 0)
                return;
            hold(dev);
            dir = dev;
            n = pick(2) == 0 ? 2 : 5;
            memcpy(g_name, n == 2 ? "zz" : "null0", n + 1);
        }
        struct vnode *victim = NULL;
        if (dir->ops->lookup(dir, g_name, n, &victim) != 0)
            return;
        struct vnode *named = pick(6) == 0 ? dir : victim;
        struct vnode *at = pick(8) == 0 ? victim : dir;
        const char *name = pick(8) == 0 ? "zz" : g_name;
        int rc = ramfs_unlink_chr_at(at, name, named);
        note_rc(163, rc);
        note(164, victim->flags & VNODE_DEAD);
        vnode_put(victim);
        return;
    }
    if (vn) {
        note(165, vn->ino);
        hold(vn);
    }
}

static void op_anon(void)
{
    struct vnode *vn = NULL;
    int rc = ramfs_anon_reg(0100644 | pick(0777), &vn);
    note_rc(170, rc);
    if (rc == 0) {
        note(171, vn->mode);
        note(172, vn->nlink);
        note(173, vn->flags);
        hold(vn);
    }
}

/* ---- mounts ---- */

static void do_mount(void)
{
    memset(&g_mnt, 0, sizeof(g_mnt));
    g_mnt.next_ino = 1 + pick(5);
    unsigned flags = pick(2) ? MOUNT_SCRATCH : 0;
    if (pick(6) == 0)
        note_rc(180, ramfs_fs_type.mount(&ramfs_fs_type, (struct blkdev *)&g_mnt, flags, &g_mnt));
    int rc;
    g_fail_vnode = pick(3) == 0;
    do {
        rc = ramfs_fs_type.mount(&ramfs_fs_type, NULL, flags, &g_mnt);
        note_rc(181, rc);
    } while (rc != 0);
    note(182, g_mnt.flags);
    note(183, g_mnt.cache_limit_pages);
    note(184, g_mnt.root->mode);
    note_str(185, ramfs_fs_type.name, strlen(ramfs_fs_type.name));
    note(186, ramfs_fs_type.magic);
    note(187, ramfs_fs_type.sync == NULL);
    g_mounted = true;
    g_root_mnt = &g_mnt;
    if (pick(2) == 0)
        ramfs_set_anon_mount(&g_mnt);
}

static void do_unmount(void)
{
    while (g_nheld > 0)
        unhold(g_nheld - 1);
    note_rc(190, ramfs_fs_type.unmount(&g_mnt));
    struct vnode *root = g_mnt.root;
    root->flags &= ~VNODE_PINNED;
    vnode_put(root);
    ramfs_set_anon_mount(NULL);
    g_root_mnt = NULL;
    g_mounted = false;
}

static void session(void)
{
    for (unsigned r = 0; r < ROUNDS; r++) {
        if (r % EPOCH == 0) {
            if (g_mounted)
                do_unmount();
            do_mount();
            if (pick(2) == 0) {
                note(195, r);
                ramfs_populate_boot();
            }
        }
        unsigned op = pick(22);
        note(200, op);
        g_cred.euid = pick(3);
        g_cred.egid = pick(3) + 100;
        if (op < 5)
            op_make();
        else if (op < 8)
            op_lookup();
        else if (op < 10)
            op_remove();
        else if (op < 12)
            op_rename();
        else if (op < 13)
            op_readdir();
        else if (op < 16)
            op_file();
        else if (op < 19)
            op_chr();
        else if (op < 20)
            op_anon();
        else if (g_nheld > 0)
            unhold(pick(g_nheld));
        for (unsigned i = 0; i < g_nmutex; i++)
            EXPECT(!g_held[i]);
    }
    do_unmount();
    /* An anonymous file before any anonymous mount: refused. */
    struct vnode *vn = NULL;
    note_rc(196, ramfs_anon_reg(0644, &vn));
}

static void test_ramfs_histories(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < bootarchive_count(); i++) {
        if (g_arch[i].link[0] == '\0') {
            g_arch[i].data = g_data;
            g_arch[i].size = sizeof(g_data) - 1 - i % 5;
        }
    }
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = klog_hook;
    harness_klog_min = KLOG_ERROR + 1;   /* in the digest, not on the terminal */
    session();
    harness_klog_hook = NULL;
    EXPECT(g_vnodes == 0);
    EXPECT(g_live == 0);
    EXPECT(g_fifos == 0);
    EXPECT(g_files == 0);
    printf("  ramfs digest 0x%016llx\n", (unsigned long long)g_digest);
    EXPECT(g_digest == RAMFS_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "ramfs_histories", test_ramfs_histories },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
