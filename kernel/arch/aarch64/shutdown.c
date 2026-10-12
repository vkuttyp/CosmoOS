/*
 * shutdown.c - Emulator exit through semihosting
 * (docs/kernel/arch/aarch64/design.md, "Console, fw_cfg, PCI, shutdown").
 *
 * SYS_EXIT_EXTENDED (0x20) with ADP_Stopped_ApplicationExit makes QEMU
 * exit with the given status. The status is encoded exactly as the x86
 * isa-debug-exit device would report it, (code << 1) | 1, so the boot
 * harness decodes both the same way. Without semihosting enabled the
 * instruction is undefined and the caller's halt follows.
 */

#include <arch/shutdown.h>
#include <kernel/acpi.h>
#include <kernel/types.h>

#define SEMIHOSTING_SYS_EXIT_EXTENDED 0x20u
#define ADP_STOPPED_APPLICATION_EXIT  0x20026u

void arch_emulator_exit(unsigned code)
{
    /* Semihosting is QEMU's (with -semihosting-config): on anything else
     * HLT #0xF000 is an undefined instruction, not an exit. */
    if (!acpi_platform_is_qemu())
        return;
    static volatile uint64_t block[2];
    block[0] = ADP_STOPPED_APPLICATION_EXIT;
    block[1] = ((uint64_t)code << 1) | 1u;
    register uint64_t w0 __asm__("x0") = SEMIHOSTING_SYS_EXIT_EXTENDED;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)block;
    __asm__ volatile("dsb sy\n\thlt #0xF000" : "+r"(w0) : "r"(x1) : "memory");
}
