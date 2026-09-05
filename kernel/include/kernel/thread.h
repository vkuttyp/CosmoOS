/*
 * thread.h - Kernel threads.
 *
 * Lifetime is reference counted: thread_create returns with two
 * references (the thread's own, dropped after it exits and is switched
 * away from; and the creator's, dropped by thread_join or thread_put).
 * Stack and struct are freed when the count reaches zero, never by the
 * thread itself.
 *
 * Concurrency: fields marked (rq) change only under the run-queue lock
 * of t->cpu; the rest are written by the thread itself or at creation.
 */

#ifndef KERNEL_THREAD_H
#define KERNEL_THREAD_H

#include <kernel/completion.h>
#include <kernel/lockdep_core.h>
#include <kernel/list.h>
#include <kernel/percpu.h>
#include <kernel/types.h>

#include <arch/context.h>
#include <arch/fpu.h>

typedef uint32_t tid_t;

#define THREAD_NAME_MAX   32
#define THREAD_STACK_SIZE (16u * 1024u)

#define SCHED_PRIO_COUNT   64
#define SCHED_PRIO_HIGHEST 0
#define SCHED_PRIO_DEFAULT 32
#define SCHED_PRIO_LOWEST  (SCHED_PRIO_COUNT - 1)

enum thread_state {
    THREAD_READY,     /* on a run queue */
    THREAD_RUNNING,   /* current on some CPU */
    THREAD_BLOCKED,   /* waiting; not on a run queue */
    THREAD_EXITED,    /* finished; awaiting reap */
};

#define THREAD_FLAG_IDLE  (1u << 0)  /* a CPU's idle thread */
#define THREAD_FLAG_BOOT  (1u << 1)  /* thread 0: stack is the boot stack */

struct waitqueue;
struct process;

struct thread {
    tid_t tid;
    char name[THREAD_NAME_MAX];
    struct process *proc;               /* NULL for kernel threads; holds a process ref */
    struct list_node proc_link;         /* in process.threads, under process.lock */
    uintptr_t user_entry;               /* first user-mode instruction (user threads) */
    uintptr_t user_sp;                  /* initial user stack pointer */
    uintptr_t tls_base;                 /* user FS base (arch_prctl ARCH_SET_FS), restored on every switch to user */
    struct arch_fpu_state *fpu;         /* vector/x87 state this thread owns, or NULL (arch/fpu.h); saved and
                                           restored by the arch switch hook, freed with the thread */
    enum thread_state state;            /* (rq) */
    struct arch_context ctx;
    vaddr_t stack_base;
    size_t stack_size;
    void (*entry)(void *arg);
    void *arg;
    int priority;                       /* (rq) */
    cpumask_t affinity;
    int cpu;                            /* (rq) run queue this thread belongs to */
    uint64_t slice_left_ns;             /* (rq) */
    uint64_t run_time_ns;               /* (rq) */
    uint64_t last_start_ns;             /* (rq) */
    uint64_t switches;                  /* (rq) times switched in */
    struct list_node rq_link;           /* (rq) */
    struct list_node all_link;          /* global list, under thread_list_lock */
    struct waitqueue *waiting_on;       /* diagnostics */
    struct completion exited;
    int exit_code;
    uint32_t refcount;
    unsigned flags;
    struct lockdep_held held_mutex[LOCKDEP_MAX_HELD_MUTEX];   /* lockdep: mutexes this thread holds */
    unsigned nr_held_mutex;
};

/* Create a kernel thread and make it runnable. NULL on allocation
 * failure. `name` is copied. */
struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority);

/* Same, restricted to the CPUs in `affinity` (must include at least one
 * online CPU, else NULL). */
struct thread *thread_create_on(void (*entry)(void *arg), void *arg, const char *name, int priority,
                                cpumask_t affinity);

/* Terminate the calling thread. Never returns. */
void thread_exit(int code) __noreturn;

/* Wait for `t` to exit, drop the creator's reference, return its code. */
int thread_join(struct thread *t);

void thread_get(struct thread *t);
void thread_put(struct thread *t);

static inline struct thread *thread_current(void)
{
    return this_cpu()->current;
}

/* True if the address lies inside `t`'s stack (backtrace validation). */
bool thread_stack_contains(const struct thread *t, uintptr_t addr);

/* Diagnostics. */
void thread_dump_all(void);
unsigned thread_count(void);

#endif /* KERNEL_THREAD_H */
