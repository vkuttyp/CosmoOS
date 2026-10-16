/*
 * procfs.c - /proc: facts about processes, addressable as files
 * (docs/kernel-services/filesystem/procfs/design.md, invariants P1-P3).
 *
 * The rule that keeps this from becoming the dumping ground the
 * constitution's section 56 warns about: a file belongs here if it
 * describes a process, or if it is one of a fixed list of Linux's own
 * system files that Linux programs read by that path (meminfo, stat,
 * loadavg, uptime, mounts: what free, top, df and uptime open) or that
 * the shell's users read by name (version, cpuinfo), in Linux's format
 * (P4). Anything else system-wide stays in sysctl, whose
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
#include <kernel/mountns.h>
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
#include <kernel/version.h>
#include <kernel/vfs.h>

#include <arch/cpu.h>

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
                 PROC_NET_DIR = 14, PROC_NET_FILE = 15,
                 /* Shell completeness S3: a process's environment and
                  * links, and two more system files. */
                 PROC_ENVIRON = 16, PROC_CWD = 17, PROC_EXE = 18, PROC_ROOT_LINK = 19,
                 PROC_VERSION = 20, PROC_CPUINFO = 21 };

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
 * mount (a listing longer than this ends at the last whole line). Two
 * exceptions: a /proc/net file, a line per socket, up to
 * PROCNET_TEXT_MAX; and cpuinfo, a block per online CPU
 * (proc_text_cap). */
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

/* What `stat` and `status` both say about a process's threads, from one
 * hold of its lock. Its CPU time counts each thread's current running
 * stretch too (sched_thread_run_ns), which the scheduler charges only at
 * a switch-out; rq->lock nests inside p->lock. */
struct proc_threads {
    unsigned nr_threads, live;
    int cpu;
    bool stopped;
    uint64_t run_ns;
};

static void proc_threads(struct process *p, struct proc_threads *out)
{
    uint64_t now = clock_now_ns();
    *out = (struct proc_threads){ 0 };
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    out->nr_threads = p->nr_threads;
    out->stopped = p->stopped;
    struct thread *t;
    list_for_each_entry(t, &p->threads, proc_link) {
        out->run_ns += sched_thread_run_ns(t, now);
        if (t->state == THREAD_RUNNING || t->state == THREAD_READY)
            out->live++;
        if (t == p->main_thread)
            out->cpu = t->cpu;
    }
    spin_unlock_irqrestore(&p->lock, s);
}

/* Linux's state letter: `Z` once exited, `T` while job control has it
 * stopped, `R` with a thread running or ready, else `S`. */
static char proc_state(const struct process *p, const struct proc_threads *t)
{
    return p->state == PROCESS_EXITED ? 'Z' : t->stopped ? 'T' : (t->live ? 'R' : 'S');
}

/*
 * The session's controlling terminal as proc(5)'s tty_nr, and its
 * foreground group (tpgid, -1 for none). This kernel numbers no devices
 * (st_rdev is 0; inventory), so the number is Linux's for the same
 * terminal: the console is 5,1 and /dev/pts/N is 136,N.
 */
static void proc_tty(pid_t sid, unsigned *nr, int *tpgid)
{
    *nr = 0;
    *tpgid = -1;
    struct tty *t = tty_controlling(sid);
    if (t == NULL)
        return;
    unsigned major = 0, minor = 0;
    if (strcmp(t->name, "console") == 0) {
        major = 5;
        minor = 1;
    } else if (strncmp(t->name, "pts/", 4) == 0) {
        major = 136;
        for (const char *c = t->name + 4; *c >= '0' && *c <= '9'; c++)
            minor = minor * 10 + (unsigned)(*c - '0');
    }
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    pid_t fg = t->fg_pgid;
    spin_unlock_irqrestore(&t->lock, s);
    tty_put(t);
    if (major != 0)
        *nr = (major << 8) | (minor & 0xffu) | ((minor & ~0xffu) << 12);
    if (fg != 0)
        *tpgid = (int)fg;
}

/*
 * Linux's `key:\tvalue` lines (proc(5)), which BusyBox's ps reads for the
 * real ids (`Uid:`, `Gid:`), followed by what this kernel adds: the
 * privileges, the domain, the system call count and the CPU time in
 * nanoseconds. The memory lines are there while the process has a space
 * (P38), and absent after, as Linux leaves them out of a zombie's.
 */
static int render_status(struct process *p, char *out, size_t n)
{
    struct proc_threads th;
    proc_threads(p, &th);
    uint64_t mapped, resident;
    bool space = process_space_usage(p, &mapped, &resident);
    const struct credentials *c = &p->cred;
    char privs[64];
    render_privs(__atomic_load_n(&c->privs, __ATOMIC_RELAXED), privs, sizeof(privs));
    char state = proc_state(p, &th);
    const char *word = state == 'Z' ? "zombie" : state == 'T' ? "stopped" : state == 'R' ? "running" : "sleeping";
    int used = ksnprintf(out, n,
                         "Name:\t%s\n"
                         "Umask:\t%04o\n"
                         "State:\t%c (%s)\n"
                         "Tgid:\t%u\n"
                         "Pid:\t%u\n"
                         "PPid:\t%u\n"
                         "Uid:\t%u\t%u\t%u\t%u\n"
                         "Gid:\t%u\t%u\t%u\t%u\n",
                         p->name, (unsigned)__atomic_load_n(&p->umask, __ATOMIC_RELAXED), state, word, p->pid, p->pid, p->parent_pid, c->ruid, c->euid,
                         c->suid, c->euid, c->rgid, c->egid, c->sgid, c->egid);
    if (space && used >= 0 && (size_t)used < n)
        used += ksnprintf(out + used, n - (size_t)used,
                          "VmSize:\t%8llu kB\n"
                          "VmRSS:\t%8llu kB\n",
                          (unsigned long long)mapped * (PAGE_SIZE / 1024),
                          (unsigned long long)resident * (PAGE_SIZE / 1024));
    if (used >= 0 && (size_t)used < n)
        used += ksnprintf(out + used, n - (size_t)used,
                          "Threads:\t%u\n"
                          "Privs:\t%s\n"
                          "Domain:\t%u\n"
                          "Syscalls:\t%llu\n"
                          "Cpu_ns:\t%llu\n",
                          th.nr_threads, privs, p->domain, (unsigned long long)p->syscalls,
                          (unsigned long long)th.run_ns);
    return used;
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
 * split is kept (all of cpu_ns is utime), and there is no nice value
 * (priority 20, nice 0). The memory sizes are the space's own counts,
 * read under the rule exit and exec follow (P38): vsize in bytes, rss in
 * pages, both 0 once the space is gone.
 */
static int render_pid_stat(struct process *p, char *out, size_t n)
{
    struct proc_threads th;
    proc_threads(p, &th);
    uint64_t mapped, resident;
    (void)process_space_usage(p, &mapped, &resident);
    /* Under g_process_table_lock, as every other reader takes them. */
    pid_t pgid = 0, sid = 0;
    (void)process_getpgid(p->pid, &pgid);
    (void)process_getsid(p->pid, &sid);
    unsigned tty_nr;
    int tpgid;
    proc_tty(sid, &tty_nr, &tpgid);
    return ksnprintf(out, n,
                     "%u (%s) %c %u %d %d %u %d 0 0 0 0 0 %llu 0 0 0 20 0 %u 0 %llu %llu %llu "
                     "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 %d 0 0 0 0 0\n",
                     p->pid, p->name, proc_state(p, &th), p->parent_pid, pgid, sid, tty_nr, tpgid,
                     proc_ticks(th.run_ns), th.nr_threads, proc_ticks(p->start_ns),
                     (unsigned long long)mapped * PAGE_SIZE, (unsigned long long)resident,
                     th.cpu < 0 ? 0 : th.cpu);
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

/* Linux's first line, with what uname says: the release, the compiler,
 * then uname's version (shell completeness S3). Nobody here built it at
 * a known user@host, so that part of Linux's line is not invented. */
static int render_version(char *out, size_t n)
{
    return ksnprintf(out, n, "Linux version %s (clang %s) %s %s %s\n", KERNEL_LINUX_RELEASE, __clang_version__,
                     KERNEL_NAME, KERNEL_VERSION, COSMO_BUILD_ID);
}

/* A block per online CPU: its index, then the architecture's lines
 * (arch_cpu_info, the boot CPU's identification), then a blank line. */
static int render_cpuinfo(char *out, size_t n)
{
    int used = 0;
    for (unsigned c = 0; c < CONFIG_MAX_CPUS && (size_t)used < n; c++) {
        if (!cpu_online(c))
            continue;
        used += ksnprintf(out + used, n - (size_t)used, "processor\t: %u\n", c);
        if ((size_t)used < n)
            used += arch_cpu_info(out + used, n - (size_t)used);
        if ((size_t)used < n)
            used += ksnprintf(out + used, n - (size_t)used, "\n");
    }
    return used;
}

static int render(uint64_t ino, char *out, size_t n)
{
    switch (PROC_INO_KIND(ino)) {
    case PROC_VERSION: return render_version(out, n);
    case PROC_CPUINFO: return render_cpuinfo(out, n);
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
    char text[];   /* PROC_TEXT_MAX, or a /proc/net file's own length */
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

#if CONFIG_DEBUG
/* Rendered texts alive now: the proc-exit-read self-test's leak check. */
static unsigned g_proc_texts;
unsigned procfs_texts_live(void)
{
    return __atomic_load_n(&g_proc_texts, __ATOMIC_RELAXED);
}
#define PROC_TEXT_COUNT(d) __atomic_add_fetch(&g_proc_texts, (unsigned)(d), __ATOMIC_RELAXED)
#else
#define PROC_TEXT_COUNT(d) ((void)0)
#endif

static void proc_evict(struct vnode *vn)
{
    if (vn->fs_priv != NULL)
        PROC_TEXT_COUNT(-1);
    kfree(vn->fs_priv);
    vn->fs_priv = NULL;
}

static const struct vnode_ops proc_file_ops = {
    .readpage = proc_readpage,
    .evict = proc_evict,
};

/* A /proc/net file's text, kept at its own length: the stack renders it
 * into a buffer that grew as it needed, and only the text is kept. */
static int net_text(enum procnet_file f, struct proc_text **out)
{
    char *buf;
    int len = procnet_render(f, &buf);
    if (len < 0)
        return len;
    struct proc_text *t = kmalloc(sizeof(*t) + (size_t)len + 1, 0);
    if (t != NULL) {
        memcpy(t->text, buf, (size_t)len + 1);
        t->len = (size_t)len;
    }
    kfree(buf);
    *out = t;
    return t != NULL ? 0 : -ENOMEM;
}

/* The buffer a file renders into: a page, except cpuinfo, a block per
 * online CPU that a page does not hold past about two dozen of them. */
#define PROC_CPUINFO_BLOCK 512u   /* one CPU's block: x86-64's is about 170 bytes */
static size_t proc_text_cap(enum proc_kind kind)
{
    if (kind != PROC_CPUINFO)
        return PROC_TEXT_MAX;
    size_t online = 0;
    for (unsigned c = 0; c < CONFIG_MAX_CPUS; c++)
        if (cpu_online(c))
            online++;
    return online * PROC_CPUINFO_BLOCK > PROC_TEXT_MAX ? online * PROC_CPUINFO_BLOCK : PROC_TEXT_MAX;
}

/* Instantiate one of a process's files, rendering it now. */
static int proc_file(struct mount *mnt, pid_t pid, enum proc_kind kind, struct vnode **out)
{
    uint64_t ino = PROC_INO(pid, kind);
    struct proc_text *t;
    if (kind == PROC_NET_FILE) {
        int rc = net_text((enum procnet_file)pid, &t);
        if (rc)
            return rc;
    } else {
        size_t cap = proc_text_cap(kind);
        t = kmalloc(sizeof(*t) + cap, 0);
        if (t == NULL)
            return -ENOMEM;
        int len = render(ino, t->text, cap);
        if (len < 0) {
            kfree(t);
            return len;
        }
        t->len = (size_t)len < cap ? (size_t)len : cap - 1;   /* ksnprintf returns what it wanted */
    }
    struct vnode *vn = proc_vnode(mnt, ino, VNODE_REG, 0444, &proc_file_ops);
    if (vn == NULL) {
        kfree(t);
        return -ENOMEM;
    }
    vn->fs_priv = t;
    PROC_TEXT_COUNT(1);
    vn->size = t->len;
    *out = vn;
    return 0;
}

/* --- looking inside a process (shell completeness S3) -----------------------
 *
 * The environment, and where a process's working directory, root and
 * executable are. Linux guards these with its ptrace read check; with no
 * ptrace here the rule is that check's (P5): the process itself; a holder
 * of PRIV_PROC; or a reader whose effective ids are all three of the
 * target's ids, while the target is dumpable -- a process that cleared
 * PR_SET_DUMPABLE keeps these from its own user. P1 has already decided
 * whether the process is visible at all.
 */
static bool proc_may_inspect(struct process *p)
{
    if (process_current() == p)
        return true;
    const struct credentials *c = cred_current();
    if (cred_has(c, PRIV_PROC))
        return true;
    /* The target's ids and its flag from one hold of its lock, which is
     * what setresuid and setresgid change them under (and the flag with
     * them): never a tuple that did not exist. */
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    const struct credentials *t = &p->cred;
    bool ids = c->euid == t->ruid && c->euid == t->euid && c->euid == t->suid && c->egid == t->rgid &&
               c->egid == t->egid && c->egid == t->sgid;
    bool dumpable = __atomic_load_n(&p->dumpable, __ATOMIC_RELAXED) != 0;
    spin_unlock_irqrestore(&p->lock, s);
    return ids && dumpable;
}

/*
 * The environment exec gave the process, from its exec record (process.h):
 * the kernel keeps the copy because reading the process's memory back is
 * not safe -- its space can be torn down under the reader (P34). The
 * rule is checked at the open, not the lookup, so the file is listed and
 * stat'ed like Linux's and only reading it is refused (EACCES), and the
 * text is rendered there (P3); until then the file is empty.
 */
static int proc_environ_open(struct vnode *vn, struct file *f)
{
    (void)f;
    struct process *p = proc_get_visible(PROC_INO_PID(vn->ino));
    if (p == NULL)
        return -ESRCH;   /* the path resolved; the process did not (P3) */
    if (!proc_may_inspect(p)) {
        process_put(p);
        return -EACCES;
    }
    struct exec_record *r = process_exec_record(p);
    process_put(p);
    size_t len = r != NULL ? r->env_len : 0;
    struct proc_text *t = kmalloc(sizeof(*t) + len + 1, 0);
    if (t == NULL) {
        exec_record_put(r);
        return -ENOMEM;
    }
    if (len)
        memcpy(t->text, exec_record_env(r), len);
    t->text[len] = '\0';
    t->len = len;
    exec_record_put(r);
    if (vn->fs_priv != NULL) {   /* a second open of the same vnode keeps the first one's text */
        kfree(t);
        return 0;
    }
    vn->fs_priv = t;
    PROC_TEXT_COUNT(1);
    vn->size = len;
    return 0;
}

static const struct vnode_ops proc_environ_ops = {
    .readpage = proc_readpage,
    .evict = proc_evict,
    .open = proc_environ_open,
};

static int proc_environ(struct mount *mnt, pid_t pid, struct vnode **out)
{
    struct process *p = proc_get_visible(pid);
    if (p == NULL)
        return -ESRCH;
    uint32_t uid = p->cred.euid, gid = p->cred.egid;
    process_put(p);
    /* 0444, not Linux's 0400: the VFS checks the mode before the open
     * hook, and a 0400 file would refuse a PRIV_PROC holder of another
     * uid there (without PRIV_SYS_ADMIN) before P5 could admit it. The
     * hook is the whole rule. */
    struct vnode *vn = proc_vnode(mnt, PROC_INO(pid, PROC_ENVIRON), VNODE_REG, 0444, &proc_environ_ops);
    if (vn == NULL)
        return -ENOMEM;
    vn->uid = uid;   /* the process's own, as Linux's */
    vn->gid = gid;
    *out = vn;
    return 0;
}

/* Whether the reader stands where `p` does: the same root and the same
 * mounts, so a path `p` holds names the same file for the reader. The
 * namespaces are fixed at spawn; a root changes under its process's
 * lock (chroot, P39), so `p`'s is read there and the reader's through
 * vfs_current_root. Only addresses are compared. */
static bool proc_same_root(struct process *p)
{
    const struct process *me = process_current();
    /* A NULL root is the global root and a NULL namespace the initial
     * one (process.h): compare what each resolves against, so an
     * explicit global root is the same as the implicit one. */
    struct vnode *global = vfs_root(), *mine = vfs_current_root();
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    const struct vnode *theirs = p->root ? p->root : global;
    bool same = mine == theirs;
    spin_unlock_irqrestore(&p->lock, s);
    vnode_put(mine);
    vnode_put(global);
    const struct mount_ns *myns = me && me->mntns ? me->mntns : mountns_initial();
    const struct mount_ns *theirns = p->mntns ? p->mntns : mountns_initial();
    return same && myns == theirns;
}

/*
 * A link's target: `cwd` the working directory's path, `root` "/", `exe`
 * the executable's traversed name from the exec record. -EACCES when the
 * reader may not look inside (P5); -ENOENT for an exited process, for an
 * image that has no name (the boot archive's), and for a process whose
 * root or mounts differ from the reader's -- its paths do not name the
 * reader's files, and this kernel cannot say where its root is in the
 * reader's tree (inventory).
 */
static int link_target(enum proc_kind kind, pid_t pid, char *buf, size_t n)
{
    struct process *p = proc_get_visible(pid);
    if (p == NULL)
        return -ENOENT;
    int rc = 0;
    if (!proc_may_inspect(p))
        rc = -EACCES;
    else if (p->state == PROCESS_EXITED || !proc_same_root(p))
        rc = -ENOENT;
    else if (kind == PROC_CWD)
        process_cwd_path(p, buf, n);
    else if (kind == PROC_ROOT_LINK)
        strlcpy(buf, "/", n);
    else {
        struct exec_record *r = process_exec_record(p);
        if (r != NULL && r->exe_len != 0)
            strlcpy(buf, exec_record_exe(r), n);
        else
            rc = -ENOENT;
        exec_record_put(r);
    }
    process_put(p);
    return rc != 0 ? rc : (int)strlen(buf);
}

static int proc_link_readlink(struct vnode *vn, char *buf, size_t len)
{
    char tmp[VFS_PATH_MAX];
    int n = link_target(PROC_INO_KIND(vn->ino), PROC_INO_PID(vn->ino), tmp, sizeof(tmp));
    if (n < 0)
        return n;
    if ((size_t)n > len)
        n = (int)len;
    memcpy(buf, tmp, (size_t)n);
    return n;
}

static const struct vnode_ops proc_link_ops = {
    .readlink = proc_link_readlink,
};

/* The link is there for any process the reader may see, as Linux lists
 * it; reading it is what the rule decides (a refused link's size is 0). */
static int proc_link(struct mount *mnt, pid_t pid, enum proc_kind kind, struct vnode **out)
{
    struct process *p = proc_get_visible(pid);
    if (p == NULL)
        return -ENOENT;
    uint32_t uid = p->cred.euid, gid = p->cred.egid;
    process_put(p);
    char tmp[VFS_PATH_MAX];
    int n = link_target(kind, pid, tmp, sizeof(tmp));
    struct vnode *vn = proc_vnode(mnt, PROC_INO(pid, kind), VNODE_LNK, 0777, &proc_link_ops);
    if (vn == NULL)
        return -ENOMEM;
    vn->uid = uid;
    vn->gid = gid;
    vn->size = n > 0 ? (uint64_t)n : 0;
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
    if (len == 7 && memcmp(name, "environ", 7) == 0)
        return proc_environ(dir->mnt, pid, out);
    if (len == 3 && memcmp(name, "cwd", 3) == 0)
        return proc_link(dir->mnt, pid, PROC_CWD, out);
    if (len == 3 && memcmp(name, "exe", 3) == 0)
        return proc_link(dir->mnt, pid, PROC_EXE, out);
    if (len == 4 && memcmp(name, "root", 4) == 0)
        return proc_link(dir->mnt, pid, PROC_ROOT_LINK, out);
    return -ENOENT;   /* a file appears here only when somebody adds one (P2) */
}

static int proc_pid_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    pid_t pid = PROC_INO_PID(dir->ino);
    static const struct {
        const char *name;
        enum proc_kind kind;
        enum vnode_type type;
    } files[] = { { "status", PROC_STATUS, VNODE_REG }, { "limits", PROC_LIMITS, VNODE_REG },
                  { "fd", PROC_FD_DIR, VNODE_DIR },       { "stat", PROC_PID_STAT, VNODE_REG },
                  { "cmdline", PROC_CMDLINE, VNODE_REG }, { "environ", PROC_ENVIRON, VNODE_REG },
                  { "cwd", PROC_CWD, VNODE_LNK },         { "exe", PROC_EXE, VNODE_LNK },
                  { "root", PROC_ROOT_LINK, VNODE_LNK } };

    while (*pos < 2 + sizeof(files) / sizeof(files[0])) {
        uint64_t i = *pos;
        int rc;
        if (i == 0)
            rc = cb(arg, ".", 1, dir->ino, VNODE_DIR);
        else if (i == 1)
            rc = cb(arg, "..", 2, PROC_ROOT_INO, VNODE_DIR);
        else
            rc = cb(arg, files[i - 2].name, strlen(files[i - 2].name), PROC_INO(pid, files[i - 2].kind),
                    files[i - 2].type);
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
    { "meminfo", 7, PROC_MEMINFO }, { "stat", 4, PROC_STAT },       { "loadavg", 7, PROC_LOADAVG },
    { "uptime", 6, PROC_UPTIME },   { "mounts", 6, PROC_MOUNTS },   { "version", 7, PROC_VERSION },
    { "cpuinfo", 7, PROC_CPUINFO },
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
