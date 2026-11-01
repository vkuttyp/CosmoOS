/*
 * ceesafe_panic.c - CeeSafe's panic hook in pkg, the first user program that
 * links code generated from CeeSafe (pkg/pkgparse.cee). A failed CeeSafe
 * check is a bug in pkg, not in what it reads: it prints the reason and the
 * CeeSafe source line and aborts, so a transaction stops before it writes.
 */

#include <stdio.h>
#include <stdlib.h>

#include "ceesafe/rt.h"

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc)
{
    fprintf(stderr, "pkg: ceesafe panic: %s at %s:%u:%u\n", ceesafe_panic_reason(kind), loc.file,
            (unsigned)loc.line, (unsigned)loc.column);
    abort();
}
