/*
 * test_procfs.c - /proc (kernel-services/filesystem/procfs/procfs.c) on the host: roadmap 2.0 SA7. A digest recorded
 * from the C over random process tables -- processes visible and not, running, stopped and exited, with threads,
 * terminals, limits, privileges, command lines, environments, roots, namespaces and open handles of every kind --
 * walked from the mount's root: every directory listed, from the start and resumed where a callback stopped it,
 * every name it lists and the names it must refuse looked up, every file read page by page, every link read into
 * buffers of every size, environ opened once and twice, while the allocator and the vnode cache fail now and then.
 * Every call the code makes into the kernel, with its arguments, and every result are in the digest; every vnode,
 * text, process, terminal, record and handle reference taken is given back.
 */

#include "harness.h"

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/mountns.h>
#include <kernel/net/procnet.h>
#include <kernel/pagecache.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/tty.h>
#include <kernel/vfs.h>

#include <arch/cpu.h>

/* The C library's headers are not included: their pid_t is not the kernel's. */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void free(void *p);
int printf(const char *fmt, ...);

extern struct fs_type procfs_fs_type;
unsigned procfs_texts_live(void);

/* The digest of the C (procfs.c, 2026-11-07), which the CeeSafe port gives. */
#define PROCFS_DIGEST 0x750297368390729dull
#define ROUNDS 3000u
#define SEED 0x9F0C0F5A2026B10Cull

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

/* ---- the allocator: malloc, counted, filled with a pattern; an allocation fails now and then ---- */

static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    note(2, size);
    note(3, flags);
    if (chance(25))
        return NULL;
    void *p = malloc(size ? size : 1);
    EXPECT(p != NULL);
    memset(p, 0xA5, size);
    g_live++;
    return p;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size, 0);
    if (p != NULL)
        memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- references: vnodes the code made are evicted and freed at their last put ---- */

static const struct kobject_type g_vnode_type = { .name = "vnode" };
static const struct kobject_type g_proc_type = { .name = "process" };
static long g_vnodes;

void kobject_get(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    obj->refcount++;
}

void kobject_put(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    if (--obj->refcount != 0 || obj->type != &g_vnode_type)
        return;
    struct vnode *vn = (struct vnode *)obj;
    note(5, vn->ino);
    if (vn->ops != NULL && vn->ops->evict != NULL)
        vn->ops->evict(vn);
    EXPECT(vn->fs_priv == NULL);
    free(vn);
    g_vnodes--;
}

void vnode_put(struct vnode *vn) { kobject_put(&vn->obj); }

struct vnode *vnode_alloc(struct mount *mnt, uint64_t ino)
{
    note(6, ino);
    if (chance(30))
        return NULL;
    struct vnode *vn = calloc(1, sizeof(*vn));
    EXPECT(vn != NULL);
    vn->obj.type = &g_vnode_type;
    vn->obj.refcount = 1;
    vn->mnt = mnt;
    vn->ino = ino;
    vn->nlink = 1;
    g_vnodes++;
    return vn;
}

/* The roots a process may stand in: the global one and another. */
static struct vnode g_global_root = { .obj = { .refcount = 1 } }, g_other_root = { .obj = { .refcount = 1 } };
static struct mount_ns *g_ns_initial = (struct mount_ns *)&g_global_root.ino;   /* two addresses told apart */
static struct mount_ns *g_ns_other = (struct mount_ns *)&g_other_root.ino;
static bool g_reader_chrooted;

struct vnode *vfs_root(void)
{
    note(7, 0);
    vnode_get(&g_global_root);
    return &g_global_root;
}

struct vnode *vfs_current_root(void)
{
    note(7, 1);
    struct vnode *r = g_reader_chrooted ? &g_other_root : &g_global_root;
    vnode_get(r);
    return r;
}

struct mount_ns *mountns_initial(void)
{
    note(7, 2);
    return g_ns_initial;
}

/* ---- the process table ---- */

#define NPROC 6u
#define NTHREAD 3u

struct fake_obj {
    struct kobject obj;
    unsigned kind;   /* 1 a terminal, 2 a file by its path, 3 a file by its inode, 4 a file with no vnode, 5-7 other */
    char name[1200];
    struct file file;
    struct vnode vn;
};

static const struct kobject_type g_pipe_type = { .name = "pipe" }, g_anon_type = { .name = NULL };

struct fake_proc {
    struct process p;
    struct thread t[NTHREAD];
    bool visible, exists;
    struct fake_obj *fds[HANDLE_TABLE_SIZE];
    struct exec_record *rec;
};

static struct fake_proc *g_procs[NPROC];
static struct process *g_current;
static struct credentials g_kernel_cred;
static unsigned g_last_pid;

static struct fake_proc *fake_of(struct process *p)
{
    for (unsigned i = 0; i < NPROC; i++)
        if (&g_procs[i]->p == p)
            return g_procs[i];
    EXPECT(false);
    return g_procs[0];
}

struct process *process_lookup(pid_t pid)
{
    note(10, (uint64_t)(uint32_t)pid);
    for (unsigned i = 0; i < NPROC; i++)
        if (g_procs[i]->exists && g_procs[i]->p.pid == pid) {
            kobject_get(&g_procs[i]->p.obj);
            return &g_procs[i]->p;
        }
    return NULL;
}

bool process_visible_to_current(const struct process *p)
{
    struct fake_proc *f = fake_of((struct process *)p);
    if (chance(40))
        f->visible = !f->visible;   /* a process that leaves the reader's view between two calls */
    note(11, f->visible);
    return f->visible;
}

struct process *process_current(void)
{
    note(12, g_current ? (uint64_t)g_current->pid : 0);
    return g_current;
}

const struct credentials *cred_current(void)
{
    note(13, 0);
    return g_current ? &g_current->cred : &g_kernel_cred;
}

unsigned process_list_visible(pid_t *out, unsigned max)
{
    note(14, max);
    unsigned total = 0;
    if (pick(8) == 0) {   /* more than a listing names */
        total = 250 + pick(20);
        for (unsigned i = 0; i < total && i < max; i++)
            out[i] = (pid_t)(1000 + i);
    } else {
        for (unsigned i = 0; i < NPROC; i++)
            if (g_procs[i]->exists && g_procs[i]->visible && total < max)
                out[total++] = g_procs[i]->p.pid;
        if (pick(4) == 0 && total < max)
            out[total++] = 99999;   /* gone by the time it is looked up */
    }
    note(15, total);
    return total;
}

unsigned process_last_pid(void)
{
    note(16, g_last_pid);
    return g_last_pid;
}

bool process_space_usage(struct process *p, uint64_t *mapped, uint64_t *resident)
{
    note(17, (uint64_t)p->pid);
    *mapped = next() % 100000;
    *resident = next() % 50000;
    return p->state != PROCESS_EXITED && pick(6) != 0;
}

int process_getpgid(pid_t pid, pid_t *out)
{
    note(18, (uint64_t)(uint32_t)pid);
    for (unsigned i = 0; i < NPROC; i++)
        if (g_procs[i]->p.pid == pid) {
            *out = g_procs[i]->p.pgid;
            return 0;
        }
    return -ESRCH;
}

int process_getsid(pid_t pid, pid_t *out)
{
    note(19, (uint64_t)(uint32_t)pid);
    for (unsigned i = 0; i < NPROC; i++)
        if (g_procs[i]->p.pid == pid) {
            *out = g_procs[i]->p.sid;
            return 0;
        }
    return -ESRCH;
}

int process_nice_get(struct process *p)
{
    note(20, (uint64_t)(int64_t)p->nice);
    return p->nice;
}

void process_cpu_times_locked(struct process *p, uint64_t now_ns, struct cputime_pair *self,
                              struct cputime_pair *children)
{
    note(21, (uint64_t)p->pid);
    note(22, now_ns);
    self->user_ns = next() % 90000000000ull;
    self->system_ns = next() % 9000000000ull;
    children->user_ns = next() % 5000000000ull;
    children->system_ns = next() % 500000000ull;
}

uint64_t sched_thread_run_ns(struct thread *t, uint64_t now)
{
    note(23, (uint64_t)t->cpu);
    note(24, now);
    return next() % 1000000000ull;
}

struct exec_record *process_exec_record(struct process *p)
{
    struct fake_proc *f = fake_of(p);
    note(25, f->rec != NULL);
    if (f->rec != NULL)
        f->rec->refs++;
    return f->rec;
}

void exec_record_put(struct exec_record *r)
{
    note(26, r != NULL);
    if (r != NULL) {
        EXPECT(r->refs > 1);
        r->refs--;
    }
}

/* ---- terminals ---- */

static struct tty g_ttys[3];
static unsigned g_tty_refs;

struct tty *tty_controlling(pid_t sid)
{
    note(30, (uint64_t)(uint32_t)sid);
    unsigned k = pick(4);
    if (k == 3)
        return NULL;
    g_ttys[k].fg_pgid = pick(3) == 0 ? 0 : (pid_t)(1 + pick(40));
    g_tty_refs++;
    return &g_ttys[k];
}

void tty_put(struct tty *t)
{
    EXPECT(g_tty_refs > 0);
    note(31, (uint64_t)(t - g_ttys));
    g_tty_refs--;
}

bool tty_name_of_open(struct kobject *obj, char *buf, size_t n)
{
    struct fake_obj *o = (struct fake_obj *)obj;
    note(32, o->kind);
    if (o->kind != 1)
        return false;
    strlcpy(buf, o->name, n);
    return true;
}

/* ---- handles ---- */

struct kobject *handle_get(struct handle_table *t, int h, unsigned *rights_out)
{
    note(33, (uint64_t)(int64_t)h);
    struct fake_proc *f = NULL;
    for (unsigned i = 0; i < NPROC; i++)
        if (&g_procs[i]->p.handles == t)
            f = g_procs[i];
    EXPECT(f != NULL);
    EXPECT(h >= 0 && h < HANDLE_TABLE_SIZE);
    struct fake_obj *o = f->fds[h];
    if (o == NULL || chance(30))
        return NULL;   /* closed, or closed since it was listed */
    *rights_out = 0x3;
    o->obj.refcount++;
    return &o->obj;
}

struct file *file_from_kobject(struct kobject *obj)
{
    struct fake_obj *o = (struct fake_obj *)obj;
    note(34, o->kind);
    return o->kind >= 2 && o->kind <= 4 ? &o->file : NULL;
}


/* ---- clocks, CPUs, the scheduler, memory ---- */

static uint64_t g_now, g_cpu_mask;

uint64_t clock_now_ns(void)
{
    g_now += next() % 3000000000ull;
    note(40, g_now);
    return g_now;
}

uint64_t clock_realtime_ns(void)
{
    uint64_t rt = pick(5) == 0 ? g_now / 2 : g_now + 1700000000000000000ull + next() % 1000000000000ull;
    note(41, rt);
    return rt;
}

bool cpu_online(unsigned cpu)
{
    return cpu < 64 && (g_cpu_mask >> cpu & 1);
}

uint64_t sched_cpu_idle_ns(unsigned cpu, uint64_t now)
{
    note(42, cpu);
    note(43, now);
    return pick(6) == 0 ? now + next() % 1000 : next() % (now + 1);
}

uint64_t sched_switch_count(unsigned cpu)
{
    note(44, cpu);
    return next() % 100000000;
}

unsigned sched_cpu_load(unsigned cpu)
{
    note(45, cpu);
    return pick(4);
}

void sched_loadavg(unsigned long out[3], unsigned shift)
{
    note(46, shift);
    for (unsigned i = 0; i < 3; i++) {
        if (pick(3) == 0) {   /* a whole number, or just below one: where the rounding carries */
            unsigned long whole = (unsigned long)pick(4) << shift;
            out[i] = whole - pick(2);
        } else {
            out[i] = (unsigned long)(next() % (40ul << shift));
        }
    }
}

unsigned thread_count(void)
{
    note(47, 0);
    return 3 + pick(300);
}

void pmm_get_stats(struct pmm_stats *st)
{
    note(48, 0);
    memset(st, 0, sizeof(*st));
    st->total_pages = 1u << 20;
    st->free_pages = next() % st->total_pages;
}

void pagecache_get_stats(struct pagecache_stats *st)
{
    note(49, 0);
    memset(st, 0, sizeof(*st));
    st->pages = next() % 100000;
}

int arch_cpu_info(char *buf, size_t len)
{
    note(50, len);
    static const char line[] = "vendor_id\t: HostVendor\nmodel name\t: A host CPU at a speed nobody measured\n";
    unsigned reps = 1 + pick(6);
    int used = 0;
    for (unsigned i = 0; i < reps; i++)
        used += ksnprintf((size_t)used < len ? buf + used : buf, (size_t)used < len ? len - (size_t)used : 0, "%s",
                          line);
    return used;
}

/* ---- the VFS's mount listing and the network stack's tables ---- */

int vfs_mounts_render(char *out, size_t n)
{
    note(51, n);
    if (pick(12) == 0)
        return -ENOMEM;
    unsigned lines = pick(3) == 0 ? 60 + pick(80) : pick(6);
    int used = 0;
    for (unsigned i = 0; i < lines && (size_t)used < n; i++)
        used += ksnprintf(out + used, n - (size_t)used, "dev%u /mnt/m%u cosmofs rw 0 0\n", i, i * 7);
    return used;
}

const char *const procnet_names[PROCNET_COUNT] = {
    [PROCNET_DEV] = "dev",   [PROCNET_ROUTE] = "route", [PROCNET_ARP] = "arp",   [PROCNET_TCP] = "tcp",
    [PROCNET_TCP6] = "tcp6", [PROCNET_UDP] = "udp",     [PROCNET_UDP6] = "udp6", [PROCNET_IF_INET6] = "if_inet6",
};

int procnet_render(enum procnet_file f, char **out)
{
    note(52, f);
    if (pick(8) == 0)
        return -ENOMEM;
    size_t len = pick(3) == 0 ? 4000 + pick(9000) : pick(300);
    char *buf = malloc(len + 1);
    EXPECT(buf != NULL);
    g_live++;
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)('a' + (i * 7 + f) % 26);
    buf[len] = '\0';
    *out = buf;
    return (int)len;
}

/* ---- a random world ---- */

static void make_world(void)
{
    g_cpu_mask = pick(4) == 0 ? ~0ull : pick(3) == 0 ? (1ull << (1 + pick(63))) | 1 : 1 + next() % 0xffff;
    g_last_pid = pick(70000);
    g_reader_chrooted = pick(6) == 0;
    static const char *const tty_names[3] = { "console", "pts/", "ttyS0" };
    static char names[3][24];
    for (unsigned k = 0; k < 3; k++) {
        memset(&g_ttys[k], 0, sizeof(g_ttys[k]));
        if (k == 1) {
            unsigned minor = pick(3) == 0 ? 300 + pick(2000) : pick(255);
            ksnprintf(names[k], sizeof(names[k]), "pts/%u%s", minor, pick(5) == 0 ? "x" : "");
        } else
            ksnprintf(names[k], sizeof(names[k]), "%s", tty_names[k]);
        g_ttys[k].name = names[k];
    }
    for (unsigned i = 0; i < NPROC; i++) {
        struct fake_proc *f = calloc(1, sizeof(*f));
        EXPECT(f != NULL);
        g_procs[i] = f;
        struct process *p = &f->p;
        p->obj.type = &g_proc_type;
        p->obj.refcount = 1;
        p->pid = pick(5) == 0 ? (pid_t)(100000000 + pick(800000000)) : (pid_t)(1 + i * 7 + pick(7));
        p->parent_pid = (pid_t)pick(30);
        ksnprintf(p->name, sizeof(p->name), pick(6) == 0 ? "a-name-that-fills-its-buffer-to-the-end" : "proc%u", i);
        f->exists = pick(8) != 0;
        f->visible = pick(5) != 0;
        p->state = pick(6) == 0 ? PROCESS_EXITED : pick(8) == 0 ? PROCESS_EXITING : PROCESS_RUNNING;
        p->stopped = pick(6) == 0;
        p->umask = pick(0777);
        p->domain = pick(4);
        p->syscalls = next() % 10000000;
        p->start_ns = next() % 100000000000ull;
        p->nice = (int32_t)pick(40) - 20;
        p->pgid = (pid_t)pick(20);
        p->sid = (pid_t)pick(20);
        p->dumpable = pick(5) != 0;
        p->chrooted_since_exec = pick(7) == 0;
        p->root = pick(5) == 0 ? &g_other_root : pick(2) ? &g_global_root : NULL;
        p->mntns = pick(6) == 0 ? g_ns_other : pick(2) ? g_ns_initial : NULL;
        ksnprintf(p->cwd_path_locked, sizeof(p->cwd_path_locked), pick(8) == 0 ? "/" : "/home/u%u/work", i);
        uint32_t uid = pick(3) == 0 ? 0 : 1000 + pick(3), gid = pick(3) == 0 ? 0 : 100 + pick(3);
        p->cred.ruid = uid;
        p->cred.euid = pick(6) == 0 ? uid + 1 : uid;
        p->cred.suid = pick(8) == 0 ? 0 : uid;
        p->cred.rgid = gid;
        p->cred.egid = pick(8) == 0 ? gid + 2 : gid;
        p->cred.sgid = pick(8) == 0 ? gid + 1 : gid;
        p->cred.privs = pick(3) == 0 ? COSMO_PRIV_ALL : (uint32_t)pick(1u << COSMO_PRIV_COUNT);
        for (unsigned r = 0; r < COSMO_RLIMIT_COUNT; r++)
            p->rlim.v[r] = pick(2) ? COSMO_RLIM_INFINITY : next() % 100000;
        p->cmdline_len = (uint16_t)(pick(4) == 0 ? PROCESS_CMDLINE_MAX : pick(40));
        for (unsigned c = 0; c < p->cmdline_len; c++)
            p->cmdline[c] = c % 5 == 4 ? '\0' : (char)('A' + (c + i) % 26);
        list_init(&p->threads);
        unsigned nt = pick(NTHREAD + 1);
        p->nr_threads = nt;
        p->main_thread = nt ? &f->t[0] : NULL;
        for (unsigned t = 0; t < nt; t++) {
            f->t[t].state = (enum thread_state)pick(4);
            f->t[t].cpu = pick(5) == 0 ? -1 : (int)pick(8);
            list_push_back(&p->threads, &f->t[t].proc_link);
        }
        p->cputime.gone_run_ns = next() % 1000000000;
        if (pick(4) != 0) {
            size_t exe = pick(4) == 0 ? 0 : 5 + pick(60);
            size_t env = pick(3) == 0 ? 0 : pick(2) ? 3000 + pick(4000) : pick(200);
            struct exec_record *r = calloc(1, sizeof(*r) + exe + 1 + env + 1);
            EXPECT(r != NULL);
            r->refs = 1;
            r->exe_len = (uint16_t)exe;
            r->env_len = (uint16_t)env;
            for (size_t c = 0; c < exe; c++)
                r->data[c] = c == 0 ? '/' : (char)('a' + (c * 3 + i) % 26);
            for (size_t c = 0; c < env; c++)
                r->data[exe + 1 + c] = c % 9 == 8 ? '\0' : (char)('K' + c % 10);
            f->rec = r;
        }
        p->exec_rec = f->rec;
        for (unsigned h = 0; h < HANDLE_TABLE_SIZE; h++) {
            if (pick(4) != 0 && h > 3)
                continue;
            if (pick(3) == 0)
                continue;
            struct fake_obj *o = calloc(1, sizeof(*o));
            EXPECT(o != NULL);
            o->obj.refcount = 1;
            o->kind = 1 + pick(7);
            o->obj.type = o->kind == 6 ? &g_anon_type : o->kind == 7 ? NULL : &g_pipe_type;
            if (o->kind == 1)
                ksnprintf(o->name, sizeof(o->name), pick(4) == 0 ? "/dev/pts/a-terminal-whose-path-is-longer-than-"
                                                                  "sixty-four-bytes-%u" : "/dev/pts/%u", h);
            o->file.dir_path = o->kind == 2 ? o->name : NULL;
            if (o->kind == 2 && pick(6) == 0) {   /* longer than a link's buffer */
                memset(o->name, 'd', 1100);
                o->name[0] = '/';
                o->name[1100] = '\0';
            } else if (o->kind == 2) {
                ksnprintf(o->name, sizeof(o->name), "/srv/dir%u", h);
            }
            o->vn.ino = 4096 + h;
            o->file.vn = o->kind == 3 ? &o->vn : NULL;
            f->fds[h] = o;
        }
    }
    unsigned cur = pick(NPROC + 2);
    g_current = cur < NPROC ? &g_procs[cur]->p : cur == NPROC ? &g_procs[0]->p : NULL;
    g_kernel_cred = (struct credentials){ .privs = COSMO_PRIV_ALL };
}

static void free_world(void)
{
    EXPECT(g_tty_refs == 0);
    for (unsigned i = 0; i < NPROC; i++) {
        struct fake_proc *f = g_procs[i];
        EXPECT(f->p.obj.refcount == 1);
        if (f->rec != NULL) {
            EXPECT(f->rec->refs == 1);
            free(f->rec);
        }
        for (unsigned h = 0; h < HANDLE_TABLE_SIZE; h++) {
            EXPECT(f->fds[h] == NULL || f->fds[h]->obj.refcount == 1);   /* every handle_get's reference put */
            free(f->fds[h]);
        }
        free(f);
    }
}

/* ---- the walk ---- */

#define MAX_NAMES 96u

struct listing {
    unsigned n, stop_at;
    char names[MAX_NAMES][16];
};

static int on_dirent(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type)
{
    struct listing *l = arg;
    note(60, len);
    mix(name, len);
    note(61, ino);
    note(62, type);
    if (l->stop_at != 0 && --l->stop_at == 0)
        return 7;   /* the caller's buffer is full: the walk resumes from *pos */
    if (l->n < MAX_NAMES && len < sizeof(l->names[0])) {
        memcpy(l->names[l->n], name, len);
        l->names[l->n][len] = '\0';
        l->n++;
    }
    return 0;
}

static void list_dir(struct vnode *vn, struct listing *l)
{
    uint64_t pos = 0;
    l->n = 0;
    for (unsigned guard = 0; guard < 400; guard++) {
        l->stop_at = pick(3) == 0 ? 1 + pick(6) : 0;
        int rc = vn->ops->readdir(vn, &pos, on_dirent, l);
        note(63, (uint64_t)(int64_t)rc);
        note(64, pos);
        if (rc != 7)
            break;
    }
    if (pick(4) == 0) {   /* a walk resumed far past the end */
        uint64_t far = pos + pick(400);
        l->stop_at = 0;
        struct listing spare = { 0 };
        int rc = vn->ops->readdir(vn, &far, on_dirent, &spare);
        note(65, (uint64_t)(int64_t)rc);
        note(66, far);
    }
}

static void explore(struct vnode *vn, unsigned depth);

static void look(struct vnode *dir, const char *name, size_t len, unsigned depth)
{
    struct vnode *child = NULL;
    int rc = dir->ops->lookup(dir, name, len, &child);
    note(70, len);
    mix(name, len);
    note(71, (uint64_t)(int64_t)rc);
    if (rc != 0) {
        EXPECT(child == NULL);
        return;
    }
    EXPECT(child != NULL);
    explore(child, depth + 1);
    vnode_put(child);
}

static void read_pages(struct vnode *vn)
{
    static uint8_t page[PAGE_SIZE];
    for (uint64_t i = 0; i <= vn->size / PAGE_SIZE + 1; i++) {
        memset(page, 0x5A, sizeof(page));
        int rc = vn->ops->readpage(vn, i, page);
        note(80, (uint64_t)(int64_t)rc);
        mix(page, sizeof(page));
    }
}

static void explore(struct vnode *vn, unsigned depth)
{
    const struct vnode_ops *o = vn->ops;
    EXPECT(o != NULL);
    note(90, vn->ino);
    note(91, vn->type);
    note(92, vn->mode);
    note(93, vn->uid);
    note(94, vn->gid);
    note(95, vn->size);
    note(96, vn->flags);
    note(97, (uint64_t)(o->lookup != NULL) | (uint64_t)(o->readdir != NULL) << 1 |
                 (uint64_t)(o->readpage != NULL) << 2 | (uint64_t)(o->readlink != NULL) << 3 |
                 (uint64_t)(o->open != NULL) << 4 | (uint64_t)(o->evict != NULL) << 5);
    EXPECT(vn->mnt != NULL);
    if (o->open != NULL) {
        struct file f = { 0 };
        for (unsigned k = 0, opens = 1 + pick(2); k < opens; k++) {
            int rc = o->open(vn, &f);
            note(98, (uint64_t)(int64_t)rc);
            note(99, vn->size);
        }
    }
    if (o->readpage != NULL)
        read_pages(vn);
    if (o->readlink != NULL) {
        static const size_t sizes[] = { 0, 1, 3, 64, VFS_PATH_MAX };
        for (unsigned k = 0; k < 2; k++) {
            char buf[VFS_PATH_MAX + 8];
            memset(buf, 0x77, sizeof(buf));
            size_t len = sizes[pick(5)];
            struct process *reader = g_current;
            if (pick(6) == 0)
                g_current = NULL;   /* read by the kernel, or after the reader left */
            int rc = o->readlink(vn, buf, len);
            g_current = reader;
            note(100, len);
            note(101, (uint64_t)(int64_t)rc);
            mix(buf, len + 4);
        }
    }
    if (o->readdir == NULL || o->lookup == NULL || depth >= 3)
        return;
    struct listing *l = calloc(1, sizeof(*l));
    EXPECT(l != NULL);
    list_dir(vn, l);
    static const char *const extra[] = { "..", "", "0", "01", "1x", "1234567890", "999999999", "statu", "status",
                                         "limits", "stat", "cmdline", "fd", "environ", "cwd", "exe", "root", "self",
                                         "net", "dev", "route", "meminfo", "mounts", "007", "63", "64", "100", "x",
                                         ".x" };
    for (unsigned i = 0; i < l->n; i++)
        if (pick(4) != 0)
            look(vn, l->names[i], strlen(l->names[i]), depth);
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
        if (pick(3) == 0)
            look(vn, extra[i], strlen(extra[i]), depth);
    if (pick(2) == 0) {
        char name[12];
        int n = ksnprintf(name, sizeof(name), "%u", g_procs[pick(NPROC)]->p.pid);
        look(vn, name, (size_t)n, depth);
    }
    free(l);
}

static void test_walk(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    for (unsigned round = 0; round < ROUNDS; round++) {
        g_faults = round % 4 != 0;
        make_world();
        struct mount mnt;
        memset(&mnt, 0, sizeof(mnt));
        struct fs_type *fs = &procfs_fs_type;
        note(110, fs->magic);
        mix(fs->name, strlen(fs->name));
        if (pick(20) == 0) {
            int rc = fs->mount(fs, (struct blkdev *)&mnt, 0, &mnt);
            note(111, (uint64_t)(int64_t)rc);
            EXPECT(rc == -EINVAL);
        }
        int rc = fs->mount(fs, NULL, 0, &mnt);
        note(112, (uint64_t)(int64_t)rc);
        if (rc == 0) {
            EXPECT(mnt.root != NULL && (mnt.root->flags & VNODE_PINNED));
            explore(mnt.root, 0);
            note(113, procfs_texts_live());
            struct vnode *root = mnt.root;
            rc = fs->unmount(&mnt);
            note(114, (uint64_t)(int64_t)rc);
            EXPECT(rc == 0 && root->flags == 0 && root->nlink == 0);
            vnode_put(root);
        }
        EXPECT(procfs_texts_live() == 0);
        EXPECT(g_vnodes == 0);
        EXPECT(g_live == 0);
        EXPECT(g_global_root.obj.refcount == 1 && g_other_root.obj.refcount == 1);
        free_world();
    }
    printf("procfs digest 0x%016llx\n", (unsigned long long)g_digest);
    EXPECT(g_digest == PROCFS_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "procfs_walk", test_walk },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
