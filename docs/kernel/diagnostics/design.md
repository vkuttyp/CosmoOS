# Diagnostics Subsystem: Design

## console.c: sink fan-out

`console_register` prepends a `struct console_sink` to a singly linked
list; `console_write` walks the list under the console spinlock and
calls each sink's `write`. Since Phase 6 the list can change after boot:
the `virtio_console` module registers a sink when it loads and
`console_unregister` unlinks it on unload under the console lock, so a
concurrent writer never sees a half-removed list; registration prepends
without the lock (a single pointer store, safe against readers).
Prepending means the most
recently registered (usually more capable) device sees output first;
the virtio console therefore precedes the UART. `console_puts` is
`console_write(s, strlen(s))`.

A sink's `write` must be non-blocking beyond polling its device, because
`console_write` is called from interrupt and panic context.

## log.c: levels, prefix, truncation

Levels: `KLOG_DEBUG` (0), `KLOG_INFO`, `KLOG_WARN`, `KLOG_ERROR`,
`KLOG_PANIC` (4). Each line is prefixed with an 8-character tag:

```
[DEBUG]  [ INFO]  [ WARN]  [ERROR]  [PANIC]
```

The fixed width keeps columns aligned and lets the harness match
`^\[ INFO\] boot complete` exactly.

`kvlog` drops messages below `g_level`, copies the tag into a
`KLOG_LINE_MAX` (256) byte stack buffer, formats the message after it
with `kvsnprintf`, and if the result would overflow, truncates so that a
newline and terminator still fit. It then emits the line in one
`console_write`. Lines are never dropped for length, only shortened.

Default level is `KLOG_DEBUG` when `CONFIG_DEBUG=1` (debug builds) and
`KLOG_INFO` otherwise; `klog_set_level` changes it at runtime.

`kprintf`/`kvprintf` format the same way but add no prefix and no newline.
They exist for the banner, self-test lines, and panic dumps, where the
output grammar is part of the test contract.

## printf.c: the formatter

`kvsnprintf(buf, size, fmt, ap)` implements C99 `vsnprintf` semantics for
integers and strings:

- conversions `c s d i u x X o p %`;
- flags `-` `0` `+` space `#`;
- width and precision as digits or `*`;
- length modifiers `hh h l ll z t j`.

`%p` ignores width/precision and always prints `0x` plus 16 lowercase hex
digits (18 characters) so addresses align in dumps. Precision 0 with
value 0 prints nothing, as in C99. `#` adds `0x`/`0X` for non-zero hex and
a leading `0` for octal. `%s` with NULL prints `(null)`.

The return value is the length the full output would have had; when
`size > 0` the buffer is always NUL-terminated at `min(pos, size - 1)`.
`ksnprintf(NULL, 0, ...)` is therefore a valid way to measure.

Floating point is absent by design: the kernel is compiled with
`-mgeneral-regs-only`, so there is no FP state and no reason to link
conversion code that could not be exercised.

Output goes through a small `struct out` cursor; no intermediate buffer
larger than the 24-byte digit scratch is used.

## panic.c: fatal error reporting

Semantics follow constitution section 56:

| Primitive | Meaning | Returns |
|---|---|---|
| `panic(fmt, ...)` | unrecoverable state | never |
| `panic_frame(frame, fmt, ...)` | same, originating in a trap; `frame` is dumped and the trace starts from the interrupted context | never |
| `BUG()` / `BUG_ON(cond)` | an invariant the code relies on was violated; panic with `BUG:` prefix and `file:line (func)` | never |
| `KASSERT(cond)` | checked invariant, on in every build; panic with the expression text and location | never on failure |
| `WARN(cond, fmt, ...)` | unexpected but survivable; `klog` at WARN with location; evaluates to the condition | always |

Output order in `panic_common`, chosen so the most useful line survives
even if the rest is lost:

1. `KERNEL PANIC: <reason>`
2. `CPU: <id>  context: boot (no threads yet)`
3. register dump via `arch_trap_frame_dump` when a frame was supplied
4. `stack trace:` followed by `  #n 0x...` lines from `backtrace_print`
5. `halting.`

Then `arch_emulator_exit(ARCH_EMULATOR_EXIT_FAILURE)` and
`arch_cpu_halt_forever()`.

Recursion guard: `g_panicking` is set on entry; a second panic prints
`KERNEL PANIC (recursive)` with its reason only, requests failure exit,
and halts. Interrupts are disabled at the start of every panic so a
handler cannot interleave with the report.

`backtrace_print(from)` collects up to 32 frames with `arch_backtrace`
and prints each as `#n <addr>`, marking addresses outside kernel text
with `(outside kernel text)`. It prints `(no frames)` when the walk
yields nothing and `... (truncated)` when it hits the cap.

## shutdown.c: orderly termination

`kernel_shutdown(status)` logs the status, disables interrupts, calls
`arch_emulator_exit` with `ARCH_EMULATOR_EXIT_SUCCESS` (0x10) or
`ARCH_EMULATOR_EXIT_FAILURE` (0x11), logs `shutdown: halting CPU`, and
halts. The exit request may be honoured asynchronously (QEMU) or ignored
(hardware); either way the halt is reached. QEMU turns the value into an
exit status of `(value << 1) | 1`, so the harness expects 33 for success
and 35 for failure. Real power-off waits for ACPI.

## selftest.c: proving the phase works

Enabled by `CONFIG_SELFTEST` (default 1 for `BUILD=debug`, 0 for
`release`; override with `SELFTEST=`). Each test is a
`bool fn(const char **reason)`; `CHECK(cond)` sets `reason` to the failing
expression and line. Output grammar:

```
SELFTEST: <name padded to 16> ... ok
SELFTEST: <name padded to 16> ... FAIL: <reason>
SELFTEST: PASS (<n> tests)
SELFTEST: FAIL (<k> of <n>)
```

The harness treats any `SELFTEST: FAIL` line as a forbidden marker and,
when any `SELFTEST:` line appears, requires `SELFTEST: PASS`.

Tests and what each proves:

- **printf**: 21 format cases covering every conversion, flag, length,
  truncation return value, and the zero-size buffer.
- **string**: `memset`, `memcpy`, `strlen`, `strnlen`, `memmove` in both
  overlap directions, `strcmp`/`strncmp` ordering, `strlcpy` truncation
  and return value.
- **bootinfo**: non-empty map, usable memory > 0, kernel virtual base
  equals `__kernel_start`, no two entries overlap, and the kernel's
  physical range is never typed `usable`.
- **irq-state**: nested `arch_irq_save`/`restore` preserve IF.
- **breakpoint-trap**: full trap path; see `docs/kernel/interrupt/testing.md`.

`selftest_run_all` returns the failure count; `kernel_main` maps a
non-zero count to `KERNEL_EXIT_FAILURE`.

## Crash test

`make test-crash` builds with `CRASH_TEST=1` into a sibling output tree.
After the self-tests, `kernel_main` logs
`crash test: writing to an unmapped address on purpose` and writes to
`0xFFFF900000000000`, a canonical address whose PML4 slot (288) has no
entry in the bootstrap tables. The resulting `#PF` (error code 2:
not-present, write, kernel) has no registered handler, so
`arch_trap_unhandled` calls `panic_frame`. If the write does not fault,
the kernel logs an error and reports failure anyway, so a broken page
table cannot make the crash test pass. The harness runs with
`--expect-panic` and requires the full report and exit code 35.

## string.c

Byte loops for `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`,
`strnlen`, `strcmp`, `strncmp`, `strlcpy`. They carry the standard names
because the compiler emits calls to them for aggregate copies; word-sized
fast paths wait for a measurement (coding rule 9).

## What is missing and planned

- **Ring buffer**: `kvlog` will also append to a fixed buffer readable by
  a future `dmesg`; the console becomes one consumer.
- **Symbol resolution**: an embedded sorted symbol table so traces print
  `function+offset`.
- **Timestamps**: once a monotonic clock exists.
- **Lock diagnostics**: might-sleep assertions, lock ordering checks.
- **GDB stub**: remote debugging over a second UART.
- **Crash dumps**: after storage exists.
