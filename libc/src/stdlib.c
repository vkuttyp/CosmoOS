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

#include "libc.h"

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

int atexit(void (*fn)(void))
{
    if (g_natexit >= ATEXIT_MAX)
        return -1;
    g_atexit[g_natexit++] = fn;
    return 0;
}

void exit(int status)
{
    while (g_natexit > 0)
        g_atexit[--g_natexit]();
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

static size_t env_count(void)
{
    size_t n = 0;
    while (environ && environ[n])
        n++;
    return n;
}

char *getenv(const char *name)
{
    size_t nl = strlen(name);
    for (size_t i = 0; environ && environ[i]; i++)
        if (strncmp(environ[i], name, nl) == 0 && environ[i][nl] == '=')
            return environ[i] + nl + 1;
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (name == NULL || *name == '\0' || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    size_t nl = strlen(name);
    size_t n = env_count();
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], name, nl) == 0 && environ[i][nl] == '=') {
            if (!overwrite)
                return 0;
            char *e = malloc(nl + strlen(value) + 2);
            if (e == NULL)
                return -1;
            sprintf(e, "%s=%s", name, value);
            environ[i] = e;   /* the old string may be the kernel's: leaked, not freed */
            return 0;
        }
    }
    char **nenv = malloc((n + 2) * sizeof(char *));
    if (nenv == NULL)
        return -1;
    memcpy(nenv, environ, n * sizeof(char *));
    nenv[n] = malloc(nl + strlen(value) + 2);
    if (nenv[n] == NULL) {
        free(nenv);
        return -1;
    }
    sprintf(nenv[n], "%s=%s", name, value);
    nenv[n + 1] = NULL;
    if (g_env_owned)
        free(environ);
    environ = nenv;
    g_env_owned = 1;
    return 0;
}

int unsetenv(const char *name)
{
    size_t nl = strlen(name);
    size_t n = env_count();
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], name, nl) == 0 && environ[i][nl] == '=') {
            memmove(&environ[i], &environ[i + 1], (n - i) * sizeof(char *));
            return 0;
        }
    }
    return 0;
}
