/*
 * procfs.c - /proc: facts about processes, addressable as files
 * (docs/kernel-services/filesystem/procfs/design.md, invariants P1-P3).
 *
 * The rule that keeps this from becoming the dumping ground the
 * constitution's section 56 warns about is in the name: a file belongs
 * here only if it describes a process. System-wide values stay in
 * sysctl, whose names are a curated list; the log stays in dmesg;
 * device nodes stay in /dev.
 *
 * What this adds that procinfo does not is addressability -- a
 * process's facts at a path, readable by anything that reads files,
 * rather than a struct returned to a caller that knows the ABI.
 */

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>

/*
 * The inode number carries what the vnode is: a pid and which of its
 * files. Nothing else is stored, so a vnode needs no private state and
 * the filesystem holds no table that could disagree with the process
 * table.
 */
enum proc_kind { PROC_ROOT = 0, PROC_PID_DIR = 1, PROC_STATUS = 2, PROC_LIMITS = 3 };

#define PROC_INO(pid, kind) (((uint64_t)(pid) << 8) | (uint64_t)(kind))
#define PROC_INO_PID(ino)   ((pid_t)((ino) >> 8))
#define PROC_INO_KIND(ino)  ((enum proc_kind)((ino) & 0xff))
#define PROC_ROOT_INO       PROC_INO(0, PROC_ROOT)

/* Rendered text is at most this; a status block is a few hundred bytes
 * and a limits block is one line per resource. */
#define PROC_TEXT_MAX 1024

static const struct vnode_ops proc_root_ops;
static const struct vnode_ops proc_pid_dir_ops;
static const struct vnode_ops proc_file_ops;

/*
 * A pid the caller may see, referenced, or NULL. The rule itself lives
 * in the process layer and is the one procinfo applies, so /proc cannot
 * show or name what `ps` would not (P1).
 */
static struct process *proc_get_visible(pid_t pid)
{
    struct process *p = process_lookup(pid);
    if (p == NULL)
        return NULL;
    if (!process_visible_to_current(p)) {
        process_put(p);
        return NULL;
    }
    return p;
}

static struct vnode *proc_vnode(struct mount *mnt, uint64_t ino, enum vnode_type type, uint32_t mode,
                                const struct vnode_ops *ops)
{
    /* Deliberately not hashed: every lookup makes a new vnode, so a
     * file's text is rendered afresh for each open rather than served
     * from the page cache of a vnode somebody opened earlier. */
    struct vnode *vn = vnode_alloc(mnt, ino);
    if (vn == NULL)
        return NULL;
    vn->type = type;
    vn->mode = mode;
    vn->uid = 0;
    vn->gid = 0;
    vn->ops = ops;
    return vn;
}

/* --- rendering -------------------------------------------------------------
 *
 * Both files are produced from the process here, into the caller's
 * buffer, and nothing is kept between calls (P3).
 */

static const char *const g_states[] = { "running", "exiting", "exited" };

static int render_status(struct process *p, char *out, size_t n)
{
    unsigned nr_threads;
    uint64_t run_ns = 0;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    nr_threads = p->nr_threads;
    struct thread *t;
    list_for_each_entry(t, &p->threads, proc_link)
        run_ns += t->run_time_ns;
    spin_unlock_irqrestore(&p->lock, s);

    unsigned st = (unsigned)p->state;
    return ksnprintf(out, n,
                     "name: %s\n"
                     "pid: %u\n"
                     "ppid: %u\n"
                     "state: %s\n"
                     "uid: %u\n"
                     "gid: %u\n"
                     "domain: %u\n"
                     "threads: %u\n"
                     "syscalls: %llu\n"
                     "cpu_ns: %llu\n",
                     p->name, p->pid, p->parent_pid,
                     st < sizeof(g_states) / sizeof(g_states[0]) ? g_states[st] : "?", p->cred.euid,
                     p->cred.egid, p->domain, nr_threads, (unsigned long long)p->syscalls,
                     (unsigned long long)run_ns);
}

static int render_limits(struct process *p, char *out, size_t n)
{
    static const char *const names[COSMO_RLIMIT_COUNT] = { "as", "mem", "nofile", "nproc", "vmem" };
    int used = 0;
    for (unsigned i = 0; i < COSMO_RLIMIT_COUNT && used < (int)n; i++) {
        uint64_t v = p->rlim.v[i];
        if (v == COSMO_RLIM_INFINITY)
            used += ksnprintf(out + used, n - (size_t)used, "%s: unlimited\n", names[i]);
        else
            used += ksnprintf(out + used, n - (size_t)used, "%s: %llu\n", names[i], (unsigned long long)v);
    }
    return used;
}

static int render(uint64_t ino, char *out, size_t n)
{
    struct process *p = proc_get_visible(PROC_INO_PID(ino));
    if (p == NULL)
        return -ESRCH;   /* the path resolved; the process did not (P3) */
    int len = PROC_INO_KIND(ino) == PROC_STATUS ? render_status(p, out, n) : render_limits(p, out, n);
    process_put(p);
    return len;
}

/* --- files ------------------------------------------------------------------
 *
 * A file is rendered once, when it is opened, and that text is what
 * every read of that handle returns. The alternative -- measuring the
 * length now and rendering again at read time -- lets the two disagree:
 * a process whose syscall count gains a digit between them renders
 * longer than the size a reader is clamped to, and one that shrinks
 * leaves trailing zeroes. A file that reports a length must return that
 * length.
 *
 * So each open is a snapshot. Vnodes are not hashed, so opening again
 * takes a fresh one; a handle held open keeps what it read, which is a
 * truthful record of the moment it was taken rather than a mixture of
 * two.
 */
struct proc_text {
    size_t len;
    char text[PROC_TEXT_MAX];
};

static int proc_readpage(struct vnode *vn, uint64_t index, void *buf)
{
    memset(buf, 0, PAGE_SIZE);
    const struct proc_text *t = vn->fs_priv;
    if (index != 0 || t == NULL)
        return 0;   /* nothing here is longer than a page */
    memcpy(buf, t->text, t->len < PAGE_SIZE ? t->len : PAGE_SIZE);
    return 0;
}

static void proc_evict(struct vnode *vn)
{
    kfree(vn->fs_priv);
    vn->fs_priv = NULL;
}

static const struct vnode_ops proc_file_ops = {
    .readpage = proc_readpage,
    .evict = proc_evict,
};

/* Instantiate one of a process's files, rendering it now. */
static int proc_file(struct mount *mnt, pid_t pid, enum proc_kind kind, struct vnode **out)
{
    struct proc_text *t = kmalloc(sizeof(*t), 0);
    if (t == NULL)
        return -ENOMEM;
    uint64_t ino = PROC_INO(pid, kind);
    int len = render(ino, t->text, sizeof(t->text));
    if (len < 0) {
        kfree(t);
        return len;
    }
    t->len = (size_t)len;
    struct vnode *vn = proc_vnode(mnt, ino, VNODE_REG, 0444, &proc_file_ops);
    if (vn == NULL) {
        kfree(t);
        return -ENOMEM;
    }
    vn->fs_priv = t;
    vn->size = t->len;
    *out = vn;
    return 0;
}

/* --- a process's directory -------------------------------------------------- */

static int proc_pid_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    pid_t pid = PROC_INO_PID(dir->ino);
    if (len == 6 && memcmp(name, "status", 6) == 0)
        return proc_file(dir->mnt, pid, PROC_STATUS, out);
    if (len == 6 && memcmp(name, "limits", 6) == 0)
        return proc_file(dir->mnt, pid, PROC_LIMITS, out);
    return -ENOENT;   /* a file appears here only when somebody adds one (P2) */
}

static int proc_pid_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    pid_t pid = PROC_INO_PID(dir->ino);
    static const struct {
        const char *name;
        enum proc_kind kind;
    } files[] = { { "status", PROC_STATUS }, { "limits", PROC_LIMITS } };

    while (*pos < 2 + sizeof(files) / sizeof(files[0])) {
        uint64_t i = *pos;
        int rc;
        if (i == 0)
            rc = cb(arg, ".", 1, dir->ino, VNODE_DIR);
        else if (i == 1)
            rc = cb(arg, "..", 2, PROC_ROOT_INO, VNODE_DIR);
        else
            rc = cb(arg, files[i - 2].name, strlen(files[i - 2].name), PROC_INO(pid, files[i - 2].kind),
                    VNODE_REG);
        if (rc)
            return rc;
        (*pos)++;
    }
    return 0;
}

static const struct vnode_ops proc_pid_dir_ops = {
    .lookup = proc_pid_lookup,
    .readdir = proc_pid_readdir,
};

static int proc_pid_dir(struct mount *mnt, pid_t pid, struct vnode **out)
{
    struct process *p = proc_get_visible(pid);
    if (p == NULL)
        return -ENOENT;   /* not EACCES: that would confirm the pid exists (P1) */
    process_put(p);
    struct vnode *vn = proc_vnode(mnt, PROC_INO(pid, PROC_PID_DIR), VNODE_DIR, 0555, &proc_pid_dir_ops);
    if (vn == NULL)
        return -ENOMEM;
    *out = vn;
    return 0;
}

/* --- /proc itself ----------------------------------------------------------- */

/* A decimal pid, or -1. Strict: "01" and "1x" are not pids, and a name
 * that is not a pid must miss rather than be rounded into one. */
static pid_t parse_pid(const char *name, size_t len)
{
    if (len == 0 || len > 9 || (len > 1 && name[0] == '0'))
        return -1;
    pid_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] < '0' || name[i] > '9')
            return -1;
        v = v * 10 + (name[i] - '0');
    }
    return v > 0 ? v : -1;
}

static int proc_root_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    /* `self` is resolved to the caller here, which is how a process
     * reads its own facts without knowing its pid. A directory rather
     * than a symbolic link because this VFS has none. */
    if (len == 4 && memcmp(name, "self", 4) == 0) {
        struct process *me = process_current();
        if (me == NULL)
            return -ENOENT;
        return proc_pid_dir(dir->mnt, me->pid, out);
    }
    pid_t pid = parse_pid(name, len);
    if (pid < 0)
        return -ENOENT;
    return proc_pid_dir(dir->mnt, pid, out);
}

/* As many pids as one listing will name. A machine with more than this
 * many visible processes lists the first PROC_MAX_PIDS of them; the
 * alternative is an allocation whose size a caller chooses. */
#define PROC_MAX_PIDS 256

/*
 * The listing obeys the same rule as the lookup, because a listing that
 * names what it will not open is a leak with extra steps: the names
 * alone say which pids exist (P1).
 *
 * The pids are collected first and emitted afterwards: the process
 * table is walked under a spinlock, and the callback writes into a
 * buffer, which is not work to do inside one.
 */
static int proc_root_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    static const struct {
        const char *name;
        size_t len;
    } fixed[] = { { ".", 1 }, { "..", 2 }, { "self", 4 } };
    const uint64_t nr_fixed = sizeof(fixed) / sizeof(fixed[0]);

    while (*pos < nr_fixed) {
        uint64_t i = *pos;
        int rc = cb(arg, fixed[i].name, fixed[i].len, dir->ino, VNODE_DIR);
        if (rc)
            return rc;
        (*pos)++;
    }

    pid_t *pids = kmalloc(PROC_MAX_PIDS * sizeof(*pids), 0);
    if (pids == NULL)
        return -ENOMEM;
    unsigned total = process_list_visible(pids, PROC_MAX_PIDS);
    if (total > PROC_MAX_PIDS)
        total = PROC_MAX_PIDS;
    int rc = 0;
    for (unsigned i = (unsigned)(*pos - nr_fixed); i < total; i++) {
        char name[12];
        int n = ksnprintf(name, sizeof(name), "%u", (unsigned)pids[i]);
        rc = cb(arg, name, (size_t)n, PROC_INO(pids[i], PROC_PID_DIR), VNODE_DIR);
        if (rc)
            break;
        (*pos)++;
    }
    kfree(pids);
    return rc;
}

static const struct vnode_ops proc_root_ops = {
    .lookup = proc_root_lookup,
    .readdir = proc_root_readdir,
};

static int procfs_mount(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt)
{
    (void)fs;
    (void)flags;
    if (bdev != NULL)
        return -EINVAL;   /* nothing here comes off a device */
    struct vnode *root = proc_vnode(mnt, PROC_ROOT_INO, VNODE_DIR, 0555, &proc_root_ops);
    if (root == NULL)
        return -ENOMEM;
    root->flags |= VNODE_PINNED;   /* the mount's reference on its root */
    mnt->root = root;
    return 0;
}

static int procfs_unmount(struct mount *mnt)
{
    struct vnode *root = mnt->root;
    root->flags &= ~VNODE_PINNED;
    root->nlink = 0;
    return 0;
}

struct fs_type procfs_fs_type = {
    .name = "procfs",
    .mount = procfs_mount,
    .unmount = procfs_unmount,
    .sync = NULL,
};
