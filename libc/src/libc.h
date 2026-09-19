/* libc.h - Internal declarations shared by the library's source files. */

#ifndef LIBC_INTERNAL_H
#define LIBC_INTERNAL_H

#include <stddef.h>
#include <sys/types.h>

#include <stdio.h>

#include <cosmo/syscall.h>

/* Translate the kernel's negative errno convention: sets errno and
 * returns -1 on failure, the value itself otherwise. */
long __syscall_ret(long r);

void __libc_start(int argc, char **argv, char **envp) __attribute__((noreturn));
/* tcb.c: the thread pointer. __cosmo_tcb_init installs the first thread's
 * block and must run before anything that can set errno. A created
 * thread's block is installed by the kernel from `cosmo_thread.tls`. */
int __cosmo_tcb_init(void);
unsigned __cosmo_tcb_tid(void);
/* How much storage one thread's block and TLS image need, and the layout
 * inside it: `__cosmo_tcb_place` copies the image and returns the thread
 * pointer, or NULL if the storage is too small. */
size_t __cosmo_tcb_storage(void);
void *__cosmo_tcb_place(void *storage, size_t len);

/* auxv.c: where the kernel left the auxiliary vector. Recorded by
 * __libc_start, which is the only code that sees `envp` before anything
 * can replace `environ` with a heap copy. */
void __cosmo_auxv_init(char **envp);

void __stdio_init(void);
void __stdio_flush_all(void);


/* stdio's lock, and the unlocked write core, so printf can hold the lock
 * across a whole format rather than per chunk (libc/src/stdio.c). */
/*
 * A snapshot of the environment's ARRAY, taken under `stdlib.c`'s lock
 * and owned by the caller (`free` it). For readers of `environ`
 * outside that file: `spawnvp` hands the array to the kernel and
 * cannot hold a libc lock across a system call, and reading the
 * global directly is the use-after-free `setenv` creates when it
 * frees the old array (invariant L8;
 * docs/audit/next-subsystem-libc-shared-tables.md).
 *
 * A SHALLOW copy is enough, and only because `setenv` leaks the
 * strings it replaces rather than freeing them -- the pointers in the
 * snapshot stay valid for as long as the caller holds it. NULL on
 * allocation failure.
 */
char **__env_snapshot(void);

void __stdio_lock(void);
void __stdio_unlock(void);
size_t __fwrite_nolock(const void *buf, size_t size, size_t n, FILE *f);

/*
 * The condition variable's test seam (docs/audit/next-subsystem-condvar.md).
 *
 * `cosmo_cond_wait`'s lost-wakeup guarantee lives in a window a few
 * instructions wide -- between its read of `seq` and its `futex_wait` --
 * and **nothing another thread can do reaches inside it**. Unlocking the
 * mutex makes a blocked signaller runnable, not running; the waiter is not
 * preempted and reaches `futex_wait` first, so a broken implementation
 * passes. Two drafts of the test tried and could not.
 *
 * So the library is instrumented instead: if this is non-NULL,
 * `cosmo_cond_wait` calls it inside the window, on the waiting thread.
 * A test that arms it with a function that signals synchronously puts the
 * signal exactly where it has to be, with no scheduler involved.
 *
 * **It is taken, not read** -- an atomic exchange with NULL, so it fires
 * once for the arming that asked for it. It has to be: this is
 * process-global, every condition wait in the program goes through here,
 * and a probe left installed fires inside some *other* test's waiter,
 * calling that callback against the wrong mutex and predicate.
 *
 * Compiled in unconditionally and not behind an `#ifdef`, because a seam
 * that exists only in a test build proves things about a binary nobody
 * runs.
 *
 * **The cost is an acquire-release exchange, not a load** -- it has to be,
 * because taking the probe is what stops it firing in a later waiter, and
 * an earlier draft of this comment described the load it used to be. A
 * read-modify-write on a process-global word, on every condition wait, is
 * a real cost and is named here rather than rounded down: it sits
 * immediately before a `futex_wait` syscall, which is several orders of
 * magnitude more expensive, and that is the reason it is acceptable
 * rather than the claim that it is free.
 *
 * It is libc's, not a program's: no public header declares it.
 */
extern void (*__cosmo_cond_probe)(void);

#endif
