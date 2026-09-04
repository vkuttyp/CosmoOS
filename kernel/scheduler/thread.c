/*
 * thread.c - Kernel thread lifecycle.
 */

#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/string.h>
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

struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{
    struct thread *t = thread_prepare(entry, arg, name, priority, 0);
    if (t == NULL)
        return NULL;
    sched_enqueue_new(t);
    return t;
}

void thread_exit(int code)
{
    struct thread *self = thread_current();
    KASSERT((self->flags & (THREAD_FLAG_IDLE | THREAD_FLAG_BOOT)) == 0);
    KASSERT(preemptible());

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
    if (t->stack_base != 0 && (t->flags & THREAD_FLAG_BOOT) == 0)
        vm_kernel_free(t->stack_base);
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
    kprintf("%4s %-20s %-8s %3s %3s %10s %8s\n", "tid", "name", "state", "pri", "cpu", "run_ms", "switch");
    list_for_each_entry(t, &g_all_threads, all_link) {
        kprintf("%4u %-20s %-8s %3d %3d %10llu %8llu\n", t->tid, t->name, states[t->state], t->priority,
                t->cpu, (unsigned long long)(t->run_time_ns / 1000000), (unsigned long long)t->switches);
    }
    spin_unlock_irqrestore(&g_thread_list_lock, s);
}
