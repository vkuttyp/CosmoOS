/*
 * lxnointerp.c - a position-independent executable whose PT_INTERP names
 * a file that does not exist (/lib/ld-cosmo-missing.so.1). It never runs:
 * lxtest's auxv section requires its execve to return ENOENT with the
 * caller intact (roadmap 1.1 P4; docs/compat/linux/testing.md).
 */

#include "lxabi.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lx_puts("lxnointerp: ran without its interpreter\n");
    return 1;
}
