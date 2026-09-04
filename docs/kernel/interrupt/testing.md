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

### Static analysis

`make analyze` runs the clang analyzer on `interrupt.c`.

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

Look for `SELFTEST: breakpoint-trap ... ok` in
`out/x86_64-debug/boot-test.log`.
