/*
 * lxdyn.c - a position-independent executable with a program interpreter
 * (docs/compat/linux/testing.md). PT_INTERP names lxinterp, which the
 * kernel loads and starts first; lxinterp applies this image's RELATIVE
 * relocations and jumps to _start. The checks: the image sits at
 * USER_PIE_BASE, a pointer table in writable data was relocated (it is
 * what lxinterp fixed up), argv arrived intact through the interpreter.
 */

#include "lxabi.h"

extern char __ehdr_start[] __attribute__((visibility("hidden")));

/* Absolute addresses in initialised, writable data: R_X86_64_RELATIVE
 * relocations (the tables are not const, so the compiler cannot fold the
 * reads below and the linker keeps them). */
static const char *names[] = { "alpha", "beta", "gamma" };
static int (*entry_fn)(int, char **) = main;

int main(int argc, char **argv)
{
    int ok = 1;
    volatile int idx = argc;   /* 1: opaque to the compiler */
    if ((unsigned long)__ehdr_start != 0x555500000000UL)
        ok = 0;
    if (names[idx][0] != 'b' || names[idx + 1][4] != 'a')
        ok = 0;
    if (entry_fn != main)
        ok = 0;
    if (argc < 1 || argv[0][0] != '/')
        ok = 0;
    /* Live in the same process as lxinterp's line: a system call works too. */
    if (sc0(LX_getpid) <= 0)
        ok = 0;
    lx_puts(ok ? "lxdyn: ok\n" : "lxdyn: FAIL\n");
    return ok ? 0 : 1;
}
