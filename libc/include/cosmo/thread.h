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
 * is one critical section. **`errno` is still one global**: a threaded
 * program must not rely on it across threads, because the value it reads
 * may be another thread's. That is a wrong error code, never corruption,
 * and a per-thread `errno` needs a thread-local-storage model, which is
 * its own unit.
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

#endif /* COSMO_THREAD_H */
