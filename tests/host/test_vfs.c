/*
 * test_vfs.c - The VFS core (kernel-services/vfs/vfs.c) on the host: roadmap 2.0 SA7. A digest recorded from the C
 * over random histories on a filesystem of the test's own, whose every operation fails now and then: the root
 * mounted, filesystems registered, mounted and unmounted from devices whose partitions overlap, in two mount
 * namespaces, with maintenance passes held across an unmount's drain; paths of every shape walked from the root,
 * from held directories and from a confined root, through symbolic links that loop, dangle, run long and cannot be
 * read; files opened with every flag, read, written, sought, synced, flushed, listed and filled; entries made,
 * linked, removed and renamed over one another, in sticky directories and as users with and without the privilege;
 * attributes set and owners changed; the root switched and the old one released; devices formatted; and the
 * held-walk seam's every interleaving of walk and swapper. Every call into the kernel with its arguments, every
 * result, name, inode number and log line are in the digest; every vnode, mount, file and allocation is given back.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/mountns.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/pagecache.h>
#include <kernel/part.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>

#include "../../kernel-services/vfs/vfs_internal.h"

/* The C library's headers are not included: their pid_t is not the kernel's. */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void free(void *p);
int printf(const char *fmt, ...);

/* The digest of the C (vfs.c, 2026-11-08) was 0x4170262f9ea8d3cf, which the CeeSafe port gave. This one is the
 * port's with the two bugs this test found fixed: an old root's unmount, and orphans that wait for their users. */
#define VFS_DIGEST 0x45371c2423e2e476ull
#define EPOCHS 12u
#define ROUNDS 2500u   /* an epoch's */
#define SEED 0x5F3A9C0DE2026118ull

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
#ifdef VFS_TRACE   /* every note, one per line: two runs' first difference is where they part */
    printf("N %u %llu\n", which, (unsigned long long)value);
#endif
    mix(&which, sizeof(which));
    mix(&value, sizeof(value));
}

static void note_rc(uint32_t which, int64_t rc) { note(which, (uint64_t)rc); }


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

/* ---- the allocator ---- */

static long g_live;

static void *alloc(size_t size, bool zero)
{
    note(3, size);
    if (chance(40)) {
        note(4, 0);
        return NULL;
    }
    void *p = zero ? calloc(1, size) : malloc(size);
    EXPECT(p != NULL);
    if (!zero)
        memset(p, 0xa5, size);
    g_live++;
    return p;
}

void *kmalloc(size_t size, unsigned flags)
{
    note(5, flags);
    return alloc(size, (flags & KMEM_ZERO) != 0);
}

void *kzalloc(size_t size) { return alloc(size, true); }

void kfree(void *p)
{
    if (p == NULL)
        return;
    g_live--;
    free(p);
}

/* ---- objects: counted, released on the last put ---- */

static long g_objects;

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    obj->type = type;
    obj->refcount = 1;
    g_objects++;
    note_str(6, type->name, strlen(type->name));
}

void kobject_get(struct kobject *obj)
{
    EXPECT(obj->refcount > 0 && obj->refcount < 100000);
    obj->refcount++;
}

static void release(struct kobject *obj)
{
    g_objects--;
    note_str(7, obj->type->name, strlen(obj->type->name));
    if (obj->type->release)
        obj->type->release(obj);
}

void kobject_put(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    if (--obj->refcount == 0)
        release(obj);
}

bool kobject_put_and_lock(struct kobject *obj, spinlock_t *lock, arch_irq_state_t *state)
{
    EXPECT(obj->refcount > 0);
    if (obj->refcount == 1) {
        *state = spin_lock_irqsave(lock);
        obj->refcount = 0;
        return true;
    }
    obj->refcount--;
    return false;
}

void kobject_release_final(struct kobject *obj)
{
    EXPECT(obj->refcount == 0);
    release(obj);
}

uint32_t kobject_refcount(const struct kobject *obj) { return obj->refcount; }

/* ---- mutexes: held or not, and given back after a panic ---- */

#define NHELDLOCK 16u
static struct mutex *g_locked[NHELDLOCK];

void mutex_init(struct mutex *m, const char *name)
{
    note(8, strlen(name));
    m->owner = NULL;
}

static void lock(struct mutex *m)
{
    EXPECT(m->owner == NULL);
    m->owner = (struct thread *)1;
    for (unsigned i = 0; i < NHELDLOCK; i++)
        if (g_locked[i] == NULL) {
            g_locked[i] = m;
            return;
        }
    EXPECT(false);
}

void mutex_lock(struct mutex *m) { lock(m); }

void mutex_lock_nested(struct mutex *m, unsigned subclass)
{
    EXPECT(subclass == VNODE_NESTED_CHILD || subclass == VNODE_NESTED_PARENT2);
    lock(m);
}

void mutex_unlock(struct mutex *m)
{
    EXPECT(m->owner != NULL);
    m->owner = NULL;
    for (unsigned i = 0; i < NHELDLOCK; i++)
        if (g_locked[i] == m) {
            g_locked[i] = NULL;
            return;
        }
    EXPECT(false);
}

static unsigned mutexes_held(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NHELDLOCK; i++)
        n += g_locked[i] != NULL;
    return n;
}

static void unlock_all(void)
{
    for (unsigned i = 0; i < NHELDLOCK; i++)
        if (g_locked[i] != NULL) {
            g_locked[i]->owner = NULL;
            g_locked[i] = NULL;
        }
    harness_release_all_locks();
}

bool lockdep_is_held(const void *l, unsigned kind)
{
    EXPECT(kind == LOCKDEP_KIND_MUTEX);
    return ((const struct mutex *)l)->owner != NULL;
}

static bool held(struct mutex *m) { return m->owner != NULL; }

/* ---- the clock, the credentials, the process, the threads ---- */

static uint64_t g_clock = 1762560000000000000ull;

uint64_t clock_realtime_ns(void)
{
    g_clock += 1000003;
    return g_clock;
}

uint64_t clock_now_ns(void)
{
    g_clock += 7;
    return g_clock - 1762560000000000000ull;
}

static struct credentials g_cred;

const struct credentials *cred_current(void) { return &g_cred; }

bool cred_in_group(const struct credentials *c, uint32_t gid)
{
    if (c->egid == gid)
        return true;
    for (uint32_t i = 0; i < c->ngroups; i++)
        if (c->groups[i] == gid)
            return true;
    return false;
}

static void set_cred(unsigned who)
{
    memset(&g_cred, 0, sizeof(g_cred));
    switch (who) {
    case 0: g_cred.privs = PRIV_ALL; break;                                    /* the kernel, init */
    case 1: break;                                                             /* uid 0, no privilege */
    case 2: g_cred.euid = g_cred.ruid = 1000; g_cred.egid = 1000; g_cred.ngroups = 1; g_cred.groups[0] = 50; break;
    default: g_cred.euid = g_cred.ruid = 1001; g_cred.egid = 50; g_cred.privs = PRIV_ALL; break;   /* dormant */
    }
    note(9, who);
}

static struct process g_proc;
static bool g_in_process;
static struct percpu g_cpu;
static struct thread g_thread[2];

struct process *process_current(void) { return g_in_process ? &g_proc : NULL; }
struct percpu *arch_percpu_get(void) { return &g_cpu; }

static bool g_kill;
bool process_kill_pending(void)
{
    note(10, g_kill);
    return g_kill;
}

static struct vnode *g_chroot;   /* the process's root when it is confined: the test's reference */

struct vnode *vfs_current_root(void)
{
    note(11, g_chroot != NULL);
    if (g_chroot != NULL) {
        vnode_get(g_chroot);
        return g_chroot;
    }
    if (g_root_mount == NULL)
        return NULL;
    return vfs_root();
}

void pmm_get_stats(struct pmm_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->total_pages = 4096;
}

/* ---- block devices: a disk with two partitions and three disks ---- */

#define NBD 6u
static struct blkdev g_bd[NBD];
static const struct kobject_type g_bd_type = { .name = "blkdev" };

const struct blkdev *blk_part_disk(const struct blkdev *bd)
{
    unsigned i = (unsigned)(bd - g_bd);
    EXPECT(i < NBD);
    return i == 1 || i == 2 ? &g_bd[0] : NULL;
}

static unsigned bdid(const struct blkdev *bd) { return bd ? (unsigned)(bd - g_bd) : 99u; }

/* ---- the page cache: what each call is asked and what it says ---- */

void pagecache_init(struct pagecache *pc)
{
    memset(pc, 0, sizeof(*pc));
    note(20, 0);
}

int pagecache_sync(struct vnode *vn)
{
    note(21, vn->ino);   /* a vnode's release syncs without its lock: nobody else can reach it */
    if (chance(6)) {
        vn->pc.wb_seq++;
        note(22, 0);
        return -EIO;
    }
    vn->pc.nr_dirty = 0;
    return 0;
}

unsigned pagecache_drop(struct vnode *vn, bool named)
{
    note(23, vn->ino);
    note(24, named);
    unsigned lost = vn->pc.nr_dirty && pick(2) ? vn->pc.nr_dirty : 0;
    vn->pc.nr_dirty = 0;
    return lost;
}

int64_t pagecache_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    EXPECT(held(&vn->lock));
    note(25, vn->ino);
    note(26, off);
    note(27, len);
    if (chance(10))
        return -EIO;
    size_t n = off >= vn->size ? 0 : vn->size - off < len ? (size_t)(vn->size - off) : len;
    if (n > 3000 && pick(2))
        n = 3000;   /* a short read now and then */
    memset(buf, (int)(vn->ino + off), n);
    return (int64_t)n;
}

int64_t pagecache_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    EXPECT(held(&vn->lock));
    note(28, vn->ino);
    note(29, off);
    note(30, len);
    mix(buf, len < 64 ? len : 64);
    if (chance(8))
        return -ENOSPC;
    if (chance(20))
        return -EIO;
    if (off + len > vn->size)
        vn->size = off + len;
    vn->pc.nr_dirty += 1 + (unsigned)(len / 4096);
    return (int64_t)len;
}

void pagecache_throttle(struct vnode *vn) { note(31, vn->ino); }

void pagecache_lock(struct vnode *vn)
{
    EXPECT(held(&vn->lock));
    note(32, vn->ino);
}

void pagecache_unlock(struct vnode *vn) { note(33, vn->ino); }

bool pagecache_text_busy(struct vnode *vn)
{
    bool busy = chance(12);
    note(34, vn->ino + busy);
    return busy;
}

uint32_t pagecache_wb_seq(struct pagecache *pc) { return pc->wb_seq; }

bool pagecache_error_since(struct pagecache *pc, uint32_t seen, int *err, uint32_t *now)
{
    note(35, seen);
    *now = pc->wb_seq;
    *err = -EIO;
    return pc->wb_seq != seen;
}

void pagecache_set_limit(uint64_t pages) { note(36, pages); }
void pagecache_dirty_init(void) { note(37, 0); }

/* ---- character devices' tables, by inode number ---- */

static const struct chrdev_ops g_chr_fills = { .fills = true };
static const struct chrdev_ops g_chr_plain = { .fills = false };

const struct chrdev_ops *ramfs_chr_ops_of(const struct vnode *vn)
{
    note(38, vn->ino);
    return vn->ino % 3 == 0 ? &g_chr_fills : vn->ino % 3 == 1 ? &g_chr_plain : NULL;
}

/* ---- waits: a sleep is whatever the scenario says happens meanwhile ---- */

static bool g_expired;
static void sleep_drain(void);
static void (*g_sleep)(void);
static unsigned g_sleeps;

void waitqueue_init(struct waitqueue *wq, const char *name)
{
    (void)wq;
    note(40, strlen(name));
}

unsigned waitqueue_wake_all(struct waitqueue *wq)
{
    (void)wq;
    note(41, 0);
    return 0;
}

void waitqueue_prepare(struct waitqueue *wq, struct wait_entry *e) { (void)wq; (void)e; }
void waitqueue_finish(struct waitqueue *wq, struct wait_entry *e) { (void)wq; (void)e; }
void wait_timeout_init(struct wait_timeout *wt, struct waitqueue *wq) { (void)wt; (void)wq; g_expired = false; }
void wait_timeout_fired(struct timer *t, void *arg) { (void)t; (void)arg; }
bool wait_timeout_expired(const struct wait_timeout *wt) { (void)wt; return g_expired; }
void timer_setup(struct timer *t, timer_fn fn, void *arg) { (void)t; (void)fn; (void)arg; }
void timer_start(struct timer *t, uint64_t ns) { (void)t; note(42, ns); }
bool timer_cancel_sync(struct timer *t) { (void)t; return false; }

void sched_block_current(void)
{
    note(43, g_sleeps);
    EXPECT(g_sleep != NULL && g_sleeps < 1000);
    if (g_sleeps >= 1000)
        __builtin_trap();
    if (++g_sleeps > 40 || g_sleep == NULL) {
        g_expired = true;
        sleep_drain();   /* nothing else will end the passes an unmount waits for */
        return;
    }
    g_sleep();
}

/* ---- kernel threads: the orphan reaper is started, never run; the test reaps by hand ---- */

static unsigned g_threads;
static struct thread g_reaper;

struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{
    (void)entry;
    (void)arg;
    note_str(45, name, strlen(name));
    note_rc(46, priority);
    g_threads++;
    return chance(3) ? NULL : &g_reaper;
}

struct list_node *vfs_floor_orphans(void);

/* ---- the test's filesystem: trees of nodes and entries, every operation failing now and then ---- */

#define NTREE 10u
#define NNODE 48u
#define NENT 72u
#define TLEN 1100u

struct tnode {
    bool used, cached, pin;
    enum vnode_type type;
    uint32_t mode, uid, gid, nlink;
    uint64_t size;
    unsigned parent;
    struct vnode *pinned;   /* the filesystem's own reference, while linked */
    unsigned tlen;
    char target[TLEN];
};

struct tent {
    bool used;
    unsigned dir, node;
    size_t len;
    char name[VFS_NAME_MAX + 1];
};

struct tree {
    bool used, mounted, bare;
    unsigned id;
    struct mount *mnt;
    struct tnode n[NNODE];
    struct tent e[NENT];
};

static struct tree g_tree[NTREE];
static bool g_sync_fails, g_dotdot_fails;   /* the next commit fails; the next ".." cannot be read */
static bool g_open_fails;                    /* a device refuses its next open */
static unsigned g_tree_ids;
static void (*g_lookup_hook)(struct tree *t, unsigned e);

static struct tree *tree_of(const struct vnode *vn) { return vn->fs_priv; }
static unsigned node_of(const struct vnode *vn) { return (unsigned)vn->ino - 1; }

static const struct vnode_ops g_dir_ops, g_reg_ops, g_lnk_ops, g_chr_ops, g_chr2_ops, g_other_ops, g_bdir_ops,
    g_bnone_ops;

static const struct vnode_ops *ops_for(const struct tree *t, unsigned i)
{
    const struct tnode *n = &t->n[i];
    if (t->bare)
        return n->type == VNODE_DIR ? &g_bdir_ops : &g_bnone_ops;
    switch (n->type) {
    case VNODE_DIR: return &g_dir_ops;
    case VNODE_REG: return &g_reg_ops;
    case VNODE_LNK: return &g_lnk_ops;
    case VNODE_CHR: return i % 2 ? &g_chr_ops : &g_chr2_ops;
    default: return &g_other_ops;
    }
}

static int node_vnode(struct tree *t, unsigned i, struct vnode **out)
{
    struct vnode *vn = vnode_lookup_cached(t->mnt, i + 1);
    if (vn != NULL) {
        note(50, vn->ino);
        *out = vn;
        return 0;
    }
    vn = vnode_alloc(t->mnt, i + 1);
    if (vn == NULL)
        return -ENOMEM;
    struct tnode *n = &t->n[i];
    vn->type = n->type;
    vn->mode = n->mode;
    vn->uid = n->uid;
    vn->gid = n->gid;
    vn->nlink = n->nlink;
    vn->size = n->size;
    vn->fs_priv = t;
    vn->ops = ops_for(t, i);
    n->cached = true;
    vnode_hash_insert(vn);
    if (n->pin) {
        vnode_get(vn);
        vn->flags |= VNODE_PINNED;
        n->pinned = vn;
    }
    note(51, vn->ino);
    *out = vn;
    return 0;
}

static int new_node(struct tree *t, enum vnode_type type, uint32_t mode)
{
    for (unsigned i = 1; i < NNODE; i++) {
        struct tnode *n = &t->n[i];
        if (n->used || n->cached)
            continue;
        memset(n, 0, sizeof(*n));
        n->used = true;
        n->type = type;
        n->mode = mode;
        n->uid = g_cred.euid;
        n->gid = g_cred.egid;
        n->nlink = type == VNODE_DIR ? 2 : 1;
        n->pin = !t->bare && type == VNODE_REG && pick(4) == 0;
        return (int)i;
    }
    return -ENOSPC;
}

static int find_entry(const struct tree *t, unsigned dir, const char *name, size_t len)
{
    for (unsigned e = 0; e < NENT; e++)
        if (t->e[e].used && t->e[e].dir == dir && t->e[e].len == len && memcmp(t->e[e].name, name, len) == 0)
            return (int)e;
    return -1;
}

static bool dir_empty(const struct tree *t, unsigned dir)
{
    for (unsigned e = 0; e < NENT; e++)
        if (t->e[e].used && t->e[e].dir == dir)
            return false;
    return true;
}

static int add_entry(struct tree *t, unsigned dir, const char *name, size_t len, unsigned node)
{
    EXPECT(len <= VFS_NAME_MAX);
    for (unsigned e = 0; e < NENT; e++)
        if (!t->e[e].used) {
            t->e[e].used = true;
            t->e[e].dir = dir;
            t->e[e].node = node;
            t->e[e].len = len;
            memcpy(t->e[e].name, name, len);
            t->e[e].name[len] = '\0';
            return 0;
        }
    return -ENOSPC;
}

/* A name gone from node `i`, whose vnode `vn` the VFS holds. */
static void drop_name(struct tree *t, unsigned i, struct vnode *vn)
{
    struct tnode *n = &t->n[i];
    if (n->type == VNODE_DIR)
        n->nlink = 0;
    else
        n->nlink--;
    vn->nlink = n->nlink;
    if (n->nlink != 0)
        return;
    n->used = false;
    if (n->pinned != NULL) {
        struct vnode *p = n->pinned;
        n->pinned = NULL;
        p->flags &= ~VNODE_PINNED;
        vnode_put(p);
    }
}

static int fail(int rc)
{
    note_rc(52, rc);
    return rc;
}

static void note_op(uint32_t op, const struct vnode *dir, const char *name, size_t len)
{
    note(op, dir->ino);
    note(53, tree_of(dir)->id);
    note_str(54, name, len);
}

static int t_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    EXPECT(held(&dir->lock) && dir->type == VNODE_DIR);
    note_op(60, dir, name, len);
    struct tree *t = tree_of(dir);
    if (chance(60))
        return fail(-EIO);
    unsigned d = node_of(dir);
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        if (g_dotdot_fails) {
            g_dotdot_fails = false;
            return fail(-EIO);
        }
        return node_vnode(t, t->n[d].parent, out);
    }
    int e = find_entry(t, d, name, len);
    if (e < 0)
        return -ENOENT;
    if (g_lookup_hook != NULL)
        g_lookup_hook(t, (unsigned)e);
    return node_vnode(t, t->e[e].node, out);
}

static int make(struct vnode *dir, const char *name, size_t len, enum vnode_type type, uint32_t mode,
                const char *target, struct vnode **out)
{
    struct tree *t = tree_of(dir);
    note(61, type);
    note(62, mode);
    if (chance(20))
        return fail(-ENOSPC);
    unsigned d = node_of(dir);
    if (find_entry(t, d, name, len) >= 0)
        return fail(-EEXIST);   /* the VFS's own look failed: the filesystem is the one that knows */
    int i = new_node(t, type, mode);
    if (i < 0)
        return fail(i);
    if (add_entry(t, d, name, len, (unsigned)i) != 0) {
        t->n[i].used = false;
        return fail(-ENOSPC);
    }
    struct tnode *n = &t->n[i];
    n->parent = d;
    if (target != NULL) {
        n->tlen = (unsigned)strlen(target);
        EXPECT(n->tlen < TLEN);
        memcpy(n->target, target, n->tlen);
    }
    if (type == VNODE_DIR) {
        t->n[d].nlink++;
        dir->nlink++;
    }
    return node_vnode(t, (unsigned)i, out);
}

static int t_create(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    EXPECT(held(&dir->lock));
    note_op(63, dir, name, len);
    return make(dir, name, len, VNODE_REG, mode, NULL, out);
}

static int t_mkdir(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    EXPECT(held(&dir->lock));
    note_op(64, dir, name, len);
    return make(dir, name, len, VNODE_DIR, mode, NULL, out);
}

static int t_mknod(struct vnode *dir, const char *name, size_t len, uint32_t mode, enum vnode_type type,
                   struct vnode **out)
{
    EXPECT(held(&dir->lock));
    note_op(65, dir, name, len);
    return make(dir, name, len, type, mode, NULL, out);
}

static int t_symlink(struct vnode *dir, const char *name, size_t len, const char *target, struct vnode **out)
{
    EXPECT(held(&dir->lock));
    note_op(66, dir, name, len);
    note_str(67, target, strlen(target));
    return make(dir, name, len, VNODE_LNK, 0777, target, out);
}

static int t_readlink(struct vnode *vn, char *buf, size_t len)
{
    EXPECT(held(&vn->lock));
    note(68, vn->ino);
    note(69, len);
    if (chance(25))
        return fail(-EIO);
    struct tnode *n = &tree_of(vn)->n[node_of(vn)];
    if (chance(30))
        return 0;   /* a link whose target reads empty */
    size_t k = n->tlen < len ? n->tlen : len;
    memcpy(buf, n->target, k);
    return (int)k;
}

static int t_unlink(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    EXPECT(held(&dir->lock) && held(&victim->lock));
    note_op(70, dir, name, len);
    note(71, victim->ino);
    if (chance(15))
        return fail(-EIO);
    struct tree *t = tree_of(dir);
    int e = find_entry(t, node_of(dir), name, len);
    EXPECT(e >= 0 && t->e[e].node == node_of(victim));
    t->e[e].used = false;
    drop_name(t, node_of(victim), victim);
    return 0;
}

static int t_rmdir(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    EXPECT(held(&dir->lock) && held(&victim->lock));
    note_op(72, dir, name, len);
    note(73, victim->ino);
    struct tree *t = tree_of(dir);
    if (!dir_empty(t, node_of(victim)))
        return fail(-ENOTEMPTY);
    if (chance(15))
        return fail(-EIO);
    int e = find_entry(t, node_of(dir), name, len);
    EXPECT(e >= 0 && t->e[e].node == node_of(victim));
    t->e[e].used = false;
    t->n[node_of(dir)].nlink--;
    dir->nlink--;
    drop_name(t, node_of(victim), victim);
    return 0;
}

static int t_link(struct vnode *dir, const char *name, size_t len, struct vnode *target)
{
    EXPECT(held(&dir->lock) && held(&target->lock));
    note_op(74, dir, name, len);
    note(75, target->ino);
    if (chance(15))
        return fail(-EIO);
    struct tree *t = tree_of(dir);
    if (add_entry(t, node_of(dir), name, len, node_of(target)) != 0)
        return fail(-ENOSPC);
    t->n[node_of(target)].nlink++;
    target->nlink++;
    return 0;
}

static int t_rename(struct vnode *odir, const char *oname, size_t olen, struct vnode *victim, struct vnode *ndir,
                    const char *nname, size_t nlen, struct vnode *replaced)
{
    EXPECT(held(&odir->lock) && held(&ndir->lock));
    note_op(76, odir, oname, olen);
    note_op(77, ndir, nname, nlen);
    note(78, victim->ino);
    note(79, replaced ? replaced->ino : 0);
    struct tree *t = tree_of(odir);
    if (chance(15))
        return fail(-EIO);
    unsigned od = node_of(odir), nd = node_of(ndir);
    if (replaced != NULL) {
        if (replaced->type == VNODE_DIR && !dir_empty(t, node_of(replaced)))
            return fail(-ENOTEMPTY);
        int r = find_entry(t, nd, nname, nlen);
        EXPECT(r >= 0 && t->e[r].node == node_of(replaced));
        t->e[r].used = false;
        if (replaced->type == VNODE_DIR) {
            t->n[nd].nlink--;
            ndir->nlink--;
        }
        drop_name(t, node_of(replaced), replaced);
    }
    int e = find_entry(t, od, oname, olen);
    EXPECT(e >= 0 && t->e[e].node == node_of(victim));
    t->e[e].dir = nd;
    t->e[e].len = nlen;
    memcpy(t->e[e].name, nname, nlen);
    t->e[e].name[nlen] = '\0';
    if (victim->type == VNODE_DIR && od != nd) {
        t->n[node_of(victim)].parent = nd;
        t->n[od].nlink--;
        odir->nlink--;
        t->n[nd].nlink++;
        ndir->nlink++;
    }
    return 0;
}

static int t_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    EXPECT(held(&dir->lock));
    note(80, dir->ino);
    note(81, *pos);
    if (chance(15))
        return fail(-EIO);
    struct tree *t = tree_of(dir);
    uint64_t k = 0;
    for (unsigned e = 0; e < NENT; e++) {
        const struct tent *te = &t->e[e];
        if (!te->used || te->dir != node_of(dir))
            continue;
        if (k++ < *pos)
            continue;
        int r = cb(arg, te->name, te->len, te->node + 1, t->n[te->node].type);
        note_rc(82, r);
        if (r)
            break;
        (*pos)++;
    }
    return 0;
}

static int t_truncate(struct vnode *vn, uint64_t size)
{
    EXPECT(held(&vn->lock));
    note(83, vn->ino);
    note(84, size);
    if (chance(10))
        return fail(-EIO);
    vn->size = size;
    return 0;
}

static int t_writepage(struct vnode *vn, uint64_t index, const void *buf)
{
    (void)vn;
    (void)index;
    (void)buf;
    EXPECT(false);   /* the page cache is the test's: nothing writes a page */
    return 0;
}

static int t_sync(struct vnode *vn)
{
    EXPECT(held(&vn->lock));
    note(85, vn->ino);
    return chance(8) ? fail(-EIO) : 0;
}

static void t_evict(struct vnode *vn)
{
    note(86, vn->ino);
    struct tree *t = tree_of(vn);
    struct tnode *n = &t->n[node_of(vn)];
    EXPECT(n->cached && n->pinned == NULL);
    n->cached = false;
    if (n->used) {
        n->mode = vn->mode;
        n->uid = vn->uid;
        n->gid = vn->gid;
        n->size = vn->size;
    }
}

static int t_setattr(struct vnode *vn)
{
    EXPECT(held(&vn->lock));
    note(87, vn->ino);
    note(88, vn->mode);
    note(89, ((uint64_t)vn->uid << 32) | vn->gid);
    return chance(8) ? fail(-EIO) : 0;
}

static int t_open(struct vnode *vn, struct file *f)
{
    note(90, vn->ino);
    note(91, file_flags(f));
    if (chance(6) || g_open_fails) {
        g_open_fails = false;
        return fail(-EACCES);
    }
    f->priv = vn;
    return 0;
}

static void t_release(struct vnode *vn, struct file *f)
{
    note(92, vn->ino);
    EXPECT(f->priv == vn);
}

static int64_t chr_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    EXPECT(!held(&vn->lock));
    note(93, vn->ino);
    note(94, off);
    note(95, len);
    if (chance(10))
        return fail(-EAGAIN);
    size_t n = len < 700 ? len : pick(3) ? 700 : len;
    memset(buf, 'c', n);
    return (int64_t)n;
}

static int64_t chr_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    EXPECT(!held(&vn->lock));
    note(96, vn->ino);
    note(97, off);
    mix(buf, len < 32 ? len : 32);
    return chance(10) ? fail(-EPIPE) : (int64_t)(len < 500 ? len : 500);
}

static int64_t t_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    EXPECT(f->priv == vn);
    return chr_read(vn, off, buf, len);
}

static int64_t t_write_file(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    EXPECT(f->priv == vn);
    return chr_write(vn, off, buf, len);
}

static unsigned t_ready(struct vnode *vn, struct file *f)
{
    (void)f;
    note(98, vn->ino);
    return pick(4);
}

static struct waitqueue g_chr_wq;

static struct waitqueue *t_poll_wq(struct vnode *vn, struct file *f, unsigned events)
{
    (void)f;
    note(99, vn->ino);
    note(100, events);
    return pick(2) ? &g_chr_wq : NULL;
}

static int t_set_nonblock(struct vnode *vn, struct file *f, int on)
{
    (void)f;
    note(101, vn->ino);
    note_rc(102, on);
    return (int)pick(2);
}

static const struct vnode_ops g_dir_ops = {
    .lookup = t_lookup, .create = t_create, .symlink = t_symlink, .mkdir = t_mkdir, .mknod = t_mknod,
    .unlink = t_unlink, .link = t_link, .rmdir = t_rmdir, .rename = t_rename, .readdir = t_readdir,
    .sync = t_sync, .evict = t_evict, .setattr = t_setattr,
};
static const struct vnode_ops g_reg_ops = {
    .truncate = t_truncate, .writepage = t_writepage, .sync = t_sync, .evict = t_evict, .setattr = t_setattr,
};
static const struct vnode_ops g_lnk_ops = { .readlink = t_readlink, .evict = t_evict, .setattr = t_setattr };
static const struct vnode_ops g_chr_ops = {
    .open = t_open, .release = t_release, .read_file = t_read_file, .write_file = t_write_file, .ready = t_ready,
    .poll_wq = t_poll_wq, .set_nonblock = t_set_nonblock, .evict = t_evict, .setattr = t_setattr,
};
static const struct vnode_ops g_chr2_ops = { .read = chr_read, .write = chr_write, .evict = t_evict };
static const struct vnode_ops g_other_ops = { .evict = t_evict, .setattr = t_setattr };
static const struct vnode_ops g_bdir_ops = { .lookup = t_lookup, .evict = t_evict };
static const struct vnode_ops g_bnone_ops = { .evict = t_evict };

/* ---- the filesystem types ---- */

static struct fs_type g_tfs, g_bare;

static void seed(struct tree *t, unsigned parent, const char *name, enum vnode_type type, uint32_t mode,
                 const char *target)
{
    int i = new_node(t, type, mode);
    EXPECT(i > 0);
    t->n[i].uid = 0;
    t->n[i].gid = 0;
    t->n[i].parent = parent;
    t->n[i].pin = false;
    if (target != NULL) {
        t->n[i].tlen = (unsigned)strlen(target);
        memcpy(t->n[i].target, target, t->n[i].tlen);
    }
    if (type == VNODE_DIR)
        t->n[parent].nlink++;
    EXPECT(add_entry(t, parent, name, strlen(name), (unsigned)i) == 0);
}

static int t_mount(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt)
{
    note(110, bdid(bdev));
    note(111, flags);
    note(112, fs == &g_bare);
    EXPECT(mnt->root == NULL);
    if (chance(12))
        return fail(-EIO);
    struct tree *t = NULL;
    for (unsigned k = 0; k < NTREE && t == NULL; k++) {
        bool busy = g_tree[k].used;
        for (unsigned i = 0; i < NNODE && !busy; i++)
            busy = g_tree[k].n[i].cached;
        if (!busy)
            t = &g_tree[k];
    }
    if (t == NULL)
        return fail(-ENOSPC);
    memset(t, 0, sizeof(*t));
    t->used = t->mounted = true;
    t->bare = fs == &g_bare;
    t->id = ++g_tree_ids;
    t->mnt = mnt;
    struct tnode *root = &t->n[0];
    root->used = true;
    root->type = VNODE_DIR;
    root->mode = (flags & MOUNT_SCRATCH) ? 01777 : 0755;
    root->nlink = 2;
    seed(t, 0, "c", VNODE_CHR, 0666, NULL);
    if (t->bare) {
        seed(t, 0, "a", VNODE_DIR, 0755, NULL);
        seed(t, 0, "f", VNODE_REG, 0644, NULL);
        seed(t, 0, "l", VNODE_LNK, 0777, "a");
        seed(t, 2, "f", VNODE_REG, 0600, NULL);
    }
    if (bdev != NULL && g_faults && pick(3) == 0) {
        struct blkdev *list[MOUNT_HELD_MAX + 1];
        unsigned n = pick(4) == 0 ? MOUNT_HELD_MAX + 1 : 1 + pick(2);
        for (unsigned i = 0; i < n; i++)
            list[i] = &g_bd[pick(NBD)];
        int rc = vfs_mount_set_held(mnt, list, n);
        note_rc(113, rc);
        if (rc == 0 && pick(2)) {
            rc = vfs_mount_set_held(mnt, list, 1);   /* and replaced */
            note_rc(113, rc);
        }
        if (rc) {
            t->used = t->mounted = false;
            return rc;
        }
    }
    int rc = node_vnode(t, 0, &mnt->root);
    if (rc) {
        t->used = t->mounted = false;
        return fail(rc);
    }
    mnt->fs_priv = t;
    return 0;
}

static int t_unmount(struct mount *mnt)
{
    struct tree *t = mnt->fs_priv;
    note(114, t->id);
    EXPECT(t->mounted && t->mnt == mnt);
    for (unsigned i = 0; i < NNODE; i++)
        if (t->n[i].pinned != NULL) {
            struct vnode *p = t->n[i].pinned;
            t->n[i].pinned = NULL;
            p->flags &= ~VNODE_PINNED;
            vnode_put(p);
        }
    t->mounted = false;
    t->used = false;
    return chance(10) ? fail(-EIO) : 0;
}

static int t_fs_sync(struct mount *mnt)
{
    struct tree *t = mnt->fs_priv;
    note(115, t->id);
    EXPECT(t->mounted);
    return chance(8) || g_sync_fails ? fail(-EIO) : 0;
}

static int t_statfs(struct mount *mnt, struct vfs_statfs *out)
{
    note(116, ((struct tree *)mnt->fs_priv)->id);
    out->blocks = 64;
    out->bfree = out->bavail = 32;
    return chance(5) ? fail(-EIO) : 0;
}

static int t_format(struct blkdev *bdev, const char *label)
{
    note(117, bdid(bdev));
    note_str(118, label ? label : "", label ? strlen(label) : 0);
    return chance(4) ? fail(-EIO) : 0;
}

static int t_format_mirror(struct blkdev **bd, unsigned n, const char *label)
{
    note(119, n);
    for (unsigned i = 0; i < n; i++)
        note(120, bdid(bd[i]));
    return t_format(bd[0], label);
}

static int t_format_keyed(struct blkdev **bd, unsigned n, const char *label, const uint8_t uuid[16],
                          const uint8_t master[32], const struct cosmo_keyslot *slots)
{
    (void)slots;
    mix(uuid, 16);
    mix(master, 32);
    return t_format_mirror(bd, n, label);
}

static struct fs_type g_tfs = {
    .name = "tfs", .magic = 0x7466, .mount = t_mount, .unmount = t_unmount, .sync = t_fs_sync,
    .format = t_format, .format_mirror = t_format_mirror, .format_keyed = t_format_keyed,
};
static int t_mount_fails(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt)
{
    (void)fs;
    (void)bdev;
    (void)flags;
    (void)mnt;
    return fail(-EIO);
}

static struct fs_type g_failfs = { .name = "failfs", .mount = t_mount_fails, .unmount = t_unmount };
static struct fs_type g_bare = {
    .name = "bare", .magic = 0x6261, .mount = t_mount, .unmount = t_unmount, .statfs = t_statfs,
};

/* ---- the test's references ---- */

#define NHELD 12u
#define NFILE 10u
#define NPASS 4u
#define NNS 3u

static struct vnode *g_held[NHELD];
static char g_held_name[NHELD][VFS_PATH_MAX];
static struct file *g_file[NFILE];
static struct mount *g_pass[NPASS];
static struct mount_ns *g_ns[NNS];   /* [0] the initial namespace, borrowed; the others the test's */
static struct mount *g_detached;
static struct mount *g_old_root;

static void put_held(unsigned i)
{
    if (g_held[i] == NULL)
        return;
    note(130, g_held[i]->ino);
    if (g_held[i] == g_chroot) {
        vnode_put(g_chroot);
        g_chroot = NULL;
    }
    vnode_put(g_held[i]);
    g_held[i] = NULL;
    g_held_name[i][0] = '\0';
}

static void keep(struct vnode *vn, const char *name)
{
    unsigned i = pick(NHELD);
    put_held(i);
    g_held[i] = vn;
    strlcpy(g_held_name[i], name ? name : "", sizeof(g_held_name[i]));
}

static struct vnode *some_start(const char **name)
{
    unsigned i = pick(NHELD + 3);
    if (i >= NHELD || g_held[i] == NULL || g_held[i]->type != VNODE_DIR) {   /* a start is a directory (at_base) */
        if (name)
            *name = pick(4) ? NULL : "/";
        return NULL;
    }
    if (name)
        *name = g_held_name[i][0] || pick(3) ? g_held_name[i] : pick(2) ? "rel" : NULL;
    return g_held[i];
}

static struct vnode *some_held(void)
{
    unsigned i = pick(NHELD);
    for (unsigned k = 0; k < NHELD; k++)
        if (g_held[(i + k) % NHELD] != NULL)
            return g_held[(i + k) % NHELD];
    return NULL;
}

static void put_file(unsigned i)
{
    if (g_file[i] == NULL)
        return;
    note(131, g_file[i]->vn->ino);
    file_put(g_file[i]);
    g_file[i] = NULL;
}

/* Everything the test holds, as a process that exits gives it back. */
static void drop_all(void)
{
    for (unsigned i = 0; i < NHELD; i++)
        put_held(i);
    for (unsigned i = 0; i < NFILE; i++)
        put_file(i);
    for (unsigned k = 0; k < NPASS; k++)
        if (g_pass[k] != NULL) {
            vfs_mount_release(g_pass[k]);
            g_pass[k] = NULL;
        }
    if (g_chroot != NULL) {
        vnode_put(g_chroot);
        g_chroot = NULL;
    }
}

/* ---- paths ---- */

static const char *const g_words[] = { "a", "b", "c", "d", "f", "l", "m", "n", "p", "s", "x", ".", "..", "l2" };
#define NWORDS (sizeof(g_words) / sizeof(g_words[0]))
#define PATHCAP 1400u

static void make_path(char *buf)
{
    size_t at = 0;
    unsigned shape = pick(40);
    if (shape == 0) {   /* longer than a path */
        while (at < VFS_PATH_MAX + 20) {
            buf[at++] = 'a';
            buf[at++] = '/';
        }
        buf[at] = '\0';
        return;
    }
    if (shape == 1) {
        buf[0] = '\0';
        return;
    }
    if (pick(2))
        buf[at++] = '/';
    if (pick(12) == 0)
        buf[at++] = '/';
    unsigned k = pick(4) == 0 ? pick(5) : 1 + pick(2);
    for (unsigned c = 0; c < k; c++) {
        if (c > 0) {
            buf[at++] = '/';
            if (pick(15) == 0)
                buf[at++] = '/';
        }
        if (pick(60) == 0) {   /* longer than a name */
            memset(buf + at, 'q', VFS_NAME_MAX + 1);
            at += VFS_NAME_MAX + 1;
        } else if (pick(50) == 0) {   /* the longest name */
            memset(buf + at, 'r', VFS_NAME_MAX);
            at += VFS_NAME_MAX;
        } else {
            const char *w = g_words[pick(NWORDS)];
            size_t n = strlen(w);
            memcpy(buf + at, w, n);
            at += n;
        }
    }
    if (k == 0 && at == 0)
        buf[at++] = '.';
    if (pick(10) == 0)
        buf[at++] = '/';
    buf[at] = '\0';
    EXPECT(at < PATHCAP);
}

static void make_target(char *buf)
{
    unsigned shape = pick(12);
    if (shape == 0) {   /* a target that takes up nearly a whole path */
        memset(buf, 'a', VFS_PATH_MAX - 2);
        buf[VFS_PATH_MAX - 2] = '\0';
        return;
    }
    if (shape == 1) {
        strlcpy(buf, "l", PATHCAP);   /* itself, in a directory where it is "l" */
        return;
    }
    make_path(buf);
    if (buf[0] == '\0' || strlen(buf) >= VFS_PATH_MAX)
        strlcpy(buf, "..", PATHCAP);
}

static void note_vn(uint32_t which, int rc, const struct vnode *vn)
{
    note_rc(which, rc);
    if (rc == 0) {
        note(which + 1, vn->ino);
        note(which + 2, vn->type);
        note(which + 3, tree_of(vn)->id);
    }
}

static void note_stat(const struct cosmo_stat *st)
{
    note(140, st->ino);
    note(141, st->type);
    note(142, st->mode);
    note(143, st->nlink);
    note(144, ((uint64_t)st->uid << 32) | st->gid);
    note(145, st->size);
    note(146, st->mtime_ns);
    note(147, st->ctime_ns);
    note(148, st->dev);
}

/* ---- the processes a call runs as ---- */

static void as_someone(void)
{
    set_cred(pick(5) ? 0 : 1 + pick(3));
    g_in_process = pick(4) != 0;
    unsigned ns = pick(NNS);
    g_proc.mntns = ns == 0 || g_ns[ns] == NULL ? NULL : g_ns[ns];
    g_proc.nr_threads = 1 + pick(2);
    strlcpy(g_proc.name, pick(2) ? "racer" : "other", sizeof(g_proc.name));
    g_cpu.current = &g_thread[pick(2)];
    note(150, g_in_process);
    note(151, ns);
}

static void as_kernel(void)
{
    set_cred(0);
    g_in_process = false;
    g_cpu.current = &g_thread[0];
}

/* ---- the operations ---- */

static unsigned some_flags(void)
{
    static const unsigned acc[] = { COSMO_O_RDONLY, COSMO_O_WRONLY, COSMO_O_RDWR, COSMO_O_ACCMODE };
    unsigned f = acc[pick(10) == 0 ? 3 : pick(3)];
    if (pick(3) == 0)
        f |= COSMO_O_CREAT;
    if (pick(6) == 0)
        f |= COSMO_O_EXCL;
    if (pick(6) == 0)
        f |= COSMO_O_TRUNC;
    if (pick(6) == 0)
        f |= COSMO_O_APPEND;
    if (pick(8) == 0)
        f |= COSMO_O_NONBLOCK;
    if (pick(8) == 0)
        f |= COSMO_O_DIRECTORY;
    if (pick(8) == 0)
        f |= COSMO_O_NOFOLLOW;
    return f;
}

static void op_lookup(void)
{
    char path[PATHCAP];
    make_path(path);
    const char *startname;
    struct vnode *start = some_start(&startname);
    struct vnode *vn = NULL;
    unsigned how = pick(4);
    note(160, how);
    note_str(161, path, strlen(path));
    int rc;
    char name[VFS_PATH_MAX];
    name[0] = '\0';
    if (how == 0) {
        rc = vfs_lookup_nofollow(start, path, &vn);
    } else if (how == 1) {
        size_t n = pick(8) == 0 ? pick(3) : pick(8) == 0 ? 6 : sizeof(name);
        rc = vfs_lookup_named(start, startname, pick(30) ? path : NULL, &vn, name, n);
        if (rc == 0)
            note_str(162, name, strlen(name));
    } else {
        rc = vfs_lookup(start, path, &vn);
    }
    note_vn(163, rc, vn);
    if (rc == 0) {
        if (pick(3))
            keep(vn, name);
        else
            vnode_put(vn);
    }
}

static void op_open(void)
{
    char path[PATHCAP];
    make_path(path);
    const char *startname;
    struct vnode *start = some_start(&startname);
    unsigned flags = some_flags();
    uint32_t mode = pick(2) ? 0644 : pick(2) ? 0 : 07777;
    struct file *f = NULL;
    unsigned how = pick(5);
    note(170, how);
    note(171, flags);
    note_str(172, path, strlen(path));
    int rc;
    char name[VFS_PATH_MAX];
    if (how == 0) {
        size_t n = pick(6) == 0 ? pick(3) : pick(6) == 0 ? 5 : sizeof(name);
        rc = vfs_open_named(start, startname, path, flags, mode, &f, name, n);
        if (n > 0)
            note_str(173, name, strnlen(name, n));
        if (rc == 0 && pick(2))
            file_set_dir_path(f, name);
    } else if (how == 1) {
        struct vnode *vn = some_held();
        if (vn == NULL)
            return;
        vnode_get(vn);
        rc = vfs_open_vnode(vn, flags, &f);
    } else {
        rc = vfs_open(start, path, flags, mode, &f);
    }
    note_rc(174, rc);
    if (rc)
        return;
    note(175, f->vn->ino);
    note(176, f->pos);
    if (pick(3) == 0)
        file_set_dir_path(f, pick(2) ? "/a/b" : "rel");
    unsigned i = pick(NFILE);
    put_file(i);
    g_file[i] = f;
}

static int g_put_calls;
static int put_piece(void *ctx, const void *piece, size_t n)
{
    (void)ctx;
    mix(piece, n < 16 ? n : 16);
    note(180, n);
    g_put_calls++;
    return pick(6) == 0 ? -EFAULT : 0;
}

static void op_file(void)
{
    unsigned i = pick(NFILE);
    struct file *f = g_file[i];
    if (f == NULL)
        return;
    static uint8_t buf[9000];
    unsigned what = pick(22);
    note(181, what);
    note(182, f->vn->ino);
    int64_t rc = 0;
    size_t len = pick(4) == 0 ? 0 : pick(3) == 0 ? 5000 + pick(3000) : pick(600);
    uint64_t off = pick(3) ? pick(9000) : 0;
    const struct kobject_io_type *io = (const struct kobject_io_type *)f->obj.type;
    switch (what) {
    case 0: rc = file_read(f, buf, len); break;
    case 1: memset(buf, (int)what, len); rc = file_write(f, buf, len); break;
    case 2: rc = file_pread(f, buf, len, off); break;
    case 3: memset(buf, 7, len); rc = file_pwrite(f, buf, len, off); break;
    case 4: {
        static const int64_t offs[] = { 0, 10, -5, INT64_MAX, -100000 };
        int64_t to = offs[pick(5)];
        rc = file_seek(f, to, (int)pick(4));
        break;
    }
    case 5: {
        struct cosmo_stat st;
        rc = file_stat(f, &st);
        note_stat(&st);
        break;
    }
    case 6: rc = file_sync(f); break;
    case 7: rc = file_flush(f); break;
    case 8: case 9: {
        size_t n = pick(4) == 0 ? pick(sizeof(struct cosmo_dirent) + 3) : 24 + pick(200);
        rc = file_readdir(f, buf, n);
        if (rc > 0)
            mix(buf, (size_t)rc);
        break;
    }
    case 10: rc = io->read(&f->obj, buf, len); break;
    case 11: rc = io->write(&f->obj, buf, len); break;
    case 12: rc = io->flush(&f->obj); break;
    case 13: rc = io->ready(&f->obj); break;
    case 14: rc = io->poll_wq(&f->obj, pick(4)) != NULL; break;
    case 15: rc = io->set_nonblock(&f->obj, (int)pick(3) - 1); break;
    case 16: {
        g_put_calls = 0;
        size_t cap = 1 + pick(4) * 1000;
        rc = io->read_filled(&f->obj, buf, cap, pick(9000), put_piece, NULL);
        note(183, (uint64_t)g_put_calls);
        note(184, f->pos);
        break;
    }
    case 17: rc = file_nonblocking(f); break;
    case 18: rc = file_set_nonblock(f, (int)pick(3) - 1); break;
    case 19: rc = file_dir_base_ok(f); break;
    case 20: rc = file_from_kobject(&f->obj) == f; break;
    default: {
        struct kobject other = { .type = &g_bd_type };
        rc = file_from_kobject(&other) == NULL;
        break;
    }
    }
    note_rc(185, rc);
    if (what == 0 || what == 2 || what == 10)
        if (rc > 0)
            mix(buf, (size_t)rc < 64 ? (size_t)rc : 64);
    if (pick(8) == 0)
        put_file(i);
}

static void op_mutate(void)
{
    char path[PATHCAP], path2[PATHCAP];
    make_path(path);
    struct vnode *start = some_start(NULL);
    unsigned what = pick(13);
    note(190, what);
    note_str(191, path, strlen(path));
    int rc = 0;
    uint32_t mode = pick(4) ? 0755 : pick(2) ? 01777 : 0;
    switch (what) {
    case 0: case 1: rc = vfs_mkdir(start, path, mode); break;
    case 2: rc = vfs_unlink(start, path); break;
    case 3: rc = vfs_rmdir(start, path); break;
    case 4: {
        static const enum vnode_type types[] = { VNODE_SOCK, VNODE_FIFO, VNODE_REG };
        struct vnode *vn = NULL;
        rc = vfs_mknod(start, path, mode, types[pick(3)], &vn);
        note_vn(192, rc, vn);
        if (rc == 0)
            vnode_put(vn);
        break;
    }
    case 5: case 6: {
        make_target(path2);
        rc = vfs_symlink(start, path, pick(40) == 0 ? (pick(2) ? NULL : "") : path2);
        break;
    }
    case 7: {
        make_path(path2);
        note_str(193, path2, strlen(path2));
        struct vnode *nstart = some_start(NULL);
        rc = vfs_link(start, path, nstart, path2, pick(2));
        break;
    }
    case 8: case 9: {
        make_path(path2);
        note_str(193, path2, strlen(path2));
        rc = pick(2) ? vfs_rename(start, path, path2) : vfs_rename2(start, path, some_start(NULL), path2);
        break;
    }
    case 10: rc = vfs_truncate(start, path, pick(5000)); break;
    case 11: {
        struct vnode *vn = some_held();
        if (vn)
            rc = vfs_ftruncate(vn, pick(5000));
        break;
    }
    default: {
        /* A regular file made and written, so that names and pages exist to work on. */
        struct file *f = NULL;
        rc = vfs_open(start, path, COSMO_O_RDWR | COSMO_O_CREAT, 0666, &f);
        if (rc == 0) {
            static const uint8_t data[300] = { 1, 2, 3 };
            note_rc(194, file_write(f, data, 1 + pick(300)));
            file_put(f);
        }
        break;
    }
    }
    note_rc(195, rc);
}

static void op_query(void)
{
    char path[PATHCAP];
    make_path(path);
    struct vnode *start = some_start(NULL);
    unsigned what = pick(8);
    note(200, what);
    note_str(201, path, strlen(path));
    int rc = 0;
    switch (what) {
    case 0: case 1: {
        struct cosmo_stat st;
        rc = what ? vfs_lstat(start, path, &st) : vfs_stat(start, path, &st);
        if (rc == 0)
            note_stat(&st);
        break;
    }
    case 2: case 3: {
        struct vfs_statfs sf;
        memset(&sf, 0, sizeof(sf));
        struct vnode *vn = some_held();
        if (what == 3 && vn != NULL) {
            if (pick(3) == 0) {
                vn->mnt->cache_limit_pages = pick(3) * 50;
                vn->mnt->cache_pages = pick(120);
            }
            rc = vnode_statfs(vn, &sf);
        } else {
            rc = vfs_statfs(start, path, &sf);
        }
        if (rc == 0 || what == 3) {
            note(202, sf.magic);
            note(203, sf.blocks);
            note(204, sf.bfree);
            note(205, sf.flags);
        }
        break;
    }
    case 4: case 5: {
        char buf[VFS_PATH_MAX];
        size_t n = pick(5) == 0 ? pick(3) : sizeof(buf);
        rc = vfs_readlink(start, path, buf, n);
        if (rc > 0)
            mix(buf, (size_t)rc);
        break;
    }
    case 6: {
        struct vnode *vn = some_held();
        if (vn != NULL) {
            struct cosmo_stat st;
            vnode_stat(vn, &st);
            note_stat(&st);
            rc = vfs_permission(vn, pick(8));
        }
        break;
    }
    default: {
        struct vnode *vn = some_held();
        if (vn == NULL)
            break;
        if (pick(2)) {
            unsigned what2 = pick(8);
            rc = vfs_setattr(vn, what2, pick(2) ? 04755 : 0600, 12345);
            note(206, what2);
        } else {
            static const uint32_t ids[] = { VFS_ID_KEEP, 0, 1000, 50, 1001 };
            uint32_t uid = ids[pick(5)];
            rc = vfs_chown(vn, uid, ids[pick(5)]);
        }
        note(207, vn->mode);
        note(208, ((uint64_t)vn->uid << 32) | vn->gid);
        break;
    }
    }
    note_rc(209, rc);
}

static const char *const g_mpaths[] = { "/m", "/n", "/a/m", "m", "/s", "/", "/x/y", "/m/s" };

static struct blkdev *some_bdev(void)
{
    return pick(3) == 0 ? NULL : &g_bd[pick(NBD)];
}

static void op_mount(void)
{
    unsigned what = pick(10);
    note(210, what);
    int rc = 0;
    const char *path = g_mpaths[pick(sizeof(g_mpaths) / sizeof(g_mpaths[0]))];
    note_str(211, path, strlen(path));
    static const char *const fsnames[] = { "tfs", "tfs", "bare", "zz" };
    const char *fsname = fsnames[pick(4)];
    unsigned flags = pick(3) == 0 ? MOUNT_RDONLY : pick(3) == 0 ? MOUNT_SCRATCH : pick(3) == 0 ? MOUNT_CACHE_IS_STORE : 0;
    switch (what) {
    case 0: case 1: case 2: {
        const char *startname;
        struct vnode *start = some_start(&startname);
        rc = pick(3) ? vfs_mount_at(start, startname, path, fsname, some_bdev(), flags)
                     : vfs_mount(path, fsname, some_bdev(), flags);
        break;
    }
    case 3: case 4: case 5: {
        unsigned uflags = pick(4) == 0 ? VFS_UMOUNT_FORCE : 0;
        struct vnode *start = some_start(NULL);
        bool absolute = pick(2);
        rc = absolute ? vfs_umount2(path, uflags) : vfs_umount_at(start, path, uflags);
        break;
    }
    case 6: rc = vfs_sync(); break;
    case 7: {
        unsigned k = pick(NPASS);
        if (g_pass[k] != NULL) {
            vfs_mount_release(g_pass[k]);
            g_pass[k] = NULL;
        } else {
            struct mount *m = NULL;
            struct vnode *h = some_held();
            uint64_t id = pick(10) == 0 ? 0 : h != NULL && pick(2) ? h->mnt->id : pick(3) == 0 ? g_root_mount->id : pick(200);
            rc = vfs_mount_acquire(id, pick(30) ? &m : NULL);
            note(212, id);
            if (rc == 0)
                g_pass[k] = m;
        }
        break;
    }
    case 8: {
        note(213, vfs_mount_count());
        note(214, vfs_vnode_count());
        if (pick(20) == 0)
            vfs_dump();
        break;
    }
    default: {
        struct blkdev *bd = &g_bd[pick(NBD)];
        note(215, vfs_bdev_mounted(bd));
        struct vnode *h = some_held();
        note(216, vfs_bdev_mounted_except(bd, h != NULL ? h->mnt : g_root_mount));
        break;
    }
    }
    note_rc(217, rc);
}

static void op_devices(void)
{
    unsigned what = pick(6);
    note(220, what);
    static const char *const fsnames[] = { "tfs", "bare", "zz" };
    const char *fsname = fsnames[pick(3)];
    int rc = 0;
    struct blkdev *bd[2];
    bd[0] = &g_bd[pick(NBD)];
    bd[1] = &g_bd[pick(NBD)];
    switch (what) {
    case 0: rc = vfs_format(fsname, bd[0], pick(2) ? "label" : pick(2) ? "" : NULL); break;
    case 1: rc = vfs_format_mirror(fsname, bd, 1 + pick(2), "pool"); break;
    case 2: {
        static const uint8_t uuid[16] = { 9 }, master[32] = { 4 };
        rc = vfs_format_keyed(fsname, bd, 1 + pick(2), "crypt", uuid, master, NULL);
        break;
    }
    case 3: {
        if (g_detached != NULL) {
            rc = vfs_mount_detached_release(g_detached, pick(2));
            g_detached = NULL;
        } else {
            struct mount *m = NULL;
            struct fs_type *fs = pick(8) == 0 ? NULL : &g_tfs;
            struct blkdev *dev = pick(8) == 0 ? NULL : bd[0];
            rc = vfs_mount_detached(fs, dev, pick(2) ? MOUNT_RDONLY : 0, &m);
            if (rc == 0)
                g_detached = m;
        }
        break;
    }
    case 4: {
        struct mount *m = NULL;
        rc = vfs_mount_internal(pick(4) ? "tfs" : "zz", &m);
        if (rc == 0) {
            note(221, m->flags);
            note_rc(222, vfs_mount_detached_release(m, pick(2)));
        }
        break;
    }
    default: {
        /* A vnode made by hand and dropped before it was hashed: its filesystem never set its operations. */
        struct vnode *root = g_root_mount->root;
        struct vnode *vn = vnode_alloc(root->mnt, 1000 + pick(5));
        if (vn != NULL) {
            if (pick(2)) {
                vn->type = VNODE_REG;
                vn->pc.nr_dirty = pick(3);
            }
            vnode_put(vn);
        }
        struct vnode *c = vnode_lookup_cached(root->mnt, 1 + pick(6));
        note(223, c != NULL);
        if (c != NULL)
            vnode_put(c);
        break;
    }
    }
    note_rc(224, rc);
}

static bool any_type(const struct vnode *vn, void *arg)
{
    note(225, vn->ino);
    return vn->type == *(const enum vnode_type *)arg;
}

static void op_namespaces(void)
{
    unsigned what = pick(5);
    note(230, what);
    unsigned k = 1 + pick(NNS - 1);
    switch (what) {
    case 0: case 1:
        if (g_ns[k] == NULL) {
            struct mount_ns *ns = NULL;
            int rc = mountns_create(pick(2) || g_ns[NNS - k] == NULL ? mountns_initial() : g_ns[NNS - k], &ns);
            note_rc(231, rc);
            if (rc == 0)
                g_ns[k] = ns;
        }
        break;
    case 2:
        if (g_ns[k] != NULL) {
            /* What the test holds on a mount only this namespace sees keeps that mount, as an orphan, until it is
             * let go (vfs_mount_orphaned). */
            if (g_proc.mntns == g_ns[k])
                g_proc.mntns = NULL;
            if (pick(2))
                drop_all();
            mountns_put(g_ns[k]);
            g_ns[k] = NULL;
        }
        break;
    case 3: {
        note(234, vfs_mount_reap_orphans());
        struct vnode *vn = some_held();
        if (vn != NULL) {
            enum vnode_type ty = pick(2) ? VNODE_REG : VNODE_LNK;
            note(232, vnode_cache_any(vn->mnt, any_type, &ty));
        }
        break;
    }
    default: {
        /* Confined: the process's root a held directory, or the global root again. */
        if (g_chroot != NULL) {
            vnode_put(g_chroot);
            g_chroot = NULL;
        } else {
            struct vnode *vn = some_held();
            if (vn != NULL && vn->type == VNODE_DIR) {
                vnode_get(vn);
                g_chroot = vn;
                note(233, vn->ino);
            }
        }
        break;
    }
    }
}

static void op_switch(void)
{
    if (g_old_root != NULL) {
        int rc = vfs_release_old_root(g_old_root);
        note_rc(240, rc);
        if (rc != -EBUSY)
            g_old_root = NULL;
        return;
    }
    if (g_chroot != NULL)
        return;
    struct mount *old = NULL;
    const char *path = g_mpaths[pick(sizeof(g_mpaths) / sizeof(g_mpaths[0]))];
    note_str(241, path, strlen(path));
    struct vnode *start = some_start(NULL), *to = NULL;
    /* Onto a mount without a device: the test releases the last root by hand, and only the VFS can give back a
     * device's claim. */
    if (vfs_lookup(start, path, &to) == 0) {
        bool device = to->mnt->bdev != NULL;
        vnode_put(to);
        if (device)
            return;
    }
    int rc = vfs_switch_root(start, path, &old);
    note_rc(242, rc);
    if (rc == 0)
        g_old_root = old;
}

/* ---- the drain: an unmount waits for the passes, which end meanwhile ---- */

static int g_drain_what = -1;   /* the scripted drain's choice; -1: random */

static void sleep_drain(void)
{
    unsigned what = g_drain_what >= 0 ? (unsigned)g_drain_what : pick(4);
    note(250, what);
    if (what == 0) {
        for (unsigned k = 1; k < NNS; k++)
            if (g_ns[k] == NULL) {
                struct mount_ns *ns = NULL;
                int rc = mountns_create(mountns_current(), &ns);   /* gained during the drain */
                note_rc(251, rc);
                if (rc == 0)
                    g_ns[k] = ns;
                break;
            }
    }
    for (unsigned k = 0; k < NPASS; k++)
        if (g_pass[k] != NULL) {
            vfs_mount_release(g_pass[k]);
            g_pass[k] = NULL;
            if (what != 1)
                break;
        }
}

static void session(unsigned rounds)
{
    for (unsigned r = 0; r < rounds; r++) {
        as_someone();
        g_sleep = sleep_drain;
        g_sleeps = 0;
        unsigned op = pick(30);
        note(260, op);
        switch (op) {
        case 0: case 1: case 2: case 3: case 4: op_lookup(); break;
        case 5: case 6: case 7: case 8: op_open(); break;
        case 9: case 10: case 11: case 12: case 13: op_file(); break;
        case 14: case 15: case 16: case 17: case 18: case 19: op_mutate(); break;
        case 20: case 21: op_query(); break;
        case 22: case 23: op_mount(); break;
        case 24: op_devices(); break;
        case 25: op_namespaces(); break;
        case 26: op_switch(); break;
        case 27: put_held(pick(NHELD)); break;
        case 28: put_file(pick(NFILE)); break;
        default: {
            struct vnode *vn = vfs_root();
            note(261, vn->ino);
            vnode_put(vn);
            break;
        }
        }
        g_sleep = NULL;
        EXPECT(mutexes_held() == 0);
        EXPECT(harness_locks_held() == 0);
    }
}

/* ---- the held-walk seam: a walk and a swapper, interleaved by hand ---- */

static struct vnode *g_seam_old;   /* the swapper's old directory */
static unsigned g_seam_mode;

static void note_record(const struct vfs_cwd_hold_record *r)
{
    note(270, ((uint64_t)r->held << 0) | ((uint64_t)r->held_matches_old << 1) | ((uint64_t)r->released_after_put << 2) |
                  ((uint64_t)r->resumed_dead << 3) | ((uint64_t)r->swapper_was_held << 4) |
                  ((uint64_t)r->walk_timed_out << 5) | ((uint64_t)r->swap_timed_out << 6) |
                  ((uint64_t)r->interrupted << 7));
    note(271, r->ref_at_hold);
    note(272, r->ref_before_put);
    note(273, r->ref_at_release);
    note(274, r->ref_at_resume);
    note_rc(275, r->swap_rc);
    note(276, r->t_hold_ns);
    note(277, r->t_swap_wait_ns);
    note(278, r->t_swap_done_ns);
}

static void sleep_seam(void)
{
    note(280, g_seam_mode);
    note(281, vfs_test_cwd_hold_state());
    switch (g_seam_mode) {
    case 0: {   /* the swapper on another thread: wait, put, release */
        struct thread *walker = g_cpu.current;
        g_cpu.current = &g_thread[1];
        vfs_cwd_hold_swapper_enter();
        vfs_cwd_hold_swap_wait();
        vfs_cwd_hold_before_put(g_seam_old);
        vnode_put(g_seam_old);
        vfs_cwd_hold_after_put(g_seam_old);
        vfs_cwd_hold_swapper_leave();
        g_cpu.current = walker;
        break;
    }
    case 1: g_expired = true; break;
    case 2: g_kill = true; break;
    case 3:   /* the held walk's own thread reaches chdir and gives up */
        vfs_cwd_hold_swapper_enter();
        vfs_cwd_hold_swapper_leave();
        break;
    case 4: {
        struct vfs_cwd_hold_record rec;
        vfs_test_cwd_hold_disarm(&rec);
        note_record(&rec);
        break;
    }
    case 5: {   /* the swapper releases without its put */
        struct thread *walker = g_cpu.current;
        g_cpu.current = &g_thread[1];
        vfs_cwd_hold_swapper_enter();
        vfs_cwd_hold_before_put(NULL);
        vfs_cwd_hold_after_put(NULL);
        g_cpu.current = walker;
        break;
    }
    default: g_expired = pick(2); g_kill = !g_expired; break;
    }
}

static void seam_walk(unsigned mode, struct vnode *start, const char *path)
{
    g_seam_mode = mode;
    g_sleeps = 0;
    g_kill = false;
    g_sleep = sleep_seam;
    vfs_test_cwd_hold_arm("racer");
    note(282, vfs_test_cwd_hold_state());
    struct vnode *vn = NULL;
    int rc = vfs_lookup(start, path, &vn);
    note_vn(283, rc, vn);
    if (rc == 0)
        vnode_put(vn);
    note(287, vfs_test_cwd_hold_state());
    if (mode == 0)
        vfs_cwd_hold_swapper_enter();   /* released already: not the seam's business */
    struct vfs_cwd_hold_record rec;
    memset(&rec, 0, sizeof(rec));
    vfs_test_cwd_hold_disarm(pick(4) ? &rec : NULL);
    note_record(&rec);
    g_sleep = NULL;
    g_kill = false;
}

static void test_seam(void)
{
    as_kernel();
    g_faults = false;
    g_in_process = true;
    g_proc.mntns = NULL;
    g_proc.pid = 42;
    g_proc.nr_threads = 2;
    strlcpy(g_proc.name, "racer", sizeof(g_proc.name));
    g_cpu.current = &g_thread[0];
    struct vnode *dir = NULL;
    EXPECT(vfs_mkdir(NULL, "/seam", 0755) == 0 || true);
    EXPECT(vfs_lookup(NULL, "/seam", &dir) == 0);
    for (unsigned mode = 0; mode < 7; mode++) {
        if (mode == 0) {
            vnode_get(dir);   /* the cwd reference the swapper puts */
            g_seam_old = dir;
        }
        seam_walk(mode, dir, pick(2) ? "x" : ".");
    }
    /* Not this process's, a kernel thread's, and the swapper's own walk: no hold. */
    strlcpy(g_proc.name, "other", sizeof(g_proc.name));
    seam_walk(1, dir, ".");
    strlcpy(g_proc.name, "racer", sizeof(g_proc.name));
    g_in_process = false;
    seam_walk(1, dir, ".");
    g_in_process = true;

    /* The swapper first: it waits for a hold that does not come, and is timed out or killed. */
    for (unsigned mode = 1; mode < 3; mode++) {
        g_seam_mode = mode;
        g_sleeps = 0;
        g_sleep = sleep_seam;
        g_kill = false;
        vfs_test_cwd_hold_arm("racer");
        g_cpu.current = &g_thread[1];
        vfs_cwd_hold_swapper_enter();
        vfs_cwd_hold_swap_wait();
        note(288, vfs_test_cwd_hold_state());
        vfs_cwd_hold_before_put(dir);
        vfs_cwd_hold_after_put(dir);
        /* The swapper's own relative walk is not held. */
        struct vnode *vn = NULL;
        int rc = vfs_lookup(dir, ".", &vn);
        note_vn(289, rc, vn);
        if (rc == 0)
            vnode_put(vn);
        vfs_cwd_hold_swapper_leave();
        vfs_cwd_hold_swapper_leave();   /* again: not registered */
        g_cpu.current = &g_thread[0];
        struct vfs_cwd_hold_record rec;
        vfs_test_cwd_hold_disarm(&rec);
        note_record(&rec);
        g_sleep = NULL;
        g_kill = false;
    }
    /* One thread: chdir has no walk to race. Disarmed: nothing at all. */
    g_proc.nr_threads = 1;
    vfs_test_cwd_hold_arm("racer");
    vfs_cwd_hold_swapper_enter();
    vfs_cwd_hold_swap_wait();
    vfs_cwd_hold_swapper_leave();
    vfs_test_cwd_hold_disarm(NULL);
    vfs_cwd_hold_swapper_enter();
    vfs_cwd_hold_swap_wait();
    vfs_cwd_hold_before_put(dir);
    vfs_cwd_hold_after_put(dir);
    vfs_cwd_hold_swapper_leave();
    g_proc.nr_threads = 2;

    /* A walk resumed on what is not a directory is the use-after-free the seam exists to catch. */
    struct file *f = NULL;
    EXPECT(vfs_open(NULL, "/seam/file", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    struct vnode *reg = f->vn;
    vnode_get(reg);
    file_put(f);
    g_seam_mode = 1;
    g_sleeps = 0;
    g_sleep = sleep_seam;
    vfs_test_cwd_hold_arm("racer");
    struct vnode *vn = NULL;
    EXPECT_PANIC(vfs_lookup(reg, "x", &vn));
    unlock_all();
    note_str(290, harness_last_panic, strlen(harness_last_panic));
    vfs_test_cwd_hold_disarm(NULL);
    g_sleep = NULL;
    vnode_put(reg);
    vnode_put(dir);
    g_in_process = false;
}

/* ---- scripted: what random histories reach too rarely ---- */

static int g_hook_left;

static void replace_entry(struct tree *t, unsigned e)
{
    if (g_hook_left == 0 || t->n[t->e[e].node].type != VNODE_REG)
        return;
    g_hook_left--;
    int i = new_node(t, VNODE_REG, 0644);
    EXPECT(i > 0);
    t->n[i].pin = false;
    t->n[i].parent = t->e[e].dir;
    t->n[t->e[e].node].nlink--;
    if (t->n[t->e[e].node].nlink == 0)
        t->n[t->e[e].node].used = false;
    t->e[e].node = (unsigned)i;
    note(300, (unsigned)i);
}

static struct mount *g_switch_extra, *g_moving;

/* Between switch_root's passes: an unmount of a mount that was to move begins. */
static void unmount_during_lookup(struct tree *t, unsigned e)
{
    (void)t;
    (void)e;
    g_lookup_hook = NULL;
    g_moving->unmounting = true;
    note(301, g_moving->id);
}

static void test_scripted(void)
{
    as_kernel();
    g_faults = false;
    g_in_process = false;

    /* Registration: nameless, incomplete, twice. */
    struct fs_type nameless = { .mount = t_mount, .unmount = t_unmount };
    struct fs_type halfway = { .name = "half", .mount = t_mount };
    note_rc(310, vfs_register_fs(&nameless));
    note_rc(311, vfs_register_fs(&halfway));
    note_rc(312, vfs_register_fs(&g_tfs));
    note(313, vfs_find_fs("half") == NULL);
    note_rc(314, vfs_mount_root("tfs", NULL, 0));

    /* A rename whose victim is replaced between its two looks: once, and every time. */
    EXPECT(vfs_mkdir(NULL, "/r", 0755) == 0);
    EXPECT(vfs_mkdir(NULL, "/r/d", 0755) == 0);
    for (int left = 1; left <= 40; left += 39) {
        struct file *f = NULL;
        EXPECT(vfs_open(NULL, "/r/v", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
        file_put(f);
        g_hook_left = left;
        g_lookup_hook = replace_entry;
        note_rc(315, vfs_rename(NULL, "/r/v", "/r/d/w"));
        g_lookup_hook = NULL;
        note_rc(316, vfs_unlink(NULL, "/r/v"));
        note_rc(316, vfs_unlink(NULL, "/r/d/w"));
    }
    /* A directory under itself, the two parents in both orders, and a directory onto its own parent. */
    EXPECT(vfs_mkdir(NULL, "/r/d/e", 0755) == 0);
    note_rc(317, vfs_rename(NULL, "/r/d", "/r/d/e/z"));
    note_rc(317, vfs_rename(NULL, "/r/d/e", "/r/e"));
    note_rc(317, vfs_rename(NULL, "/r/e", "/r/d/e"));
    note_rc(317, vfs_rename(NULL, "/r/d/e", "/r/d"));
    note_rc(317, vfs_rename(NULL, "/r/d/e", "/r"));
    EXPECT(vfs_mkdir(NULL, "/q", 0755) == 0);
    note_rc(317, vfs_rename(NULL, "/r/d/e", "/q/e"));
    note_rc(317, vfs_rename(NULL, "/q/e", "/r/d/e"));

    /* Mounts: on the root, twice on one directory, a busy device, a relative target. */
    EXPECT(vfs_mkdir(NULL, "/m", 0755) == 0);
    EXPECT(vfs_mkdir(NULL, "/n", 0755) == 0);
    EXPECT(vfs_mkdir(NULL, "/s", 0755) == 0);
    note_rc(320, vfs_mount("/", "tfs", NULL, 0));
    note_rc(321, vfs_mount("/m", "tfs", &g_bd[0], 0));
    note_rc(322, vfs_mount("/m", "tfs", &g_bd[3], 0));
    note_rc(323, vfs_mount("/n", "tfs", &g_bd[1], 0));
    note_rc(324, vfs_mount("s", "tfs", &g_bd[3], 0));
    note_rc(324, vfs_umount2("/s", 0));
    note_rc(325, vfs_mount("/r/v", "tfs", NULL, 0));
    note_rc(326, vfs_umount2("/r", 0));
    note_rc(327, vfs_umount2("/", 0));

    /* Switching the root: /m, a top-level mount, needs a free /m in the new root; one is made. */
    note_rc(330, vfs_switch_root(NULL, "/n", &g_switch_extra));
    note_rc(331, vfs_switch_root(NULL, "/", &g_switch_extra));
    EXPECT(vfs_mount("/n", "tfs", NULL, 0) == 0);
    note_rc(332, vfs_switch_root(NULL, "/m", &g_switch_extra));   /* /m's root has no /n */
    note_rc(333, vfs_switch_root(NULL, "/n", &g_switch_extra));   /* /n's root has no /m */
    EXPECT(vfs_mkdir(NULL, "/n/m", 0755) == 0);
    EXPECT(vfs_mkdir(NULL, "/m/s", 0755) == 0);
    EXPECT(vfs_mount("/m/s", "tfs", NULL, 0) == 0);   /* nested: moves with /m */
    struct vnode *mv = NULL;
    EXPECT(vfs_lookup(NULL, "/m", &mv) == 0);
    g_moving = mv->mnt;
    vnode_put(mv);
    g_lookup_hook = unmount_during_lookup;
    note_rc(334, vfs_switch_root(NULL, "/n", &g_switch_extra));   /* the table changed between the passes */
    g_lookup_hook = NULL;
    g_moving->unmounting = false;
    struct mount_ns *ns = NULL;
    EXPECT(mountns_create(mountns_initial(), &ns) == 0);
    note_rc(336, vfs_switch_root(NULL, "/n", &g_switch_extra));   /* /m is seen twice */
    mountns_put(ns);
    struct vnode *keep_old = NULL;
    EXPECT(vfs_lookup(NULL, "/r", &keep_old) == 0);
    struct mount *old = NULL;
    note_rc(337, vfs_switch_root(NULL, "/n", &old));
    EXPECT(old != NULL);
    note_rc(338, vfs_release_old_root(old));   /* /r is held */
    vnode_put(keep_old);
    note_rc(339, vfs_release_old_root(old));
    note(340, vfs_mount_count());

    /* An unmount that waits for a pass, and one whose namespace gains a view meanwhile. */
    struct mount *m = NULL;
    struct vnode *ms = NULL;
    for (int what = 2; what >= 0; what -= 2) {   /* the passes end; or a namespace is made first, then they end */
        note_rc(340, vfs_mount("/m/s", "tfs", NULL, 0));
        EXPECT(vfs_lookup(NULL, "/m/s", &ms) == 0);
        uint64_t sid = ms->mnt->id;
        vnode_put(ms);
        EXPECT(vfs_mount_acquire(sid, &m) == 0);
        g_pass[0] = m;
        EXPECT(vfs_mount_acquire(sid, &m) == 0);
        g_pass[1] = m;
        g_sleep = sleep_drain;
        g_sleeps = 0;
        g_drain_what = what;
        note_rc(341, vfs_umount2("/m/s", 0));
        g_drain_what = -1;
        g_sleep = NULL;
        EXPECT(g_pass[0] == NULL && g_pass[1] == NULL);
    }
    note_rc(341, vfs_umount2("/m/s", 0));   /* the namespace made meanwhile still sees it */
    EXPECT(vfs_lookup(NULL, "/m", &ms) == 0);
    uint64_t id = ms->mnt->id;
    vnode_put(ms);
    for (unsigned k = 1; k < NNS; k++)
        if (g_ns[k] != NULL) {
            mountns_put(g_ns[k]);
            g_ns[k] = NULL;
        }
    /* A pass on an unmounting mount, and the release with no pass to end. */
    struct mount *mm = NULL;
    EXPECT(vfs_mount_acquire(id, &mm) == 0);
    mm->unmounting = true;
    note_rc(342, vfs_mount_acquire(id, &m));
    mm->unmounting = false;
    vfs_mount_release(mm);
    kobject_get(&mm->obj);
    EXPECT_PANIC(vfs_mount_release(mm));
    unlock_all();
    kobject_put(&mm->obj);
    vfs_mount_release(NULL);
    note_rc(343, vfs_umount2("/m/s", VFS_UMOUNT_FORCE));
    note_rc(344, vfs_umount2("/m", 0));
}

/* ---- edges: one case each ---- */

static struct mount *g_hook_mnt;
static int g_hook_kind;

/* Between switch_root's passes, in the new root's lookups: a mount that was to move starts unmounting, or the
 * new root leaves the old one. */
static void switch_hook(struct tree *t, unsigned e)
{
    (void)e;
    if (t->mnt != g_hook_mnt)
        return;
    g_lookup_hook = NULL;
    if (g_hook_kind == 0)
        g_moving->unmounting = true;
    else
        g_hook_mnt->parent = NULL;
    note(302, (unsigned)g_hook_kind);
}

static struct vnode *must_lookup(const char *path)
{
    struct vnode *vn = NULL;
    EXPECT(vfs_lookup(NULL, path, &vn) == 0);
    return vn;
}

static void teardown(void);

static void make_file(const char *path, size_t bytes)
{
    struct file *f = NULL;
    EXPECT(vfs_open(NULL, path, COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    static const uint8_t data[6000] = { 5 };
    if (bytes)
        EXPECT(file_write(f, data, bytes) == (int64_t)bytes);
    file_put(f);
}

static void own(const char *path, uint32_t uid)
{
    struct vnode *vn = must_lookup(path);
    vn->uid = uid;
    vnode_put(vn);
}

static void test_edges(void)
{
    as_kernel();
    g_faults = false;
    EXPECT(g_root_mount == NULL);

    /* No root yet: an internal mount's absolute link names nothing. */
    struct mount *im = NULL;
    EXPECT(vfs_mount_internal("tfs", &im) == 0);
    note_rc(370, vfs_symlink(im->root, "l", "/x"));
    struct vnode *vn = NULL;
    note_rc(371, vfs_lookup(im->root, "l/y", &vn));
    note_rc(372, vfs_mount_detached_release(im, false));
    note_rc(373, vfs_register_fs(&g_failfs));
    note_rc(373, vfs_mount_root("failfs", NULL, 0));
    struct fs_type dup = { .name = "tfs", .mount = t_mount, .unmount = t_unmount };
    note_rc(374, vfs_register_fs(&dup));

    EXPECT(vfs_mount_root("tfs", NULL, 0) == 0);
    static const char *const dirs[] = { "/x", "/u", "/u/dd", "/u/ee", "/dd", "/n", "/m", "/k" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        EXPECT(vfs_mkdir(NULL, dirs[i], i == 0 ? 01777 : 0755) == 0);
    make_file("/f", 5000);
    make_file("/x/f", 10);
    own("/x", 1000);
    own("/u", 1000);
    own("/u/ee", 1000);

    /* Forty-one components, and a link target as long as a path. */
    char p[200];
    size_t at = 0;
    for (unsigned i = 0; i <= VFS_MAX_COMPONENTS; i++) {
        p[at++] = '.';
        p[at++] = '/';
    }
    p[at++] = 'f';
    p[at] = '\0';
    note_rc(375, vfs_lookup(NULL, p, &vn));
    static char longest[VFS_PATH_MAX + 1];
    memset(longest, 'a', VFS_PATH_MAX);
    note_rc(376, vfs_symlink(NULL, "/lt", longest));

    /* Under a directory removed while held. */
    struct vnode *dd = must_lookup("/dd");
    EXPECT(vfs_rmdir(NULL, "/dd") == 0);
    note_rc(377, vfs_mkdir(dd, "x", 0755));
    note_rc(377, vfs_unlink(dd, "x"));
    struct vnode *nod = NULL;
    note_rc(377, vfs_mknod(dd, "x", 0644, VNODE_FIFO, &nod));
    note_rc(377, vfs_rename2(NULL, "/f", dd, "y"));
    note_rc(377, vfs_symlink(dd, "x", "t"));
    note_rc(377, vfs_link(NULL, "/f", dd, "x", true));
    vnode_put(dd);

    /* Links across mounts and past the limit. */
    EXPECT(vfs_mount("/m", "tfs", NULL, 0) == 0);
    note_rc(378, vfs_link(NULL, "/f", NULL, "/m/f", true));
    struct vnode *f = must_lookup("/f");
    uint32_t was = f->nlink;
    f->nlink = VFS_LINK_MAX;
    note_rc(379, vfs_link(NULL, "/f", NULL, "/f2", true));
    f->nlink = was;
    vnode_put(f);

    /* A set-group-id program its group may run loses the bit to a chown. */
    make_file("/g", 0);
    struct vnode *g = must_lookup("/g");
    note_rc(380, vfs_setattr(g, VFS_SET_MODE, 02710, 0));
    note_rc(380, vfs_chown(g, VFS_ID_KEEP, 50));
    note(380, g->mode);
    vnode_put(g);

    /* A walk into a mount being unmounted, and a second unmount of it. */
    struct vnode *mv = must_lookup("/m");
    mv->mnt->unmounting = true;
    note_rc(381, vfs_lookup(NULL, "/m/x", &vn));
    note_rc(381, vfs_umount2("/m", 0));
    note_rc(381, vfs_umount_at(mv, ".", 0));   /* "." from inside reaches it */
    mv->mnt->unmounting = false;
    struct mount *mm = mv->mnt;
    vnode_put(mv);
    g_sync_fails = true;
    note_rc(381, vfs_umount_at(mm->root, ".", 0));   /* the commit fails: the mount stays */
    g_sync_fails = false;

    /* The last namespace to see a mount goes, and its commit fails. */
    struct mount_ns *ns = NULL;
    EXPECT(mountns_create(mountns_initial(), &ns) == 0);
    g_in_process = true;
    g_proc.mntns = ns;
    EXPECT(vfs_mount("/k", "tfs", NULL, 0) == 0);
    g_in_process = false;
    g_proc.mntns = NULL;
    g_sync_fails = true;
    mountns_put(ns);
    g_sync_fails = false;

    /* A confined walk whose climb to its root cannot read a "..". */
    struct vnode *ee = must_lookup("/u/ee");
    g_chroot = must_lookup("/u");
    g_dotdot_fails = true;
    note_rc(381, vfs_lookup(ee, "x", &vn));
    g_dotdot_fails = false;
    vnode_put(ee);
    vnode_put(g_chroot);
    g_chroot = NULL;

    /* A filesystem without links. */
    EXPECT(vfs_mount("/k", "bare", NULL, 0) == 0);
    note_rc(381, vfs_link(NULL, "/k/f", NULL, "/k/g", true));
    note_rc(381, vfs_umount2("/k", 0));

    /* The root's pass, a device's claimer asked about itself, a budget's statfs. */
    struct mount *pm = NULL;
    note_rc(382, vfs_mount_acquire(g_root_mount->id, &pm));
    if (pm != NULL)
        vfs_mount_release(pm);
    EXPECT(vfs_mount("/k", "tfs", &g_bd[3], MOUNT_CACHE_IS_STORE) == 0);
    struct vnode *kv = must_lookup("/k");
    note(383, vfs_bdev_mounted_except(&g_bd[3], kv->mnt));
    note(383, vfs_bdev_mounted_except(&g_bd[3], NULL));
    struct vfs_statfs sf;
    for (unsigned used = 4; used <= 20; used += 16) {
        kv->mnt->cache_limit_pages = 10;
        kv->mnt->cache_pages = used;
        note_rc(384, vnode_statfs(kv, &sf));
        note(384, sf.blocks);
        note(384, sf.bfree);
    }
    kv->mnt->cache_pages = 0;
    vnode_put(kv);
    note_rc(384, vfs_umount2("/k", 0));

    /* A sticky directory and a directory moved, as users. */
    set_cred(3);   /* 1001 */
    make_file("/x/h", 0);
    note_rc(385, vfs_rename(NULL, "/x/f", "/x/i"));   /* not the entry's owner, nor the directory's */
    note_rc(385, vfs_rename(NULL, "/x/h", "/x/f"));   /* the entry replaced is not the caller's */
    set_cred(2);   /* 1000 */
    note_rc(385, vfs_rename(NULL, "/u/dd", "/u/ee/dd"));   /* its ".." is not the caller's to rewrite */
    set_cred(3);
    note_rc(385, vfs_unlink(NULL, "/x/f"));
    as_kernel();
    g_dotdot_fails = true;
    note_rc(385, vfs_rename(NULL, "/u/dd", "/u/ee/dd"));   /* the ancestry cannot be read */

    /* A start's name too long for the buffer; a listing whose first entry does not fit. */
    char nm[4];
    struct vnode *u = must_lookup("/u");
    note_rc(385, vfs_lookup_named(u, "/abcdefgh", "ee", &vn, nm, sizeof(nm)));
    vnode_put(u);
    struct file *df = NULL;
    EXPECT(vfs_open(NULL, "/", COSMO_O_RDONLY, 0, &df) == 0);
    uint8_t small[20];
    note_rc(385, file_readdir(df, small, sizeof(struct cosmo_dirent) + 3));
    file_put(df);

    /* One file told of another's write-back failure; a flush with pages to write. */
    struct file *f1 = NULL, *f2 = NULL;
    EXPECT(vfs_open(NULL, "/f", COSMO_O_RDWR, 0, &f1) == 0);
    EXPECT(vfs_open(NULL, "/f", COSMO_O_RDWR, 0, &f2) == 0);
    f1->vn->pc.wb_seq++;
    note_rc(386, file_sync(f2));
    static const uint8_t data[100] = { 9 };
    note_rc(386, file_write(f2, data, sizeof(data)));
    note_rc(386, file_flush(f2));
    const struct kobject_io_type *io = (const struct kobject_io_type *)f1->obj.type;
    static uint8_t buf[8000];
    for (unsigned i = 0; i < 6; i++) {
        note_rc(386, file_seek(f1, 0, COSMO_SEEK_SET));
        note_rc(386, io->read_filled(&f1->obj, buf, i < 3 ? 1000 : 4000, i < 3 ? 5000 : 6000, put_piece, NULL));
    }
    file_put(f1);
    file_put(f2);

    /* A directory opened for writing through its vnode; a file on a read-only mount. */
    struct file *f3 = NULL;
    EXPECT(vfs_open_vnode(must_lookup("/u"), COSMO_O_RDWR, &f3) == 0);
    note_rc(387, file_pwrite(f3, data, 10, 0));
    file_put(f3);
    EXPECT(vfs_mount("/k", "tfs", NULL, MOUNT_RDONLY) == 0);
    EXPECT(vfs_open_vnode(must_lookup("/k/c"), COSMO_O_RDWR, &f3) == 0);
    note_rc(387, file_pwrite(f3, data, 10, 0));
    file_put(f3);
    g_open_fails = true;
    note_rc(387, vfs_open_vnode(must_lookup("/k/c"), COSMO_O_RDWR, &f3));
    note_rc(387, vfs_link(NULL, "/k/c", NULL, "/k/d", true));
    note_rc(387, vfs_umount2("/k", 0));

    /* Switching the root: a mount nested in the new root comes along; /m in the new root is a file, then a
     * directory; the passes see a change; then the switch, a second one before the old root is released, and
     * ".." at the old root. */
    EXPECT(vfs_mount("/n", "tfs", NULL, 0) == 0);
    EXPECT(vfs_mkdir(NULL, "/n/k", 0755) == 0);
    EXPECT(vfs_mount("/n/k", "tfs", NULL, 0) == 0);
    make_file("/n/m", 0);
    struct mount *old = NULL;
    note_rc(388, vfs_switch_root(NULL, "/n", &old));
    EXPECT(vfs_unlink(NULL, "/n/m") == 0);
    EXPECT(vfs_mkdir(NULL, "/n/m", 0755) == 0);
    mv = must_lookup("/m");
    g_moving = mv->mnt;
    vnode_put(mv);
    struct vnode *nv = must_lookup("/n");
    g_hook_mnt = nv->mnt;
    vnode_put(nv);
    for (g_hook_kind = 0; g_hook_kind < 2; g_hook_kind++) {
        struct mount *parent = g_hook_mnt->parent;
        g_lookup_hook = switch_hook;
        note_rc(389, vfs_switch_root(NULL, "/n", &old));
        g_lookup_hook = NULL;
        g_moving->unmounting = false;
        g_hook_mnt->parent = parent;
    }
    struct vnode *rroot = must_lookup("/");
    note_rc(390, vfs_switch_root(NULL, "/n", &old));
    note_rc(391, vfs_switch_root(NULL, "/m", &g_switch_extra));
    int rc = vfs_lookup(rroot, "..", &vn);
    note_vn(392, rc, vn);
    vnode_put(vn);
    note_rc(393, vfs_umount_at(rroot, ".", 0));   /* the old root is on no directory: not a namespace's to unmount */
    note_rc(393, vfs_umount_at(rroot, ".", VFS_UMOUNT_FORCE));
    vnode_put(rroot);
    note_rc(396, vfs_release_old_root(old));
    teardown();
}

/* ---- orphans: mounts a dying namespace leaves to what still uses them ---- */

static void test_orphans(void)
{
    as_kernel();
    g_faults = false;
    EXPECT(vfs_mount_root("tfs", NULL, 0) == 0);
    EXPECT(vfs_mkdir(NULL, "/o", 0755) == 0);
    EXPECT(vfs_mkdir(NULL, "/p", 0755) == 0);

    /* A file open on a mount only the namespace saw: the mount waits for it, and the file works meanwhile. */
    struct mount_ns *ns = NULL;
    EXPECT(mountns_create(mountns_initial(), &ns) == 0);
    g_in_process = true;
    g_proc.mntns = ns;
    EXPECT(vfs_mount("/p", "tfs", &g_bd[3], 0) == 0);
    struct file *f = NULL;
    EXPECT(vfs_open(NULL, "/p/f", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    g_in_process = false;
    g_proc.mntns = NULL;
    mountns_put(ns);
    note(400, g_threads);
    note(401, vfs_bdev_mounted(&g_bd[3]));   /* still the orphan's */
    static const uint8_t data[50] = { 3 };
    note_rc(402, file_write(f, data, sizeof(data)));
    note_rc(402, file_sync(f));
    note(403, vfs_mount_reap_orphans());   /* nothing yet: the file is open */
    file_put(f);
    note(403, vfs_mount_reap_orphans());   /* the file was its last user */
    note(401, vfs_bdev_mounted(&g_bd[3]));
    EXPECT(list_empty(vfs_floor_orphans()));

    /* A mount nested on one only another namespace sees: when that namespace goes, the outer mount waits for the
     * nested one, whose mountpoint is one of its directories. */
    EXPECT(vfs_mount("/o", "tfs", NULL, 0) == 0);
    EXPECT(vfs_mkdir(NULL, "/o/d", 0755) == 0);
    EXPECT(mountns_create(mountns_initial(), &ns) == 0);   /* sees /o */
    EXPECT(vfs_mount("/o/d", "tfs", NULL, 0) == 0);        /* the initial namespace's alone */
    struct vnode *inner = must_lookup("/o/d");
    note_rc(404, vfs_umount2("/o", 0));   /* the initial namespace forgets /o; the other still sees it */
    mountns_put(ns);                       /* /o is an orphan, and /o/d is on it */
    note(405, vfs_mount_reap_orphans());
    EXPECT(!list_empty(vfs_floor_orphans()));
    struct vnode *up = NULL;
    int urc = vfs_lookup(inner, "..", &up);   /* out of the nested mount, into the orphan */
    note_vn(406, urc, up);
    if (up != NULL) {
        note_rc(407, vfs_umount_at(up, ".", 0));      /* an orphan is no namespace's to unmount */
        vnode_put(up);
    }
    struct vnode *root = inner->mnt->root;
    vnode_put(inner);
    note_rc(408, vfs_umount_at(root, ".", 0));   /* the nested mount, from inside it */
    note(405, vfs_mount_reap_orphans());          /* its mountpoint was the orphan's last use */
    EXPECT(list_empty(vfs_floor_orphans()));
    teardown();
}

/* ---- teardown: everything the test holds, then every mount, then the root by hand ---- */

static void teardown(void)
{
    as_kernel();
    g_faults = false;
    drop_all();
    g_in_process = true;   /* each namespace unmounts what it sees, then goes */
    for (unsigned round = 0; round < 8 && vfs_mount_count() > 1; round++)
        for (unsigned k = 0; k < NNS; k++) {
            if (k > 0 && g_ns[k] == NULL)
                continue;
            g_proc.mntns = k == 0 ? NULL : g_ns[k];
            struct mount *mnt;
            bool again = true;
            while (again) {
                again = false;
                list_for_each_entry(mnt, &g_mounts, link) {
                    if (mnt == g_root_mount || mnt->mountpoint == NULL)
                        continue;
                    int rc = vfs_umount_at(mnt->root, ".", VFS_UMOUNT_FORCE);
                    note_rc(354, rc);
                    if (rc == 0 || rc == -EIO) {
                        again = true;
                        break;
                    }
                }
            }
        }
    g_in_process = false;
    g_proc.mntns = NULL;
    for (unsigned k = 1; k < NNS; k++)
        if (g_ns[k] != NULL) {
            mountns_put(g_ns[k]);
            g_ns[k] = NULL;
        }
    note(355, vfs_mount_reap_orphans());
    EXPECT(list_empty(vfs_floor_orphans()));
    if (g_detached != NULL) {
        note_rc(350, vfs_mount_detached_release(g_detached, false));
        g_detached = NULL;
    }
    if (g_old_root != NULL) {
        note_rc(351, vfs_release_old_root(g_old_root));
        g_old_root = NULL;
    }
    for (unsigned round = 0; round < 8 && vfs_mount_count() > 1; round++) {
        struct mount *mnt;
        bool again = true;
        while (again) {
            again = false;
            list_for_each_entry(mnt, &g_mounts, link) {
                if (mnt == g_root_mount || mnt->mountpoint == NULL)
                    continue;
                /* From inside the mount, "." is its root; the mount's own reference is the one the walk borrows. */
                int rc = vfs_umount_at(mnt->root, ".", VFS_UMOUNT_FORCE);
                note_rc(352, rc);
                if (rc == 0 || rc == -EIO) {
                    again = true;
                    break;
                }
            }
        }
    }
    if (vfs_mount_count() != 1) {
        struct mount *mnt;
        list_for_each_entry(mnt, &g_mounts, link) {
            unsigned refs = 0;
            struct mount_ns_ref *ref;
            list_for_each_entry(ref, &mnt->ns_refs, mnt_link)
                refs++;
            printf("  left: mount %llu (%s) root %d on %llu, %u refs, unmounting %d, parent %llu\n",
                   (unsigned long long)mnt->id, mnt->fs->name, mnt == g_root_mount,
                   (unsigned long long)(mnt->mountpoint ? mnt->mountpoint->ino : 0), refs, mnt->unmounting,
                   (unsigned long long)(mnt->parent ? mnt->parent->id : 0));
            for (unsigned b = 0; b < VNODE_HASH; b++) {
                struct vnode *vn;
                list_for_each_entry(vn, &mnt->vnodes[b], hash_link)
                    printf("    vnode %llu type %u refs %u flags %x\n", (unsigned long long)vn->ino, vn->type,
                           vn->obj.refcount, vn->flags);
            }
        }
    }
    EXPECT(vfs_mount_count() == 1);
    struct mount *r = g_root_mount;
    list_remove(&r->link);
    note_rc(353, r->fs->unmount(r));
    struct vnode *root = r->root;
    r->root = NULL;
    vnode_put(root);
    g_root_mount = NULL;
    kobject_put(&r->obj);
}

static void check_clean(void)
{
    EXPECT(mutexes_held() == 0);
    EXPECT(g_live == 0);
    EXPECT(g_objects == 0);
    for (unsigned i = 0; i < NBD; i++)
        EXPECT(g_bd[i].obj.refcount == 1);
    for (unsigned k = 0; k < NTREE; k++)
        for (unsigned i = 0; i < NNODE; i++)
            EXPECT(!g_tree[k].n[i].cached);
}

/* A fresh root with something in it: directories, files, links, a sticky directory and two mounts. */
static void start_epoch(void)
{
    as_kernel();
    g_faults = false;
    EXPECT(g_root_mount == NULL);
    EXPECT(vfs_mount_root("tfs", NULL, 0) == 0);
    static const char *const dirs[] = { "/a", "/b", "/d", "/m", "/n", "/s", "/a/b", "/a/m" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        EXPECT(vfs_mkdir(NULL, dirs[i], 0755) == 0);
    EXPECT(vfs_mkdir(NULL, "/x", 01777) == 0);
    static const char *const files[] = { "/f", "/a/f", "/b/f", "/x/f" };
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        struct file *f = NULL;
        EXPECT(vfs_open(NULL, files[i], COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
        file_put(f);
    }
    EXPECT(vfs_symlink(NULL, "/l", "a") == 0);
    EXPECT(vfs_symlink(NULL, "/l2", "/a/f") == 0);
    EXPECT(vfs_symlink(NULL, "/a/l", "../b") == 0);
    EXPECT(vfs_symlink(NULL, "/a/p", "/a/p") == 0);
    struct vnode *x = NULL;
    EXPECT(vfs_lookup(NULL, "/x", &x) == 0);
    x->uid = 1000;   /* /tmp-like, owned by a user */
    vnode_put(x);
    EXPECT(vfs_mount("/m", "tfs", &g_bd[pick(NBD)], 0) == 0);
    EXPECT(vfs_mount("/s", "bare", NULL, pick(2) ? MOUNT_RDONLY : 0) == 0);
    EXPECT(vfs_mkdir(NULL, "/m/d", 0700) == 0);
}

static void setup_devices(void)
{
    for (unsigned i = 0; i < NBD; i++) {
        g_bd[i].obj.type = &g_bd_type;
        g_bd[i].obj.refcount = 1;
        g_bd[i].name[0] = (char)('p' + i);
    }
}

/* Before the VFS exists, and twice. Its own function: a longjmp does not cross the sessions' loops. */
static void test_init(void)
{
    g_faults = false;
    EXPECT_PANIC(vfs_mount("/m", "tfs", NULL, 0));
    unlock_all();
    vfs_init();
    EXPECT_PANIC(vfs_init());
    unlock_all();
    EXPECT_PANIC(vfs_root());
    unlock_all();
}

static void test_vfs_sessions(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = klog_hook;
    harness_klog_min = 99;
    setup_devices();
    g_cpu.self = &g_cpu;
    g_cpu.current = &g_thread[0];
    as_kernel();

    test_init();
    note_rc(360, vfs_register_fs(&g_bare));
    {
        struct vnode *vn = NULL;
        note_rc(361, vfs_lookup(NULL, "/a", &vn));   /* no root: nothing is absolute */
        note_rc(362, vfs_mount_root("zz", NULL, 0));
    }
    test_scripted();
    test_seam();
    note_rc(363, vfs_mount_root("tfs", NULL, 0));   /* once */
    teardown();
    check_clean();
    test_edges();
    check_clean();
    test_orphans();
    check_clean();

    for (unsigned e = 0; e < EPOCHS; e++) {
        start_epoch();
        g_faults = true;
        session(ROUNDS);
        teardown();
        check_clean();
    }
    printf("  vfs digest 0x%016llx\n", (unsigned long long)g_digest);
    EXPECT(g_digest == VFS_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "vfs_sessions", test_vfs_sessions },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
