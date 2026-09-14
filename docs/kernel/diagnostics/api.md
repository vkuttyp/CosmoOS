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

## `kernel/fbcon.h`

### `void fbcon_init(void)`

Map the framebuffer the loader reported, replay the newest screenful of
the log ring onto it, and register a console sink named `fbcon`. Called
once, from `kernel_main` after `vmm_init`. A machine with no framebuffer
(or one whose description the validator refuses, or one overlapping free
memory) keeps the serial console and gets a line saying why; none of
that is an error.

### `bool fbcon_present(void)`

Whether the sink is registered and drawing.

### `bool fbcon_geometry(struct fbcon_geometry *out)` / `void fbcon_cursor(uint32_t *col, uint32_t *row)` / `void fbcon_get_stats(struct fbcon_stats *out)`

The mapped base, pitch, pixel size, cell grid, the two packed colours and
the scroll chunk; where the cursor is; and how much has been drawn
(`bytes`, `glyphs`, `scrolls`, `repaints`). These exist for the
self-test, which reads pixels back and compares them against the font
table -- which is what makes a display testable with no screen and no
screenshot. Nothing else should need them.

## `kernel/bootinfo.h` (the framebuffer)

### `bool bootinfo_fb_validate(const struct cosmoboot_info *info, struct bootinfo_framebuffer *out, const char **why)`

True when the description is one the kernel may write to, with `out`
filled; false with a static reason in `why` otherwise, including the
ordinary case of no framebuffer at all. A pure function of the boot
fields, with no kernel dependencies, so it compiles unchanged into the
kernel, `tests/host/test_fbvalid.c` and `tests/fuzz/fuzz_fbvalid.c`. It
refuses: rows that do not fit the memory claimed, a pitch shorter than a
row, a pixel that is not 8, 16, 24 or 32 bits, a colour field that runs
off the end of a pixel, a base of zero, and a range that wraps the
address space.

### `const struct bootinfo_framebuffer *bootinfo_framebuffer(void)`

The validated description, or NULL. Set once by `bootinfo_init`.

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

## `kernel/lockup.h`, `kernel/lockup_core.h`

Design: `design.md`, "lockup.c". Kernel-internal; no user interface.

### `struct cpu_sample`
- `want` (the request pending for this CPU, written by the asker), `seq`
  (the request this answer belongs to; stored last, with release), `pc`,
  `sp`, `trace[LOCKUP_TRACE_MAX]`, `depth`, `nmi`, `when_ns`. One per
  CPU in `struct percpu`; written only by that CPU.

### `bool lockup_sample_all(const struct arch_trap_frame *self, uint64_t timeout_ns, cpumask_t *answered)`
- **Purpose**: every other online CPU records its frame; this CPU's from
  `self` (a handler) or its own walk (`NULL`, a thread).
- **Returns**: `false` at once, sending nothing, when another CPU's
  report is in progress; `true` with the mask of CPUs whose sample is
  current (the caller's included), holding the reporter slot until
  `lockup_print_samples`.
- **Concurrency**: any context, never sleeps, never waits for another
  reporter; one total wait bound for all targets. Called from the tick.

### `bool lockup_answer(struct arch_trap_frame *frame, bool nmi)`
- **Purpose**: handler side; records if a request is pending for this
  CPU. No locks, no printing; NMI-safe. Called by the `IPI_SAMPLE`
  handler and, on x86-64, by the paranoid path for `X86_TRAP_NMI`.

### `bool lockup_sample_cpu(unsigned cpu, uint64_t timeout_ns, struct cpu_sample *out)` / `void lockup_profile(unsigned cpu, unsigned n, uint64_t gap_ns)`
- One CPU's frame now (claims the slot, asks that CPU alone, copies its
  answer, releases); `n` such samples `gap_ns` apart printed one line
  each (the top frames) -- what a CPU that is cycling rather than stuck
  shows. Never the caller's own CPU; never sleeps (`udelay`). The
  watchdog profiles every busy CPU with eight samples 250 µs apart; the
  hard-lockup report profiles its target where an NMI can reach it.

### `void lockup_print_samples(cpumask_t answered)`
- Prints each CPU's sample or, for one that did not answer, its tick
  sample and age; releases the reporter slot. `int lockup_reporter(void)`
  names the holder (-1 when free).

### `void lockup_tick(struct arch_trap_frame *frame, uint64_t now_ns)`
- The detectors' per-tick step; called by `sched_tick`. Off until
  `lockup_init()` (after SMP bring-up in `kernel_main`).

### `unsigned lockup_watch_target(uint64_t online, unsigned k)` (`lockup_core.h`)
- Pure: the online CPU with the next-higher id, wrapping; `k` when alone.
  Host-tested (`tests/host/test_lockup.c`).

### `void lockup_get_stats(struct lockup_stats *out)` / `void lockup_set_thresholds(uint64_t soft_ns, uint64_t hard_ns, bool expected)`
- Diagnostics and the test hook: report counters and the last reports'
  facts; thresholds (0 restores 10 s) and whether the next report is
  expected (its line then says so, and the harness's forbidden marker
  does not match it).

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
