/*
 * auxv.c - the auxiliary vector the kernel left on the initial stack.
 *
 * `__libc_start` records where it is, because only it sees `envp` before
 * anything can change `environ`: the vector sits immediately after envp's
 * NULL terminator, and a program that called `setenv` first would have had
 * `environ` replaced by a heap copy with no vector behind it.
 */

#include <stddef.h>

#include <cosmo/auxv.h>

#include "libc.h"

static const unsigned long *g_auxv;   /* tag/value pairs, ending at AT_NULL */

void __cosmo_auxv_init(char **envp)
{
    if (envp == NULL)
        return;
    char **e = envp;
    while (*e != NULL)
        e++;
    g_auxv = (const unsigned long *)(e + 1);   /* past the terminator */
}

unsigned long cosmo_getauxval(unsigned long tag)
{
    if (g_auxv == NULL)
        return 0;
    for (const unsigned long *a = g_auxv; a[0] != COSMO_AT_NULL; a += 2)
        if (a[0] == tag)
            return a[1];
    return 0;
}
