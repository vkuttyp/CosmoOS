/*
 * arch/shutdown.h - Emulator exit hook.
 *
 * arch_emulator_exit() asks a hosting emulator (QEMU's isa-debug-exit on
 * x86, a semihosting call on AArch64) to terminate with `code`. On
 * real hardware, or an emulator without the device, the request is a
 * no-op and the function returns. The code values are a contract with
 * tests/boot/run_boot_test.py.
 *
 * arch_power_off() turns the machine off through the firmware: PSCI
 * SYSTEM_OFF on AArch64. It returns when there is no way to (x86-64 has
 * none yet: ACPI S5 needs the DSDT's \_S5 values, which needs AML); the
 * caller halts.
 *
 * arch_restart() resets the machine (shell completeness S5): PSCI
 * SYSTEM_RESET on AArch64; the FADT reset register, then 0xCF9, the
 * keyboard controller and a triple fault on x86-64. It returns only where
 * none of them worked; the caller halts.
 */

#ifndef ARCH_SHUTDOWN_H
#define ARCH_SHUTDOWN_H

#define ARCH_EMULATOR_EXIT_SUCCESS 0x10u
#define ARCH_EMULATOR_EXIT_FAILURE 0x11u

void arch_emulator_exit(unsigned code);
void arch_power_off(void);
void arch_restart(void);

#endif /* ARCH_SHUTDOWN_H */
