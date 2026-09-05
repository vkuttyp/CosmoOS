/*
 * arch/aarch64/serial.c - PL011 output for the loader
 * (docs/kernel/arch/aarch64/design.md, "The loader"). The `virt`
 * machine's UART sits at 0x09000000, identity-mapped by the firmware's
 * tables and by ours.
 */

#include "loader.h"

#define PL011_BASE 0x09000000ull
#define UART_DR    0x000
#define UART_FR    0x018
#define FR_TXFF    (1u << 5)

static volatile uint32_t *const g_uart = (volatile uint32_t *)(uintptr_t)PL011_BASE;
static bool g_present;

void arch_serial_init(void)
{
    /* No probe is possible without knowing the platform; the firmware
     * configured the UART it uses for its own console. */
    g_present = true;
}

bool arch_serial_present(void)
{
    return g_present;
}

void arch_serial_putc(char c)
{
    if (!g_present)
        return;
    while (g_uart[UART_FR / 4] & FR_TXFF)
        ;
    g_uart[UART_DR / 4] = (uint32_t)(uint8_t)c;
}
