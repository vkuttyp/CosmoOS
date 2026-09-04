/*
 * x86/pic.h - Legacy 8259A interrupt controllers. Private to x86-64.
 *
 * The PICs are remapped away from the exception vectors and fully masked
 * at boot. They stay masked: the LAPIC/IOAPIC take over in Phase 3. The
 * remap matters even when masked, because a spurious IRQ 7/15 can still
 * arrive and must not be confused with #DF or #GP.
 */

#ifndef X86_PIC_H
#define X86_PIC_H

#include <stdbool.h>

void pic_init_masked(void);

/* Acknowledge an IRQ (0-15) that was delivered through the PIC. Handles
 * the spurious-IRQ cases. `irq` is the PIC line, not the vector. */
void pic_eoi(unsigned irq);

/* True if the IRQ that just arrived is spurious and must not be EOI'd
 * as a real one. */
bool pic_is_spurious(unsigned irq);

#endif /* X86_PIC_H */
