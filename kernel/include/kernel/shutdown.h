/*
 * shutdown.h - Orderly kernel termination.
 *
 * kernel_shutdown() is the single exit point for a kernel that has
 * finished what it was asked to do. It disables interrupts, asks the
 * architecture layer to terminate an emulator if one is present (this is
 * how the QEMU test harness learns the outcome), else to power the
 * machine off through the firmware (PSCI on AArch64), and otherwise halts
 * the CPU forever. It never returns. There is no power-off on x86-64 off
 * QEMU (ACPI S5 needs AML).
 *
 * kernel_reboot() (shell completeness S5) is reboot(2)'s: restart the
 * machine, halt it, or power it off (kernel_shutdown's success). The
 * caller has checked PRIV_SYS_ADMIN. Like Linux's, it syncs nothing:
 * init, and BusyBox's `reboot -f`, sync before they call it.
 */

#ifndef KERNEL_SHUTDOWN_H
#define KERNEL_SHUTDOWN_H

#include <kernel/compiler.h>

enum kernel_exit_status {
    KERNEL_EXIT_SUCCESS = 0,
    KERNEL_EXIT_FAILURE = 1,
};

void kernel_shutdown(enum kernel_exit_status status) __noreturn;

enum kernel_reboot_cmd {
    KERNEL_REBOOT_RESTART,
    KERNEL_REBOOT_HALT,
    KERNEL_REBOOT_POWER_OFF,
};

void kernel_reboot(enum kernel_reboot_cmd cmd) __noreturn;

#endif /* KERNEL_SHUTDOWN_H */
