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
| `irq-sync` (`kernel/core/quiescetest.c`): a handler spinning 20 ms on CPU 1, raised with `arch_ipi_send`; `interrupt_unregister_sync` returns only after the handler set its `done` flag (≥ 10 ms) | a handler is a read-side section; `synchronize_irq` outlasts it (`docs/kernel/quiesce/testing.md`) |
| register on `arch_trap_vector_count()` → `-EINVAL`; NULL fn → `-EINVAL` | argument validation |

### `irq-writers` self-test (`kernel/core/quiescetest.c`)

For 16 rounds, two pinned threads rendezvous and compete to register one
allocated vector. Exactly one must succeed and the other return `-EBUSY`.
A real IPI then calls the winner; distinct handlers check their argument
identity, vector and frame. Two threads subsequently compete to unregister
the winning handler, with one success and one `-ENOENT`. After joining the
writers, the caller waits through `synchronize_irq` before checking exact
handler/dispatch counts and reusing either record or probe storage.

SMP runs place writers on separate CPUs and deliver the IPI to the remote
CPU. UP runs keep the same rendezvous and checks on one CPU, using a
self-IPI; they do not prove simultaneous writer contention. This test
combines mutation serialization, real entry/dispatch and grace-period
reuse. Dispatch is triggered after registration finishes, so overlap of
publication with dispatch remains covered by the host test, not this test.
The existing `irq-sync` test separately proves a grace period outlasts a
handler deliberately held active.

Allocation/rendezvous failures release and join any created workers,
unpublish any registration, and wait before releasing storage. An IPI
delivery timeout fails stop with the vector, handler and stack probes
retained: returning would let a delayed IPI access expired storage. This
is not NMI/#MC mutation coverage or exhaustive interrupt-entry validation.

### `irq-unhandled` self-test (`kernel/core/quiescetest.c`)

Allocate and bind a vector without installing a handler, then send two
rounds of real IPIs to every online CPU, including the caller. Each round
requires the exact dispatch-count increase and a grace period before the
next send. Repeated delivery on the same vector checks that the unhandled
path returns through interrupt completion (EOI). The test then registers
a handler on that vector, sends a self-IPI, verifies its identity and
count, and unregisters synchronously before freeing the vector.

On four CPUs the boot log contains eight architecture-specific unhandled
warnings; on UP it contains two. Architecture-wide unhandled totals use
atomic increments; their log lines can appear out of numeric order.
A missing delivery fails stop at a one-second guard, retaining the vector
and any live probe. Grace periods follow observed dispatch, not merely
sending an IPI. This covers ordinary unregistered-vector policy, not the
LAPIC's hardware-spurious vector, GIC spurious INTIDs, or fatal exceptions.
A temporary x86 UP negative control suppressed EOI only when no handler
was registered: the first self-IPI logged normally, the second remained
pending, and the exact delivery-timeout panic fired. The normal UP case
passes both rounds. This proves the repeated-send check detects a missing
EOI; the injection is not part of the production source.

### Unhandled-exception path (`make test-crash`)

A `#PF` with no handler registered must reach `arch_trap_unhandled` and
panic with a complete report; `tests/boot/run_boot_test.py --expect-panic`
verifies the markers and exit code 35. This covers the `record == NULL`
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

- Exhaustive architecture interrupt-entry interleavings and NMI/#MC
  mutation. Host publication tests do not model those entry protocols;
  `irq-sync` and `irq-writers` exercise real IPIs and grace-period paths.
- Handler re-entrancy: a handler that triggers the same vector. Currently
  undefined and unwanted; a lock-diagnostics layer should detect it.

## Host tests

`tests/host/test_interrupt.c` compiles the actual `interrupt.c` with host
architecture shims. It is included in `make host-test` (ASan/UBSan), with
`make host-test-interrupt-tsan` providing a separate TSan binary.

Before starting threads, the test exercises runtime vector counts of 16
and the full 1344-slot table capacity (I-INT-10). The last valid vector
accepts registration and dispatch, preserving the handler's argument and
frame. Dispatching the first invalid vector or `UINT_MAX` must call
`panic_frame` with the original frame and exact diagnostic; a NULL frame
is checked too. Architecture counts of 1345 and `UINT_MAX` must instead
call `panic` during initialization. Panic interception uses `longjmp`
only in this single-threaded phase, with reinitialization after a failed
init; it does not exercise the kernel panic renderer or shutdown path.

Invalid-vector API calls, NULL and wrong-function removals, duplicate
registration, and absent-handler removals must return the documented
errors without damaging a live registration or triggering a grace period.
Both successful synchronous-removal variants must unpublish before the
stub grace period and increment its statistic once afterwards. Dispatch
counts remain cumulative across removal and reuse. This checks wrapper
ordering and error handling, not real grace-period completion.

For 64 rounds, four writers race to register one empty vector while four
other threads dispatch it. Exactly one registration must succeed; all
handlers check their function/argument identity. Every handled and
unhandled dispatch contributes to an exact final counter. Four removers
then race and exactly one succeeds. A diagnostic reader runs throughout,
including record reuse, sampling counts and immortal names. Argument
validation and wrong-handler removal are checked too.

IRQ masking is a host no-op: the real per-vector lock must serialize
writers. Dispatch threads are joined before unregister/reuse, and the
grace-period function is a stub; this test does not prove the kernel's
epoch protocol. Real spurious-vector policy remains outside this host
test. The `irq-sync`, `irq-writers`, `irq-unhandled` and trap
boot tests supply integration evidence.

The pre-fix source fails TSan on the plain counter load versus atomic
dispatch increment. A second negative control, retaining atomic diagnostics
but removing writer exclusion, fails on racing slot publication. Neither
mutation is part of the source tree.

Boundary negative controls each fail at the intended assertion: changing
the dispatch guard from `>=` to `>` permits the first invalid vector to
return; disabling the initialization guard accepts an oversized table;
and synchronizing after a failed removal reaches the grace-period stub
while the registration is still published. These temporary source
mutations are not part of the production dispatcher.

## Running

```sh
make test              # includes the self-test
make test-crash        # unhandled exception path
make analyze
```

Look for `SELFTEST: breakpoint-trap ... ok`, `SELFTEST: acpi ... ok`, and
`SELFTEST: irq-route ... ok` in `out/x86_64-debug/boot-test.log`; the
debug log also shows `irq: GSI 2 -> vector 49 on CPU 0 (selftest-pit)`.
