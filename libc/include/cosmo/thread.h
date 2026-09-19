/*
 * thread.h - Native threads, and a mutex over the futex
 * (docs/kernel/process/design.md, "Native threads").
 *
 * A thread runs in the calling process's address space and is scheduled on
 * any CPU. Everything a thread needs that the kernel does not provide is
 * here: the stack and its guard, the trampoline that makes a return from
 * the caller's function impossible, and the convention that turns the
 * kernel's `clear_tid` word into a join.
 *
 * What is safe to call from several threads (`docs/libc/invariants.md`,
 * L8): the **allocator** and **stdio** are locked, so `malloc`, `free`,
 * `realloc` and `printf` may be called from any thread -- a whole `printf`
 * is one critical section -- and **`errno` is per-thread**, a field of the
 * block behind each thread's thread pointer (`cosmo/tcb.h`). A thread this
 * header creates has one before its first instruction.
 *
 * **`atexit`'s table and the environment are safe too**, as of
 * docs/audit/next-subsystem-libc-shared-tables.md: `stdlib.c` takes one
 * lock over both. This header used to tell callers to use them from a
 * single thread because neither took one -- `setenv` growing `environ`
 * calls `free()` on the array a concurrent `getenv` may be walking, and
 * `g_atexit[g_natexit++]` is a read-modify-write. Call them from any
 * thread now, `spawnvp` and `spawnve` included -- those copy the
 * environment under the lock rather than reading the global. One
 * contract remains: the pointer `getenv` returns stays valid because
 * `setenv` leaks the string it replaces rather than freeing it.
 *
 * No longer shared: `strerror`'s buffer is `_Thread_local`, and
 * `getcwd(NULL)` never was -- it `malloc`s per call and hands the buffer
 * to its caller, so it stopped being shared state when the allocator took
 * its lock. Those two were the functions that returned a pointer to a
 * static; the two above are process state, which is a different thing and
 * is why fixing one did not fix the other. A program that wants
 * per-thread storage of its own uses `__thread`
 * (`docs/audit/next-subsystem-pt-tls.md`).
 *
 * The warning that remains is about threads this header did *not* make.
 * libc's `errno` is an unconditional load through the thread pointer, so a
 * thread that calls libc must have a block whose **prefix is libc's**
 * (`cosmo/tcb.h`). Two cases break that, and neither can be detected:
 *
 *   - `tls = 0`: there is no block at all. There is no fallback and cannot
 *     be one -- on x86-64, reading `%fs:0` with a zero base faults at
 *     address zero before any check could run.
 *   - `tls` pointing at a layout of the caller's own: before `errno` moved
 *     behind the thread pointer, a raw thread could carry any thread
 *     pointer it liked and still call libc. It cannot now -- libc would
 *     read and write that memory as its own block, overwriting whatever
 *     the caller keeps at the `errno` and tid offsets and answering
 *     `cosmo_thread_id()` with nonsense.
 *
 * Either way the fix is the same: call `cosmo_tcb_install` on storage that
 * starts with libc's prefix, and keep the caller's own fields at
 * COSMO_TCB_SIZE or beyond.
 *
 * Everything in this header needs none of that: each function returns
 * `-errno` rather than setting the global, takes no library lock, and maps
 * its stacks with `mmap`.
 */

#ifndef COSMO_THREAD_H
#define COSMO_THREAD_H

#include <stddef.h>

typedef unsigned cosmo_tid_t;

/* A joinable handle. Opaque in practice; laid out here so a caller can
 * keep one on its stack. */
typedef struct {
    cosmo_tid_t tid;
    volatile unsigned done;     /* the kernel's clear_tid word: the tid, then 0 */
    void *stack;                /* the mapping this library made, or NULL */
    size_t stack_size;
    void *(*fn)(void *);
    void *arg;
    void *ret;
} cosmo_thread_t;

/* Create a thread running fn(arg). `stack_size` 0 takes the default.
 * Returns 0, or -errno; on success `t` is joinable exactly once. */
int cosmo_thread_start(cosmo_thread_t *t, void *(*fn)(void *), void *arg, size_t stack_size);

/* Wait for `t` to finish and collect what `fn` returned (NULL is allowed).
 * Returns 0 or -errno; the thread's stack is released here, so a joined
 * handle must not be joined again. */
int cosmo_thread_join(cosmo_thread_t *t, void **ret);

/* End the calling thread. A thread must never return from `fn` -- the
 * trampoline calls this with its result -- and the process ends when its
 * last thread does. */
void cosmo_thread_finish(void *ret) __attribute__((noreturn));

/* The calling thread's id: a process's first thread answers its pid. */
cosmo_tid_t cosmo_thread_id(void);

/*
 * A mutex, and the futex's whole point. Three states so that an uncontended
 * unlock is one atomic store and no syscall: 0 free, 1 held, 2 held with
 * waiters.
 */
typedef struct {
    volatile unsigned state;
} cosmo_mutex_t;

#define COSMO_MUTEX_INIT { 0 }

void cosmo_mutex_lock(cosmo_mutex_t *m);
void cosmo_mutex_unlock(cosmo_mutex_t *m);
int cosmo_mutex_trylock(cosmo_mutex_t *m);   /* 0, or -EBUSY */

/*
 * A condition variable: how a thread waits for something *another thread
 * will do* (docs/audit/next-subsystem-condvar.md). A mutex answers "not at
 * the same time as you"; this answers "not until you have done the thing",
 * and until it existed every program that needed one wrote a futex
 * protocol by hand.
 *
 * One word, because there is nothing else to keep: a sequence number that
 * every signal increments. No waiter count, no associated mutex, no
 * allocation and nothing to destroy -- the same shape as `cosmo_mutex_t`,
 * and for the same reason: a primitive that cannot fail to be created can
 * be a static object in the program that uses it.
 */
typedef struct {
    volatile unsigned seq;
} cosmo_cond_t;

#define COSMO_COND_INIT { 0 }

/*
 * **Wait in a loop, on a predicate. Always.**
 *
 *     cosmo_mutex_lock(&m);
 *     while (!ready)
 *         cosmo_cond_wait(&c, &m);
 *     cosmo_mutex_unlock(&m);
 *
 * `cosmo_cond_wait` may return with nothing having happened. That is not
 * an apology for the implementation, it is the interface: it is what lets
 * the structure be one word with no bookkeeping, and it is what every
 * other condition variable specifies, so a reader who knows one knows
 * this one. A caller who writes `if` instead of `while` has written a bug
 * that passes every test on an unloaded machine.
 *
 * The caller must hold `m` on entry and holds it again on return -- which
 * is the whole point of handing the mutex over: the predicate cannot
 * change under a waiter that is deciding whether to sleep.
 *
 * A signaller must change the predicate **under the same mutex**. That is
 * not advice: it is what makes a wakeup impossible to lose, because it
 * puts every signal's increment of `seq` either before the waiter read
 * `seq` (so the waiter has not slept yet and will see the predicate) or
 * after it (so `futex_wait` compares unequal and does not sleep at all).
 *
 * `cosmo_cond_timedwait` returns 0 if it was woken or woke spuriously, and
 * `-ETIMEDOUT` if the interval expired. It does **not** return "the
 * predicate is true" -- it has never seen the predicate. A caller that
 * must give up keeps its own deadline and re-checks:
 *
 *     uint64_t deadline = cosmo_clock_ns() + budget;
 *     cosmo_mutex_lock(&m);
 *     while (!ready) {
 *         uint64_t now = cosmo_clock_ns();
 *         if (now >= deadline) break;
 *         cosmo_cond_timedwait(&c, &m, deadline - now);
 *     }
 *
 * `timeout_ns` is *relative*, as `cosmo_futex_wait`'s is, so passing the
 * same value each time round a loop waits longer than intended -- hence
 * the deadline above rather than a bare budget.
 *
 * A `cosmo_cond_t` may be destroyed when no thread is waiting on it. That
 * is the caller's knowledge, not the library's, and it is the same rule
 * the mutex has. There is nothing to free.
 */
void cosmo_cond_wait(cosmo_cond_t *c, cosmo_mutex_t *m);
int cosmo_cond_timedwait(cosmo_cond_t *c, cosmo_mutex_t *m, unsigned long long timeout_ns);
void cosmo_cond_signal(cosmo_cond_t *c);
void cosmo_cond_broadcast(cosmo_cond_t *c);

#endif /* COSMO_THREAD_H */
