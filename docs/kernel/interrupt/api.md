# Interrupt Subsystem: API

Header: `kernel/include/kernel/interrupt.h`. Internal kernel ABI; may
change until the module ABI (Phase 5) freezes it. No function here
allocates or sleeps.

```c
typedef void (*interrupt_handler_fn)(unsigned vector,
                                     struct arch_trap_frame *frame,
                                     void *arg);
```

Handler contract: runs in interrupt context on the interrupted stack with
interrupts disabled; must not sleep, allocate, or take sleeping locks;
`frame` is valid only until return; `arg` is whatever was registered.

---

### `void interrupt_init(void)`
- **Purpose**: size and zero the table.
- **Lifetime**: called once by `kernel_main` before `arch_irq_enable`.
  Calling it again discards all registrations.
- **Concurrency**: single CPU, interrupts disabled by construction.
- **Failure modes**: panics if `arch_trap_vector_count()` exceeds
  `INTERRUPT_MAX_VECTORS` (1344; x86-64 uses 256, AArch64 1312).

### `int interrupt_register(unsigned vector, interrupt_handler_fn fn, void *arg, const char *name)`
- **Purpose**: install `fn` on `vector`.
- **Inputs**: `vector` from `arch_trap_vector()` or a controller
  allocation; `fn` non-NULL; `arg` opaque; `name` string literal or
  otherwise immortal (NULL becomes `"?"`).
- **Outputs**: `0`, `-EINVAL` (vector out of range or `fn` NULL),
  `-EBUSY` (already registered).
- **Ownership**: the table does not own `arg` or `name`. The registrant
  keeps them valid until `interrupt_unregister` returns.
- **Concurrency**: disables local interrupts for the update; publishes
  with release semantics. Safe to call from any context including a
  handler (it is non-blocking), though registering from inside a handler
  is unusual.
- **ABI**: internal.

### `int interrupt_unregister(unsigned vector, interrupt_handler_fn fn)`
- **Purpose**: remove `fn` from `vector`.
- **Outputs**: `0`, `-EINVAL` (bad vector or NULL `fn`), `-ENOENT` (`fn`
  is not the installed handler).
- **Lifetime**: after return, on a single CPU, no further invocations of
  `fn` occur and `arg` may be freed. Under SMP a grace period will be
  required; see `design.md`.

### `void interrupt_dispatch(unsigned vector, struct arch_trap_frame *frame)`
- **Purpose**: deliver a trap to its handler. Called only by the
  architecture layer's trap entry.
- **Inputs**: `vector` as decoded by the arch; `frame` on the interrupted
  stack.
- **Blocking**: never; runs the handler synchronously.
- **Failure modes**: `vector >= arch_trap_vector_count()` panics with the
  frame. No handler → `arch_trap_unhandled()` (fatal for exceptions).

### `uint64_t interrupt_count(unsigned vector)`
- **Purpose**: diagnostics. Number of dispatches of `vector` since
  `interrupt_init`, handled or not.
- **Outputs**: 0 for an out-of-range vector.
- **Concurrency**: unsynchronised read; may be momentarily stale under
  SMP.

### `const char *interrupt_handler_name(unsigned vector)`
- **Purpose**: diagnostics. Name given at registration.
- **Outputs**: NULL if unregistered or out of range. The string belongs
  to the registrant.

---

## Obtaining vectors

```c
int v = arch_trap_vector(ARCH_TRAP_PAGE_FAULT);   /* -1 if unsupported */
if (v >= 0)
    rc = interrupt_register((unsigned)v, vmm_page_fault, NULL, "vmm-pf");
```

Literal vector numbers in generic code violate Invariant 1. Vectors for
external devices come from the IRQ layer below (`irq_request` allocates
one through `arch_vector_alloc`).

### `int interrupt_unregister_vector(unsigned vector)`
- **Purpose**: remove whatever handler `vector` has, for owners of a
  vector that do not track the function pointer (the IRQ layer).
- **Outputs**: `0`, `-EINVAL`, `-ENOENT` (nothing registered).
- **Concurrency**: as `interrupt_unregister`.

---

## Phase 3: hardware IRQ lines (`kernel/include/kernel/irq.h`)

GSI-numbered interrupt lines behind the controllers. All functions take
`g_irq_lock` (irqsave). `irq_request`/`irq_release` are not for
interrupt context (they program the controller and touch the vector
table); `irq_enable`/`irq_disable` are interrupt-safe.

### `void irq_init(void)`
- Clears the GSI table and calls `arch_irqc_init()` (LAPIC + IOAPICs from
  ACPI). Once, after `acpi_init`, before `timer_init`. Panics without a
  local APIC.

### `int irq_request(irq_t irq, interrupt_handler_fn fn, void *arg, const char *name, unsigned flags, unsigned cpu)`
- **Purpose**: allocate a dynamic vector, install `fn` on it, and program
  the controller so `irq` delivers that vector to `cpu`, initially
  masked.
- **Inputs**: `irq` is a GSI (`< IRQ_MAX` = 1024: an IOAPIC input on
  x86-64, a GIC INTID on AArch64); `flags` is
  `IRQ_TRIGGER_EDGE` (0) or `IRQ_TRIGGER_LEVEL`, optionally
  `IRQ_POLARITY_LOW`; `cpu` must be a registered CPU.
- **Outputs**: `0`, `-EINVAL`, `-EBUSY` (GSI already requested),
  `-ENOSPC` (no vector), `-ENODEV` (no I/O APIC covers the GSI).
- **Ownership**: `arg` and `name` as for `interrupt_register`.

### `int irq_release(irq_t irq)`
- Masks the line, removes the handler (`interrupt_unregister_vector`),
  frees the vector. `-ENOENT` if not requested.

### `int irq_enable(irq_t irq)` / `int irq_disable(irq_t irq)`
- Unmask / mask the redirection entry. `-EINVAL` if not requested.

### `irq_t irq_legacy_to_gsi(unsigned isa_irq, unsigned *flags_out)`
- Apply the MADT interrupt source overrides (bus 0) to an ISA IRQ; the
  identity mapping and edge/high otherwise. `flags_out` receives the
  trigger/polarity to pass to `irq_request`. On QEMU, ISA IRQ 0 maps to
  GSI 2.

### `int irq_vector_of(irq_t irq)`
- Diagnostics: the assigned vector or `-1`.

## Phase 6: message-signalled interrupts (`kernel/include/kernel/irq.h`)

### `int irq_request_msi(interrupt_handler_fn fn, void *arg, const char *name, unsigned cpu, struct irq_msi_msg *msg)`
- **Purpose**: give a device a vector it can raise by writing a message.
  Allocates a vector from the dynamic range with `arch_vector_alloc`,
  registers `fn` on it, and fills `msg->addr`/`msg->data` through
  `arch_irqc_msi_compose`. The PCI core programs the message into an
  MSI-X table entry or the MSI capability.
- **Inputs**: non-NULL `fn` and `msg`; `cpu` the target CPU.
- **Outputs**: the vector (>= 0), `-EINVAL`, or `-ENOSPC`.
- **Concurrency**: `g_irq_lock` spinlock; no allocation; thread context
  by convention. Dispatch and EOI are the ordinary vector path: an MSI
  is indistinguishable from any other vector once it arrives.

### `int irq_release_msi(int vector)`
- **Purpose**: unregister the handler and free the vector. The device
  must be masked first (the PCI core does this in `pci_msix_release`).
- **Outputs**: 0 or `-EINVAL`.

## Phase 3: controller interface (`kernel/include/arch/irqc.h`)

Implemented by `kernel/arch/x86_64/irqc.c` over `lapic.c` and
`ioapic.c`; generic code never includes those.

| Function | Contract |
|---|---|
| `arch_irqc_init()` | boot CPU: LAPIC at the MADT base, all IOAPICs registered and fully masked |
| `arch_irqc_init_cpu()` | calling AP: its LAPIC |
| `arch_vector_alloc()` / `arch_vector_free(v)` | dynamic vectors under a spinlock; `-ENOSPC` when exhausted |
| `arch_irqc_route(gsi, vector, cpu, flags)` | program the redirection entry masked; `-ENODEV`, `-EINVAL` |
| `arch_irqc_mask(gsi)` / `arch_irqc_unmask(gsi)` | bit 16 of the entry |
| `arch_irqc_eoi(vector)` | no-op for exceptions and the spurious vector; PIC EOI for 32–47; LAPIC EOI otherwise. Called by the arch dispatch tail after every interrupt handler |
| `arch_irqc_gsi_count()` | highest covered GSI + 1 |
| `arch_irqc_spurious_vector()` | 255 |
| `arch_ipi_send(cpu, vector)` / `arch_ipi_broadcast_others(vector)` | fixed-delivery IPIs (used by the SMP work) |

### `int arch_irqc_msi_compose(unsigned vector, unsigned cpu, uint64_t *addr, uint32_t *data)`
- **Purpose**: the architecture's message format for `vector` on `cpu`.
  x86-64 (`kernel/arch/x86_64/irqc.c`): `addr = 0xFEE00000 | apic_id <<
  12` (physical destination, fixed delivery, edge), `data = vector`.
  AArch64 (`kernel/arch/aarch64/gic.c`): allocates an SPI from the GICv2m
  frame, routes it to `vector` on `cpu`, `addr` = frame + 0x40
  (`MSI_SETSPI_NS`), `data` = the INTID.
- **Outputs**: 0, or `-EINVAL` for a CPU without an APIC id below 256
  (xAPIC physical mode only) or a vector outside 48 to 238; on AArch64
  `-EINVAL` without a GICv2m frame, `-ENOSPC` when its SPIs are all taken.
- **Concurrency**: pure on x86-64; takes the GIC lock on AArch64. Any
  context.

## x86-64 vector map

(The AArch64 map — INTIDs 0..1019, spurious 1020, exceptions 1024..1029,
dynamic 1056..1311 — is in `docs/kernel/arch/aarch64/design.md`.)

| Range | Use |
|---|---|
| 0–31 | exceptions |
| 32–47 | legacy PIC, masked; kept for spurious identification |
| 48–238 | dynamic: `irq_request`, the LAPIC tick (`arch_timer_vector`), IPIs |
| 239–254 | reserved |
| 255 | LAPIC spurious vector (no EOI) |

## Example handler

```c
static void timer_tick(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    struct timer_state *t = arg;
    t->ticks++;
    /* no logging on the hot path, no locks that can sleep */
}
```
