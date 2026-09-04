# Diagnostics Subsystem: API

All functions: internal kernel ABI, no allocation, no sleeping, no locks,
safe in interrupt and panic context unless noted. Format strings are
checked at compile time; a non-literal format is a build error.

---

## `kernel/console.h`

### `void console_register(struct console_sink *sink)` *(Module ABI v1)*
- **Purpose**: add an output device. The UART registers at boot; the
  `virtio_console` module registers a `virtio-console` sink when it
  loads.
- **Inputs**: `sink` with `name` and non-NULL `write`; `next` is owned by
  the console.
- **Ownership/lifetime**: the object must stay valid until
  `console_unregister` (module unload) or forever (static).
- **Concurrency**: the list is prepended without the console lock; safe
  at boot and from a module `init` while other CPUs are not logging.
- **Failure modes**: NULL `sink` or NULL `write` is ignored.

### `void console_unregister(struct console_sink *sink)` *(Module ABI v1)*
- **Purpose**: unlink a sink before its memory goes away.
- **Concurrency**: unlinks without taking the console spinlock, so it
  races a concurrent `console_write` on another CPU (documented gap,
  `docs/kernel/device/invariants.md` D12). Unknown sinks are ignored.

### `bool console_has_sink(const char *name)`
- **Purpose**: whether a sink of that name is registered (self-tests).
- **Concurrency**: lock-free walk; any context.

### `void console_write(const char *s, size_t len)` / `void console_puts(const char *s)`
- **Purpose**: emit bytes to every sink.
- **Concurrency**: no lock; concurrent writers may interleave lines.

## `kernel/log.h`

### `void klog(enum klog_level level, const char *fmt, ...)` / `void kvlog(level, fmt, va_list)`
- **Purpose**: one prefixed, newline-terminated log line.
- **Inputs**: `level` in `KLOG_DEBUG..KLOG_PANIC` (higher values clamp to
  PANIC); printf-style `fmt`.
- **Outputs**: nothing; dropped if `level < klog_get_level()`; truncated
  to `KLOG_LINE_MAX - 2` characters of text.
- **Memory**: ~320 bytes of stack.
- Macros: `kdebug(...)`, `kinfo(...)`, `kwarn(...)`, `kerror(...)`.

### `void klog_set_level(enum klog_level)` / `enum klog_level klog_get_level(void)`
- **Purpose**: threshold control. Default DEBUG in debug builds, INFO
  otherwise.
- **Concurrency**: plain global; becomes atomic under SMP.

### `void kprintf(const char *fmt, ...)` / `void kvprintf(const char *fmt, va_list)`
- **Purpose**: raw output, no prefix, no added newline. For banners,
  self-test lines, and dumps whose format is a test contract.
- **Outputs**: truncated at `KLOG_LINE_MAX - 1` characters per call.

## `kernel/printf.h`

### `int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)` / `int ksnprintf(char *buf, size_t size, const char *fmt, ...)`
- **Purpose**: C99 `vsnprintf` for integers and strings.
- **Inputs**: `buf` may be NULL only when `size == 0`.
- **Outputs**: length of the untruncated result; `buf` NUL-terminated
  when `size > 0`.
- **Supported**: `%c %s %d %i %u %x %X %o %p %%`; flags `- 0 + space #`;
  width/precision including `*`; lengths `hh h l ll z t j`. `%p` is
  always `0x` + 16 hex digits.
- **Failure modes**: unknown conversions are echoed verbatim; a trailing
  lone `%` is printed. Never returns negative.
- **ABI**: internal; the conversion set may grow, never shrink.

## `kernel/panic.h`

### `void panic(const char *fmt, ...) __noreturn`
- **Purpose**: unrecoverable error. Prints reason, CPU, stack trace;
  requests emulator failure exit; halts.
- **Concurrency**: disables local interrupts; re-entrant (second panic
  prints a short note and halts).

### `void panic_frame(const struct arch_trap_frame *frame, const char *fmt, ...) __noreturn`
- **Purpose**: panic originating in a trap; adds the register dump and
  starts the trace at the interrupted context.
- **Inputs**: `frame` valid for the call (it is on the interrupted stack).

### `void backtrace_print(const struct arch_trap_frame *from)`
- **Purpose**: print up to 32 return addresses. `from` NULL means the
  caller's own stack.

### Macros
- `BUG()` → `panic("BUG: at file:line (func)")`.
- `BUG_ON(cond)` → panic with the condition text if true.
- `KASSERT(cond)` → panic `assertion failed: cond at file:line (func)` if
  false. Active in every build.
- `WARN(cond, fmt, ...)` → logs `WARN at file:line: ...` at WARN level
  when `cond` is true; evaluates to `!!(cond)`. Never halts.

## `kernel/shutdown.h`

### `void kernel_shutdown(enum kernel_exit_status status) __noreturn`
- **Purpose**: finish a run. `KERNEL_EXIT_SUCCESS` → emulator code 0x10
  (QEMU exit 33); `KERNEL_EXIT_FAILURE` → 0x11 (35). Disables interrupts,
  requests exit, halts.
- **Blocking**: never returns.

## `kernel/selftest.h`

### `int selftest_run_all(void)`
- **Purpose**: run the boot self-tests, print the `SELFTEST:` lines.
- **Outputs**: number of failed tests.
- **Lifetime**: call once from `kernel_main` after `interrupt_init` and
  `arch_irq_enable`; the trap test needs a live IDT and dispatcher.
- **Side effects**: registers and unregisters a `#BP` handler; restores IF.
- **Compiled**: only when `CONFIG_SELFTEST=1`.

## `kernel/string.h`

`memcpy`, `memmove`, `memset`, `memcmp`, `strlen`, `strnlen`, `strcmp`,
`strncmp`, `strlcpy`. Standard C semantics; `strlcpy` returns
`strlen(src)` and always terminates when `size > 0`. Callers own bounds
checking. `memcpy` regions must not overlap; use `memmove` otherwise.

## `kernel/errno.h`

`EPERM 1, ENOENT 2, EIO 5, ENOMEM 12, EBUSY 16, EEXIST 17, EINVAL 22,
ENOSPC 28, ERANGE 34, ENOSYS 38`. Kernel APIs return `0` or `-E*`. The
numbering matches Unix so the user ABI can pass values through unchanged.
