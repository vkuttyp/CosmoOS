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

enum process_state { PROCESS_RUNNING, PROCESS_EXITING, PROCESS_EXITED };

struct syscall_args;
typedef int64_t (*syscall_fn)(struct syscall_args *a);

struct personality {
    const char *name;
    const syscall_fn *table;
    unsigned count;
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
    struct vnode *cwd;                 /* referenced */
    char cwd_path[1024];               /* VFS_PATH_MAX; normalised absolute path of cwd */

    /* Phase 11: the Linux personality's state (NULL for native processes). */
    struct linux_state *linux;
    uint64_t image_end;                /* page after the highest loaded segment (brk starts here) */
};

/* How spawn builds a child (kernel creators pass NULL: console handles
 * 0-2, root working directory, no parent). */
struct process_handle_map {
    int child;
    int parent;
};
struct process_spawn_attr {
    struct process *parent;                        /* the calling process */
    const struct process_handle_map *handles;      /* validated by the caller */
    unsigned nr_handles;                           /* 0 with handles NULL: inherit 0, 1, 2 */
    struct vnode *cwd;                             /* NULL: the parent's */
    const char *cwd_path;
};

/* One-time setup (process table, caches). Requires sched_init. */
void process_init(void);

/* Build a process from a static ELF image in memory and start its main
 * thread. `argv`/`envp` are NULL-terminated arrays (may be NULL).
 * Returns 0 and a referenced process, or -ENOEXEC/-ENOMEM/-EINVAL. */
int process_create_from_elf(const void *image, size_t size, const char *name, const char *const argv[],
                            const char *const envp[], const struct process_spawn_attr *attr, struct process **out);

/* Phase 9: create from an executable file, on behalf of the calling
 * process (kernel/process/spawn.c). `handles` must be validated as the
 * caller's; `cwd` may be NULL. Returns 0 and the child's pid. */
int process_spawn(const char *path, const char *const argv[], const char *const envp[],
                  const struct process_handle_map *handles, unsigned nr_handles, const char *cwd, pid_t *pid_out);

/* Collect an exited child: pid > 0 for that child, -1 for any. Returns
 * 0 with *pid_out (0 when WNOHANG found none), -ECHILD, -EINTR. */
#define PROCESS_WAIT_NOHANG (1u << 0)
int process_wait_child(int pid, unsigned flags, pid_t *pid_out, int *status_out);

/* Terminate `p` asynchronously with status 128 + sig. */
void process_kill(struct process *p, int sig);
/* Delivery points: system-call boundary, return to user mode, killable waits. */
void process_check_kill(void);
void process_return_to_user(void);

/* Working directory. */
int process_chdir(const char *path);

/* Credentials of the calling process (kernel/cred.h rules; -1 keeps an id). */
int process_setresuid(int64_t ruid, int64_t euid, int64_t suid);
int process_setresgid(int64_t rgid, int64_t egid, int64_t sgid);
int process_setgroups(const uint32_t *groups, unsigned n);
int path_normalize(const char *base, const char *rel, char *out, size_t n);

/* Introspection for procinfo: fills up to `count` records, returns the total. */
struct cosmo_procinfo;
unsigned process_info(struct cosmo_procinfo *buf, unsigned count);

/* Terminate the calling process (all its threads; only one exists in
 * this phase) with `status`. Never returns. */
void process_exit(int status) __noreturn;

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
unsigned linux_auxv(struct process *p, const struct elf_info *info, uint64_t random_addr, uint64_t *w, unsigned max);

#endif /* KERNEL_PROCESS_H */
