/*
 * shutdown.c - Orderly kernel termination.
 */

#include <kernel/log.h>
#include <kernel/shutdown.h>
#include <kernel/smp.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/shutdown.h>

void kernel_shutdown(enum kernel_exit_status status)
{
    kinfo("shutdown: exit status %d", (int)status);
    smp_stop_others();
    arch_irq_disable();

    /* An emulator's exit carries the status to the test harness; it
     * returns where there is none (hardware, Parallels Desktop, a QEMU
     * without the device). Then the firmware's power-off, and the halt
     * below only where there is neither. */
    arch_emulator_exit(status == KERNEL_EXIT_SUCCESS ? ARCH_EMULATOR_EXIT_SUCCESS
                                                     : ARCH_EMULATOR_EXIT_FAILURE);
    arch_power_off();

    kinfo("shutdown: halting CPU");
    arch_cpu_halt_forever();
}
