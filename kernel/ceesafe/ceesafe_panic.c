/*
 * ceesafe_panic.c - CeeSafe's panic hook in the kernel.
 *
 * Code written in CeeSafe (kernel/block/gpt.c, generated from CeeSafe) checks
 * every index, length and arithmetic result. A check that fails cannot
 * return to code that assumed it held, so it stops the kernel here with
 * CeeSafe's reason and the CeeSafe source line: an out-of-bounds read becomes a
 * panic, never memory corruption. CeeSafe's runtime (ceesafe_rt_core.c) names
 * the reason.
 */

#include <kernel/panic.h>

#include "ceesafe/rt.h"

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc)
{
    panic("ceesafe panic: %s at %s:%u:%u", ceesafe_panic_reason(kind), loc.file, (unsigned)loc.line,
          (unsigned)loc.column);
}
