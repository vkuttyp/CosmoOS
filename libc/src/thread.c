/* thread.c - native threads and a futex mutex (docs/kernel/process/design.md). */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cosmo/syscall.h>
#include <cosmo/tcb.h>
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

/* From the block, not the kernel: every thread caches its own id there --
 * the first thread's when libc installs its block, a created thread's in
 * the trampoline -- so this is a load where it used to be a syscall. */
cosmo_tid_t cosmo_thread_id(void)
{
    return (cosmo_tid_t)__cosmo_tcb_tid();
}

int cosmo_thread_start(cosmo_thread_t *t, void *(*fn)(void *), void *arg, size_t stack_size)
{
    if (t == NULL || fn == NULL)
        return -EINVAL;
    /*
     * Zeroed before anything can fail, so a handle whose start was refused
     * is a handle `join` safely refuses (stack == NULL) rather than
     * indeterminate stack memory it would wait on or fault reading. The
     * mappings below can fail, and a caller that checks the return and
     * then joins the lot -- which a test did -- must not be punished for
     * it.
     */
    memset(t, 0, sizeof(*t));
    size_t size = stack_size ? stack_size : STACK_DEFAULT;
    size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);

    /*
     * One mapping carries three things: a guard page, the stack, and the
     * page holding this thread's libc block (cosmo/tcb.h) -- errno and the
     * cached tid. One mapping rather than two because the block's lifetime
     * is exactly the stack's: the join's munmap frees both, and a thread
     * cannot outlive the storage its errno lives in.
     *
     *   [ guard PAGE ][ stack `size` ][ block PAGE ]
     *
     * The guard stays directly below the stack, which is what it is for --
     * the kernel gives a thread stack none, so a library that allocates one
     * must. The guard costs a reservation of the whole span as PROT_NONE
     * and a MAP_FIXED replacement of everything above the lowest page;
     * that page keeps the reservation's PROT_NONE and an overflow faults
     * there. (This comment used to describe a hole punched in the
     * reservation and a fixed map into it, which the MAP_FIXED unit
     * removed; it was left describing the old sequence.) There is an
     * mprotect syscall now, and mapping read/write then protecting the
     * guard page would be simpler still. Kept as is: reserve-and-replace
     * was proved against four mutations in that unit, and respending the
     * proof for one syscall on a cold path is a separate decision, filed
     * in the inventory. The block sits *above* the stack, where a stack
     * that grows down never reaches it.
     */
    /*
     * The block's page also carries this thread's TLS image, so its size
     * comes from the program rather than being one page: a program with a
     * large `__thread` array needs room for a copy per thread.
     */
    size_t tcb = (__cosmo_tcb_storage() + PAGE - 1u) & ~(size_t)(PAGE - 1u);
    /*
     * Reserve, then REPLACE the upper part. The reservation is held
     * across both calls, so there is no instant when this range is
     * unowned and no `mmap(NULL, ...)` from another thread can be
     * handed it.
     *
     * This used to be reserve, punch a hole, fill it -- and the punch
     * and the fill were two syscalls with the hole unmapped between
     * them. `vm_user_find_free` looks for exactly such a gap, and the
     * fill then lost with `EEXIST` because `MAP_FIXED` refused to
     * overwrite. CI caught it three times on aarch64, and a bounded
     * retry shipped to make losing harmless. `MAP_FIXED` now replaces
     * as POSIX says (docs/audit/next-subsystem-map-fixed.md), the
     * punch is gone, and the retry with it: there is nothing left to
     * lose.
     */
    char *base = mmap(NULL, size + PAGE + tcb, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED)
        return -errno;                         /* the reservation */
    if (mmap(base + PAGE, size + tcb, PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED) {
        int e = errno;
        munmap(base, size + PAGE + tcb);       /* the reservation is still whole */
        return -e;                             /* the replacement */
    }

    t->fn = fn;
    t->arg = arg;
    t->stack = base;
    t->stack_size = size + PAGE + tcb;

    unsigned long top = (unsigned long)base + PAGE + size;
    top &= ~15ul;   /* the kernel requires 16-byte alignment */

    /*
     * The block, prepared by the creator because the new thread may touch
     * errno on its first instruction: page-aligned (so 16-aligned), with
     * the `self` word x86-64's accessor reads through %fs:0 and a zero
     * errno. The tid is left zero, which means "not asked yet" -- the
     * creator does not know it until `thread_create` returns, by which time
     * the thread may already be reading it, and a thread that never asks
     * should not pay a syscall to be told.
     */
    /*
     * The block and this thread's own copy of the TLS image, laid out by
     * the library that owns both (`__cosmo_tcb_place`): the thread pointer
     * is not always the block's address -- on AArch64 the ELF ABI reserves
     * 16 bytes at the thread pointer and puts `__thread` variables above
     * them -- and the image is copied here, per thread, because a template
     * is not an allocation.
     */
    char *storage = base + PAGE + size;
    char *tp_p = (char *)__cosmo_tcb_place(storage, tcb);
    if (tp_p == NULL) {
        munmap(base, size + PAGE + tcb);
        t->stack = NULL;
        return -EINVAL;
    }
    struct __cosmo_tcb *blk = (struct __cosmo_tcb *)(tp_p - COSMO_TCB_TP_OFFSET);
    blk->self = blk;
    blk->err = 0;
    blk->tid = 0;
    unsigned long tp = (unsigned long)tp_p;

    struct cosmo_thread req = {
        .entry = (unsigned long)thread_trampoline,
        .arg = (unsigned long)t,
        .stack_top = top,
        .tls = tp,
        .clear_tid = (unsigned long)&t->done,
        .flags = 0,
        .reserved = 0,
    };
    long rc = cosmo_thread_create(&req);
    if (rc < 0) {
        munmap(base, size + PAGE + tcb);
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

struct __cosmo_thread_stats __cosmo_thread_stats;   /* see libc.h */

static inline void stat_inc(volatile unsigned *p)
{
    __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
}

/*
 * The contended path, from a caller that has already seen `c` in the
 * word. From here the lock is *always* taken by exchanging 2 in, never 1,
 * so "held, and a waiter may exist" survives the handover and the next
 * unlock wakes. Taking it with 1 instead -- which this library did until
 * a review -- strands a waiter whenever three or more contend: the winner
 * leaves 1 behind, the unlock sees 1 and wakes nobody, and a thread
 * already asleep on 2 is never called again. The cost of the conservative
 * 2 is one futex_wake with no waiter.
 *
 * A condition waiter enters here directly, with `c` = 1, whatever the word
 * says: it may have been *requeued* onto this mutex by a broadcast and
 * woken by the holder's unlock, and if it then took a free mutex with the
 * fast path's 1 its own unlock would wake nobody and the waiters still
 * asleep behind it would never be called -- the same strand, through a
 * different door (docs/audit/next-subsystem-native-thread-door.md).
 */
static void mutex_lock_contended(cosmo_mutex_t *m, unsigned c)
{
    if (c != 2)
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQ_REL);
    while (c != 0) {
        stat_inc(&__cosmo_thread_stats.mutex_sleeps);
        cosmo_futex_wait(&m->state, 2, 0);
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQ_REL);
    }
}

void cosmo_mutex_lock(cosmo_mutex_t *m)
{
    unsigned c = cas(&m->state, 0, 1);
    if (c == 0)
        return;                        /* uncontended: one atomic, no syscall */
    mutex_lock_contended(m, c);
}

void cosmo_mutex_unlock(cosmo_mutex_t *m)
{
    if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_ACQ_REL) != 1) {
        __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
        if (cosmo_futex_wake(&m->state, 1) == 0)
            stat_inc(&__cosmo_thread_stats.empty_wakes);
    }
}

int cosmo_mutex_trylock(cosmo_mutex_t *m)
{
    return cas(&m->state, 0, 1) == 0 ? 0 : -EBUSY;
}

/* --- a condition variable over the same futex ------------------------------
 *
 * The wait every threaded program was writing by hand
 * (docs/audit/next-subsystem-condvar.md). `cosmo/thread.h` carries the
 * contract a caller needs; what follows is why it cannot lose a wakeup.
 *
 * `seq` counts signals. A waiter reads it **while still holding the
 * mutex**, and a signaller must change the predicate under that same
 * mutex -- so every signal is ordered against that read, and there are
 * only two cases. A signal before the read: the predicate is already
 * true, and the caller's `while` sees it without ever waiting. A signal
 * after the read: it incremented `seq`, so `futex_wait` finds the word
 * different from the value it was given and returns without sleeping.
 * Neither case sleeps on a signal that has already happened, which is the
 * whole of the property.
 *
 * The one window left is between the unlock and the `futex_wait`, and it
 * is the kernel's to close rather than this file's: `futex_wait` compares
 * the word under the bucket lock and a waiter caught between its compare
 * and its enqueue observes `wake_seq` and retries (kernel/ipc/futex.c).
 * This code depends on that rather than reinventing it.
 */

void (*__cosmo_cond_probe)(void);   /* see libc.h; NULL in every real program */
void (*__cosmo_cond_bcast_probe)(int phase);

int cosmo_cond_timedwait(cosmo_cond_t *c, cosmo_mutex_t *m, unsigned long long timeout_ns)
{
    /*
     * Record the mutex, then read `seq` -- both sequentially consistent,
     * because with the broadcast's "increment `seq`, then read the mutex"
     * they are the Dekker shape, and a broadcaster that does not hold the
     * mutex is ordered against this wait by nothing else. Under a total
     * order there are two outcomes and both are safe: the broadcaster's
     * read came after this store and it sees our mutex, or it came before
     * and then its increment came before our read of `seq`, so the
     * `futex_wait` below compares unequal and never sleeps. Relaxed
     * accesses permit the third outcome on AArch64 -- both miss -- and
     * that is a waiter asleep with nobody coming.
     */
    __atomic_store_n(&c->mutex, m, __ATOMIC_SEQ_CST);
    /*
     * Read before the unlock. This single ordering is the property; moving
     * it below the unlock is the lost-wakeup bug, and the probe below is
     * how a test can actually make that bug fail.
     */
    unsigned seq = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);
    cosmo_mutex_unlock(m);

    void (*probe)(void) = __atomic_exchange_n(&__cosmo_cond_probe, 0, __ATOMIC_ACQ_REL);
    if (probe)
        probe();

    long rc = cosmo_futex_wait(&c->seq, seq, timeout_ns);
    /*
     * Through the contended path, never the fast one: this thread may have
     * been requeued onto the mutex and woken by an unlock, and there may
     * be others asleep behind it that only an unlock finding 2 will reach.
     * It cannot tell, so it always holds at 2 (mutex_lock_contended). The
     * price is one empty wake after a plain signal; the measurement in
     * docs/libc/testing.md carries it.
     */
    mutex_lock_contended(m, 1);
    /*
     * Only a timeout is reported. Everything else -- woken, `-EAGAIN`
     * because `seq` had already moved, or any error this call cannot do
     * anything about -- is a return to the caller's `while`, which is
     * where the predicate is. Treating an unexpected error as a spurious
     * wakeup is safe *because* the caller loops; it is not safe in an
     * interface where the caller may not.
     */
    return rc == -ETIMEDOUT ? -ETIMEDOUT : 0;
}

void cosmo_cond_wait(cosmo_cond_t *c, cosmo_mutex_t *m)
{
    (void)cosmo_cond_timedwait(c, m, 0);   /* 0 is "no timer" to SYS_futex_wait */
}

void cosmo_cond_signal(cosmo_cond_t *c)
{
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_ACQ_REL);
    cosmo_futex_wake(&c->seq, 1);
}

/*
 * Wake one waiter and move the rest onto the mutex, where the unlocks let
 * them through one at a time -- instead of waking all of them to contend
 * at once, of which all but one go straight back to sleep on the mutex.
 * With eight waiters that herd was seven sleeps on the mutex word per
 * broadcast, by construction; with the requeue it is none (`thrtest`,
 * "the herd", and docs/libc/testing.md for the numbers as run).
 *
 * The requeue is not a drop-in, because `cosmo_mutex_unlock` wakes only
 * when it finds 2. Three rules (docs/audit/next-subsystem-native-thread-door.md):
 *
 *   1. A condition waiter relocks through the contended path -- in
 *      cosmo_cond_timedwait, so each woken waiter holds at 2 and its
 *      unlock reaches the next.
 *   2. AFTER the requeue, make the word reachable: read it and act --
 *      2, a holder's unlock will wake, done; 1, mark it 2; 0, nobody
 *      holds it, so wake one of the moved waiters ourselves, and rule 1
 *      makes that one the head of the chain. After and not before,
 *      because a mark made before the requeue can be undone by another
 *      holder's unlock landing in between, leaving the waiters on a free
 *      word with no unlock coming.
 *   3. Which is why the broadcaster need not hold the mutex: rule 2 asks
 *      what the word says, not who holds it.
 *
 * And the word is read only after the requeue reported a waiter. The
 * recorded pointer can outlive the mutex it names -- nothing clears it
 * when the last waiter leaves -- so on our own account we pass it to the
 * kernel as an address (which it never loads) and load through it only
 * once a thread that was waiting with that very mutex has been woken or
 * moved onto it: that thread is about to relock it, so it is alive. What
 * remains is the lifetime rule in cosmo/thread.h.
 */
void cosmo_cond_broadcast(cosmo_cond_t *c)
{
    unsigned seq = __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    cosmo_mutex_t *m = __atomic_load_n((cosmo_mutex_t *volatile *)&c->mutex, __ATOMIC_SEQ_CST);
    if (m == NULL)
        return;                        /* nobody has ever waited: nothing to move, nothing to wake */

    void (*probe)(int) = __atomic_exchange_n(&__cosmo_cond_bcast_probe, 0, __ATOMIC_ACQ_REL);
    if (probe)
        probe(0);
    long n = cosmo_futex_requeue(&c->seq, &m->state, 1, ~0u, seq);
    if (probe)
        probe(1);
    if (n == -EAGAIN) {
        /* A concurrent broadcaster moved `seq` first; its requeue owns the
         * waiters and its rule 2 makes them reachable. Not our word to
         * touch. */
        stat_inc(&__cosmo_thread_stats.bcast_eagain);
        return;
    }
    if (n <= 0)
        return;                        /* nobody was waiting; `m` is not read */
    __atomic_fetch_add(&__cosmo_thread_stats.bcast_requeued, (unsigned)n, __ATOMIC_RELAXED);

    for (;;) {
        unsigned s = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
        if (s == 2)
            break;                     /* whoever holds it will wake on unlock */
        if (s == 0) {
            cosmo_futex_wake(&m->state, 1);   /* free: start the chain ourselves */
            break;
        }
        if (cas(&m->state, 1, 2) == 1)
            break;                     /* held at 1, now 2: its unlock will wake */
        /* The word moved under the CAS; look again. */
    }
}

int cosmo_thread_kill(cosmo_tid_t tid, int sig)
{
    return (int)cosmo_syscall2(SYS_thread_kill, tid, sig);
}
