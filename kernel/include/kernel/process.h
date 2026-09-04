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
#include <kernel/handle.h>
#include <kernel/list.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <kernel/vmm.h>

typedef uint32_t pid_t;

#define PROCESS_NAME_MAX 32

/* User address-space layout (x86-64 canonical lower half, page aligned). */
#define USER_LO         VM_USER_LO
#define USER_HI         VM_USER_HI
#define USER_STACK_TOP  0x00007FFFFFFF0000ULL
#define USER_STACK_SIZE ((size_t)8 << 20)
#define USER_MMAP_BASE  0x0000100000000000ULL   /* hint-less mmap searches upward from here */

enum process_state { PROCESS_RUNNING, PROCESS_EXITING, PROCESS_EXITED };

struct credentials {
    uint32_t uid;
    uint32_t gid;
};

struct syscall_args;
typedef int64_t (*syscall_fn)(struct syscall_args *a);

struct personality {
    const char *name;
    const syscall_fn *table;
    unsigned count;
};

struct vm_space;
struct thread;

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
};

/* One-time setup (process table, caches). Requires sched_init. */
void process_init(void);

/* Build a process from a static ELF image in memory and start its main
 * thread. `argv`/`envp` are NULL-terminated arrays (may be NULL).
 * Returns 0 and a referenced process, or -ENOEXEC/-ENOMEM/-EINVAL. */
int process_create_from_elf(const void *image, size_t size, const char *name, const char *const argv[],
                            const char *const envp[], struct process **out);

/* Terminate the calling process (all its threads; only one exists in
 * this phase) with `status`. Never returns. */
void process_exit(int status) __noreturn;

/* Wait for `p` to exit and return its status. */
int process_wait_exit(struct process *p);

/* Process of the calling thread, or NULL for a kernel thread. Not
 * referenced: valid while the caller runs on that thread. */
struct process *process_current(void);

struct process *process_lookup(pid_t pid);   /* referenced or NULL */

static inline void process_get(struct process *p) { kobject_get(&p->obj); }
static inline void process_put(struct process *p) { kobject_put(&p->obj); }

/* Called by thread_put when the last thread of a process is reaped. */
void process_last_thread_gone(struct process *p);

unsigned process_count(void);
void process_dump_all(void);

extern const struct personality personality_native;

#endif /* KERNEL_PROCESS_H */
