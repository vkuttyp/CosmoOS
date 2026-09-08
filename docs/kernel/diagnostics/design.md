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

## fbcon.c: the framebuffer console

The second kind of console device, and the first that is not a UART: the
display the firmware has already lit. The loader locates the Graphics
Output Protocol, takes the mode it finds -- never `SetMode`, because
choosing modes is the beginning of the display driver section 60 defers
-- and reports base, size, geometry and the pixel format as three
(shift, bits) pairs in boot protocol v6. Everything the console then
writes is bounded by `bootinfo_fb_validate`, a pure function of those
fields (`kernel/core/fbvalid.c`), which is also a host test and a fuzz
target: the failure it prevents is writing outside the mapping on
someone else's firmware, which is exactly the bug a machine with no
serial port cannot report.

`fbcon_init` runs straight after `vmm_init`, the first moment the range
can be mapped (`vm_map_phys`, uncached). It refuses a framebuffer that
overlaps memory the page allocator may hand out -- one of the two would
scribble on the other -- clears the screen, and registers a sink. What
was printed before it existed is not lost: the newest screenful of the
log ring is replayed, so the display starts with the boot rather than
with whichever line happened to be next. Older lines are not replayed,
because scrolling them through an uncached framebuffer costs hundreds of
megabytes of bus traffic to draw frames nobody sees, and `dmesg` has the
whole ring either way.

The face is 8x8 cells with 5x7 glyphs, drawn in the tree
(`tools/mkfont.py` generates `kernel/core/font8x8.c` from glyph art in
the script); copying a font table in is the borrowed artifact "a new
operating system from scratch" excludes. A byte outside 0x20..0x7e draws
a box, so nothing is silently invisible.

Two things shape the rest. The sink runs on the panic path, so it
allocates nothing, sleeps never, and holds no lock of its own --
`console.c` already serialises sinks and drops that lock for good in
panic mode, which is the same arrangement the serial sinks rely on. And
it keeps the screen as characters in RAM (`cols x rows` bytes, allocated
once at init), which makes a scroll a memmove of the shadow followed by
a repaint from it: writes only, no reads of the framebuffer, which are
several times more expensive uncached.

A scroll moves several rows at once (`rows / 8`) rather than one. The
cost of a scroll is a screen repaint and does not depend on how far it
moves, so scrolling by one row would repaint the screen for every line
printed; measured on the harness, that is 10.9 ms a line against 1.2 ms
at eight rows, and a boot prints thousands of lines
(`docs/kernel/diagnostics/testing.md`, "Benchmarks"). What the chunk
costs is blank space at the bottom, which the next lines fill.

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
