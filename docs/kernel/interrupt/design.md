# Interrupt Subsystem: Design

## The table

One `struct interrupt_slot` per vector, `INTERRUPT_MAX_VECTORS` = 1344
slots, statically allocated. `interrupt_init` asks the architecture how
many vectors it actually dispatches (`arch_trap_vector_count()`: 256 on
x86-64, 1312 on AArch64) and panics if that exceeds the constant, so an
architecture with a larger vector space fails loudly at boot rather than
indexing past the array. AArch64 folds the 1020 GIC INTIDs, a spurious
vector, the synchronous exception kinds and 256 dynamic software vectors
into this space in its arch layer (`docs/kernel/arch/aarch64/design.md`,
"Vector numbering").

## Registration

```
interrupt_register(v, fn, arg, name):
    v >= g_vector_count || fn == NULL  → -EINVAL
    s = arch_irq_save()
    slot.fn != NULL                    → -EBUSY
    slot.arg = arg; slot.name = name
    barrier()
    atomic_store_release(&slot.fn, fn)
    arch_irq_restore(s)
```

`arg` and `name` are written before `fn` is published with release
semantics, so a dispatcher that observes `fn` (acquire) also observes the
fields it needs. Local interrupts are disabled around the update so a
trap on the same CPU cannot observe a half-written slot.

Unregistration is the mirror image: clear `fn` first (release), then
`arg`/`name`. It returns `-ENOENT` if `fn` is not the function currently
installed, which catches a subsystem unregistering someone else's handler.

## Dispatch

```
interrupt_dispatch(v, frame):
    v >= g_vector_count → panic_frame(...)        arch bug
    slot.count++
    fn = atomic_load_acquire(&slot.fn)
    fn == NULL → arch_trap_unhandled(v, frame); return
    fn(v, frame, slot.arg)
```

The count is incremented before the lookup so unhandled vectors are
counted too; `interrupt_count` therefore answers "how often did this
vector fire", not "how often was it handled".

## Handler context

Handlers run:

- on the interrupted stack (no stack switch except `#DF` on IST1),
- with interrupts disabled (interrupt gates clear IF),
- before any EOI (the arch layer sends it after the handler returns),
- with a frame pointer that is valid only until they return.

Therefore handlers must not sleep, must not allocate (there is no
allocator, and when there is one its interrupt-safe variant will be a
distinct API), must not take a lock that can sleep, and must be short.
Anything longer is deferred; the deferred-work mechanism arrives with the
scheduler in Phase 3 and will be the recommended pattern from constitution
section 53 (minimal handler → queue work → worker thread).

## SMP plan

The single-CPU version is already written in the shape SMP needs:

1. Publish/consume of `fn` is release/acquire, so a handler installed on
   CPU A is seen complete by CPU B.
2. The grace period on unregistration exists since the lifetime pass:
   CPU B may have loaded the record just before CPU A cleared it and
   still be running the handler, so `interrupt_unregister_sync` (and the
   IRQ layer's release paths) call `synchronize_irq`, one
   `synchronize_quiesce`, before `arg` may be freed
   (`docs/kernel/quiesce/design.md`). Each slot publishes a pointer to an
   immutable `{fn, arg, name}` record (two per slot, alternating), so a
   dispatcher that loaded a record uses a coherent pair even while the
   slot is being re-registered.
3. Registration from two CPUs racing for the same slot needs a
   compare-and-swap instead of load-then-store; that change is local to
   `interrupt_register`.
4. `count` becomes a per-CPU counter or an atomic increment; per-CPU is
   the section 21 recommendation for high-frequency counters.

No global lock is planned (Invariant 12).

## Relationship with the architecture layer

| Concern | Owner |
|---|---|
| Vector numbering, which vectors are exceptions | arch (`arch_trap_vector`, `arch_trap_is_exception`) |
| Register save/restore, stack, IRET | arch (`isr.S`) |
| EOI to the controller | arch (`x86_trap_dispatch` → `pic_eoi`) |
| Policy for unregistered vectors | arch (`arch_trap_unhandled`), because whether a vector is fatal is an architecture fact |
| Handler table, counts, names | generic (this subsystem) |

The arch layer calls exactly one function here, `interrupt_dispatch`.
This subsystem calls exactly four arch functions: `arch_trap_vector_count`,
`arch_irq_save`, `arch_irq_restore`, `arch_trap_unhandled`.

## Error handling

All failures are returned, never logged silently: `-EINVAL`, `-EBUSY`,
`-ENOENT`. The only panic is the out-of-range vector in dispatch, which
cannot be caused by a caller and indicates a corrupted frame or a
mismatch between the stub count and `arch_trap_vector_count`.

## Diagnostics

`interrupt_count(v)` and `interrupt_handler_name(v)` are read without
synchronisation; they are for logs and a future `/proc`-style view, not
for control flow. `arch_trap_unhandled` uses the count of unhandled
interrupts in its warning line.

## Memory

Zero allocations. `sizeof(g_slots)` is 256 × 32 = 8 KiB of `.bss`.

## Security

No user-reachable surface. Handler pointers live in kernel `.bss`, which
is mapped `RW+NX`; a write primitive into it would be a full compromise
regardless, so no additional hardening (such as a read-only table after
boot) is planned until the module loader exists and needs it.
