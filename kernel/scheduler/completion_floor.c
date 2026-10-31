/*
 * completion_floor.c - The C under kernel/scheduler/completion.cee (roadmap
 * 2.0 SA3): the module exports, a C macro. The completions are CeeSafe
 * (completion.cee, generated into completion.c and
 * kernel/include/kernel/completion_types.h).
 */

#include <kernel/completion.h>
#include <kernel/module.h>

/* Module ABI exports (docs/kernel/module/api.md): drivers wait for their
 * own commands with a completion (NVMe admin commands). */
EXPORT_SYMBOL(completion_init);
EXPORT_SYMBOL(complete);
EXPORT_SYMBOL(complete_linger);
EXPORT_SYMBOL(completion_done);
EXPORT_SYMBOL(wait_for_completion);
EXPORT_SYMBOL(wait_for_completion_timeout);
