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

## lockup.c: a program counter seen from another CPU

`kernel/core/lockup.c` (the unit `docs/audit/next-subsystem-lockup.md`).
The rule, stated once: **a CPU's interrupted context is recorded by that
CPU, in its own handler, into its own per-CPU buffer, without a lock and
without printing; whoever asked reads the buffers and prints.** It
follows from what an NMI may do (nothing that blocks, nothing that takes
a lock the interrupted code may hold -- the console's included) and from
what a frame-pointer walk needs (`arch_backtrace`'s bounds checks are
written for the local thread's stack; another CPU's live stack cannot be
walked from outside).

**The tick sample.** `tick_isr` stores the interrupted PC
(`arch_trap_frame_pc(frame)`) and `now` into `percpu.last_tick_pc` /
`last_tick_ns` before running timers: two stores per tick. It is the
always-present half: what a CPU was doing at its last tick and when that
was. A CPU whose last tick is seconds old is not taking interrupts, and
every other per-CPU field is as old as that.

**The request.** `lockup_sample_all(self, timeout_ns, &answered)` claims
the single reporter slot (`g_reporter`, one compare-and-swap) and
returns `false` at once, having sent nothing, when another CPU holds it:
a reporter may be waiting with interrupts off (the tick is where the
detectors run), and a second CPU spinning for the slot with its own
interrupts off would stop its own ticks and, on AArch64, its own
answers. It then increments the request sequence, writes it into each
online target's `sample.want`, sends the sample interrupt -- on x86-64 an
NMI (`arch_ipi_send_nmi`: the LAPIC ICR with delivery mode NMI, which
carries no vector), on AArch64 the ordinary `IPI_SAMPLE` SGI
(`arch_ipi_send_nmi` returns `false` there; no NMI-class interrupt is
configured) -- records its own sample from `self` (a handler's frame) or
its own walk (a thread), and waits **once, for every target together**,
until each target's `sample.seq` equals the request or the total bound
passes (5 ms in the watchdog and the detectors, whether 3 CPUs or 63 are
asked) -- one deadline, counted where it is armed (`samples_waits`), which
is how the test checks it without a clock (a guest's wall clock measures
its host: the lockup-bound unit). The slot is held until `lockup_print_samples` returns, so no
later request overwrites a buffer under the printer.

**The answer.** `lockup_answer(frame, nmi)`: if this CPU's `sample.want`
equals its `sample.seq` no request is pending for it and it returns
`false`; otherwise it fills `pc`, `sp`, the trace (`arch_backtrace`
from the frame, up to `LOCKUP_TRACE_MAX` = 16), `nmi` and `when_ns`,
stores `seq` with release and returns `true`. A nested interrupt inside
the answer sees the same pending request and writes the same values.
On AArch64 the `IPI_SAMPLE` handler is this function. On x86-64
`x86_trap_paranoid` calls it for `X86_TRAP_NMI` and then **dispatches
exactly as before whenever a handler is registered on the vector**: a
registered NMI handler sees every NMI, sample or not, so nothing a
handler owns is ever swallowed; only with no handler registered does
the answer decide -- an NMI that answered a request returns, one that
did not falls to `arch_trap_unhandled` and panics, as before. The
residual is the architecture's: an x86 NMI carries no vector and no
source, so an unowned NMI landing on a CPU with no registered NMI
handler inside the one-delivery window between a request and its
answer is recorded as the sample and its own cause is not reported --
where before it was a panic without a cause either. No source on this
kernel's machines raises one (no NMI watchdog, no SERR/PERR routing, no
NMI IPI but this one), and a source added later registers a handler and
is then never in the residual.

**What answers and what does not.** On x86-64 a CPU answers whether its
interrupts are on or off, holding a spinlock or not, in a handler or
not. On AArch64 a CPU with interrupts masked does not answer within the
bound, and the printout for it is the tick sample with its age -- for a
spinner with interrupts off, the PC at its last tick before the mask,
the last useful fact available. The two outcomes print as what they
are:

```
cpu 1: pc 0xffffffff80112a60 sp 0xffffc00010407fc0 (nmi, 14 us ago)
  #0  0xffffffff80112a60
  #1  0xffffffff80112b9c
cpu 1: no answer in 5 ms; last tick 205 ms ago at pc 0xffffffff80009f20
```

The requester's own sample is labelled `self`.

**Who prints it.** `sched_dump` prints, per CPU, `last tick N ms ago pc
0x...` on its `cpu %u:` line and a live `run_ms` for the running thread
(`run_time_ns` is charged at switch-out, so a thread that has not
switched for eight seconds used to show the run time it had before),
then every subsystem hook registered with `sched_dump_register` (the
network workers' per-CPU counters). The self-test watchdog's
`[WATCHDOG]` block is that dump, every CPU's sample, and **eight more
samples, 250 µs apart, of every CPU that is running something**
(`lockup_profile`): one sample says where a CPU is, eight say what it
is cycling through -- which is how the worker's spin was read (its wait
condition, its dequeue, the wait's exit, never a packet). The two
detectors below print the samples with their reports; the hard-lockup
report profiles its target where an NMI can reach it.

**Soft lockup: a CPU that has not switched while something waits.**
`lockup_tick`, from `sched_tick` on every CPU for itself: if the current
thread is not idle, `rq->nr_running > 0`, and `rq->switches` is
unchanged since the last tick, `stall_ns` grows by a tick; otherwise it
resets. At `g_soft_ns` (10 s) the CPU prints once per episode `soft
lockup: cpu N running 'name' for M ms with K runnable`, its own trace
from the tick's frame, and every other CPU's sample. Under fixed
priorities a runnable thread that never runs for ten seconds while
another runs is a strictly lower priority behind a spinner or a
scheduler bug (an equal priority gets a slice every 10 ms); no thread in
this tree does ten seconds of work above another's priority, so the
report is a bug report. The episode ends when `switches` changes.

**Hard lockup: a CPU that stopped ticking.** On every one of its ticks
each CPU `k` checks its **watch target**, `lockup_watch_target(online,
k)`: the online CPU with the next-higher id, wrapping. With two or more
CPUs online every online CPU has exactly one watcher whatever holes the
mask has (`{0,2,3}`: 0 watches 2, 2 watches 3, 3 watches 0); a lone
CPU's target is itself and the check is skipped. If the target's
`ticks` differs from the value last seen, record it and reset; else
`watch_stall_ns` grows by a tick; a target that changed resets. At
`g_hard_ns` (10 s) the watcher prints once per episode `hard lockup:
cpu J no tick for M ms; last tick N ms ago at pc 0x... (seen from cpu
k)` and every CPU's sample -- on x86-64 the NMI reaches the stalled CPU
and the report carries its live frame; on AArch64 it carries the tick
sample. The cadence is the tick, so a threshold lowered by a test fires
at the threshold plus one tick; the cost is one load and one compare
per tick. Every online CPU ticks at `CONFIG_HZ` (`smp-ticks` asserts
it), so "no tick" is a stall, not idleness.

**The harness.** `soft lockup:` and `hard lockup:` are forbidden markers
in every boot test; the self-tests' own reports say `expected soft
lockup:` and do not match. With `--kernel <elf>` (the Makefile passes
`$(KERNEL_ELF)`), `run_boot_test.py` resolves every `#n 0x...` frame and
`pc 0x...` in the log with `llvm-symbolizer` and prints a
`---- symbols ----` table in its own report when the run failed or a
panic was expected (`make test-crash`); the log stays the run's
evidence. The kernel still prints addresses.

**Found by building.** At `-O1` clang gives an AArch64 leaf function no
frame record, so a walk from a PC inside one skipped its caller (the
sample of a spinning leaf showed the thread trampoline where the
spinner's caller should be); the AArch64 kernel is now built with
`-mno-omit-leaf-frame-pointer` (`build/arch/aarch64.mk`). x86-64 keeps
the leaf frame with `-fno-omit-frame-pointer` alone. And a CPU with
interrupts masked cannot acknowledge a TLB shootdown, whose waiter
panics after one second: a test that masks a CPU must stop it before
joining any thread whose stack teardown needs that acknowledgement, and
must keep every mask well under a second.

## What is missing and planned

- **Ring buffer**: `kvlog` will also append to a fixed buffer readable by
  a future `dmesg`; the console becomes one consumer.
- **Symbol resolution in the kernel**: an embedded sorted symbol table so
  traces print `function+offset` on the console; the harness resolves
  them with the ELF today (`--kernel`).
- **An NMI-class interrupt on AArch64** (GICv3 pseudo-NMI, a priority-mask
  discipline through every interrupt save and restore), so a spinner
  with interrupts masked answers a lockup sample live there too.
- **Timestamps**: once a monotonic clock exists.
- **Lock diagnostics**: might-sleep assertions, lock ordering checks.
- **GDB stub**: remote debugging over a second UART.
- **Crash dumps**: after storage exists.
