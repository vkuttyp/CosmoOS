# Interrupt Subsystem: Testing

## Current coverage

### `breakpoint-trap` self-test (`kernel/core/selftest.c`)

Runs in every `CONFIG_SELFTEST=1` boot (`make test`, debug builds). Steps
and what each proves:

| Step | Proves |
|---|---|
| `arch_trap_vector(ARCH_TRAP_BREAKPOINT) >= 0` and `arch_trap_is_exception` | symbolic vector lookup |
| `interrupt_register(v, bp_handler, &st, "selftest-bp") == 0` | install path |
| second `interrupt_register` on `v` → `-EBUSY` | I-INT-1 |
| `interrupt_unregister(v, other_handler)` → `-ENOENT` | identity check on removal |
| `interrupt_handler_name(v)` is `"selftest-bp"` | diagnostics |
| `arch_debug_break()` then `st.hits == 1`, `st.vector == v` | real hardware trap reached the handler with the right vector |
| `kernel_text_contains(st.pc)` | the frame passed to the handler is the interrupted context |
| `interrupt_count(v) == before + 1` | I-INT-6 |
| `arch_irq_enabled()` afterwards | IRETQ restored RFLAGS; the dispatcher did not leak IF state |
| `interrupt_unregister(v, bp_handler) == 0`, name now NULL | removal path |
| register on `arch_trap_vector_count()` → `-EINVAL`; NULL fn → `-EINVAL` | argument validation |

### Unhandled-exception path (`make test-crash`)

A `#PF` with no handler registered must reach `arch_trap_unhandled` and
panic with a complete report; `tests/boot/run_boot_test.py --expect-panic`
verifies the markers and exit code 35. This covers the `fn == NULL`
branch of `interrupt_dispatch` for an exception.

### `acpi` self-test (`kernel/scheduler/schedtest.c`)

`acpi_available()`, a non-zero `acpi_madt_lapic_base()`, at least one
MADT processor entry, `acpi_find_table("APIC")` non-NULL and a bogus
signature NULL. Proves the RSDP → XSDT → MADT walk and checksum path
that the controllers depend on.

### `irq-route` self-test (`kernel/scheduler/schedtest.c`)

End-to-end hardware interrupt through the I/O APIC, using the PIT as a
test source via `arch/testhooks.h` (`kernel/arch/x86_64/pit.c`):

| Step | Proves |
|---|---|
| `arch_test_periodic_irq_start(200)` → ISA IRQ 0 | PIT channel 0 in rate-generator mode at 200 Hz |
| `irq_legacy_to_gsi(0)` → GSI 2 with edge/high flags | MADT interrupt source override applied (QEMU remaps IRQ 0 to GSI 2) |
| `irq_request(gsi, pit_handler, ...)` == 0 | vector allocated (49 on a fresh boot), handler installed, redirection entry programmed masked |
| second `irq_request` on the GSI → `-EBUSY` | one owner per line |
| `irq_vector_of(gsi) >= 48` | dynamic range |
| `irq_enable`, `udelay(50 ms)`, `hits >= 5` | unmask → IOAPIC → LAPIC → vector → `interrupt_dispatch` → handler, with `arch_irqc_eoi` after each (10 expected, 5 required for TCG slack) |
| `irq_disable`, no new hits over a further 20 ms window | mask stops delivery |
| `irq_release` == 0, `irq_vector_of` == -1 | handler removed, vector returned |
| `arch_test_periodic_irq_stop()` | PIT quiesced |

The test skips itself (logging why) when there is no periodic ISA
source or no I/O APIC covers the GSI, so it does not fail on platforms
that lack them; QEMU q35 has both.

### Static analysis

`make analyze` runs the clang analyzer on `interrupt.c`, `irq.c`, and
the controller drivers.

## Not yet covered

- The `fn == NULL` branch for a non-exception vector (spurious IRQ
  logging). Needs a way to raise an arbitrary vector; `int $N` with a
  runtime `N` requires either a jump table of stubs or self-modifying
  code, so this will come with the LAPIC (send a self-IPI on a known
  free vector).
- Concurrency: registration racing dispatch on another CPU; grace period
  on unregister. Requires SMP (Phase 3) and will be a stress test plus, if
  feasible, a model-checked version of the publish/consume protocol.
- Out-of-range vector into `interrupt_dispatch` (panic path). Only an
  arch bug can produce it; a host-side unit test with a stubbed arch layer
  is the right vehicle once host tests exist.
- Handler re-entrancy: a handler that triggers the same vector. Currently
  undefined and unwanted; a lock-diagnostics layer should detect it.

## Host-side unit tests (planned)

`interrupt.c` depends on only four arch functions, all trivially stubbed.
A host test binary compiling `interrupt.c` against a fake `arch/` can
cover every return code and the dispatch branches without QEMU. This is
the first candidate for the host test harness in `tests/`.

## Running

```sh
make test              # includes the self-test
make test-crash        # unhandled exception path
make analyze
```

Look for `SELFTEST: breakpoint-trap ... ok`, `SELFTEST: acpi ... ok`, and
`SELFTEST: irq-route ... ok` in `out/x86_64-debug/boot-test.log`; the
debug log also shows `irq: GSI 2 -> vector 49 on CPU 0 (selftest-pit)`.
