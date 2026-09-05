# Interrupt Subsystem: Invariants

## I-INT-1: One handler per vector

A slot holds at most one function. Sharing is not supported; a second
registration returns `-EBUSY`. **Checked by** `interrupt_register` and the
`breakpoint-trap` self-test.

## I-INT-2: Generic code never contains a literal vector number

Vectors come from `arch_trap_vector()` or, later, the interrupt-controller
allocator. **Checked by review**: `grep -rn "interrupt_register(" kernel/
--include=*.c` outside `kernel/arch/` must show no integer literals in
the first argument.

## I-INT-3: `fn` is published after `arg` and `name`

Release store on install, acquire load on dispatch; clear `fn` first on
removal. A dispatcher that sees a non-NULL `fn` sees a complete slot.
**Checked by** the `__atomic_store_n(..., __ATOMIC_RELEASE)` /
`__atomic_load_n(..., __ATOMIC_ACQUIRE)` pair in `interrupt.c`; a
concurrency test needs SMP and is future work.

## I-INT-4: Table updates run with local interrupts disabled

`interrupt_register`/`interrupt_unregister` wrap their update in
`arch_irq_save`/`arch_irq_restore`. **Checked by** code structure; the
`irq-state` self-test validates the primitive they rely on.

## I-INT-5: An unregistered exception is fatal

`arch_trap_unhandled` panics for `arch_trap_is_exception(vector)`. Nothing
in this subsystem may swallow an exception. **Checked by** `make
test-crash`, where an unhandled `#PF` must produce a full panic report and
exit code 35.

## I-INT-6: Every dispatch is counted, handled or not

`slot.count++` precedes the lookup. **Checked by** the `breakpoint-trap`
self-test comparing `interrupt_count` before and after.

## I-INT-7: The table owns nothing

No allocation, no free. `arg` and `name` lifetimes are the registrant's
responsibility. **Checked by review**; there is no allocator to misuse
yet.

## I-INT-8: Handlers do not sleep, allocate, or take sleeping locks

Interrupt context rule from constitution section 53. **Checked by
review** today; lock diagnostics (a "might sleep" assertion that knows
the current context) are planned with the scheduler.

## I-INT-9: No global lock

Dispatch is lock-free by design: release/acquire on the record pointer
plus a grace period on removal (`synchronize_irq`; constitution
Invariant 12). **Checked by review and by `irq-sync`** (a handler running
on another CPU is outlasted by `interrupt_unregister_sync`).

## I-INT-10: `arch_trap_vector_count() <= INTERRUPT_MAX_VECTORS`

An architecture with more vectors must raise the constant, not truncate
(the constant became 1344 when AArch64 arrived with 1312 vectors).
**Checked by** the panic in `interrupt_init`.
