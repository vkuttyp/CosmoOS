# Diagnostics Subsystem: Testing

## Coverage today

### Boot self-tests (`make test`, `CONFIG_SELFTEST=1`)

`kernel/core/selftest.c` runs five tests; three belong to this subsystem:

- **printf**: 21 format assertions through `kvsnprintf` (via a
  `__printf`-attributed helper) covering `%s` including NULL, `%d` negative,
  `%u`, `%x`/`%X`, `#`, zero padding, width, left alignment, `+`,
  precision on integers including `%.0d` of 0, `%llu` max, `%lld` min,
  `%p` form, `%.3s`, `%3c`, `%%`, `%o`, `%zu`; then truncation into a
  4-byte buffer (return 7, content `"too"`) and a zero-size NULL buffer
  (return 5).
- **string**: `memset`, `memcpy`, `strlen`, `strnlen`, `memmove` forward and
  backward overlap, `strcmp`/`strncmp` sign, `strlcpy` truncation return.
- **bootinfo**: exercises the validated accessors the banner and memory
  dump rely on.

The remaining two (`irq-state`, `breakpoint-trap`) are documented under
`docs/kernel/arch/` and `docs/kernel/interrupt/`.

The harness (`tests/boot/run_boot_test.py`) requires `SELFTEST: PASS` when
any `SELFTEST:` line is present, forbids `SELFTEST: FAIL`, `KERNEL PANIC`,
`BUG:`, and `cosmoboot: FATAL`, requires `[ INFO] boot complete`, and
requires QEMU exit 33. This covers `klog` prefixing, `kprintf`, the
console sink, and `kernel_shutdown` success path end to end.

### Panic path (`make test-crash`)

Built with `CRASH_TEST=1`. Required markers (`--expect-panic`):

```
[ INFO] crash test: writing to an unmapped address
KERNEL PANIC: unhandled exception 14 (#PF page fault)
trap 14 ...
RIP=<16 hex> CS=
CR2=ffff900000000000 (not-present write kernel)
stack trace:
  #0 0xffffffff8xxxxxxx
halting.
```

Forbidden: `[ INFO] boot complete`, `crash test: write did not fault`,
`KERNEL PANIC (recursive)`. Required exit: 35. This proves
`panic_frame`, the output order, `arch_trap_frame_dump` integration,
`backtrace_print` from a trap frame, the failure exit code, and that the
crash hook cannot be mistaken for a clean boot.

### Static

`make analyze` (clang analyzer) over every file; `-Wformat=2` on every
call site.

## Not yet covered

- `panic()` without a frame (the trace starts at the caller): reached
  only through `KASSERT`/`BUG` today; a `CRASH_TEST` variant selecting
  `KASSERT(0)` is the easy addition.
- Recursive panic: needs a sink that panics on write, or a fault injected
  inside `arch_trap_frame_dump`.
- `WARN()` output format and its return value.
- `klog_set_level` filtering (only the build defaults are exercised).
- `kprintf` truncation at `KLOG_LINE_MAX`.
- `console_register` with multiple sinks and their ordering.
- `printf` edge cases not yet asserted: `%*d` negative width (left
  align), `%.*s`, `%hhd`/`%hd` sign extension, `%#o` of 0, `%-p`, `%jd`,
  `%td`.

## Host-side tests (planned)

`printf.c` and `string.c` are pure C with no kernel dependencies beyond
`kernel/compiler.h`; they are the first candidates for a host unit-test
harness (compile natively, compare against the host `snprintf` on
generated inputs) and for property/fuzz testing (random format strings
and arguments must never read out of bounds; the return value must equal
the host's for the supported subset). `log.c` and `console.c` can be
tested on the host with a capturing sink. `panic.c` stays a QEMU test.

## Running

```sh
make test                                   # self-tests + clean-boot markers
make test-crash                             # panic report + failure code
make BUILD=release test                     # no self-tests, markers only
make SELFTEST=0 test                        # debug build without self-tests
cat out/x86_64-debug/boot-test.log
cat out/x86_64-debug-crash/boot-test-crash.log
```
