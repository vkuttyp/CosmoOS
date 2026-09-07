/*
 * process.h - Processes: an address space, a handle table, credentials,
 * threads, and an exit status.
 *
 * Lifetime is a kobject reference count. The process table holds one
 * reference while the process is alive; each thread of the process
 * holds one; process_create_from_elf returns one to the creator.
 * Lock order: process_table.lock -> process.lock -> handle_table.lock;
 * process.lock -> vm_space.lock.
 */

#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <kernel/completion.h>
#include <kernel/cred.h>
#include <kernel/rlimit.h>
#include <kernel/handle.h>
#include <kernel/list.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <kernel/wait.h>

typedef uint32_t pid_t;

#define PROCESS_NAME_MAX 32

/* User address-space layout (the canonical lower half on both targets, page aligned). */
#define USER_LO         VM_USER_LO
#define USER_HI         VM_USER_HI
#define USER_STACK_TOP  0x00007FFFFFFF0000ULL
#define USER_STACK_SIZE ((size_t)8 << 20)
#define USER_MMAP_BASE  0x0000100000000000ULL   /* hint-less mmap searches upward from here */
#define USER_PIE_BASE    0x0000555500000000ULL   /* an ET_DYN executable's load bias (milestone 10) */
#define USER_INTERP_BASE 0x00007F0000000000ULL   /* the interpreter: first free range at or above */

enum process_state { PROCESS_RUNNING, PROCESS_EXITING, PROCESS_EXITED };

struct syscall_args;
typedef int64_t (*syscall_fn)(struct syscall_args *a);

struct thread;
struct arch_user_regs;
struct sigaction_k;
struct signal_info;
struct personality {
    const char *name;
    const syscall_fn *table;
    unsigned count;
    /* Milestone 10 (docs/kernel/process/design.md §11). Both optional. */
    int (*signal_frame)(struct arch_user_regs *regs, const struct sigaction_k *act, const struct signal_info *info,
                        uint64_t blocked_before);   /* build a handler frame; NULL: handlers cannot run */
    void (*thread_exit)(struct thread *t);          /* a thread of this personality is leaving */
};

struct vm_space;
struct thread;
struct linux_state;   /* compat/linux: per-process state of the Linux personality */

struct process {
    struct kobject obj;
    pid_t pid;
    pid_t parent_pid;
    char name[PROCESS_NAME_MAX];
    struct vm_space *space;
    struct handle_table handles;
    struct list_node threads;          /* struct thread.proc_link */
    unsigned nr_threads;
    struct credentials cred;
    struct rlimits rlim;               /* kernel/rlimit.h; written under lock by the process itself */
    uint32_t log_tokens;               /* sys_log rate limit for unprivileged callers */
    uint64_t log_refill_ns;
    const struct personality *pers;
    enum process_state state;
    int exit_status;
    struct completion exited;
    spinlock_t lock;
    struct list_node all_link;         /* process table */
    uint64_t syscalls;                 /* diagnostics */

    /* Phase 9 (docs/kernel/process/design.md, "Phase 9 additions") */
    struct process *parent;            /* referenced; NULL: kernel-created or orphan of a dead init */
    struct list_node children;         /* struct process.sibling, under this->lock */
    struct list_node sibling;
    struct waitqueue child_wq;         /* the parent waits here */
    bool reaped;                       /* status collected, or nobody will */
    int kill_sig;                      /* 0, or the signal terminating the process */
    /* The root this process sees: every absolute path starts here and
     * ".." stops here. NULL means the global root. Referenced.
     * Inherited by children, and only ever tightened -- the caller of
     * spawn names the child's root in its own namespace, so a confined
     * process can only confine further (docs/kernel/security/design.md,
     * "Per-process roots"). */
    struct vnode *root;
    struct vnode *cwd;                 /* referenced */
    char cwd_path[1024];               /* VFS_PATH_MAX; normalised absolute path of cwd */

    /* Phase 11: the Linux personality's state (NULL for native processes). */
    struct linux_state *linux;
    uint64_t image_end;                /* page after the highest loaded segment (brk starts here) */
    /* Milestone 10: signals and threads (docs/kernel/process/design.md §11). */
    struct sigaction_k *sigactions;    /* SIG_MAX entries, under lock */
    uint64_t sig_shared_pending;       /* signals sent to the process, not yet taken by a thread */
    struct signal_info *sig_shared_info;
    unsigned nr_live;                  /* threads that have not exited (nr_threads counts until reaped) */
    struct thread *main_thread;        /* the first thread; its Linux tid is the pid */
    uint64_t interp_base, exec_entry;  /* dynamic executables: the interpreter's bias, the program's entry */
    char exec_path[128];               /* AT_EXECFN */
};

/* How spawn builds a child (kernel creators pass NULL: console handles
 * 0-2, root working directory, no parent). */
/* The same shape as struct cosmo_spawn_handle: the syscall copies the
 * user's array straight into this one (native.c asserts it). */
struct process_handle_map {
    int child;
    int parent;
    unsigned rights;   /* COSMO_RIGHTS_SAME, or a subset of the parent's */
    unsigned pad;
};
struct process_spawn_attr {
    struct process *parent;                        /* the calling process */
    const struct process_handle_map *handles;      /* validated by the caller */
    unsigned nr_handles;                           /* 0 with handles NULL: inherit 0, 1, 2 */
    struct vnode *cwd;                             /* NULL: the parent's */
    const char *cwd_path;
    /* The child's root (COSMO_SPAWN_SETROOT). NULL: the parent's. A
     * child cannot be given a root its parent could not name, because
     * the caller resolves the path in its own namespace first. */
    struct vnode *root;
    bool set_cred;                                 /* validated by the caller (COSMO_SPAWN_SETCRED) */
    uint32_t uid, gid;
    const struct rlimits *rlim;                    /* NULL: the parent's limits (or the defaults) */
};

/* The credentials a spawn request names for the child. */
struct process_spawn_cred {
    uint32_t uid, gid;
};

/* One-time setup (process table, caches). Requires sched_init. */
void process_init(void);

/* Build a process from a static ELF image in memory and start its main
 * thread. `argv`/`envp` are NULL-terminated arrays (may be NULL).
 * Returns 0 and a referenced process, or -ENOEXEC/-ENOMEM/-EINVAL. */
int process_create_from_elf(const void *image, size_t size, const char *name, const char *const argv[],
                            const char *const envp[], const struct process_spawn_attr *attr, struct process **out);

/* Milestone 10: the same from an executable image plus, when it names a
 * program interpreter (PT_INTERP), the interpreter's image; `path` is what
 * AT_EXECFN reports. An ET_DYN executable loads at USER_PIE_BASE, an
 * ET_DYN interpreter at the first free range at or above USER_INTERP_BASE;
 * the process starts at the interpreter's entry when there is one. An
 * executable with PT_INTERP and no interpreter image is -ENOEXEC. */
struct process_image {
    const void *data;
    size_t size;
    const char *path;
};
int process_create_from_images(const struct process_image *exe, const struct process_image *interp, const char *name,
                               const char *const argv[], const char *const envp[],
                               const struct process_spawn_attr *attr, struct process **out);

/* Phase 9: create from an executable file, on behalf of the calling
 * process (kernel/process/spawn.c). `handles` must be validated as the
 * caller's; `cwd` may be NULL. Returns 0 and the child's pid. */
int process_spawn(const char *path, const char *const argv[], const char *const envp[],
                  const struct process_handle_map *handles, unsigned nr_handles, const char *cwd, const char *root,
                  const struct process_spawn_cred *cred, pid_t *pid_out);

/* Resource limits of the calling process (docs/kernel/security/design.md §2):
 * -EINVAL for an unknown resource or a NOFILE value above the table size,
 * -EPERM for raising without privilege. */
int process_getrlimit(unsigned resource, uint64_t *value);
int process_setrlimit(unsigned resource, uint64_t value);
/* Processes whose real uid is `ruid` (the NPROC count). */
unsigned process_count_uid(uint32_t ruid);
/* One sys_log line: always for a privileged caller, else within the rate limit. */
bool process_log_permitted(void);

/* Collect an exited child: pid > 0 for that child, -1 for any. Returns
 * 0 with *pid_out (0 when WNOHANG found none), -ECHILD, -EINTR. */
#define PROCESS_WAIT_NOHANG (1u << 0)
int process_wait_child(int pid, unsigned flags, pid_t *pid_out, int *status_out);

/* Terminate `p` asynchronously with status 128 + sig. */
void process_kill(struct process *p, int sig);
/* Delivery points: system-call boundary, return to user mode, killable waits.
 * process_return_to_user takes the frame about to be returned to (a trap
 * frame) so a signal handler frame can be set up on it; the architecture's
 * trap tail calls it with interrupts off and gets them back off. */
void process_check_kill(void);
struct arch_trap_frame;
void process_return_to_user(struct arch_trap_frame *frame);

/* Working directory. */
int process_chdir(const char *path);

/* Credentials of the calling process (kernel/cred.h rules; -1 keeps an id). */
int process_setresuid(int64_t ruid, int64_t euid, int64_t suid);
int process_setresgid(int64_t rgid, int64_t egid, int64_t sgid);
int process_setgroups(const uint32_t *groups, unsigned n);
int path_normalize(const char *base, const char *rel, char *out, size_t n);

/* Introspection for procinfo: fills up to `count` records, returns the total. */
struct cosmo_procinfo;
struct credentials;
/* Fill up to `count` records and return the total that qualify: every
 * process for a privileged viewer, else those with the viewer's real uid. */
unsigned process_info(struct cosmo_procinfo *buf, unsigned count, const struct credentials *viewer);

/* Terminate the calling process, every thread of it, with `status`. Never
 * returns: the other threads leave at their next return to user mode or
 * killable wait. */
void process_exit(int status) __noreturn;
/* End only the calling thread; the last live thread's exit ends the
 * process with `status`. Never returns. */
void process_thread_exit(int status) __noreturn;
/* A further user thread in `p` (the calling process), starting with the
 * register set `regs` and the thread pointer `tls`. Returns 0 and the new
 * (referenced by the process) thread, or -ENOMEM/-EAGAIN. */
/* Create a thread of `p` that will enter user mode with `regs` and the
 * thread pointer `tls`; it is linked in the process (process_find_thread
 * sees it) but not runnable until process_thread_start. The caller fills
 * in what must be there before the thread runs (clear_child_tid), then
 * starts it, or abandons it (it then exits at once without running user
 * code). -ENOMEM, -EAGAIN (the process is exiting, or PROCESS_MAX_THREADS). */
#define PROCESS_MAX_THREADS 256
int process_add_thread(struct process *p, const struct arch_user_regs *regs, uintptr_t tls, struct thread **out);
void process_thread_start(struct thread *t);
void process_thread_abandon(struct thread *t);
/* The thread of `p` with this Linux tid (pid for the main thread), or NULL. Not referenced. */
struct thread *process_find_thread(struct process *p, uint32_t lx_tid);

/* Wait for `p` to exit and return its status. */
int process_wait_exit(struct process *p);

/* Process of the calling thread, or NULL for a kernel thread. Not
 * referenced: valid while the caller runs on that thread. */
struct process *process_current(void);

struct process *process_lookup(pid_t pid);   /* referenced or NULL */

/* The system's init: orphans are reparented to it. Set once by kernel_main. */
void process_set_init(struct process *p);

static inline void process_get(struct process *p) { kobject_get(&p->obj); }
static inline void process_put(struct process *p) { kobject_put(&p->obj); }

/* Called by thread_put when the last thread of a process is reaped. */
void process_last_thread_gone(struct process *p);

unsigned process_count(void);
void process_dump_all(void);

extern const struct personality personality_native;
extern const struct personality personality_linux;   /* compat/linux/syscalls.c */

/* compat/linux hooks called by the process code. */
struct elf_info;
int linux_process_init(struct process *p, const struct elf_info *info);   /* allocate p->linux */
void linux_process_release(struct process *p);
/* Lay out the Linux auxiliary vector: returns words written into `w` (pairs). */
struct linux_auxv_args {
    uint64_t random_addr;    /* 16 bytes on the stack */
    uint64_t execfn_addr;    /* the path string on the stack */
    uint64_t platform_addr;  /* the machine string on the stack */
    uint64_t interp_base;    /* AT_BASE: the interpreter's bias, 0 without one */
};
unsigned linux_auxv(struct process *p, const struct elf_info *exe, const struct linux_auxv_args *x, uint64_t *w,
                    unsigned max);

#endif /* KERNEL_PROCESS_H */
