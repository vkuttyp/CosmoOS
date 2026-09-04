/*
 * syscall.c - Generic system-call dispatcher.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/process.h>
#include <kernel/syscall.h>

#include <arch/irq.h>

static uint64_t g_calls;
static uint64_t g_unknown;

int64_t syscall_dispatch(uint64_t nr, const uint64_t args[6], void *frame)
{
    struct percpu *pc = this_cpu();
    struct process *p = process_current();

    KASSERT(arch_irq_enabled());
    KASSERT(pc->irq_depth == 0 && pc->preempt_count == 0);
    if (p == NULL)
        panic("system call %llu from a thread without a process", (unsigned long long)nr);

    __atomic_fetch_add(&g_calls, 1u, __ATOMIC_RELAXED);
    p->syscalls++;

    const struct personality *pers = p->pers;
    if (nr >= pers->count || pers->table[nr] == NULL) {
        __atomic_fetch_add(&g_unknown, 1u, __ATOMIC_RELAXED);
        kdebug("syscall: pid %u unknown number %llu (%s)", p->pid, (unsigned long long)nr, pers->name);
        return -ENOSYS;
    }

    struct syscall_args a = {
        .nr = nr,
        .a = { args[0], args[1], args[2], args[3], args[4], args[5] },
        .frame = frame,
    };
    return pers->table[nr](&a);
}

uint64_t syscall_count(void)
{
    return __atomic_load_n(&g_calls, __ATOMIC_RELAXED);
}

uint64_t syscall_unknown_count(void)
{
    return __atomic_load_n(&g_unknown, __ATOMIC_RELAXED);
}
