/*
 * nova_panic.c - Nova's panic hook in the kernel.
 *
 * Code written in Nova (kernel/block/gpt.c, generated from Nova) checks
 * every index, length and arithmetic result. A check that fails cannot
 * return to code that assumed it held, so it stops the kernel here with
 * Nova's reason and the Nova source line: an out-of-bounds read becomes a
 * panic, never memory corruption. Nova's runtime (nova_rt_core.c) names
 * the reason.
 */

#include <kernel/panic.h>

#include "nova/rt.h"

_Noreturn void nova_panic(nv_panic_kind kind, nv_loc loc)
{
    panic("nova panic: %s at %s:%u:%u", nova_panic_reason(kind), loc.file, (unsigned)loc.line,
          (unsigned)loc.column);
}
