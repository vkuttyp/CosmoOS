/* thread.c - native threads and a futex mutex (docs/kernel/process/design.md). */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cosmo/thread.h>

#include "libc.h"

#define STACK_DEFAULT (64u * 1024u)
#define PAGE          4096u

/*
 * The trampoline. The kernel enters `entry` with one argument and no
 * return address, so a thread must not return; this calls the caller's
 * function and hands its result to cosmo_thread_finish, which is what
 * makes "never return from fn" true of every thread this library makes
 * rather than a rule its callers have to keep.
 */
static void thread_trampoline(void *arg)
{
    cosmo_thread_t *t = arg;
    void *ret = t->fn(t->arg);
    /* Published with release, read with acquire in the join: the joiner can
     * see the kernel's zero in `done` without ever entering futex_wait --
     * a thread that has already exited -- and then nothing else would
     * order this store before that read. */
    __atomic_store_n(&t->ret, ret, __ATOMIC_RELEASE);
    cosmo_thread_finish(ret);
}

void cosmo_thread_finish(void *ret)
{
    (void)ret;
    cosmo_thread_exit(0);
}

cosmo_tid_t cosmo_thread_id(void)
{
    return (cosmo_tid_t)cosmo_thread_self();
}

int cosmo_thread_start(cosmo_thread_t *t, void *(*fn)(void *), void *arg, size_t stack_size)
{
    if (t == NULL || fn == NULL)
        return -EINVAL;
    size_t size = stack_size ? stack_size : STACK_DEFAULT;
    size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);

    /*
     * One page of guard below the stack: the kernel gives a thread stack
     * none, so a library that allocates one must. There is no mprotect
     * syscall -- the kernel has vm_user_protect but nothing asks it -- so
     * the guard costs a reservation, a hole punched in it, and a fixed map
     * into the hole. The lowest page keeps the reservation's PROT_NONE,
     * and an overflow faults there instead of writing whatever lies below.
     */
    char *base = mmap(NULL, size + PAGE, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED)
        return -ENOMEM;
    if (munmap(base + PAGE, size) != 0) {
        munmap(base, size + PAGE);
        return -ENOMEM;
    }
    if (mmap(base + PAGE, size, PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED) {
        munmap(base, PAGE);
        return -ENOMEM;
    }

    memset(t, 0, sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    t->stack = base;
    t->stack_size = size + PAGE;

    unsigned long top = (unsigned long)base + PAGE + size;
    top &= ~15ul;   /* the kernel requires 16-byte alignment */
    struct cosmo_thread req = {
        .entry = (unsigned long)thread_trampoline,
        .arg = (unsigned long)t,
        .stack_top = top,
        .tls = 0,
        .clear_tid = (unsigned long)&t->done,
        .flags = 0,
        .reserved = 0,
    };
    long rc = cosmo_thread_create(&req);
    if (rc < 0) {
        munmap(base, size + PAGE);
        t->stack = NULL;
        return (int)rc;
    }
    t->tid = (cosmo_tid_t)rc;
    return 0;
}

int cosmo_thread_join(cosmo_thread_t *t, void **ret)
{
    if (t == NULL || t->stack == NULL)
        return -EINVAL;
    /* `done` held the tid before the thread could run and is zeroed and
     * futex-woken when it exits, so this is the whole of joining: read it,
     * and wait on it while it is still non-zero. */
    for (;;) {
        unsigned v = __atomic_load_n(&t->done, __ATOMIC_ACQUIRE);
        if (v == 0)
            break;
        long rc = cosmo_futex_wait(&t->done, v, 0);
        if (rc < 0 && rc != -EAGAIN && rc != -EINTR)
            return (int)rc;
    }
    if (ret)
        *ret = __atomic_load_n(&t->ret, __ATOMIC_RELAXED);   /* ordered by the acquire above */
    munmap(t->stack, t->stack_size);
    t->stack = NULL;
    return 0;
}

/* --- a mutex over the futex ------------------------------------------------ */

static unsigned cas(volatile unsigned *p, unsigned expect, unsigned want)
{
    __atomic_compare_exchange_n(p, &expect, want, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expect;   /* what was there */
}

void cosmo_mutex_lock(cosmo_mutex_t *m)
{
    unsigned c = cas(&m->state, 0, 1);
    if (c == 0)
        return;                        /* uncontended: one atomic, no syscall */
    /*
     * Contended. From here the lock is *always* taken by exchanging 2 in,
     * never 1, so "held, and a waiter may exist" survives the handover and
     * the next unlock wakes. Taking it with 1 instead -- which this
     * library did until a review -- strands a waiter whenever three or more
     * contend: the winner leaves 1 behind, the unlock sees 1 and wakes
     * nobody, and a thread already asleep on 2 is never called again. The
     * cost of the conservative 2 is one futex_wake with no waiter.
     */
    if (c != 2)
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQ_REL);
    while (c != 0) {
        cosmo_futex_wait(&m->state, 2, 0);
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQ_REL);
    }
}

void cosmo_mutex_unlock(cosmo_mutex_t *m)
{
    if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_ACQ_REL) != 1) {
        __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
        cosmo_futex_wake(&m->state, 1);
    }
}

int cosmo_mutex_trylock(cosmo_mutex_t *m)
{
    return cas(&m->state, 0, 1) == 0 ? 0 : -EBUSY;
}
