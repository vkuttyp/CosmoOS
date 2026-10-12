/*
 * arch/aarch64/serial.c - The loader's serial output after
 * ExitBootServices (docs/boot/design.md, "The serial console").
 *
 * Only the port the firmware's SPCR names (acpi_scan, boot protocol v9):
 * a PL011/SBSA UART or a memory-mapped 16550 at the recorded base, which
 * the firmware's identity map still covers until the loader jumps. No
 * SPCR, no serial output: a fixed address (QEMU virt's 0x09000000, which
 * this file used until v1.0.1) is a store to whatever another machine has
 * there, and a fault with no handler left to report it.
 */

#include "loader.h"
#include "cosmoboot.h"

#define PL011_DR    0x000
#define PL011_FR    0x018
#define PL011_TXFF  (1u << 5)

#define U16550_THR  0
#define U16550_LSR  5
#define U16550_THRE (1u << 5)

static uint32_t g_type;   /* COSMOBOOT_CONSOLE_*; NONE until configured */
static uintptr_t g_base;
static unsigned g_shift;  /* 16550 registers: 1 byte apart, or 4 when the SPCR says 32-bit access */

void arch_serial_init(void)
{
}

void arch_serial_configure(const struct cosmoboot_info *info)
{
    if (info->console_type == COSMOBOOT_CONSOLE_PL011 || info->console_type == COSMOBOOT_CONSOLE_16550_MMIO) {
        g_type = info->console_type;
        g_base = (uintptr_t)info->console_base;
        g_shift = info->console_access == 4 ? 2 : 0;
    }
}

bool arch_serial_present(void)
{
    return g_type != COSMOBOOT_CONSOLE_NONE;
}

void arch_serial_putc(char c)
{
    if (g_type == COSMOBOOT_CONSOLE_PL011) {
        volatile uint32_t *r = (volatile uint32_t *)g_base;
        while (r[PL011_FR / 4] & PL011_TXFF)
            ;
        r[PL011_DR / 4] = (uint32_t)(uint8_t)c;
    } else if (g_type == COSMOBOOT_CONSOLE_16550_MMIO) {
        if (g_shift == 2) {
            volatile uint32_t *r = (volatile uint32_t *)g_base;
            while ((r[U16550_LSR] & U16550_THRE) == 0)
                ;
            r[U16550_THR] = (uint32_t)(uint8_t)c;
        } else {
            volatile uint8_t *r = (volatile uint8_t *)g_base;
            while ((r[U16550_LSR] & U16550_THRE) == 0)
                ;
            r[U16550_THR] = (uint8_t)c;
        }
    }
}
