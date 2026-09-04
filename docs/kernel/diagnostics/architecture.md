# Diagnostics Subsystem

## Purpose

Everything that lets a human see what the kernel is doing and why it
stopped: console output, logging, formatting, panic reporting, stack
traces, orderly shutdown, boot self-tests, and the crash-test hook. The
constitution (section 55) asks for these "from day one"; they are the
first generic code in the tree because every other subsystem's failure is
reported through them.

Files:

| Area | Files |
|---|---|
| console | `kernel/core/console.c`, `kernel/include/kernel/console.h` |
| log | `kernel/core/log.c`, `kernel/include/kernel/log.h` |
| printf | `kernel/core/printf.c`, `kernel/include/kernel/printf.h` |
| panic, backtrace | `kernel/core/panic.c`, `kernel/include/kernel/panic.h` |
| shutdown | `kernel/core/shutdown.c`, `kernel/include/kernel/shutdown.h` |
| selftest | `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` |
| crash test | `CONFIG_CRASH_TEST` block in `kernel/core/main.c` |
| strings | `kernel/core/string.c`, `kernel/include/kernel/string.h` |

## Responsibilities

- Turn a format string and arguments into bytes on a device, in any
  context, without allocating.
- Classify messages by level and drop those below the configured
  threshold.
- On fatal error, print reason, CPU, context, registers, and stack, then
  request emulator exit with a failure code and halt.
- Terminate a successful run with a success code so the test harness can
  tell the two apart.
- Prove at boot that the subsystems present in this phase work.

## Non-responsibilities

- Device access: the serial driver is in the arch layer and registers
  itself as a sink.
- Persistence: there is no log ring buffer yet, so nothing is retrievable
  after the fact (a `dmesg` buffer is planned).
- Symbolisation: addresses are printed raw and resolved offline against
  `kernel.map`.
- Timestamps: there is no clock yet.

## Layering

```
kinfo/kwarn/kerror/kdebug/kprintf/panic/KASSERT   (callers)
        │
        ▼
log.c   ── kvsnprintf into a 256-byte stack buffer, prefix by level
        │
        ▼
console.c ── fan-out to every registered struct console_sink
        │
        ▼
serial.c (arch) ── 16550 polled write
```

`panic.c` sits beside `log.c`: it uses `kprintf` for output, calls
`arch_trap_frame_dump` and `arch_backtrace` for content, and
`arch_emulator_exit` + `arch_cpu_halt_forever` to stop.

## Interfaces

See `api.md`. In brief: `console_register/console_write/console_puts`;
`klog/kvlog/klog_set_level/klog_get_level/kprintf/kvprintf` and the
`kdebug/kinfo/kwarn/kerror` macros; `kvsnprintf/ksnprintf`;
`panic/panic_frame/backtrace_print` and `BUG/BUG_ON/KASSERT/WARN`;
`kernel_shutdown`; `selftest_run_all`; the `string.h` set.

## Data structures

- `struct console_sink { name, write, next }`: objects owned by their
  registrant (static for the UART, module memory for `virtio-console`),
  linked into a singly linked list owned by `console.c`; removed only by
  `console_unregister` at module unload.
- `g_level` (`enum klog_level`): one global threshold.
- `g_panicking` flag: recursion guard.
- Formatting state (`struct out`, `struct spec` in `printf.c`): stack only.

## Concurrency model

None of these functions take locks. On the single boot CPU that is
correct; the only re-entrancy is a trap handler logging while the
interrupted code was mid-line, which produces interleaved but
uncorrupted output because every writer formats into its own stack
buffer and hands the console a complete line. Under SMP the console will
gain a spinlock around `console_write` (with a panic-time bypass) and the
log level will become an atomic; callers do not change.

## Memory ownership

No allocation. `klog` uses `KLOG_LINE_MAX` (256) + ~64 bytes of stack;
`panic` uses a 32-entry `uintptr_t` array for the trace. Sinks are static.

## Error handling

Formatting cannot fail; over-long output is truncated (a line always ends
with `\n`, `\0`). A missing console device means silent output. Panic is
the error path for everything else and is re-entrancy safe.

## Performance considerations

Byte-at-a-time serial output at 115200 baud is the bottleneck for boot
time in verbose builds; the debug memory-map dump is the largest single
cost. Nothing is worth optimising until a log buffer decouples producers
from the UART.

## Security considerations

- Format strings are compile-time checked (`__printf` attributes with
  `-Wformat=2 -Werror`), so a non-literal format is a build error.
- Diagnostic output currently reveals kernel addresses; that is intended
  during bring-up. When user mode exists, address disclosure to
  unprivileged readers of a future log buffer must be policy-controlled
  (KASLR and `%pK`-style masking are future work).
- The crash-test hook is compiled out unless `CRASH_TEST=1`; it cannot be
  triggered at runtime.

## Testing strategy

Five self-tests (`printf`, `string`, `bootinfo`, `irq-state`,
`breakpoint-trap`) run at boot in debug builds; `make test-crash` verifies
the panic path. See `testing.md`.

## Future extensibility

Planned, in likely order: log ring buffer with `dmesg`; timestamps once a
clock exists; console spinlock for SMP; symbol table in the image for
symbolised traces; lock diagnostics ("might sleep", lock ordering);
`WARN_ONCE`; a GDB remote stub over a second serial port; crash dumps;
tracing. All of these attach below `klog` or beside `panic` without
changing the call sites.
