/* stdlib.c - Program start and exit, environment, conversions, qsort. */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cosmo/syscall.h>
#include <cosmo/thread.h>

#include "libc.h"

/*
 * One lock over this file's two process-global tables: the environment
 * and the `atexit` list (invariant L8,
 * docs/audit/next-subsystem-libc-shared-tables.md).
 *
 * The allocator and stdio took locks when native threads arrived --
 * malloc.c says an unlocked free list is "a way to corrupt a heap
 * silently ... so it is locked here rather than left to a rule callers
 * must know" -- and these two tables were left as they were. They have
 * the same hazard: `setenv` growing the array calls `free(environ)`
 * while `getenv` may be walking it, which is a use-after-free in that
 * same allocator.
 *
 * One lock rather than two because the contention is theoretical --
 * these are start-up paths -- and a second lock is a second chance to
 * take them in the wrong order.
 *
 * THE MUTEX IS NOT RECURSIVE, and two rules follow:
 *   - the public entry points take it and the helpers do not, so
 *     `env_count` below must stay unlocked: both mutators call it, and
 *     a version that locked would deadlock against itself on every
 *     environment change. This is the split malloc.c uses.
 *   - `exit` must not hold it while running an `atexit` handler, which
 *     is arbitrary program code and may call `atexit` or `getenv`.
 */
static cosmo_mutex_t g_lock = COSMO_MUTEX_INIT;

char **environ;
static int g_env_owned;   /* environ was reallocated by setenv and is ours */

#define ATEXIT_MAX 32
static void (*g_atexit[ATEXIT_MAX])(void);
static int g_natexit;

extern int main(int argc, char **argv, char **envp);

void __libc_start(int argc, char **argv, char **envp)
{
    /*
     * The auxiliary vector first -- and *only* it. The thread pointer must
     * be installed before anything that could set `errno`, which the errno
     * unit's own bug-proof exists to keep; but the block's size now depends
     * on the program's own `PT_TLS`, and finding that means reading the
     * headers the auxiliary vector points at. So exactly one thing
     * precedes the thread pointer, it touches nothing that needs one, and
     * it is here rather than inside `__cosmo_tcb_init` so that the order is
     * visible at the only place that can get it wrong.
     *
     * The first build of the TLS unit had these the other way round and the
     * first thread silently got no image: `cosmo_getauxval` returned 0, the
     * template was never found, and a `__thread` variable read whatever lay
     * at the thread pointer's offset -- which the test caught by reading an
     * initialiser that was not there.
     */
    __cosmo_auxv_init(envp);

    /*
     * The thread pointer next, before __stdio_init and before anything
     * that could set errno: from here on `errno` is a load through that
     * pointer, so there is no ordering in which a libc call precedes it.
     *
     * The failure cannot happen -- the block is a linked static object, so
     * its address is aligned and inside the program's own image, which is
     * the whole of what SYS_set_tls checks. The policy is stated anyway,
     * because "cannot happen" is where a missing branch hides: one line to
     * file descriptor 2 and exit 127, written with the raw syscall because
     * stdio is not up and errno is the thing that just failed. A program
     * that continued would fault on its first error instead.
     */
    int tcb_rc = __cosmo_tcb_init();
    if (tcb_rc != 0) {
        /*
         * Say which of the several things went wrong, because this path
         * has more ways to be reached than it used to: a malformed
         * `PT_TLS` in the program's own headers, a storage layout that
         * does not fit, or a failed mapping. One line, written with the
         * raw syscall because stdio is not up and `errno` is the thing
         * that just failed.
         */
        static const char msg[] = "libc: no thread pointer (";
        char d[4] = { '-', '0', '0', ')' };
        int e = -tcb_rc;
        if (e >= 0 && e < 100) {
            d[1] = (char)('0' + e / 10);
            d[2] = (char)('0' + e % 10);
        }
        cosmo_write(2, msg, sizeof(msg) - 1u);
        cosmo_write(2, d, sizeof(d));
        cosmo_write(2, "\n", 1);
        cosmo_exit(127);
    }
    environ = envp;
    __stdio_init();
    exit(main(argc, argv, envp));
}

/*
 * The bound check and the increment are one critical section. Unlocked
 * they were two, so two threads could both pass the check at
 * ATEXIT_MAX - 1 and both write -- one past the end of a static array
 * -- and `g_natexit++` could lose a handler outright, which for
 * `atexit` means a file not flushed with nothing to say so.
 */
int atexit(void (*fn)(void))
{
    int rc = 0;
    cosmo_mutex_lock(&g_lock);
    if (g_natexit >= ATEXIT_MAX)
        rc = -1;
    else
        g_atexit[g_natexit++] = fn;
    cosmo_mutex_unlock(&g_lock);
    return rc;
}

void exit(int status)
{
    /*
     * Take, pop, release, THEN call. A handler is arbitrary program
     * code and may call `atexit` or `getenv`; running it under a
     * non-recursive lock deadlocks the program at exit, which is the
     * worst moment to do it. The list is consistent at every point
     * between calls, and a handler that registers another gets it run
     * by the next turn of this loop.
     */
    for (;;) {
        void (*fn)(void) = NULL;
        cosmo_mutex_lock(&g_lock);
        if (g_natexit > 0)
            fn = g_atexit[--g_natexit];
        cosmo_mutex_unlock(&g_lock);
        if (fn == NULL)
            break;
        fn();
    }
    __stdio_flush_all();
    _exit(status);
}

void _exit(int status)
{
    cosmo_exit(status);
}

void abort(void)
{
    static const char msg[] = "abort()\n";
    cosmo_write(2, msg, sizeof(msg) - 1);
    _exit(134);
}

void __assert_fail(const char *expr, const char *file, int line);
void __assert_fail(const char *expr, const char *file, int line)
{
    dprintf(2, "%s:%d: assertion '%s' failed\n", file, line, expr);
    abort();
}

/* --- environment --- */

/* Unlocked: called by the mutators with `g_lock` already held. */
static size_t env_count(void)
{
    size_t n = 0;
    while (environ && environ[n])
        n++;
    return n;
}

/*
 * The lock covers the WALK, not the pointer that comes back. A later
 * `setenv` may replace that slot -- and the string it replaces is
 * leaked rather than freed (see `setenv`), which is what keeps a
 * returned pointer valid. POSIX does not promise that; this library
 * does, by not freeing.
 */
char *getenv(const char *name)
{
    size_t nl = strlen(name);
    char *found = NULL;
    cosmo_mutex_lock(&g_lock);
    for (size_t i = 0; environ && environ[i]; i++)
        if (strncmp(environ[i], name, nl) == 0 && environ[i][nl] == '=') {
            found = environ[i] + nl + 1;
            break;
        }
    cosmo_mutex_unlock(&g_lock);
    return found;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (name == NULL || *name == '\0' || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    size_t nl = strlen(name);
    int rc = 0;
    cosmo_mutex_lock(&g_lock);
    size_t n = env_count();      /* unlocked helper, under this lock */
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], name, nl) == 0 && environ[i][nl] == '=') {
            if (!overwrite)
                goto out;
            char *e = malloc(nl + strlen(value) + 2);
            if (e == NULL) {
                rc = -1;
                goto out;
            }
            sprintf(e, "%s=%s", name, value);
            /*
             * The old string is LEAKED, deliberately: it may be the
             * kernel's, and a pointer `getenv` already returned still
             * points into it. Freeing it here would turn every such
             * pointer into a dangling one, which is a worse bug than a
             * bounded leak in a start-up path.
             */
            environ[i] = e;
            goto out;
        }
    }
    {
        char **nenv = malloc((n + 2) * sizeof(char *));
        if (nenv == NULL) {
            rc = -1;
            goto out;
        }
        memcpy(nenv, environ, n * sizeof(char *));
        nenv[n] = malloc(nl + strlen(value) + 2);
        if (nenv[n] == NULL) {
            free(nenv);
            rc = -1;
            goto out;
        }
        sprintf(nenv[n], "%s=%s", name, value);
        nenv[n + 1] = NULL;
        /* The free that made this a use-after-free before the lock: a
         * reader inside `getenv` was walking this array. */
        if (g_env_owned)
            free(environ);
        environ = nenv;
        g_env_owned = 1;
    }
out:
    cosmo_mutex_unlock(&g_lock);
    return rc;
}

/*
 * Frees no string, so no pointer a reader holds is invalidated: the
 * hazard the lock closes here is an inconsistent TRAVERSAL -- a name
 * shifted into a slot a walker has already passed is missed, and a
 * walker past `i` runs against a stale tail.
 */
int unsetenv(const char *name)
{
    size_t nl = strlen(name);
    cosmo_mutex_lock(&g_lock);
    size_t n = env_count();      /* unlocked helper, under this lock */
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], name, nl) == 0 && environ[i][nl] == '=') {
            memmove(&environ[i], &environ[i + 1], (n - i) * sizeof(char *));
            break;
        }
    }
    cosmo_mutex_unlock(&g_lock);
    return 0;
}
