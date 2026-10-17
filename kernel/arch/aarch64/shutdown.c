/*
 * shutdown.c - Emulator exit through semihosting, power-off through PSCI
 * (docs/kernel/arch/aarch64/design.md, "Console, fw_cfg, PCI, shutdown").
 *
 * SYS_EXIT_EXTENDED (0x20) with ADP_Stopped_ApplicationExit makes QEMU
 * exit with the given status. The status is encoded exactly as the x86
 * isa-debug-exit device would report it, (code << 1) | 1, so the boot
 * harness decodes both the same way. A QEMU started without
 * -semihosting-config takes the HLT as an undefined instruction: the
 * instruction is in the exception table, so the trap resumes after it and
 * this returns, as it does on a machine that is not QEMU's. The caller
 * then powers off (arch_power_off) or halts.
 */

#include <arch/shutdown.h>
#include <aarch64/platform.h>
#include <kernel/acpi.h>
#include <kernel/bootinfo.h>
#include <kernel/types.h>

#define SEMIHOSTING_SYS_EXIT_EXTENDED 0x20u
#define ADP_STOPPED_APPLICATION_EXIT  0x20026u

void arch_emulator_exit(unsigned code)
{
    /* Semihosting is QEMU's (with -semihosting-config): on anything else
     * HLT #0xF000 may enter an external debugger rather than trap. */
    /* Before bootinfo_init (an early panic) the firmware is unknown: try,
     * as every boot did before v1.0.1. */
    if (bootinfo_ready() && !acpi_platform_is_qemu())
        return;
    static volatile uint64_t block[2];
    block[0] = ADP_STOPPED_APPLICATION_EXIT;
    block[1] = ((uint64_t)code << 1) | 1u;
    register uint64_t w0 __asm__("x0") = SEMIHOSTING_SYS_EXIT_EXTENDED;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)block;
    /* Listed in the exception table (kernel/extable.h): an undefined
     * instruction here resumes at 2:, which is how a QEMU without
     * semihosting returns to the caller. */
    __asm__ volatile("dsb sy\n"
                     "1:\thlt #0xF000\n"
                     "2:\n"
                     "\t.pushsection .ex_table, \"a\", @progbits\n"
                     "\t.balign 4\n"
                     "\t.long 1b - ., 2b - .\n"
                     "\t.popsection"
                     : "+r"(w0) : "r"(x1) : "memory");
}

void arch_power_off(void)
{
    aarch64_psci_system_off();
}

void arch_restart(void)
{
    aarch64_psci_system_reset();
}
