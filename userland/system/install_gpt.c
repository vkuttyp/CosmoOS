/* The GPT parser and writer (kernel/block/gpt.c, generated from Nova) built
 * for user mode: the installer lays out its table with the code the kernel
 * parses it with. Nova's runtime comes with it, and a failed Nova check
 * (an index or length out of range) ends the program with the reason. */
#include "../../kernel/block/gpt.c"
#include "../../kernel/nova/nova_rt_core.c"

#include <stdio.h>
#include <stdlib.h>

_Noreturn void nova_panic(nv_panic_kind kind, nv_loc loc)
{
    fprintf(stderr, "nova panic: %s at %s:%u:%u\n", nova_panic_reason(kind), loc.file, (unsigned)loc.line,
            (unsigned)loc.column);
    abort();
}
