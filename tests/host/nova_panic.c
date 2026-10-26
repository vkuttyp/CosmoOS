/*
 * nova_panic.c - Nova's panic hook for host tests and fuzz targets that
 * link code generated from Nova (kernel/block/gpt.c). A failed Nova check
 * aborts with the reason and the Nova source line, which ASan and the
 * fuzzer report as a crash.
 */

#include <stdio.h>
#include <stdlib.h>

#include "nova/rt.h"

_Noreturn void nova_panic(nv_panic_kind kind, nv_loc loc)
{
    fprintf(stderr, "nova panic: %s at %s:%u:%u\n", nova_panic_reason(kind), loc.file, (unsigned)loc.line,
            (unsigned)loc.column);
    abort();
}
