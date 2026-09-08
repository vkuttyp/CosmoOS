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

### The framebuffer console

**`fb-console`** (both architectures, whenever the machine has a
display). Everything is checked by reading the pixels back through the
same mapping the console draws through and comparing them against the
font table, in both colours -- which is what makes a display testable
under `-display none` with no screenshot, and what makes a wrong pixel
format a failure here rather than something illegible that a log marker
would still match. In order: a line of text is drawn cell by cell where
the cursor said it would be; a backspace moves the cursor back and the
space the tty echoes after it clears the cell; a byte outside the font
draws the replacement box, and the box is not the space (a silently
blank cell would pass a test that only asked whether something
appeared); a line longer than the screen wraps to the next row without
losing a character; and filling the screen scrolls it by exactly
`scroll_rows`, with the marker line found again where it moved to --
which also says the repaint draws what the text shadow holds. Each check
retries up to three times, because the screen is shared with everything
else that prints and a self-test run is quiet but not silent.

**`fb-geometry`** is not a boot test: the validator is a pure function,
so it is a host test (`tests/host/test_fbvalid.c`, ASan/UBSan) and a
fuzz target (`tests/fuzz/fuzz_fbvalid.c`). The host test names the
refusals; the fuzz target asserts that whatever the validator accepts is
arithmetically safe -- every row inside the memory claimed, the last
pixel of the last row inside it, every colour field inside a pixel.

**Shapes.** `QEMU_DISPLAY=on` (the default) is QEMU's built-in VGA on
q35 at 1280x800 and a `ramfb` on virt at 800x600 -- the two devices
whose firmware here hands over a linear framebuffer. `QEMU_DISPLAY=0` is
a machine with no display: the loader says so, the kernel keeps the
serial console, and the `fb-` tests skip. `QEMU_DISPLAY=virtio` is a
`virtio-gpu-pci`, whose edk2 driver offers a Blt-only mode with no
linear buffer: the loader names the format and ignores it, which is the
refusal path. `QEMU_DISPLAY=bochs` is a framebuffer on x86 and, on
AArch64, no Graphics Output Protocol at all -- the firmware there
carries no driver for it.

### Benchmarks

`fb-bench` (reports only) times the real path -- `console_puts` under
the console's own IRQ-safe lock -- because that is what a log line costs
the machine. QEMU TCG, Apple Silicon host, indicative:

| | x86_64, 1280x800 (160x100 cells) | AArch64, 800x600 (100x75 cells) |
|---|---|---|
| A line of 79 characters | 286 us (3.6 us a glyph) | 357 us (4.5 us a glyph) |
| A scroll (a whole-screen repaint) | 10.6 ms | 9.9 ms |
| Amortised, at `rows / 8` a scroll | 1.17 ms a line | 1.46 ms a line |

What the numbers decided: **the scroll chunk**. A scroll costs a screen
repaint whatever distance it moves, so the cost per line is the line
plus a scroll divided by the chunk:

| Rows a scroll | 1 | 4 | 8 (`rows / 8` here: 12) | 25 | 50 |
|---|---|---|---|---|---|
| Cost a line (x86_64) | 10.9 ms | 2.9 ms | 1.17 ms | 0.71 ms | 0.50 ms |

One row at a time is nine times worse and made a boot measurably
slower -- 40.1 s of self-tests against 34.3 s without a display; at
`rows / 8` the same boot is 32.4 s, which is the no-display figure
within noise. Past eight rows the gain is small and the cost is blank
space at the bottom of the screen, so the chunk stays there (section
21: the complexity that is kept is the complexity that was measured).

The other decision was **the text shadow**: a scroll moves characters in
RAM and repaints from them, rather than moving pixels within the
framebuffer, because reading uncached memory is several times more
expensive than writing it and the shadow removes the reads entirely.
Write-combining would be the next thing to try and needs an arch change
(`vm_cache_t` has no WC), which nothing has yet paid for.

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
