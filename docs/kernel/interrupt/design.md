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

Registration and removal hold a raw per-vector writer lock with local
IRQs masked. Competing CPUs cannot both claim an empty slot or overwrite
its alternating record index. No allocation, tracked lock, handler call,
or grace-period wait occurs under this lock; it also works during early
boot. Mutations from NMI/#MC are unsupported because they could interrupt
a writer on the same CPU. Ordinary IRQ writers are safe because IRQs are
masked during each hold. Dispatch never takes the writer lock.

```
interrupt_register(v, fn, arg, name):
    validate v and fn
    lock slot.writer with local IRQs masked
    slot.cur != NULL → unlock and return -EBUSY
    fill next alternating record: fn, arg, immortal name
    atomic_store_release(&slot.cur, record)
    unlock and restore IRQ state
```

Unregistration checks the current record's function under the same lock,
returning `-ENOENT` on mismatch, then clears only `slot.cur`. It never
clears a record a dispatcher may still hold. A grace period remains
necessary before freeing handler state or reusing a registration record;
writer serialization does not replace the owner's lifecycle coordination.

## Dispatch

```
interrupt_dispatch(v, frame):
    v >= g_vector_count → panic_frame(...)        arch bug
    atomic_fetch_add_relaxed(&slot.count, 1)
    record = atomic_load_acquire(&slot.cur)
    record == NULL → arch_trap_unhandled(v, frame); return
    record.fn(v, frame, record.arg)
```

One acquire load selects the function and argument together. The count is
incremented before lookup, including unhandled dispatches. No writer lock
is taken, including on the x86 paranoid/NMI entry path.

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

## SMP publication and lifetime

The published record pointer uses release/acquire ordering. Per-vector
writer serialization protects slot ownership and record selection across
CPUs; different vectors do not share a writer lock. Dispatch counts use
atomic increments and loads.

A CPU may load a record just before unregistration clears it, then invoke
the handler afterwards. `interrupt_unregister_sync` and the IRQ release
paths call `synchronize_irq` (one `synchronize_quiesce`) before handler
state may be freed. Record storage alternates between two entries. The
existing caller requirement to coordinate unregister, grace period and
re-registration remains: the writer lock alone does not make rapid record
reuse safe against an older dispatch. See `docs/kernel/quiesce/design.md`.

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

`interrupt_count(v)` uses an atomic relaxed load; it samples a counter
without promising the current handler's completion. Name lookup acquires
the published pointer and atomically samples its immortal name. It can
observe an old or reused record's name during replacement, but never a
non-atomic pointer race. This deliberately avoids a writer lock or grace
period in diagnostics, including NMI name lookup. Neither accessor pairs
its result with the other accessor's result or with a registration handle.

## Memory

Zero allocations. `sizeof(g_slots)` is 256 × 32 = 8 KiB of `.bss`.

## Security

No user-reachable surface. Handler pointers live in kernel `.bss`, which
is mapped `RW+NX`; a write primitive into it would be a full compromise
regardless, so no additional hardening (such as a read-only table after
boot) is planned until the module loader exists and needs it.
