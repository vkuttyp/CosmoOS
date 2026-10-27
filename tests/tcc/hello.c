/*
 * hello.c - The first program the target's C compiler builds (roadmap 1.2
 * A1; ports/tcc/README.md): rc.test compiles it with cc (tcc) twice, linked
 * statically and against libc.so, and runs both. It names how it was
 * linked, so the boot test sees each.
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *how = argc > 1 ? argv[1] : "?";
    printf("cc-hello: %s, built on this system by cc\n", how);
    return strcmp(how, "static") == 0 || strcmp(how, "dynamic") == 0 ? 0 : 1;
}
