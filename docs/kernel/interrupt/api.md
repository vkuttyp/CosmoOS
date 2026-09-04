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
  `INTERRUPT_MAX_VECTORS` (256).

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
external devices will come from the interrupt-controller layer (Phase 3/6),
which does not exist yet.

## Example handler

```c
static void timer_tick(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    struct timer_state *t = arg;
    t->ticks++;
    /* no logging on the hot path, no locks that can sleep */
}
```
