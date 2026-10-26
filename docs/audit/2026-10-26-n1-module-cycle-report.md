# N1: load and unload in cycles, a debug-build hook, 2026-10-26

The owner chose N1 on 2026-10-25, at the request of the Nova language
project ("Start what Nova asks of CosmoOS"). Nova's C8 builds a kernel
module whose state, `init` and `shutdown` come from Nova. Its acceptance
(`docs/cosmoos/c8-report.md` in Nova's repository, not this one; C8b) needs that module loaded and
unloaded at least 1,000 times per boot in a debug build, on both
architectures, with no zombie, no leak and no lockdep report, and a
control in which an object outlives `shutdown` makes a zombie that is
then swept. The owner's instruction: merge after v1.0.14 is tagged.

## Scope and baseline

The branch started on main at `57701d4b` and was rebased onto
`49abac88` (#455, MEM-9) before validation. Main's CI run 40182371765
was green on every job.

No door existed. `module_load` and `module_unload` are not exported, no
system call reaches them, and `module_load_boot` loads each boot module
once. A module's `shutdown` ran only in the self-tests, on this tree's
own fixtures. N1 is a new feature, proven by its acceptance test, so it
has no probe (AGENTS.md).

## What changed

- **Docs first (`18ac64b8`).** The module design's "Load and unload in
  cycles (debug builds)", invariant **M27**, the api and testing pages,
  N1 in `docs/roadmap-1.2.md` under "Requested by Nova". Two backlog
  lines from Nova's report, MOD-1 and LOCK-10, which N1 does not fix.
- **`module_cycle` (`b00fc378`, `kernel/module/modcycle.c`,
  `CONFIG_DEBUG` only).** It loads one module image and unloads it
  `count` times through the real loader (`module_load`,
  `module_unload`), optionally holding each load `hold_ms` first. After
  the first cycle and after the last it compares the module count,
  kmalloc's live objects, the VM's regions and its anonymous pages. The
  baseline follows the first cycle, since what a first load makes once
  and every later load reuses is not a leak. Lockdep's class count is
  reported, not judged (LOCK-10).
  - `module.cycle=<archive entry>:<count>[:<hold ms>]` on the kernel
    command line runs it once at boot, right after `module_load_boot`
    and before the self-tests and `init`, and logs one `modcycle:` line.
    A failed or unparsable request counts into the boot's verdict, as a
    boot module that fails to load does. This is the door for Nova's
    module: the kernel cycles it, and the loader stays unexported.
  - The declarations are the kernel's own: `kernel/include/kernel/module.h`
    keeps them under `#ifndef COSMO_MODULE_BUILD`, so a module sees
    none of them.
  - Release builds have only an empty `module_cycle_boot`: `llvm-nm` of
    both release kernels shows `module_cycle_boot` and no other
    `module_cycle` symbol; both debug kernels have `module_cycle`,
    `module_cycle_boot`, `module_cycle_parse` and `selftest_module_cycle`.
- **The self-test `module-cycle`** (`kernel/module/modtest.c`), in every
  debug boot: 200 cycles of `cosmotest`, every count unchanged, no
  module left. Two controls must fail the run:
  - an allocation leaked while the module is live, in two cycles after
    the baseline: the run fails with kmalloc's live objects up by
    exactly 2;
  - an object kept past `shutdown`: the unload returns `-EBUSY`, one
    zombie, the run fails; once the object is put, the next load sweeps
    the zombie (M24).
  - It also checks the command line's parser on two good and ten bad
    specs.
- **The acceptance (`4331948f`).** `make test-modcycle`, a debug `make
  test` with `module.cycle=tests/cosmotest.ko:1000`. In CI on both
  architectures. `tests/boot/run_boot_test.py`, given `module.cycle=` in
  the boot's command line, requires the `modcycle:` line to say PASS for
  the entry and the count asked for.

## The acceptance test

`make test-modcycle` at `4331948f`, 2026-10-26:

    x86_64:  [ INFO] modcycle: tests/cosmotest.ko: 1000 cycles (hold 0 ms): PASS (load failures 0, unload
             failures 0, first error 0; modules 14 -> 14; kmalloc live 5641 -> 5641; vm regions 97 -> 97,
             anon pages 161 -> 161; lockdep classes 137 -> 143)
             SELFTEST: module-cycle     ... ok (2746 ms)
             boot-test: PASS in 185.4s
    aarch64: [ INFO] modcycle: tests/cosmotest.ko: 1000 cycles (hold 0 ms): PASS (load failures 0, unload
             failures 0, first error 0; modules 14 -> 14; kmalloc live 5901 -> 5901; vm regions 88 -> 88,
             anon pages 136 -> 136; lockdep classes 134 -> 141)
             SELFTEST: module-cycle     ... ok (2815 ms)
             boot-test: PASS in 188.2s

Neither boot logged a lockdep report other than the lockdep self-tests'
`expected report` lines. The zombie lines in both logs are the
self-tests' controls (`module-cycle`'s and the older module tests'),
each followed by `freed zombie`.

Lockdep's classes grew by 6 and 7 over 1,000 cycles: LOCK-10's growth,
as Nova's C8 measured (136 -> 143), reported and not judged.

The first build of the branch failed: `module.h:207: declaration of
'struct module' will not be visible outside of this function`, in every
module's compile, because the new declarations were outside the
`COSMO_MODULE_BUILD` guard. The fix went into `b00fc378` before any
boot.

## Review fixes (`9784daf8`)

Codex found two defects in `module_cycle`, and both were real:

- **A leak past `KMALLOC_MAX_SLAB` passed.** `kmalloc` sends anything
  over 8 KiB to whole pages, counted in `kmalloc_stats.large_pages`, not
  in `live_objects`, so a module leaking a 16 KiB buffer every cycle left
  every compared count unchanged. The counts now include `large_pages`,
  and the self-test has a control 1b: two 16 KiB allocations leaked
  while the module is live must fail the run with large pages up by
  exactly 8. With `large_pages` left out of the comparison again (a throwaway
  worktree at `9784daf8`, x86-64 debug `make test`), the self-test fails
  on that control: `SELFTEST: module-cycle ... FAIL: check failed: !ok at
  line 853`.
- **The counts are machine-wide, and other threads were running.** The
  network's and irqpoll's workers start before the hook did, and
  `dhcp_boot_wait` came after it, so the design's "the counts hold
  still" was false. Under QEMU both leases were taken before the first
  cycle (serial lines 653-690 of the x86-64 boot above), which is why no
  run failed, but a slower DHCP server would have overlapped the cycles
  and failed a module that leaks nothing. The hook now runs after
  `dhcp_boot_wait`. The baseline is two equal readings 10 ms apart, and
  the last reading is taken again every 10 ms until it equals the
  baseline, for `MODULE_CYCLE_SETTLE_MS` (500 ms) at most: a buffer held
  for a moment is gone by then, and a leak never is. That narrows the
  window without isolating the run, and the design says so; counting
  allocations by the module that made them is backlog MOD-2.

CosmoReview's one finding: this report cited Nova's `c8-report.md` as
if it were in this repository. It now says where it is.

`make test-modcycle` at `9784daf8`:

    x86_64:  [ INFO] modcycle: tests/cosmotest.ko: 1000 cycles (hold 0 ms): PASS (load failures 0, unload
             failures 0, first error 0; modules 14 -> 14; kmalloc live 5642 -> 5642, large pages 656 -> 656;
             vm regions 97 -> 97, anon pages 161 -> 161; ...)
             SELFTEST: module-cycle     ... ok (4756 ms)
             boot-test: PASS in 185.7s
    aarch64: [ INFO] modcycle: ... 1000 cycles (hold 0 ms): PASS (...; kmalloc live 5902 -> 5902, large pages
             692 -> 692; vm regions 88 -> 88, anon pages 136 -> 136; ...)
             SELFTEST: module-cycle     ... ok (4975 ms)
             boot-test: PASS in 205.0s

The `modcycle:` line now follows `dhcp: 2 interface(s) bound` (serial
line 729 against 5730 on x86-64, 762 against 5763 on AArch64). The
self-test takes about 2 s longer, the controls' 500 ms waits for counts
that never return.

## The modcycle boot's budget, and the quiet readings' clock

CI's next run (40288077004, `9fdd42d2`) timed out the x86-64 modcycle
boot at 240 s (244.0 s) on a slow runner whose plain debug boots took
221.4-226.2 s and passed; the cycles add 6-12 s at middling speeds and at
least 18 s there. `make test-modcycle` now has its own budget,
`MODCYCLE_TIMEOUT`, 300 s on both architectures, with that data in the
Makefile, and the `modcycle:` line gives the cycles' own time: `PASS in
13103 ms` (x86-64) and `13449 ms` (AArch64) locally.

A local x86-64 modcycle boot then failed the `module-cycle` self-test's
8 s budget (9095 ms). The quiet readings were bounded by a count of
10 ms sleeps, and a sleep under TCG can last longer, so each control,
whose counts never come back, could wait past its 500 ms. They are now
bounded by the clock and report what they waited. Four boots after it:
the self-test 4333-4887 ms, its main run waiting 10-13 ms for quiet.

## Validation

The matrix on `4331948f` (AGENTS.md), 2026-10-26, 07:08 to 07:56 local,
before the review fixes. Every step passed:

| step | x86_64 s | aarch64 s |
| --- | --- | --- |
| host-test / fuzz | 92 / 36 | |
| analyze | 221 | 218 |
| debug test QEMU_SMP=1 / 2 / 4 | 169 / 170 / 181 | 172 / 177 / 174 |
| test-smp2 | 180 | 186 |
| test-chaos | 216 | 209 |
| test-harness-retry | 205 | 209 |
| release test | 28 | 30 |

The matrix again on `9784daf8`, after the review fixes, 2026-10-26,
09:09 to 09:56 local. Every step passed:

| step | x86_64 s | aarch64 s |
| --- | --- | --- |
| host-test / fuzz | 73 / 17 | |
| analyze | 232 | 229 |
| debug test QEMU_SMP=1 / 2 / 4 | 174 / 186 / 173 | 173 / 185 / 190 |
| test-smp2 | 175 | 184 |
| test-chaos | 184 | 205 |
| test-harness-retry | 192 | 190 |
| release test | 19 | 23 |

Branch CI is checked on the PR. On `25ce894e` one job failed,
`upgrade test (x86_64)`: `test-be`'s `be-pending-done` boot stopped
after the loader's last line; on `9784daf8`, `install test (x86_64)`:
`test-snapshot`'s `crash-switch-after` boot, the same way. The third and
fourth sightings of "The kernel stopped in the firmware's RNG call"
(flakes.md), before the kernel runs. The second was re-run and passed (attempt 2); every other job passed.

## For Nova

Nova's C8b runs against this hook: the Nova module goes into the debug
boot archive and `module.cycle=<its entry>:1000` goes on the command
line. A FAIL fails the boot's verdict in the kernel itself, as do an
entry not in the boot archive and a request that cannot be parsed
(count 1 to 1,000,000, hold at most 60,000 ms); CosmoOS's own harness
also requires the PASS line. MOD-1 means an
object made during the module's own `init` is not counted against it,
so a Nova module that hands one out can pass a cycle run while it
leaks; its controls should make their objects after `init` returns.

## Backlog

- MOD-1 and LOCK-10, added (Kernel modules; Lock validator and
  concurrency); MOD-2 (per-module accounting for `module_cycle`), from
  the review.
- LX-24 (BusyBox `yes` with a long line never exits after its pipe's
  reader goes), not N1's: found checking v1.0.14-rc1 on Parallels
  and present on v1.0.13 too; recorded in this PR, the one open.
- N1's box ticked in `docs/roadmap-1.2.md`.
