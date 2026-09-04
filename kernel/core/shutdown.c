/*
 * shutdown.c - Orderly kernel termination.
 */

#include <kernel/log.h>
#include <kernel/shutdown.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/shutdown.h>

void kernel_shutdown(enum kernel_exit_status status)
{
    kinfo("shutdown: exit status %d", (int)status);
    arch_irq_disable();

    /* An emulator may honour this asynchronously, so the halt below is
     * reached either way; on hardware it is the only effect. */
    arch_emulator_exit(status == KERNEL_EXIT_SUCCESS ? ARCH_EMULATOR_EXIT_SUCCESS
                                                     : ARCH_EMULATOR_EXIT_FAILURE);

    kinfo("shutdown: halting CPU");
    arch_cpu_halt_forever();
}
