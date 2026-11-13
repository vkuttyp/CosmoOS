# R0, second PR: the quiet screen, 2026-11-13

The owner asked, on seeing v1.1.0-rc1's plan, that a machine with a display not scroll the kernel's log past at
boot, as a Linux distribution does not, and that this be in v1.1.0. This PR makes the framebuffer console show the
kernel's errors and what programs print, and leaves every line on the serial port and in `dmesg`.

## Scope and baseline

Branch `quiet-boot` from main at `bf595f3a` (#517, v1.1.0's notes). Roadmap 1.3's R0, its second PR. In scope: which
of the kernel's log lines the framebuffer console is given. Not in scope, and recorded in the backlog: the loader's
own lines (BOOT-10) and one line per service from init (USR-9).

## What changed

- **A sink has a log floor** (`console_sink.log_floor`). `kvlog` passes each line with its level
  (`console_write_log`), and a sink whose floor is above the level is not given it. `console_write` and
  `console_puts` pass at a level above every floor: a program's output, `kprintf`, the banner and `dmesg`'s copy of
  the ring reach every sink. In panic mode every sink is given everything.
- **The framebuffer console's floor is `KLOG_ERROR`**, or none with `verbose` on the kernel command line
  (`make CMDLINE='root=boot-disk verbose' image`). The two replays of the ring onto a new screen drop the lines
  whose tag names a level below the floor (`fbcon_drop_hidden`).
- **`kernel/core/fbrules.cee`** (CeeSafe, new): the floor, the filter and the self-test `console-log-floor`, with
  two pure functions moved out of `fbcon.c`, the colour packing (`fbcon_pack`) and the font's scale (`fbcon_scale`).
- **`fb-switch`** writes a `!` after the switch on a quiet screen, where it used to read `fbcon_attach`'s own log
  line, to show the cursor moved with the screen; on a `verbose` screen it reads the log line as before.
- **Documents**: `docs/kernel/diagnostics/` (design "The quiet screen", API, I-DIAG-21, testing),
  `docs/boot/design.md` (the `verbose` key), `docs/guide/shell.md`, `docs/releases/v1.1.0.md`, backlog BOOT-10 and
  USR-9, the history entry, and a third sighting of the host test `online-late` in `docs/testing/flakes.md`.

## Found before the PR

| Finding | Fix |
|---|---|
| The first version was C, and CI's `cee-share` gate refused it: `kernel/: 91575 -> 91696 lines of C to convert (+121)` | the rules are CeeSafe (`fbrules.cee`), `pack` and `fbcon_scale` moved with them, and the kernel's C does not grow: the gate prints "no tree has more C to convert than origin/main" |
| The first version's `fb-switch` no longer checked that the cursor was at column 15 after the switch (the log line it read is hidden on a quiet screen) | the test writes its own `!` there and checks it; the `verbose` case keeps the old check |
| The first version put `sink_drop`'s comment on the new prototype in `selftest.h` | restored |
| `fbcon.c`'s `channel` shifted by whatever width and shift it was given | `fbcon_pack` gives 0 for a channel wider than 32 bits or shifted past bit 31, which the validator refuses anyway |

## Found in review

| Finding (#518) | Fix |
|---|---|
| Greptile: a release kernel with self-tests (`BUILD=release SELFTEST=1`) logs from INFO, so the test's two DEBUG lines are never written and `console-log-floor` fails | the test sets the log's level to DEBUG for its writes and restores it. By reading only: that pair does not compile, on main either (`kernel/timer/timer.c:721`, backlog BUILD-9) |
| Greptile: when the ring has wrapped, `klog_copy(buf, KLOG_RING_SIZE)` can begin in the middle of a line; the fragment has no tag and the filter keeps it | both replays ask for `KLOG_RING_SIZE - 1` bytes, for which `klog_copy` begins at a whole line |
| Greptile: `fbcon.h` uses `size_t` and does not include `<stddef.h>` | included |
| Codex: `log_floor` is in what was padding; a module built against the old header that did not zero its sink would have an arbitrary floor, and the loader accepts it as ABI 11 | `COSMO_MODULE_ABI_VERSION` is 12, and the boot harness's `hello` marker with it |

## Validation

Run on 2026-11-13 on the tree of `49e522d4` (the compiled files are those of `e86bbeca`). Both halves ran on the
other machine (`docs/development.md`, "Running the matrix": Xeon E3-1245 v2, the CI image, one container of 4 CPUs
and 6 GiB), one after the other: the developer's Mac had a one-minute load average of 94 to 135 all day from another
project's compiler suite, and its AArch64 half stopped at `make host-test` on `online-late`
(`tests/host/test_quiesce.c:425-426`), the host-scheduling failure `docs/testing/flakes.md` records, before any
boot. The same step passed on the other machine. Both runs ended `matrix: PASS`.

| Step | AArch64 | x86-64 |
|---|---|---|
| debug, release, chaos and harness-retry builds, `make cee-check` | PASS | PASS |
| `make host-test`, `make fuzz` | PASS | PASS |
| `make analyze` | clean (15 baselined, 0 unexpected, 0 stale) | clean (15 baselined, 0 unexpected, 0 stale) |
| debug `make test`, `QEMU_SMP=1` | PASS in 275.4 s | PASS in 227.2 s |
| debug `make test`, `QEMU_SMP=2` | PASS in 283.4 s | PASS in 238.9 s |
| debug `make test`, `QEMU_SMP=4` | PASS in 298.6 s | PASS in 240.9 s |
| `make test-smp2` | PASS in 288.3 s | PASS in 236.1 s |
| `make test-chaos` | PASS in 293.6 s | PASS in 241.8 s |
| `make test-harness-retry` | PASS in 296.0 s | PASS in 241.2 s |
| `make BUILD=release test` | PASS in 22.2 s | PASS in 19.1 s |

Also:

- `make BUILD=release test-display`, as CI runs it, on the other machine: x86-64 PASS in 36.9 s (the virtio-gpu
  boot, the resize, the boot with both screens, and the two panics read off the screen), AArch64 PASS in 22.0 s.
  The 1920x1080 screen reads back with 20 non-blank rows of 67, the marker at row 20.
- `make ARCH=aarch64 BUILD=release test-platforms`, as CI runs it, on the other machine: PASS, every machine,
  `noserial` (console framebuffer only) among them.
- The framebuffer self-tests in the matrix's debug boots, both architectures: `fb-console`, `console-log-floor`,
  `fb-bench` and `fb-switch` ok.
- A debug `make test-display` was run first, by mistake for the release one, and failed on both architectures in
  its second boot: `cosmofs-format`, `cosmofs-ops`, `cosmofs-crash`, `cosmofs-fixture-cleanup` and `process-user`,
  5 of 526; the screen read-backs and the four framebuffer self-tests passed in the same boots. The control, the
  same target on main at `bf595f3a` on the same machine, fails the same five self-tests (5 of 525): it is not this
  PR's, and is backlog TEST-27. A debug `test-platforms` there gave "no userland within 240s" on every machine: a
  debug boot's self-tests take longer than that on that machine.
- After the review's fixes (the table above), on the other machine: the debug and release builds and
  `make host-test` on both architectures, and a debug `make test` on each: x86-64 PASS in 238.3 s, AArch64 PASS in
  300.6 s (526 self-tests each, `console-log-floor` and `fb-switch` ok). The first of these boots failed the
  harness's `hello` marker, which named ABI v11; the marker is v12 now.
- An earlier matrix of the first version (`afddff3a`, the same behaviour in C) passed on both architectures, the
  AArch64 half on the Mac: its debug boots took 190 s to 225 s.
- `python3 scripts/cee-share.py --gate origin/main`: no tree has more C to convert than origin/main.
- `python3 scripts/check-release-version.py --tag v1.1.0 --notes docs/releases/v1.1.0.md` and
  `python3 scripts/check-docs.py`: pass.
- Debug and release builds with no warning on both architectures on the Mac (Apple clang, ceesafec 0.37.0).

## Limits

- The AArch64 boots on the other machine ran 275 s to 299 s against the 300 s budget: that machine emulates
  AArch64 on a 2012 Xeon. The 4-CPU boot passed with 1.4 s to spare. No budget was changed; a timing failure there
  is rerun on the Mac before it is believed.
- No boot with `verbose` is in the matrix: the self-tests run with the quiet floor, and `fb-switch`'s `verbose`
  branch is the check as it was before this PR.
- `fbcon_drop_hidden` has no host test or fuzz target; `console-log-floor` gives it one text.
- The screen hides WARN lines with the INFO ones. They are in `dmesg`.
- The loader's lines stay on the screen until the kernel clears it (BOOT-10), and init's and the services' lines
  are shown as they print them (USR-9).
- The quiet screen on the owner's Parallels VM is checked on v1.1.0-rc2, after this PR merges.
