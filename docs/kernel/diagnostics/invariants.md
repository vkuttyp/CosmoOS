# Diagnostics Subsystem: Invariants

## I-DIAG-1: No diagnostic path allocates

`klog`, `kprintf`, `panic`, `backtrace_print`, and every sink use only
stack storage or static objects. A panic during an allocator failure must
still print. **Checked by review**; there is no allocator yet, so the
compiler enforces it today.

## I-DIAG-2: No diagnostic path sleeps or takes a sleeping lock

All of it must work from interrupt and panic context. **Checked by
review**; a lock-diagnostics layer is planned.

## I-DIAG-3: A log line is formatted completely before it reaches the console

Each writer formats into its own stack buffer and issues one
`console_write` per line, so concurrent writers interleave at line
granularity, never mid-character. **Checked by** the structure of
`kvlog`.

## I-DIAG-4: Truncation never loses the line terminator

`kvlog` reserves room for `\n` and `\0`; `kvsnprintf` always terminates
when `size > 0`. **Checked by** the `printf` self-test truncation case.

## I-DIAG-5: Format strings are literals

Every variadic function carries `__printf(fmt_index, first_arg)`, every
`va_list` consumer carries `__printf(fmt_index, 0)`, and
`-Wformat=2 -Werror` is on. **Checked by the compiler**.

## I-DIAG-6: Panic never returns and never recurses unboundedly

`panic_common` is `__noreturn`, sets `g_panicking`, and a nested panic
takes the short path straight to halt. **Checked by** the compiler for
`noreturn` and by review for the guard.

## I-DIAG-7: Panic requests the failure exit code before halting

`panic_common` calls `arch_emulator_exit(ARCH_EMULATOR_EXIT_FAILURE)`
before `arch_cpu_halt_forever`, so a panic under QEMU always yields exit
status 35, never a hang. **Checked by** `make test-crash`.

## I-DIAG-8: The exit-code contract is fixed

`0x10` = success, `0x11` = failure, delivered through
`arch_emulator_exit`; QEMU maps them to 33 and 35. Changing them requires
changing `arch/shutdown.h` and `tests/boot/run_boot_test.py` together.
**Checked by** both `make test` and `make test-crash`.

## I-DIAG-9: The `SELFTEST:` output grammar is stable

`SELFTEST: <name> ... ok|FAIL: <reason>` per test, then
`SELFTEST: PASS (n tests)` or `SELFTEST: FAIL (k of n)`. The harness
parses these lines. **Checked by** `make test`.

## I-DIAG-10: The `[ INFO] boot complete` line marks a clean boot

The harness requires it in normal runs and forbids it in crash-test
runs. It must be emitted only after all self-tests and only when no
crash test fired. **Checked by** both harness modes.

## I-DIAG-11: `KASSERT` is active in every build type

Assertions are cheap and this is a kernel; they are not compiled out in
release. **Checked by** the macro having no `CONFIG_DEBUG` guard.

## I-DIAG-12: `%p` output is exactly 18 characters

Register dumps and traces rely on aligned columns. **Checked by** the
`printf` self-test (`0x0000000000001000`).

## I-DIAG-13: Console sinks are never removed

The sink list is append-at-boot only, which is why the walk needs no
lock. Hot-unplug of a console would require a lock or RCU on the list.
**Checked by** the absence of an unregister function.

## I-DIAG-14: The crash-test hook cannot fire in a normal build

The fault is inside `#if CONFIG_CRASH_TEST`, which is `0` unless
`CRASH_TEST=1` is passed to make, and `make test-crash` builds into a
separate output tree. **Checked by** `build/config.mk` defaults and by
`make test` forbidding the `KERNEL PANIC` marker.

## I-DIAG-15: The console never writes outside the framebuffer

Every pixel `fbcon` writes lies inside the mapping made from the
description `bootinfo_fb_validate` accepted: rows fit the memory the
firmware claimed, a pitch is at least a row wide, a pixel is 8, 16, 24
or 32 bits, and each colour field lies inside a pixel. The cursor is
bounded by the cell grid the same description gives.

**Checked by** `tests/host/test_fbvalid.c` (the refusals, including a
geometry that would overflow a 32-bit product), `tests/fuzz/fuzz_fbvalid.c`
(everything accepted is arithmetically safe, including the last pixel of
the last row), and `fb-console` (what is drawn is where it should be).

## I-DIAG-16: The framebuffer sink obeys the rules the panic path sets

It allocates nothing, sleeps never, and takes no lock: `console.c`
serialises sinks and drops its lock in panic mode, and this sink relies
on exactly that, as the serial sinks do. Its one allocation -- the text
shadow -- happens once, in `fbcon_init`, before the sink is registered.

**Checked by** review, and by the panic run (`make test-crash`), where
the report is drawn after the other CPUs have been halted.

## I-DIAG-17: A framebuffer is never memory somebody else owns

`fbcon_init` refuses a range that overlaps a usable or reclaimable
memory-map entry rather than share it with the page allocator. On
AArch64 the framebuffer is RAM the firmware reserved (`ramfb`), which is
precisely the case that makes the check worth having.

**Checked by** `overlaps_usable_ram` and the boot's free-page count,
which the framebuffer does not change.
