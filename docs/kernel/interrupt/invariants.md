# Interrupt Subsystem: Invariants

## I-INT-1: One handler per vector

A slot holds at most one function. Sharing is not supported; a second
registration returns `-EBUSY`. **Checked by** `interrupt_register` and the
`breakpoint-trap` and `irq-writers` self-tests and concurrent host registration test.

## I-INT-2: Generic code never contains a literal vector number

Vectors come from `arch_trap_vector()` or, later, the interrupt-controller
allocator. **Checked by review**: `grep -rn "interrupt_register(" kernel/
--include=*.c` outside `kernel/arch/` must show no integer literals in
the first argument.

## I-INT-3: `fn` is published after `arg` and `name`

Release publication of an immutable record pointer, acquire load on
dispatch; clear the pointer on removal. The function and argument come
from the same loaded record. **Checked by** concurrent host registration
and dispatch using distinct function/argument identities; `irq-writers` checks
the winning identity via real IPI and reuse after a grace period, while
`irq-sync` holds a handler active across unregister.

## I-INT-4: Per-vector writers serialize with local IRQs disabled

`interrupt_register` and removal share a raw per-vector writer lock and
restore the caller's IRQ state. There is no allocation, handler call, or
grace-period wait under it. NMI/#MC table mutation is unsupported.
**Checked by** code structure, competing host writers under TSan, kernel
`irq-writers` registration/removal races, and
`irq-state`/`breakpoint-trap` for the IRQ-state primitives and trap path.

## I-INT-5: An unregistered exception is fatal

`arch_trap_unhandled` panics for `arch_trap_is_exception(vector)`. Nothing
in this subsystem may swallow an exception. **Checked by** `make
test-crash`, where an unhandled `#PF` must produce a full panic report and
exit code 35.

## I-INT-6: Every dispatch is counted, handled or not

An atomic increment precedes lookup and `interrupt_count` loads atomically.
**Checked by** exact IPI counts in `irq-writers`, `breakpoint-trap`, and
the concurrent host test, which counts
handled and unhandled dispatches exactly after joining the dispatchers.
`irq-unhandled` checks real unregistered delivery on every online CPU,
repeated delivery after EOI, and handled reuse. Architecture-wide unhandled
totals also increment atomically so concurrent warning paths cannot lose updates.

## I-INT-7: The table owns nothing

No allocation, no free. `arg` and `name` lifetimes are the registrant's
responsibility. **Checked by review**; there is no allocator to misuse
yet.

## I-INT-8: Handlers do not sleep, allocate, or take sleeping locks

Interrupt context rule from constitution section 53. **Checked by
review** and `might_sleep()` at sleeping primitive entry, with IRQ context
tracked by the architecture entry path. Allocation remains a review rule;
`kmalloc` currently does not sleep.

## I-INT-9: Dispatch takes no writer lock

Dispatch is lock-free by design: release/acquire on the record pointer
plus a grace period on removal (`synchronize_irq`; constitution
Invariant 12). **Checked by review and by `irq-sync`** (a handler running
on another CPU is outlasted by `interrupt_unregister_sync`).

## I-INT-10: `arch_trap_vector_count() <= INTERRUPT_MAX_VECTORS`

An architecture with more vectors must raise the constant, not truncate
(the constant became 1344 when AArch64 arrived with 1312 vectors).
**Checked by** the panic in `interrupt_init`.
