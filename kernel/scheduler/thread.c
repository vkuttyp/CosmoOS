/*
 * thread.c - Kernel thread lifecycle.
 */

#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/lockdep.h>
#include <kernel/thread.h>
#include <kernel/vmm.h>

#include <arch/context.h>
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
    }
}

void thread_reap_later(struct thread *t)
{
    KASSERT(t->state == THREAD_EXITED);
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
    t->state = THREAD_BLOCKED;
    t->priority = priority < 0 ? 0 : priority >= SCHED_PRIO_COUNT ? SCHED_PRIO_LOWEST : priority;
    t->affinity = CPUMASK_ALL;
    t->cpu = -1;
    t->refcount = 2;
    t->flags = flags;
    list_init(&t->rq_link);
    list_init(&t->all_link);
    list_init(&t->proc_link);
    completion_init(&t->exited, "thread-exit");
    thread_register(t);
    return t;
}

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

    t->entry = entry;
    t->arg = arg;
    arch_context_init(&t->ctx, t->stack_base + t->stack_size, thread_trampoline);
    return t;
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
        bool last = --p->nr_threads == 0;
        spin_unlock_irqrestore(&p->lock, s);
        if (last)
            process_last_thread_gone(p);
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
    arch_irq_state_t s = spin_lock_irqsave(&g_thread_list_lock);
    struct thread *t;
    kprintf("%4s %-20s %-8s %3s %3s %10s %8s %s\n", "tid", "name", "state", "pri", "cpu", "run_ms", "switch",
            "waiting_on");
    list_for_each_entry(t, &g_all_threads, all_link) {
        kprintf("%4u %-20s %-8s %3d %3d %10llu %8llu %s\n", t->tid, t->name, states[t->state], t->priority,
                t->cpu, (unsigned long long)(t->run_time_ns / 1000000), (unsigned long long)t->switches,
                t->waiting_on ? (t->waiting_on->lock.name ? t->waiting_on->lock.name : "?") : "-");
    }
    spin_unlock_irqrestore(&g_thread_list_lock, s);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(thread_create);
