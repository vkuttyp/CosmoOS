/*
 * shutdown.c - Emulator exit for x86-64.
 *
 * QEMU's isa-debug-exit device (enabled by scripts/qemu-run.sh at port
 * 0xF4) terminates the emulator with status (value << 1) | 1. On hardware
 * or an emulator without the device the port write is ignored and we
 * return so the caller can halt.
 */

#include <arch/shutdown.h>

#include <x86/io.h>

#define QEMU_DEBUG_EXIT_PORT 0xF4

void arch_emulator_exit(unsigned code)
{
    outl(QEMU_DEBUG_EXIT_PORT, code);
}

/* ACPI S5 needs the SLP_TYP values of the DSDT's \_S5 object, which
 * needs an AML interpreter: nothing to do yet, and the caller halts. */
void arch_power_off(void)
{
}
