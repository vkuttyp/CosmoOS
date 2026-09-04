# Interrupt Controllers, IRQ Routing, and IPIs

Extends the Phase 1 interrupt documents (`architecture.md`, `design.md`)
with the Phase 3 additions: hardware interrupt controllers, the generic
IRQ API, vector allocation, and inter-processor interrupts.

## 1. Layers

```text
   drivers                       irq_request(gsi, handler) / irq_enable
        ▼
   kernel/interrupt/irq.c        GSI → vector map, handler table reuse,
                                 masking policy, IPI kinds
        ▼ arch/irqc.h, arch/ipi.h
   kernel/arch/x86_64/lapic.c    local APIC: init, EOI, timer LVT, ICR (IPIs)
   kernel/arch/x86_64/ioapic.c   I/O APIC(s): redirection table programming
   kernel/arch/x86_64/vectors.c  dynamic vector allocator (48..238)
        ▲
   drivers/acpi/                 MADT: LAPIC base, IOAPICs, GSI overrides, CPUs
```

The generic layer speaks in global system interrupt numbers (GSI) and
symbolic IPI kinds. Vectors, APIC ids, redirection entries, and ICR
formats never leave the arch directory (constitution section 17).

## 2. Vector space (x86-64)

| Range | Use |
|---|---|
| 0–31 | exceptions |
| 32–47 | legacy PIC (masked; retained for spurious identification) |
| 48–238 | dynamic: device IRQs, LAPIC timer, IPIs |
| 239–255 | reserved (spurious vector 255) |

`x86_vector_alloc()` hands out the lowest free dynamic vector;
`x86_vector_free()` returns it. Allocation is serialised by a spinlock.

## 3. Generic IRQ API (`kernel/irq.h`)

```c
typedef unsigned irq_t;                       /* GSI */
#define IRQ_TRIGGER_EDGE   0
#define IRQ_TRIGGER_LEVEL  (1u << 0)
#define IRQ_POLARITY_LOW   (1u << 1)

int  irq_request(irq_t irq, interrupt_handler_fn fn, void *arg, const char *name, unsigned flags);
int  irq_release(irq_t irq);
void irq_enable(irq_t irq);
void irq_disable(irq_t irq);
irq_t irq_legacy_to_gsi(unsigned isa_irq);   /* applies MADT overrides */
```

`irq_request`: allocate a vector, `interrupt_register(vector, fn, arg)`,
then `arch_irqc_route(irq, vector, cpu, flags)` to program the
controller with the entry masked; `irq_enable` unmasks. The handler
receives the vector; the IRQ→vector table lets diagnostics print both.
EOI is issued by the arch dispatch tail after the handler returns
(`arch_irqc_eoi`), so handlers never see the controller.

Level-triggered interrupts are masked in the redirection entry before
the handler runs and unmasked after EOI only if the handler returned
normally; this phase's only consumers are edge (PIT/LAPIC timer) so
level handling is implemented but exercised only by review.

## 4. Local APIC

xAPIC mode through MMIO at the MADT-reported base (usually
`0xFEE00000`), mapped once with `vm_map_phys(..., VM_CACHE_UC)`. x2APIC
is detected but not enabled in this phase (documented gap; the register
accessors are the single place to change). Initialisation per CPU:

1. Read APIC id.
2. Spurious-interrupt vector register: vector 255, APIC software enable.
3. LVT entries masked (timer, LINT0/1, error, PMC, thermal).
4. Task priority 0.
5. Record `percpu->arch.lapic_id`.

`arch_irqc_eoi(vector)` writes the EOI register for any vector ≥ 48 or
the LAPIC timer vector; the legacy PIC path stays as before for 32–47.

## 5. I/O APIC

For every IOAPIC entry in the MADT: map its MMIO, read the version
register for the redirection entry count, record the GSI base. A GSI is
resolved to (ioapic, pin) by base ranges. `arch_irqc_route` writes the
64-bit redirection entry: vector, fixed delivery, physical destination
(APIC id of the chosen CPU), polarity and trigger from flags, masked.
`arch_irqc_mask/unmask` toggle bit 16.

ISA overrides (MADT type 2) map ISA IRQs to GSIs with their real
polarity/trigger; `irq_legacy_to_gsi` applies them. The PIT is ISA IRQ 0,
usually GSI 2 on QEMU.

## 6. IPIs (SMP PR)

```c
enum ipi_kind { IPI_RESCHEDULE, IPI_TLB_FLUSH, IPI_HALT, IPI_CALL, IPI_KIND_COUNT };
void arch_ipi_send(unsigned cpu, enum ipi_kind kind);
void arch_ipi_broadcast_others(enum ipi_kind kind);
```

One dynamic vector per kind, allocated at controller init and registered
with `interrupt_register` by the subsystem that owns the kind
(scheduler for RESCHEDULE, MMU for TLB_FLUSH, panic for HALT). Delivery
is ICR fixed mode, physical destination; broadcast uses the all-excluding-
self shorthand.

## 7. ACPI dependency

`drivers/acpi/acpi.c` maps the RSDP from `cosmoboot_info.acpi_rsdp`,
validates checksums, walks the XSDT (or RSDT), and exposes
`acpi_find_table(sig)` plus a decoded MADT view:
`acpi_madt_lapic_base()`, `acpi_madt_cpus()`, `acpi_madt_ioapics()`,
`acpi_madt_overrides()`. Tables are RAM (ACPI reclaim/NVS) and are read
through the direct map. Nothing else of ACPI (AML, power management) is
touched.

## 8. Failure modes

| Condition | Behaviour |
|---|---|
| no MADT / no LAPIC | panic: this kernel needs an APIC platform |
| no IOAPIC | irq_request for device GSIs fails with -ENODEV; LAPIC timer still works |
| vector space exhausted | irq_request → -ENOSPC |
| GSI with no IOAPIC covering it | -ENODEV |
| spurious LAPIC interrupt (vector 255) | counted, no EOI |
| interrupt on a vector with no handler | arch_trap_unhandled logs, EOI still sent |
