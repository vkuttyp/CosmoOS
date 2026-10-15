/*
 * procfs.c - /proc: facts about processes, addressable as files
 * (docs/kernel-services/filesystem/procfs/design.md, invariants P1-P3).
 *
 * The rule that keeps this from becoming the dumping ground the
 * constitution's section 56 warns about: a file belongs here if it
 * describes a process, or if it is one of a fixed list of Linux's own
 * system files that Linux programs read by that path (meminfo, stat,
 * loadavg, uptime, mounts: what free, top, df and uptime open), in
 * Linux's format (P4). Anything else system-wide stays in sysctl, whose
 * names are a curated list; the log stays in dmesg; device nodes stay
 * in /dev.
 *
 * What this adds that procinfo does not is addressability -- a
 * process's facts at a path, readable by anything that reads files,
 * rather than a struct returned to a caller that knows the ABI.
 */

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/tty.h>
#include <kernel/kmalloc.h>
#include <kernel/net/procnet.h>
#include <kernel/percpu.h>
#include <kernel/pagecache.h>
#include <kernel/pmm.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>

/*
 * The inode number carries what the vnode is: a pid and which of its
 * files. Nothing else is stored, so a vnode needs no private state and
 * the filesystem holds no table that could disagree with the process
 * table.
 */
enum proc_kind { PROC_ROOT = 0, PROC_PID_DIR = 1, PROC_STATUS = 2, PROC_LIMITS = 3, PROC_SELF = 4,
                 PROC_FD_DIR = 5, PROC_FD_LINK = 6, PROC_PID_STAT = 7, PROC_CMDLINE = 8,
                 /* The system files (P4), under pid 0. */
                 PROC_MEMINFO = 9, PROC_STAT = 10, PROC_LOADAVG = 11, PROC_UPTIME = 12, PROC_MOUNTS = 13,
                 /* /proc/net (S1): the directory, and its files with the
                  * file's enum procnet_file where a pid would be. */
                 PROC_NET_DIR = 14, PROC_NET_FILE = 15 };

/* An fd link carries its handle number above the pid as well. */
#define PROC_INO(pid, kind) (((uint64_t)(pid) << 8) | (uint64_t)(kind))
#define PROC_INO_FD(pid, fd) (PROC_INO(pid, PROC_FD_LINK) | ((uint64_t)(fd) << 40))
#define PROC_INO_PID(ino)   ((pid_t)(((ino) >> 8) & 0xffffffffu))
#define PROC_INO_FDNUM(ino) ((int)((ino) >> 40))
#define PROC_INO_KIND(ino)  ((enum proc_kind)((ino) & 0xff))
#define PROC_ROOT_INO       PROC_INO(0, PROC_ROOT)
#define PROC_SELF_INO       PROC_INO(0, PROC_SELF)

/* Rendered text is at most this, one page: a status block is a few
 * hundred bytes, /proc/stat a line per CPU, /proc/mounts a line per
 * mount (a listing longer than this ends at the last whole line). A
 * /proc/net file is the exception: a line per socket, up to
 * PROCNET_TEXT_MAX. */
#define PROC_TEXT_MAX PAGE_SIZE

static const struct vnode_ops proc_root_ops;
static const struct vnode_ops proc_fd_dir_ops;
static const struct vnode_ops proc_fd_link_ops;
static int proc_pid_dir(struct mount *mnt, pid_t pid, struct vnode **out);
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

/* The privileges by name (roadmap M5), "none" when there are none. */
static void render_privs(uint32_t privs, char *out, size_t n)
{
    static const char *const names[COSMO_PRIV_COUNT] = { "mount", "sys_admin", "net", "proc", "cred", "device" };
    size_t used = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < COSMO_PRIV_COUNT; i++)
        if (privs & (1u << i))
            used += (size_t)ksnprintf(out + used, used < n ? n - used : 0, "%s%s", used ? " " : "", names[i]);
    if (used == 0)
        ksnprintf(out, n, "none");
}

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
    char privs[64];
    render_privs(__atomic_load_n(&p->cred.privs, __ATOMIC_RELAXED), privs, sizeof(privs));
    return ksnprintf(out, n,
                     "name: %s\n"
                     "pid: %u\n"
                     "ppid: %u\n"
                     "state: %s\n"
                     "uid: %u\n"
                     "gid: %u\n"
                     "privs: %s\n"
                     "domain: %u\n"
                     "threads: %u\n"
                     "syscalls: %llu\n"
                     "cpu_ns: %llu\n",
                     p->name, p->pid, p->parent_pid,
                     st < sizeof(g_states) / sizeof(g_states[0]) ? g_states[st] : "?", p->cred.euid,
                     p->cred.egid, privs, p->domain, nr_threads, (unsigned long long)p->syscalls,
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

/* Linux's clock tick for /proc times (USER_HZ): what every reader divides by. */
#define PROC_USER_HZ 100u
static unsigned long long proc_ticks(uint64_t ns)
{
    return (unsigned long long)(ns / (1000000000ull / PROC_USER_HZ));
}

/*
 * /proc/<pid>/stat, Linux's one-line record (proc(5)) through field 44,
 * which is as far as top and ps read. What this kernel does not keep
 * reads as Linux's "none": no page faults are counted, no user/system
 * split is kept (all of cpu_ns is utime), and the memory sizes are 0 --
 * a process's space may be torn down under a reader (P34), and nothing
 * reads it from here (inventory).
 */
static int render_pid_stat(struct process *p, char *out, size_t n)
{
    unsigned nr_threads, live = 0;
    int cpu = 0;
    uint64_t run_ns = 0, now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    nr_threads = p->nr_threads;
    bool stopped = p->stopped;
    struct thread *t;
    list_for_each_entry(t, &p->threads, proc_link) {
        run_ns += sched_thread_run_ns(t, now);   /* the running stretch too: rq->lock nests inside p->lock */
        if (t->state == THREAD_RUNNING || t->state == THREAD_READY)
            live++;
        if (t == p->main_thread)
            cpu = t->cpu;
    }
    spin_unlock_irqrestore(&p->lock, s);
    /* Under g_process_table_lock, as every other reader takes them. */
    pid_t pgid = 0, sid = 0;
    (void)process_getpgid(p->pid, &pgid);
    (void)process_getsid(p->pid, &sid);
    const char *state = p->state == PROCESS_EXITED ? "Z" : stopped ? "T" : (live ? "R" : "S");
    return ksnprintf(out, n,
                     "%u (%s) %s %u %d %d 0 -1 0 0 0 0 0 %llu 0 0 0 20 0 %u 0 %llu 0 0 "
                     "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 %d 0 0 0 0 0\n",
                     p->pid, p->name, state, p->parent_pid, pgid, sid, proc_ticks(run_ns), nr_threads,
                     proc_ticks(p->start_ns), cpu < 0 ? 0 : cpu);
}

/* argv as exec gave it (process.h, `cmdline`): NUL-separated, as Linux's. */
static int render_cmdline(struct process *p, char *out, size_t n)
{
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    size_t len = p->cmdline_len < n ? p->cmdline_len : n;
    memcpy(out, p->cmdline, len);
    spin_unlock_irqrestore(&p->lock, s);
    return (int)len;
}

/* --- the system files (P4) --------------------------------------------------
 *
 * Linux's formats, for the programs that parse them. A field this kernel
 * has no figure for is left out when Linux readers tolerate its absence
 * and written as 0 when they expect it.
 */

/*
 * Memory in kB. The page cache gives back pages only past its own limit,
 * not when the allocator runs short, so nothing it holds is counted as
 * available: MemAvailable is MemFree. There is no swap.
 */
static int render_meminfo(char *out, size_t n)
{
    struct pmm_stats st;
    pmm_get_stats(&st);
    struct pagecache_stats pc;
    pagecache_get_stats(&pc);
    unsigned long long kb = PAGE_SIZE / 1024;
    return ksnprintf(out, n,
                     "MemTotal:       %8llu kB\n"
                     "MemFree:        %8llu kB\n"
                     "MemAvailable:   %8llu kB\n"
                     "Buffers:        %8llu kB\n"
                     "Cached:         %8llu kB\n"
                     "SwapTotal:      %8llu kB\n"
                     "SwapFree:       %8llu kB\n"
                     "SReclaimable:   %8llu kB\n",
                     (unsigned long long)st.total_pages * kb, (unsigned long long)st.free_pages * kb,
                     (unsigned long long)st.free_pages * kb, 0ull, (unsigned long long)pc.pages * kb, 0ull, 0ull,
                     0ull);
}

/*
 * CPU time in USER_HZ ticks: a CPU's idle is its idle thread's run time
 * (sched_cpu_idle_ns), and the rest of its time since boot is busy. No
 * user/system split is kept, so busy time is all "user"; iowait, irq
 * and steal are not measured and read 0.
 */
static int render_stat(char *out, size_t n)
{
    /* Two passes: the total line comes first, so it is summed before the
     * per-CPU lines are written. A CPU's second reading is the later one,
     * so each line still only grows from one open to the next. */
    uint64_t now = clock_now_ns();
    unsigned long long busy_all = 0, idle_all = 0, ctxt = 0;
    unsigned running = 0;
    for (unsigned c = 0; c < CONFIG_MAX_CPUS; c++) {
        if (!cpu_online(c))
            continue;
        uint64_t idle = sched_cpu_idle_ns(c, now);
        busy_all += proc_ticks(now > idle ? now - idle : 0);
        idle_all += proc_ticks(idle);
        ctxt += sched_switch_count(c);
        running += sched_cpu_load(c);
    }
    int used = ksnprintf(out, n, "cpu  %llu 0 0 %llu 0 0 0 0 0 0\n", busy_all, idle_all);
    now = clock_now_ns();
    for (unsigned c = 0; c < CONFIG_MAX_CPUS && (size_t)used < n; c++) {
        if (!cpu_online(c))
            continue;
        uint64_t idle = sched_cpu_idle_ns(c, now);
        used += ksnprintf(out + used, n - (size_t)used, "cpu%u %llu 0 0 %llu 0 0 0 0 0 0\n", c,
                          proc_ticks(now > idle ? now - idle : 0), proc_ticks(idle));
    }
    uint64_t rt = clock_realtime_ns();
    unsigned long long btime = rt > now ? (rt - now) / 1000000000ull : 0;
    if ((size_t)used < n)
        used += ksnprintf(out + used, n - (size_t)used,
                          "ctxt %llu\n"
                          "btime %llu\n"
                          "processes %u\n"
                          "procs_running %u\n"
                          "procs_blocked 0\n",
                          ctxt, btime, (unsigned)process_last_pid(), running);
    return used;
}

/* The 1, 5 and 15 minute averages (sched_loadavg), runnable/all threads,
 * and the last pid given out. */
static int render_loadavg(char *out, size_t n)
{
    unsigned long l[3];
    sched_loadavg(l, 11);
    unsigned running = 0;
    for (unsigned c = 0; c < CONFIG_MAX_CPUS; c++)
        if (cpu_online(c))
            running += sched_cpu_load(c);
    /* Rounded to two places, as Linux's /proc/loadavg rounds them
     * (FIXED_1/200 added first), so this agrees with sysinfo's readers. */
    for (unsigned i = 0; i < 3; i++)
        l[i] += (1ul << 11) / 200;
#define LOAD_INT(x)  ((x) >> 11)
#define LOAD_FRAC(x) ((((x) & ((1ul << 11) - 1)) * 100) >> 11)
    return ksnprintf(out, n, "%lu.%02lu %lu.%02lu %lu.%02lu %u/%u %u\n", LOAD_INT(l[0]), LOAD_FRAC(l[0]),
                     LOAD_INT(l[1]), LOAD_FRAC(l[1]), LOAD_INT(l[2]), LOAD_FRAC(l[2]), running, thread_count(),
                     (unsigned)process_last_pid());
#undef LOAD_INT
#undef LOAD_FRAC
}

/* Seconds since boot, and the idle time of every CPU summed. */
static int render_uptime(char *out, size_t n)
{
    uint64_t now = clock_now_ns(), idle = 0;
    for (unsigned c = 0; c < CONFIG_MAX_CPUS; c++)
        if (cpu_online(c))
            idle += sched_cpu_idle_ns(c, now);
    return ksnprintf(out, n, "%llu.%02llu %llu.%02llu\n", (unsigned long long)(now / 1000000000ull),
                     (unsigned long long)(now / 10000000ull % 100), (unsigned long long)(idle / 1000000000ull),
                     (unsigned long long)(idle / 10000000ull % 100));
}

static int render(uint64_t ino, char *out, size_t n)
{
    switch (PROC_INO_KIND(ino)) {
    case PROC_MEMINFO: return render_meminfo(out, n);
    case PROC_STAT:    return render_stat(out, n);
    case PROC_LOADAVG: return render_loadavg(out, n);
    case PROC_UPTIME:  return render_uptime(out, n);
    case PROC_MOUNTS:  return vfs_mounts_render(out, n);
    default:           break;
    }
    struct process *p = proc_get_visible(PROC_INO_PID(ino));
    if (p == NULL)
        return -ESRCH;   /* the path resolved; the process did not (P3) */
    int len;
    switch (PROC_INO_KIND(ino)) {
    case PROC_STATUS:   len = render_status(p, out, n); break;
    case PROC_PID_STAT: len = render_pid_stat(p, out, n); break;
    case PROC_CMDLINE:  len = render_cmdline(p, out, n); break;
    default:            len = render_limits(p, out, n); break;
    }
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
    char text[];   /* PROC_TEXT_MAX, or PROCNET_TEXT_MAX for a /proc/net file */
};

static int proc_readpage(struct vnode *vn, uint64_t index, void *buf)
{
    memset(buf, 0, PAGE_SIZE);
    const struct proc_text *t = vn->fs_priv;
    if (t == NULL || index * PAGE_SIZE >= t->len)
        return 0;
    size_t off = (size_t)index * PAGE_SIZE, n = t->len - off;
    memcpy(buf, t->text + off, n < PAGE_SIZE ? n : PAGE_SIZE);
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
    size_t cap = kind == PROC_NET_FILE ? PROCNET_TEXT_MAX : PROC_TEXT_MAX;
    struct proc_text *t = kmalloc(sizeof(*t) + cap, 0);
    if (t == NULL)
        return -ENOMEM;
    uint64_t ino = PROC_INO(pid, kind);
    int len = kind == PROC_NET_FILE ? procnet_render((enum procnet_file)pid, t->text, cap) : render(ino, t->text, cap);
    if (len < 0) {
        kfree(t);
        return len;
    }
    t->len = (size_t)len < cap ? (size_t)len : cap - 1;   /* ksnprintf returns what it wanted */
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

/* --- a process's open handles: /proc/<pid>/fd (roadmap M4) ------------------
 *
 * One symbolic link per open handle, named by its number, whose target
 * says what it is: a terminal by the path it is reached at -- which is
 * what musl's ttyname reads, and `tty` prints -- a directory by the path
 * it was opened by, anything else by its kind and inode as Linux writes
 * them ("pipe:[ino]"). Readable by the process's own user and the
 * privileged: which files a process has open is its business (P1 decides
 * whether it is visible at all).
 */
static struct process *proc_get_own(pid_t pid)
{
    struct process *p = proc_get_visible(pid);
    if (p == NULL)
        return NULL;
    struct process *me = process_current();
    if (me != NULL && me != p && me->cred.euid != p->cred.euid && !cred_has(&me->cred, PRIV_PROC)) {
        process_put(p);
        return NULL;
    }
    return p;
}

/* The link's target for handle `fd` of `p`, or -ENOENT when it is closed. */
static int fd_target(struct process *p, int fd, char *buf, size_t n)
{
    unsigned rights;
    struct kobject *obj = handle_get(&p->handles, fd, &rights);
    if (obj == NULL)
        return -ENOENT;
    int len;
    char tmp[64];
    if (tty_name_of_open(obj, tmp, sizeof(tmp))) {
        len = ksnprintf(buf, n, "%s", tmp);
    } else {
        struct file *f = file_from_kobject(obj);
        if (f != NULL && f->dir_path != NULL)
            len = ksnprintf(buf, n, "%s", f->dir_path);
        else if (f != NULL)
            len = ksnprintf(buf, n, "file:[%llu]", (unsigned long long)(f->vn ? f->vn->ino : 0));
        else   /* the kind alone: an address would map the kernel heap for whoever reads it */
            len = ksnprintf(buf, n, "anon_inode:[%s]", obj->type && obj->type->name ? obj->type->name : "object");
    }
    kobject_put(obj);
    return len < 0 ? 0 : ((size_t)len >= n ? (int)n - 1 : len);
}

static int proc_fd_readlink(struct vnode *vn, char *buf, size_t len)
{
    struct process *p = proc_get_own(PROC_INO_PID(vn->ino));
    if (p == NULL)
        return -ENOENT;
    char tmp[VFS_PATH_MAX];
    int n = fd_target(p, PROC_INO_FDNUM(vn->ino), tmp, sizeof(tmp));
    process_put(p);
    if (n < 0)
        return n;
    if ((size_t)n > len)
        n = (int)len;
    memcpy(buf, tmp, (size_t)n);
    return n;
}

static const struct vnode_ops proc_fd_link_ops = {
    .readlink = proc_fd_readlink,
};

static int proc_fd_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    pid_t pid = PROC_INO_PID(dir->ino);
    if (len == 2 && name[0] == '.' && name[1] == '.')
        return proc_pid_dir(dir->mnt, pid, out);
    int fd = 0;
    if (len == 0 || len > 3 || (len > 1 && name[0] == '0'))
        return -ENOENT;
    for (size_t i = 0; i < len; i++) {
        if (name[i] < '0' || name[i] > '9')
            return -ENOENT;
        fd = fd * 10 + (name[i] - '0');
    }
    if (fd >= HANDLE_TABLE_SIZE)
        return -ENOENT;
    struct process *p = proc_get_own(pid);
    if (p == NULL)
        return -ENOENT;
    char tmp[VFS_PATH_MAX];
    int n = fd_target(p, fd, tmp, sizeof(tmp));
    process_put(p);
    if (n < 0)
        return n;
    struct vnode *vn = proc_vnode(dir->mnt, PROC_INO_FD(pid, fd), VNODE_LNK, 0777, &proc_fd_link_ops);
    if (vn == NULL)
        return -ENOMEM;
    vn->size = (uint64_t)n;
    *out = vn;
    return 0;
}

static int proc_fd_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    pid_t pid = PROC_INO_PID(dir->ino);
    while (*pos < 2) {
        int rc = *pos == 0 ? cb(arg, ".", 1, dir->ino, VNODE_DIR) : cb(arg, "..", 2, PROC_INO(pid, PROC_PID_DIR), VNODE_DIR);
        if (rc)
            return rc;
        (*pos)++;
    }
    struct process *p = proc_get_own(pid);
    if (p == NULL)
        return 0;
    int rc = 0;
    for (int fd = (int)(*pos - 2); fd < HANDLE_TABLE_SIZE; fd++) {
        unsigned rights;
        struct kobject *obj = handle_get(&p->handles, fd, &rights);
        if (obj != NULL) {
            kobject_put(obj);
            char name[8];
            int n = ksnprintf(name, sizeof(name), "%d", fd);
            rc = cb(arg, name, (size_t)n, PROC_INO_FD(pid, fd), VNODE_LNK);
            if (rc)
                break;
        }
        *pos = (uint64_t)fd + 3;
    }
    process_put(p);
    return rc;
}

static const struct vnode_ops proc_fd_dir_ops = {
    .lookup = proc_fd_lookup,
    .readdir = proc_fd_readdir,
};

static int proc_fd_dir(struct mount *mnt, pid_t pid, struct vnode **out)
{
    struct process *p = proc_get_own(pid);
    if (p == NULL)
        return -ENOENT;
    uint32_t uid = p->cred.euid, gid = p->cred.egid;
    process_put(p);
    struct vnode *vn = proc_vnode(mnt, PROC_INO(pid, PROC_FD_DIR), VNODE_DIR, 0500, &proc_fd_dir_ops);
    if (vn == NULL)
        return -ENOMEM;
    /* The process's own, so its user may search it (0500). */
    vn->uid = uid;
    vn->gid = gid;
    *out = vn;
    return 0;
}

static int proc_pid_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    pid_t pid = PROC_INO_PID(dir->ino);
    /* The VFS resolves `..` by asking the filesystem; a process's
     * directory sits in /proc, which its own listing already names as
     * `..` (docs/audit/next-subsystem-cwd-name.md, the second defect). */
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        vnode_get(dir->mnt->root);
        *out = dir->mnt->root;
        return 0;
    }
    if (len == 6 && memcmp(name, "status", 6) == 0)
        return proc_file(dir->mnt, pid, PROC_STATUS, out);
    if (len == 6 && memcmp(name, "limits", 6) == 0)
        return proc_file(dir->mnt, pid, PROC_LIMITS, out);
    if (len == 4 && memcmp(name, "stat", 4) == 0)
        return proc_file(dir->mnt, pid, PROC_PID_STAT, out);
    if (len == 7 && memcmp(name, "cmdline", 7) == 0)
        return proc_file(dir->mnt, pid, PROC_CMDLINE, out);
    if (len == 2 && memcmp(name, "fd", 2) == 0)
        return proc_fd_dir(dir->mnt, pid, out);
    return -ENOENT;   /* a file appears here only when somebody adds one (P2) */
}

static int proc_pid_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    pid_t pid = PROC_INO_PID(dir->ino);
    static const struct {
        const char *name;
        enum proc_kind kind;
    } files[] = { { "status", PROC_STATUS }, { "limits", PROC_LIMITS }, { "fd", PROC_FD_DIR },
                  { "stat", PROC_PID_STAT }, { "cmdline", PROC_CMDLINE } };

    while (*pos < 2 + sizeof(files) / sizeof(files[0])) {
        uint64_t i = *pos;
        int rc;
        if (i == 0)
            rc = cb(arg, ".", 1, dir->ino, VNODE_DIR);
        else if (i == 1)
            rc = cb(arg, "..", 2, PROC_ROOT_INO, VNODE_DIR);
        else
            rc = cb(arg, files[i - 2].name, strlen(files[i - 2].name), PROC_INO(pid, files[i - 2].kind),
                    files[i - 2].kind == PROC_FD_DIR ? VNODE_DIR : VNODE_REG);
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
    uint32_t uid = p->cred.euid, gid = p->cred.egid;
    process_put(p);
    struct vnode *vn = proc_vnode(mnt, PROC_INO(pid, PROC_PID_DIR), VNODE_DIR, 0555, &proc_pid_dir_ops);
    if (vn == NULL)
        return -ENOMEM;
    /* Owned by the process's user, as on Linux: top and ps name a
     * process's user by stat'ing its directory. */
    vn->uid = uid;
    vn->gid = gid;
    *out = vn;
    return 0;
}

/* --- /proc/net (S1) --------------------------------------------------------
 *
 * The network stack's tables in Linux's formats, what BusyBox's ifconfig,
 * route, netstat and arp read (P4's system files, a directory of them).
 * The stack renders them (kernel/net/procnet.h); this is only their
 * paths, rendered at the open like every other file here (P3).
 */
static int proc_net_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        vnode_get(dir->mnt->root);
        *out = dir->mnt->root;
        return 0;
    }
    for (unsigned i = 0; i < PROCNET_COUNT; i++)
        if (len == strlen(procnet_names[i]) && memcmp(name, procnet_names[i], len) == 0)
            return proc_file(dir->mnt, (pid_t)i, PROC_NET_FILE, out);
    return -ENOENT;
}

static int proc_net_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    while (*pos < 2 + PROCNET_COUNT) {
        uint64_t i = *pos;
        int rc;
        if (i == 0)
            rc = cb(arg, ".", 1, dir->ino, VNODE_DIR);
        else if (i == 1)
            rc = cb(arg, "..", 2, PROC_ROOT_INO, VNODE_DIR);
        else
            rc = cb(arg, procnet_names[i - 2], strlen(procnet_names[i - 2]), PROC_INO(i - 2, PROC_NET_FILE), VNODE_REG);
        if (rc)
            return rc;
        (*pos)++;
    }
    return 0;
}

static const struct vnode_ops proc_net_dir_ops = {
    .lookup = proc_net_lookup,
    .readdir = proc_net_readdir,
};

#define PROC_NET_INO PROC_INO(0, PROC_NET_DIR)

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

/*
 * `self` is a symbolic link to the reader's own directory, its target
 * rendered at each read -- which is how a process reads its own facts
 * without knowing its pid. It was a directory resolved to the caller at
 * lookup, from before this VFS had links; as one, a walk through it
 * named the directory `self`, a name that means a different directory
 * to every process that walks it, so a child inheriting it would hold
 * its parent's vnode under its own name (P32,
 * docs/audit/next-subsystem-cwd-name.md). As a link the walk names it
 * by pid, like any other.
 */
static int proc_self_readlink(struct vnode *vn, char *buf, size_t len)
{
    (void)vn;
    struct process *me = process_current();
    if (me == NULL)
        return -ENOENT;
    char pid[12];
    int n = ksnprintf(pid, sizeof(pid), "%u", (unsigned)me->pid);
    if ((size_t)n > len)
        n = (int)len;
    memcpy(buf, pid, (size_t)n);   /* relative: the reader's directory beside this link */
    return n;
}

static const struct vnode_ops proc_self_ops = {
    .readlink = proc_self_readlink,
};

/* The system files (P4): a fixed list, by Linux's names. */
static const struct {
    const char *name;
    size_t len;
    enum proc_kind kind;
} g_system_files[] = {
    { "meminfo", 7, PROC_MEMINFO }, { "stat", 4, PROC_STAT },     { "loadavg", 7, PROC_LOADAVG },
    { "uptime", 6, PROC_UPTIME },   { "mounts", 6, PROC_MOUNTS },
};
#define NR_SYSTEM_FILES (sizeof(g_system_files) / sizeof(g_system_files[0]))

static int proc_root_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    for (size_t i = 0; i < NR_SYSTEM_FILES; i++)
        if (len == g_system_files[i].len && memcmp(name, g_system_files[i].name, len) == 0)
            return proc_file(dir->mnt, 0, g_system_files[i].kind, out);
    if (len == 3 && memcmp(name, "net", 3) == 0) {
        struct vnode *vn = proc_vnode(dir->mnt, PROC_NET_INO, VNODE_DIR, 0555, &proc_net_dir_ops);
        if (vn == NULL)
            return -ENOMEM;
        *out = vn;
        return 0;
    }
    if (len == 4 && memcmp(name, "self", 4) == 0) {
        struct process *me = process_current();
        if (me == NULL)
            return -ENOENT;
        struct vnode *vn = proc_vnode(dir->mnt, PROC_SELF_INO, VNODE_LNK, 0777, &proc_self_ops);
        if (vn == NULL)
            return -ENOMEM;
        /* A link's size is its target's length, which lstat reports and a
         * reader may size its readlink buffer from (found in review). The
         * vnode is not hashed, so this is the looker's own pid. */
        char pid[12];
        vn->size = (uint64_t)ksnprintf(pid, sizeof(pid), "%u", (unsigned)me->pid);
        *out = vn;
        return 0;
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
    } fixed[] = { { ".", 1 }, { "..", 2 }, { "self", 4 }, { "net", 3 } };
    const uint64_t nr_fixed = sizeof(fixed) / sizeof(fixed[0]);

    while (*pos < nr_fixed) {
        uint64_t i = *pos;
        int rc = i == 2   ? cb(arg, fixed[i].name, fixed[i].len, PROC_SELF_INO, VNODE_LNK)
                 : i == 3 ? cb(arg, fixed[i].name, fixed[i].len, PROC_NET_INO, VNODE_DIR)
                          : cb(arg, fixed[i].name, fixed[i].len, dir->ino, VNODE_DIR);
        if (rc)
            return rc;
        (*pos)++;
    }
    while (*pos < nr_fixed + NR_SYSTEM_FILES) {
        size_t i = (size_t)(*pos - nr_fixed);
        int rc = cb(arg, g_system_files[i].name, g_system_files[i].len, PROC_INO(0, g_system_files[i].kind), VNODE_REG);
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
    for (unsigned i = (unsigned)(*pos - nr_fixed - NR_SYSTEM_FILES); i < total; i++) {
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
    .magic = 0x9fa0u,   /* Linux's PROC_SUPER_MAGIC */
    .name = "procfs",
    .mount = procfs_mount,
    .unmount = procfs_unmount,
    .sync = NULL,
};
