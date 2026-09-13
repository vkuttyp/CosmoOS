/*
 * tcb.c - the thread pointer, and errno behind it.
 *
 * One accessor with no cases (cosmo/tcb.h says why there cannot be one):
 * read the thread pointer, add an offset. The two architectures differ only
 * in how the pointer is read.
 */

#include <errno.h>
#include <stddef.h>

#include <cosmo/syscall.h>
#include <cosmo/tcb.h>

#include "libc.h"

/*
 * The first thread's block. Static, not mapped, on purpose: a mapping can
 * fail, and a startup path that has to handle failing to give the process
 * an `errno` has no good answer -- it cannot report the failure through the
 * thing it just failed to provide. A .bss object removes the question.
 */
static struct __cosmo_tcb main_tcb;

/*
 * x86-64 cannot read the FS base without `rdfsbase` (CR4.FSGSBASE, not
 * guaranteed), so the block's first word points at itself and this loads
 * it through the segment. AArch64 reads TPIDR_EL0 directly.
 */
static inline struct __cosmo_tcb *tcb_self(void)
{
#if defined(__x86_64__)
    struct __cosmo_tcb *t;
    __asm__("movq %%fs:0, %0" : "=r"(t));
    return t;
#elif defined(__aarch64__)
    return (struct __cosmo_tcb *)__builtin_thread_pointer();
#else
#error "no thread-pointer read for this architecture"
#endif
}

int *__errno_location(void)
{
    return &tcb_self()->err;
}

/*
 * The calling thread's id, cached in its block. Zero means "not asked
 * yet", which is unambiguous because no thread's id is ever zero: a
 * process's first thread answers its pid and every other answers
 * 0x10000 + tid.
 *
 * Lazy rather than filled at install time, for two reasons that both came
 * out of review. A tid read during `__libc_start` would make the thread
 * pointer's installation two syscalls rather than one, and the second
 * would not be in the filter's always-allowed set -- so a child of a
 * filtered process died in startup on `SYS_thread_self` instead of on
 * whatever it was confined for. And a thread that never asks for its id
 * should not pay a syscall to be told it.
 */
unsigned __cosmo_tcb_tid(void)
{
    struct __cosmo_tcb *t = tcb_self();
    if (t->tid == 0)
        t->tid = (unsigned)cosmo_thread_self();
    return t->tid;
}

/*
 * Point the calling thread at `blk`, whose `self` word this writes because
 * x86-64's accessor depends on it. The tid is cached here rather than on
 * first use: a thread that has a block has always had one, so there is no
 * "not yet cached" value to distinguish from a real tid.
 */
static int tcb_use(struct __cosmo_tcb *blk)
{
    blk->self = blk;
    blk->err = 0;
    blk->tid = 0;   /* asked for on first use; see __cosmo_tcb_tid */
    return (int)cosmo_set_tls((unsigned long long)(unsigned long)blk);
}

/*
 * Called by __libc_start before anything else, including __stdio_init and
 * anything that could set errno. Returns 0, or what SYS_set_tls refused --
 * which for a linked static object it cannot, since the address is aligned
 * and inside the program's own image; the caller has the policy for the
 * case that cannot happen.
 */
int __cosmo_tcb_init(void)
{
    return tcb_use(&main_tcb);
}

int cosmo_tcb_install(void *block, size_t len)
{
    if (block == NULL || len < COSMO_TCB_SIZE)
        return -EINVAL;
    if ((unsigned long)block % 16u)
        return -EINVAL;
    return tcb_use((struct __cosmo_tcb *)block);
}
