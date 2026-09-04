/*
 * x86/lapic.h - Local APIC (xAPIC MMIO mode). Private to x86-64.
 */

#ifndef X86_LAPIC_H
#define X86_LAPIC_H

#include <kernel/types.h>

#define LAPIC_SPURIOUS_VECTOR 255u

/* Map and enable the local APIC of the boot CPU at `base`. */
void lapic_init_bsp(paddr_t base);

/* Enable the local APIC of the calling CPU (APs). */
void lapic_init_cpu(void);

bool lapic_present(void);
uint32_t lapic_id(void);
void lapic_eoi(void);

/* Timer: divide-by-16 bus clock. One-shot with a raw count for
 * calibration, periodic for the tick. */
void lapic_timer_oneshot_raw(uint32_t count);
uint32_t lapic_timer_current(void);
void lapic_timer_periodic(unsigned vector, uint32_t count);
void lapic_timer_stop(void);

/* CPU index <-> APIC id table maintained by irqc.c. */
uint32_t x86_cpu_apic_id(unsigned cpu);
void x86_cpu_set_apic_id(unsigned cpu, uint32_t apic_id);

/* Inter-processor interrupts, fixed delivery, physical destination. */
void lapic_send_ipi(uint32_t apic_id, unsigned vector);
void lapic_send_ipi_all_others(unsigned vector);
void lapic_send_init(uint32_t apic_id);
void lapic_send_sipi(uint32_t apic_id, uint8_t start_page);

#endif /* X86_LAPIC_H */
