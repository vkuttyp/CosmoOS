/*
 * ceesafe_panic.c - CeeSafe's panic hook for host tests and fuzz targets that
 * link code generated from CeeSafe (kernel/block/gpt.c). A failed CeeSafe check
 * aborts with the reason and the CeeSafe source line, which ASan and the
 * fuzzer report as a crash.
 */

#include <stdio.h>
#include <stdlib.h>

#include "ceesafe/rt.h"

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc)
{
    fprintf(stderr, "ceesafe panic: %s at %s:%u:%u\n", ceesafe_panic_reason(kind), loc.file, (unsigned)loc.line,
            (unsigned)loc.column);
    abort();
}
