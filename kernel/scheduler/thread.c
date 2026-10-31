/*
 * thread.c - Kernel thread lifecycle.
 */

#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/process.h>
#include <kernel/random.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/lockdep.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>

#include <arch/context.h>
#include <arch/cpu.h>
#include <arch/irq.h>

#include "sched_internal.h"

static struct kmem_cache *g_thread_cache;
static LIST_HEAD(g_all_threads);
static spinlock_t g_thread_list_lock = SPINLOCK_INIT("thread_list");
static tid_t g_next_tid = 1;
static unsigned g_thread_count;

static void thread_trampoline(void)
{
    struct thread *self = thread_current();

    /* We arrive here from schedule() with its run-queue lock held and
     * interrupts disabled, exactly like a thread resuming inside
     * schedule(); finish that switch, then run. */
    sched_finish_switch();
    arch_irq_enable();

    self->entry(self->arg);
    thread_exit(0);
}

void thread_init_subsystem(void)
{
    g_thread_cache = kmem_cache_create("thread", sizeof(struct thread), 64);
    if (g_thread_cache == NULL)
        panic("thread: cannot create thread cache");
}

/* --- reaper ---
 * An exited thread's last reference may be dropped from
 * sched_finish_switch, which runs with interrupts disabled and cannot
 * perform the TLB shootdown that freeing a stack requires. Exited
 * threads are therefore handed to a kernel thread that frees them in
 * ordinary context. The rq_link is reused for the reap list. */

static LIST_HEAD(g_reap_list);
static spinlock_t g_reap_lock = SPINLOCK_INIT("reap");
static struct waitqueue g_reap_wq = WAITQUEUE_INIT(g_reap_wq);

static bool reap_pending(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_reap_lock);
    bool pending = !list_empty(&g_reap_list);
    spin_unlock_irqrestore(&g_reap_lock, s);
    return pending;
}

static unsigned g_reaps_queued, g_reaps_done;   /* atomic; thread_reaper_idle */

static void reaper_main(void *arg)
{
    (void)arg;
    for (;;) {
        wait_event(&g_reap_wq, reap_pending());

        arch_irq_state_t s = spin_lock_irqsave(&g_reap_lock);
        struct list_node *n = list_pop_front(&g_reap_list);
        spin_unlock_irqrestore(&g_reap_lock, s);
        if (n == NULL)
            continue;
        thread_put(list_entry(n, struct thread, rq_link));
        __atomic_add_fetch(&g_reaps_done, 1, __ATOMIC_RELEASE);   /* after the stack's free and its shootdown */
    }
}

bool thread_reaper_idle(void)
{
    return __atomic_load_n(&g_reaps_done, __ATOMIC_ACQUIRE) == __atomic_load_n(&g_reaps_queued, __ATOMIC_ACQUIRE);
}

void thread_reap_later(struct thread *t)
{
    KASSERT(t->state == THREAD_EXITED);
    __atomic_add_fetch(&g_reaps_queued, 1, __ATOMIC_RELEASE);
    arch_irq_state_t s = spin_lock_irqsave(&g_reap_lock);
    list_push_back(&g_reap_list, &t->rq_link);
    spin_unlock_irqrestore(&g_reap_lock, s);
    waitqueue_wake_all(&g_reap_wq);
}

void thread_reaper_start(void)
{
    struct thread *r = thread_create(reaper_main, NULL, "reaper", SCHED_PRIO_DEFAULT - 8);
    if (r == NULL)
        panic("thread: cannot create the reaper");
    thread_put(r); /* detached: nobody joins it */
}

static void thread_register(struct thread *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_thread_list_lock);
    t->tid = g_next_tid++;
    list_push_back(&g_all_threads, &t->all_link);
    g_thread_count++;
    spin_unlock_irqrestore(&g_thread_list_lock, s);
}

static void thread_unregister(struct thread *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_thread_list_lock);
    list_remove(&t->all_link);
    g_thread_count--;
    spin_unlock_irqrestore(&g_thread_list_lock, s);
}

/* Common field setup; stack and context are the caller's. */
struct thread *thread_alloc(const char *name, int priority, unsigned flags)
{
    struct thread *t = kmem_cache_alloc(g_thread_cache, KMEM_ZERO);
    if (t == NULL)
        return NULL;

    strlcpy(t->name, name ? name : "?", sizeof(t->name));
    /* The stack protector's canary (BUILD-1), fixed for the thread's life.
     * Thread 0 is the boot context made a thread, its frames already live
     * under the boot canary, so it keeps that one. Low byte zero, as
     * glibc's. */
    t->stack_canary = (flags & THREAD_FLAG_BOOT) ? this_cpu()->stack_canary : random_u64() & ~0xFFull;
    t->state = THREAD_BLOCKED;
    t->priority = priority < 0 ? 0 : priority >= SCHED_PRIO_COUNT ? SCHED_PRIO_LOWEST : priority;
    t->base_prio = t->priority;   /* the effective priority starts at the base; inheritance may lower the number */
    list_init(&t->pi_held);
    t->pi_blocked_on = NULL;
    t->affinity = CPUMASK_ALL;
    t->cpu = -1;
    t->refcount = 2;
    t->bal_gap_min = THREAD_BAL_GAP_NONE;
    t->flags = flags;
    list_init(&t->rq_link);
    list_init(&t->all_link);
    list_init(&t->proc_link);
    completion_init(&t->exited, "thread-exit");
    thread_register(t);
    return t;
}

#if CONFIG_DEBUG
/*
 * Stack high-water marks, debug builds (docs/kernel/scheduler/design.md,
 * "Stack use"). Every stack thread_prepare allocates is painted with
 * STACK_PAINT, and the lowest word that no longer holds it is as deep as
 * the stack has ever been used. Each thread is measured when it is freed
 * (thread_put) and the ones still alive at the end of the boot
 * (thread_report_stack_use), so every thread the boot made is counted
 * once at least; the deepest few are reported, and any over 75 % of its
 * stack is a warning the boot harness fails on.
 */
#define STACK_PAINT 0x5354414b5354414bull   /* "KATSKATS" */
#define STACK_TOP_N 5

struct stack_mark {
    size_t used;
    char name[THREAD_NAME_MAX];
};
static spinlock_t g_stack_lock = SPINLOCK_INIT("stack_marks");
static struct stack_mark g_stack_top[STACK_TOP_N];   /* deepest first, one per name */
static unsigned g_stack_measured, g_stack_over;

static void stack_paint(struct thread *t)
{
    uint64_t *w = (uint64_t *)t->stack_base;
    for (size_t i = 0; i < t->stack_size / sizeof(*w); i++)
        w[i] = STACK_PAINT;
}

static bool stack_painted(const struct thread *t)
{
    return t->stack_base != 0 && (t->flags & THREAD_FLAG_BOOT) == 0;
}

/* Bytes from the top of the stack down to the deepest word written. A
 * running thread's stack can be read while it changes: the answer is
 * then a lower bound, which is what a high-water mark taken early is. */
static size_t stack_used(const struct thread *t)
{
    const volatile uint64_t *w = (const volatile uint64_t *)t->stack_base;
    size_t n = t->stack_size / sizeof(uint64_t), i = 0;
    while (i < n && w[i] == STACK_PAINT)
        i++;
    return (n - i) * sizeof(uint64_t);
}

/* Any context that may take a spinlock; takes g_stack_lock. */
static void stack_note(const struct thread *t)
{
    if (!stack_painted(t))
        return;
    size_t used = stack_used(t);
    bool over = used * 4 > t->stack_size * 3;
    arch_irq_state_t s = spin_lock_irqsave(&g_stack_lock);
    g_stack_measured++;
    if (over)
        g_stack_over++;
    /* One slot per name: the deepest a thread of that name went. */
    unsigned at = STACK_TOP_N;
    for (unsigned i = 0; i < STACK_TOP_N && g_stack_top[i].used != 0; i++) {
        if (strcmp(g_stack_top[i].name, t->name) == 0) {
            at = i;
            break;
        }
    }
    if (at == STACK_TOP_N || used > g_stack_top[at].used) {
        if (at == STACK_TOP_N)
            at = STACK_TOP_N - 1;   /* evict the shallowest, if this is deeper */
        if (used > g_stack_top[at].used) {
            g_stack_top[at].used = used;
            strlcpy(g_stack_top[at].name, t->name, THREAD_NAME_MAX);
            for (; at > 0 && g_stack_top[at].used > g_stack_top[at - 1].used; at--) {
                struct stack_mark m = g_stack_top[at];
                g_stack_top[at] = g_stack_top[at - 1];
                g_stack_top[at - 1] = m;
            }
        }
    }
    spin_unlock_irqrestore(&g_stack_lock, s);
    if (over)
        kwarn("stack: thread %u '%s' used %zu of %zu bytes, over 75%%", t->tid, t->name, used, t->stack_size);
}

void thread_report_stack_use(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_thread_list_lock);
    struct thread *t;
    list_for_each_entry(t, &g_all_threads, all_link)
        stack_note(t);
    spin_unlock_irqrestore(&g_thread_list_lock, s);

    s = spin_lock_irqsave(&g_stack_lock);
    struct stack_mark top[STACK_TOP_N];
    memcpy(top, g_stack_top, sizeof(top));
    unsigned measured = g_stack_measured, over = g_stack_over;
    spin_unlock_irqrestore(&g_stack_lock, s);
    kinfo("stack: high-water %zu of %u bytes (%zu%%) on '%s'; %u stacks measured, %u over 75%%", top[0].used,
          THREAD_STACK_SIZE, top[0].used * 100 / THREAD_STACK_SIZE, top[0].name, measured, over);
    for (unsigned i = 1; i < STACK_TOP_N && top[i].used != 0; i++)
        kinfo("stack:   then %zu bytes on '%s'", top[i].used, top[i].name);
}
#else
static inline void stack_paint(struct thread *t) { (void)t; }
static inline void stack_note(const struct thread *t) { (void)t; }
#endif

struct thread *thread_prepare(void (*entry)(void *arg), void *arg, const char *name, int priority,
                              unsigned flags)
{
    KASSERT(g_thread_cache != NULL);

    struct thread *t = thread_alloc(name, priority, flags);
    if (t == NULL)
        return NULL;

    t->stack_size = THREAD_STACK_SIZE;
    t->stack_base = vm_kernel_alloc(t->stack_size, VM_KALLOC_GUARD | VM_KALLOC_POPULATE, VM_PROT_RW);
    if (t->stack_base == 0) {
        thread_unregister(t);
        kmem_cache_free(g_thread_cache, t);
        return NULL;
    }
    stack_paint(t);

    t->entry = entry;
    t->arg = arg;
    arch_context_init(&t->ctx, t->stack_base + t->stack_size, thread_trampoline);
    return t;
}

/*
 * Affinity is written under the run-queue lock of the CPU the thread is
 * on, which is the lock every reader that decides a move holds
 * (sched_migrate, pick_migratable): one discipline, no mixed access. A
 * thread on the move has its `t->cpu` rewritten under both locks, so
 * the loop re-reads it after locking, as sched_migrate does.
 */
static void set_affinity_locked(struct thread *t, cpumask_t affinity)
{
    for (;;) {
        arch_irq_state_t s = arch_irq_save();
        int cpu = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        KASSERT(cpu >= 0);
        struct runqueue *rq = sched_runqueue((unsigned)cpu);
        spin_lock(&rq->lock);
        if (t->cpu != cpu) {
            spin_unlock(&rq->lock);
            arch_irq_restore(s);
            continue;
        }
        KASSERT((affinity & CPUMASK_OF((unsigned)cpu)) != 0);
        t->affinity = affinity;
        spin_unlock(&rq->lock);
        arch_irq_restore(s);
        return;
    }
}

cpumask_t thread_pin_self(void)
{
    /* Interrupts off from reading the CPU to writing the mask: the mask
     * must name the CPU this thread is on when it takes effect, and a
     * running thread with interrupts off cannot be moved (it is
     * `rq->current`, and only READY threads move). */
    arch_irq_state_t s = arch_irq_save();
    struct thread *t = thread_current();
    cpumask_t old = t->affinity;
    set_affinity_locked(t, CPUMASK_OF(arch_cpu_id()));
    arch_irq_restore(s);
    return old;
}

void thread_set_affinity_self(cpumask_t affinity)
{
    set_affinity_locked(thread_current(), affinity);
}

void thread_set_affinity(struct thread *t, cpumask_t affinity)
{
    set_affinity_locked(t, affinity);
}

void thread_set_nice(struct thread *t, int nice)
{
    __atomic_store_n(&t->nice, nice < -20 ? -20 : nice > 19 ? 19 : nice, __ATOMIC_RELAXED);
}

struct thread *thread_create_on(void (*entry)(void *arg), void *arg, const char *name, int priority,
                                cpumask_t affinity)
{
    if ((affinity & cpu_online_mask()) == 0)
        return NULL;
    struct thread *t = thread_prepare(entry, arg, name, priority, 0);
    if (t == NULL)
        return NULL;
    t->affinity = affinity;
    sched_enqueue_new(t);
    return t;
}

struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{
    return thread_create_on(entry, arg, name, priority, CPUMASK_ALL);
}

void thread_exit(int code)
{
    struct thread *self = thread_current();
    KASSERT((self->flags & (THREAD_FLAG_IDLE | THREAD_FLAG_BOOT)) == 0);
    KASSERT(preemptible());
    lockdep_thread_exit(self);

    self->exit_code = code;
    complete(&self->exited);

    arch_irq_disable();
    self->state = THREAD_EXITED;
    schedule();
    panic("thread_exit: exited thread '%s' was scheduled again", self->name);
}

int thread_join(struct thread *t)
{
    KASSERT(t != thread_current());
    wait_for_completion(&t->exited);
    int code = t->exit_code;
    thread_put(t);
    return code;
}

void thread_get(struct thread *t)
{
    uint32_t old = __atomic_fetch_add(&t->refcount, 1u, __ATOMIC_ACQ_REL);
    KASSERT(old > 0);
}

void thread_put(struct thread *t)
{
    uint32_t old = __atomic_fetch_sub(&t->refcount, 1u, __ATOMIC_ACQ_REL);
    KASSERT(old > 0);
    if (old != 1)
        return;

    KASSERT(t->state == THREAD_EXITED);
    KASSERT(t != thread_current());
    thread_unregister(t);
    arch_fpu_free(t);
    kfree(t->sig_info);
    kfree(t->init_regs);
    stack_note(t);   /* debug builds: the high-water mark, before the stack goes */
    if (t->stack_base != 0 && (t->flags & THREAD_FLAG_BOOT) == 0)
        vm_kernel_free(t->stack_base);

    /* A user thread holds a reference on its process. Leaving the
     * process's thread list here (reaper context, interrupts enabled)
     * is what lets the last thread's departure tear the process down
     * on a stack and CR3 that were never the dead thread's. */
    struct process *p = t->proc;
    if (p != NULL) {
        arch_irq_state_t s = spin_lock_irqsave(&p->lock);
        list_remove(&t->proc_link);
        p->cputime.gone_run_ns += t->run_time_ns;   /* final: charged at its last switch-out */
        bool last = --p->nr_threads == 0;
        bool exec_waits = p->exec_thread != NULL;
        spin_unlock_irqrestore(&p->lock, s);
        if (exec_waits)
            waitqueue_wake_all(&p->exec_wq);   /* an exec waits for its siblings to be gone */
        if (last)
            process_last_thread_gone(p);
#if CONFIG_DEBUG
        if (last)
            process_test_reap_hook(p);   /* a zombie now: a test may hold its release here */
#endif
        process_put(p);
    }
    kmem_cache_free(g_thread_cache, t);
}

bool thread_stack_contains(const struct thread *t, uintptr_t addr)
{
    return t != NULL && addr >= t->stack_base && addr < t->stack_base + t->stack_size;
}

unsigned thread_count(void)
{
    return g_thread_count;
}

void thread_dump_all(void)
{
    static const char *const states[] = { "ready", "running", "blocked", "exited" };
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_thread_list_lock);
    struct thread *t;
    kprintf("%4s %-20s %-8s %3s %3s %10s %8s %s\n", "tid", "name", "state", "pri", "cpu", "run_ms", "switch",
            "waiting_on");
    list_for_each_entry(t, &g_all_threads, all_link) {
        /* run_time_ns is charged at switch-out: the running thread's
         * current stretch is added here so the figure is live. */
        uint64_t run = t->run_time_ns;
        if (t->state == THREAD_RUNNING)
            run += clock_delta_ns(now, t->last_start_ns);
        kprintf("%4u %-20s %-8s %3d %3d %10llu %8llu %s\n", t->tid, t->name, states[t->state], t->priority,
                t->cpu, (unsigned long long)(run / 1000000), (unsigned long long)t->switches,
                t->waiting_on ? (t->waiting_on->lock.name ? t->waiting_on->lock.name : "?") : "-");
    }
    spin_unlock_irqrestore(&g_thread_list_lock, s);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(thread_create);
EXPORT_SYMBOL(thread_exit);
EXPORT_SYMBOL(thread_join);
