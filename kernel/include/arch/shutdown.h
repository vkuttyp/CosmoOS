/*
 * arch/shutdown.h - Emulator exit hook.
 *
 * arch_emulator_exit() asks a hosting emulator (QEMU's isa-debug-exit on
 * x86, a semihosting call on AArch64 later) to terminate with `code`. On
 * real hardware the request is a no-op and the function returns; callers
 * must then halt themselves. The code values are a contract with
 * tests/boot/run_boot_test.py.
 */

#ifndef ARCH_SHUTDOWN_H
#define ARCH_SHUTDOWN_H

#define ARCH_EMULATOR_EXIT_SUCCESS 0x10u
#define ARCH_EMULATOR_EXIT_FAILURE 0x11u

void arch_emulator_exit(unsigned code);

#endif /* ARCH_SHUTDOWN_H */
