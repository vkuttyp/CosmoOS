/* The GPT parser and writer (kernel/block/gpt.c, generated from CeeSafe under
 * CEE_GEN: userland.mk) built for user mode: the installer lays out its
 * table with the code the kernel parses it with. CeeSafe's runtime comes with
 * it, and a failed CeeSafe check ends the program with the reason. */
#include "kernel/block/gpt.c"
#include "kernel/ceesafe/ceesafe_rt_core.c"

#include <stdio.h>
#include <stdlib.h>

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc)
{
    fprintf(stderr, "ceesafe panic: %s at %s:%u:%u\n", ceesafe_panic_reason(kind), loc.file, (unsigned)loc.line,
            (unsigned)loc.column);
    abort();
}
