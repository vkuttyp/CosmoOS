# The console held interrupts off for a whole write

Date: 2026-10-21. Base: `main` at `2acce0c3` (#429's merge). Two items:
the unexplained panic in PR #429's CI, and backlog PKG-9.

## Scope and baseline

PR #429's run 39568825512, attempt 1, job "entropy boots (x86_64)",
boot `boot-test-devrng`, panicked during the harness's `dmesg` (pid 547)
after every self-test had passed:

```text
mmu: TLB shootdown of 0xffffc0001050a000+0x2000: 2 of 3 CPUs acknowledged within 1 s
[ WARN] timer: cpu 2: no tick for 1223 ms; interrupts came back at pc 0xffffffff80130720 (last tick interrupted pc 0xffffffff80130736, thread 'dmesg')
cpu 2: did not acknowledge; running thread 4833 'dmesg', irq_depth 1, preempt_count 1, last tick 1224 ms ago at pc 0xffffffff80130736
cpu 2: pc 0x0000000000401730 sp 0x00007ffcfd2380c8 (nmi, 32 us ago)
KERNEL PANIC: mmu: TLB shootdown of 0xffffc0001050a000+0x2000 acknowledged by 2 of 3 CPUs; not by cpu 2
```

Symbolised against the run's uploaded kernel ELF: `0xffffffff80130736`
is `arch_irq_enable` (`kernel/arch/x86_64/cpu.c:248`), `0xffffffff80130720`
`arch_irq_restore_hw` (`cpu.c:241`). The flakes entry recorded that day
(d34a434b) named `klog_copy` and a host stall, without a mechanism.

## Attribution

Subsystem: the console. Faulting path: `dmesg` -> `write` -> `tty_write`
-> `console_write` -> the sinks (fbcon), against the one-second
acknowledgement wait of `mmu.c`'s shootdown on CPU 0.

- BusyBox's `dmesg` writes what it read in one `full_write`
  (`util-linux/dmesg.c:115`), 32 KiB here.
- The syscall's bounce carries up to `IO_BOUNCE_MAX` (64 KiB,
  `kernel/include/kernel/syscall.h`) in one object call, so `tty_write`
  (`kernel/tty/tty.c`) gets the whole write, and the console tty, which
  has no `ops->write`, passes it whole to `console_write`.
- `console_write` (`kernel/core/console.c`) took `g_console_lock` with
  `spin_lock_irqsave` once and passed the whole write to every sink.
  fbcon's drawing and scrolling are milliseconds a line under TCG, so
  interrupts stayed off on CPU 2 for as long as 32 KiB took to draw. The
  `spin_unlock_irqrestore` that ended the hold is the
  `arch_irq_restore_hw` where the tick came back, and the NMI sample 32 us
  before the report is `dmesg` back in user code after the write.

Both holds end in `spin_unlock_irqrestore`, so the PC alone does not
tell `klog_copy` from the console; their lengths do. `klog_copy` is a
32 KiB memory copy. The console's write is measured by `console-lines`:
1152 bytes in 28 ms on x86-64 debug on the developer's machine, so 32 KiB
is on the order of 0.8 s there, and 1.2 s on a slower CI runner fits.
The shootdown's one-second deadline is right: it caught a
defect, and it is unchanged.

## Change

- `kernel/core/console.c`: `console_write` passes a write to the sinks a
  line at a time, at most `CONSOLE_HOLD_MAX` (256,
  `kernel/include/kernel/console.h`) bytes a hold, each hold its own
  `spin_lock_irqsave`. Lines from different CPUs still do not interleave
  (a hold ends at a newline); a line longer than 256 bytes can now be cut
  by another CPU's line. Panic mode is unchanged. Under
  `CONFIG_SELFTEST`, `g_hold_max` counts the most bytes one hold passed,
  and `console_test_hold_max` reads and resets it.
- `kernel/core/fbtest.c`: the self-test `console-lines` writes 24 lines
  (1152 bytes) in one `console_write` and checks that no hold carried more
  than `CONSOLE_HOLD_MAX`, and reports the time and the longest tick gap.
- `pkg/pkg.c` (PKG-9): a version change to an older version says `pkg:
  NAME will be downgraded from A to B`; `tests/boot/pkgtx_test.py`'s
  rollback stage checks it (`pkg upgrade hello`, then `pkg install
  hello=1.0`).
- `docs/testing/flakes.md` (the entry attributed), the deferred-work
  inventory's `klog_copy` line marked misattributed,
  `docs/kernel/diagnostics/testing.md`, `docs/pkg/api.md`,
  `docs/pkg/design.md`, `docs/backlog.md`, history.

## Proof

`tools/console-lines-probe.py`: `--old` reverse-applies the fix commit's
`console.c` (the whole commit's change to it) in a throwaway worktree,
keeping the self-test and its count; a debug `make test`.

| arch | `--old` | fixed |
|---|---|---|
| x86_64 | PASS: console-lines FAIL, the whole 1152-byte write in one hold (30.8 ms) | PASS: largest hold 48 bytes (one line), 28.1 ms |
| aarch64 | PASS: console-lines FAIL, the whole write in one hold (26.2 ms) | PASS: largest hold 48 bytes, 28.4 ms |

The longest tick gap during the write, which the self-test reports but
does not bound (a time): 29.5 ms (x86_64) and 26.6 ms (AArch64) with one
hold, 13.7 and 14.3 ms a line at a time. The probes ran on `8fce3c00`;
the release-build stub added after (below) changes no debug-build code.

## Validation

Matrix on `8fce3c00` (`gmake`, macOS host):

| step | x86_64 | aarch64 |
|---|---|---|
| host-test | PASS 84 s | PASS 83 s |
| fuzz (host, `FUZZ_RUNS=50000`) | PASS 60 s | (host; once) |
| analyze | PASS 234 s | PASS 228 s |
| debug test, `QEMU_SMP=1` | PASS 258 s | FAIL 279 s, then PASS 166 s (below) |
| debug test, `QEMU_SMP=2` | PASS 165 s | PASS 179 s |
| debug test, `QEMU_SMP=4` | PASS 164 s | PASS 181 s |
| test-smp2 | PASS 163 s | PASS 165 s |
| test-chaos | PASS 198 s | PASS 203 s |
| test-harness-retry | PASS 190 s | PASS 211 s |
| release test | FAIL (compile), then PASS 50 s | FAIL (compile), then PASS 55 s |
| release test-pkgtx | FAIL (compile), then PASS 109 s | FAIL (compile), then PASS 146 s |

- The release build did not compile: `console_test_hold_max` was
  declared only under `CONFIG_SELFTEST`, and `fbtest.c` is built in
  every configuration. It is now declared always and returns 0 without
  self-tests, as `timer_test_tick_gap_max_ns` does; the release steps and
  `test-pkgtx` (whose rollback stage says `downgraded`) pass on the head.
- AArch64 `QEMU_SMP=1`: `sched-restore-loop` failed `threads_settle(before)`,
  9 threads where 10 were counted before the test: its second sighting
  (`docs/testing/flakes.md`; backlog TEST-18), not on this change's
  path. The step rerun on the head passed.

Branch CI on `1332bffd` (run for PR #431): every job green.

## Review

- **The console's contract** (Codex, Qodo): `docs/kernel/diagnostics/api.md`
  still said one call's bytes are not interleaved with another's. It now
  states the line-at-a-time guarantee (I-DIAG-3 already promised only
  lines).
- **A stalled virtio console** (Codex): `vcon_write` spins up to 200 ms
  for a device that does not return its buffer, then drops the rest of
  what it was given. Passed a write a line at a time, it would have spun
  once a line -- `dmesg`'s 32 KiB, hundreds of lines -- where it spun once
  a write. A sink now sets `dropped` when it gives up, and
  `console_write` passes it none of the rest of that call (`write_call`,
  `skip_call`); `vcon_write` sets it at both drops. The self-test
  `console-sink-drop` registers a sink that drops what it is given: in a
  write of 8 lines it is given the first only, and the next write again.
  Folded into the fix commit, so `--old` still reverts one commit.
- Answered, no change: the probe finds the fix by its subject, which
  this repository's merge commits keep; the seam is its own commit, and
  `--old` boots and fails at the check (above); PKG-9 is in this PR at
  the owner's direction.

On the head after the review: the probes again (fixed: largest hold 48
bytes, x86_64 28.1 ms, AArch64 26.8 ms; `--old`: console-lines FAIL with
the whole write in one hold, on both), `console-sink-drop` ok in all four
boots, release `make test` PASS on both.
`console-sink-drop` is not vacuous: with `write_call`'s skip removed
(x86_64 debug `make test`), the dropping sink was given 8 of the 8 lines
and the test failed at `g_csd_given == 1`.
