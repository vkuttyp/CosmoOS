# Interrupt Subsystem

## Purpose

Map a vector number to a handler, architecture-neutrally. This is the
kernel's one dispatch point for every exception, external interrupt, and
inter-processor interrupt. It is part of the trusted core (constitution
section 8: "Interrupt subsystem") and is deliberately tiny: one table, three
operations.

Files: `kernel/interrupt/interrupt.c`, `kernel/include/kernel/interrupt.h`.

## Responsibilities

- Own the vector → handler table (`INTERRUPT_MAX_VECTORS` = 1344 slots;
  x86-64 dispatches 256 vectors, AArch64 1312).
- Register and unregister handlers with defined error codes.
- Dispatch a vector to its handler, or to `arch_trap_unhandled()` when
  there is none.
- Keep per-vector dispatch counts and handler names for diagnostics.

## Non-responsibilities

- Saving/restoring registers, stack switching, end-of-interrupt signalling,
  and vector numbering: all in the architecture layer
  (`docs/kernel/arch/`).
- Deferred work, threaded handlers, softirqs: Phase 3, on top of the
  scheduler.
- Interrupt controller programming (PIC, LAPIC/IOAPIC; GIC later),
  IRQ-to-vector allocation, MSI: the interrupt-controller abstraction
  from constitution section 17 (`kernel/irq.h`, `arch/irqc.h`, Phase 3)
  sits beside this table, not inside it. Phase 6 added
  `irq_request_msi`/`irq_release_msi`: an MSI or MSI-X message is
  composed by `arch_irqc_msi_compose` and, once raised, arrives as a
  plain vector through this dispatcher; nothing here knows the device.

## Position in the system

```
CPU exception / IRQ / IPI
        │
        ▼
arch entry stub (isr.S)          ── saves struct arch_trap_frame
        │
        ▼
x86_trap_dispatch (trap.c)       ── arch owns entry, exit, EOI
        │
        ▼
interrupt_dispatch(vector, frame) ── THIS SUBSYSTEM: table lookup
        │
        ├── handler(vector, frame, arg)          registered
        └── arch_trap_unhandled(vector, frame)   not registered
```

The division is strict: the arch layer never decides what a vector means,
and this layer never touches hardware.

## Interfaces

```c
void interrupt_init(void);
int  interrupt_register(unsigned vector, interrupt_handler_fn fn, void *arg, const char *name);
int  interrupt_unregister(unsigned vector, interrupt_handler_fn fn);
void interrupt_dispatch(unsigned vector, struct arch_trap_frame *frame);
uint64_t interrupt_count(unsigned vector);
const char *interrupt_handler_name(unsigned vector);
```

Vector numbers are obtained symbolically: `arch_trap_vector(ARCH_TRAP_PAGE_FAULT)`,
never a literal. See `api.md`.

## Data structures

```c
struct interrupt_record {
    interrupt_handler_fn fn;
    void *arg;                 /* not owned */
    const char *name;          /* immortal string, atomically sampled */
};
struct interrupt_slot {
    uint32_t writer;           /* raw writer lock, never used by dispatch */
    struct interrupt_record *cur; /* release/acquire publication */
    struct interrupt_record recs[2];
    unsigned next_rec;
    uint64_t count;            /* atomic dispatch count */
};
static struct interrupt_slot g_slots[INTERRUPT_MAX_VECTORS];
static unsigned g_vector_count;   /* from arch_trap_vector_count() */
```

Static, kernel lifetime, zeroed by `interrupt_init`. The table owns
nothing it points to.

## Concurrency model

Registration and removal serialize on a raw per-vector lock with local
IRQs masked. Boot, thread and ordinary IRQ mutations are supported, not
NMI/#MC mutations. Dispatch acquires the published record pointer once
and takes no writer lock. Counts and diagnostic name pointers are atomic
samples. See `design.md` for publication and record reuse rules.

## Memory ownership

The table allocates nothing. The registrant retains `arg` and handler code
through `interrupt_unregister_sync`, or plain unregister followed by
`synchronize_irq`. Names must be immortal because diagnostic readers can
sample them without a grace-period read section. Writers must coordinate
the full unregister/grace-period/re-registration lifecycle; serializing
table mutation alone does not make early record reuse safe.

## Error handling

Explicit negative errno returns from `kernel/errno.h`: `-EINVAL` (bad
vector or NULL function), `-EBUSY` (slot taken), `-ENOENT` (function is
not the registered one). A dispatch with a vector ≥ `g_vector_count` is
an arch-layer bug and panics with the frame.

## Performance considerations

Dispatch is one bounds check, one counter increment, one acquire load,
one indirect call. There is nothing to optimise until a profile says so.

## Security considerations

- Only kernel code can register; there is no user-facing surface.
- An unhandled exception is fatal by policy, never ignored (constitution
  section 54: never silently ignore page faults).
- Handlers run with interrupts disabled on the interrupted stack, so a
  handler that loops or sleeps hangs the CPU; the context rules in
  `api.md` are mandatory.

## Testing strategy

The `breakpoint-trap` self-test exercises register, duplicate-register,
wrong-unregister, real dispatch via `int3`, count increment, unregister,
and bad-argument rejection. See `testing.md`.

## Future extensibility

- **VMM (Phase 2)** registers on `ARCH_TRAP_PAGE_FAULT` to implement demand
  paging and copy-on-write.
- **Timer (Phase 3)** registers on a LAPIC timer vector allocated by the
  interrupt-controller layer.
- **IPIs (Phase 3)** register on reserved vectors for TLB shootdown,
  reschedule, and halt.
- **Devices (Phase 6)** register on IOAPIC/MSI vectors; the controller
  layer will add IRQ-to-vector allocation and per-vector EOI callbacks
  rather than the fixed PIC range check in `trap.c`.
- **Shared vectors** (multiple handlers) are not supported and not
  planned; MSI makes them unnecessary.
