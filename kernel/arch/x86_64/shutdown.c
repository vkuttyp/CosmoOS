/*
 * shutdown.c - Emulator exit for x86-64.
 *
 * QEMU's isa-debug-exit device (enabled by scripts/qemu-run.sh at port
 * 0xF4) terminates the emulator with status (value << 1) | 1. On hardware
 * or an emulator without the device the port write is ignored and we
 * return so the caller can halt.
 *
 * arch_restart (shell completeness S5) resets the machine the ways Linux's
 * x86 reboot tries them, each given time to act: the FADT's reset register
 * (ACPI 2.0, RESET_REG_SUP), the PCI reset control at 0xCF9, the keyboard
 * controller's reset line, and a triple fault, which nothing can refuse.
 */

#include <arch/shutdown.h>
#include <kernel/acpi.h>
#include <kernel/log.h>
#include <kernel/string.h>

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

/* About 50 ms: each io_wait is one write to port 0x80, about 1 us. */
static void reset_settle(void)
{
    for (unsigned i = 0; i < 50000; i++)
        io_wait();
}

/* The FADT's RESET_REG, a generic address at offset 116, and RESET_VALUE
 * at 128, valid when flag RESET_REG_SUP (bit 10 of the flags at 112) is
 * set. System I/O space only: a memory-space register would need a
 * mapping made with interrupts off (inventory). */
static bool fadt_reset(void)
{
    const struct acpi_sdt_header *fadt = acpi_find_table("FACP");
    if (fadt == NULL || fadt->length < 129)
        return false;
    const uint8_t *b = (const uint8_t *)fadt;
    uint32_t flags;
    uint64_t addr;
    memcpy(&flags, b + 112, 4);
    memcpy(&addr, b + 120, 8);
    if (!(flags & (1u << 10)) || b[116] != 1 || addr == 0 || addr > 0xffff)
        return false;
    kinfo("reboot: restart through the ACPI reset register (port %#llx, value %#x)", (unsigned long long)addr,
          b[128]);
    outb((uint16_t)addr, b[128]);
    return true;
}

void arch_restart(void)
{
    if (fadt_reset())
        reset_settle();
    kinfo("reboot: restart through the PCI reset control (0xcf9)");
    outb(0xcf9, 0x02);
    io_wait();
    outb(0xcf9, 0x06);
    reset_settle();
    kinfo("reboot: restart through the keyboard controller");
    for (unsigned i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
        io_wait();
    outb(0x64, 0xfe);
    reset_settle();
    kinfo("reboot: restart through a triple fault");
    static const struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) no_idt = { 0, 0 };
    __asm__ volatile("lidt %0\n\tint3" : : "m"(no_idt) : "memory");
}
