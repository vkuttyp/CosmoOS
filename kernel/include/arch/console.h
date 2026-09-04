/*
 * arch/console.h - Early console bring-up.
 *
 * The architecture layer owns the first output device (a UART on both
 * initial targets). arch_console_early_init() probes it and registers it
 * as a console sink. It is the very first thing called after the stack
 * is valid, so it must not depend on any other subsystem.
 */

#ifndef ARCH_CONSOLE_H
#define ARCH_CONSOLE_H

void arch_console_early_init(void);
/* After irq_init and tty_init: route the console device's receive
 * interrupt into the console tty. Harmless when there is no device. */
void arch_console_input_init(void);

#endif /* ARCH_CONSOLE_H */
