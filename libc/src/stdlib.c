/* stdlib.c - Program start and exit, environment, conversions, qsort. */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libc.h"

char **environ;
static int g_env_owned;   /* environ was reallocated by setenv and is ours */

#define ATEXIT_MAX 32
static void (*g_atexit[ATEXIT_MAX])(void);
static int g_natexit;

extern int main(int argc, char **argv, char **envp);

void __libc_start(int argc, char **argv, char **envp)
{
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
