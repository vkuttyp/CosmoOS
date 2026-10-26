# N1: load and unload in cycles, a debug-build hook, 2026-10-26

The owner chose N1 on 2026-10-25, at the request of the Nova language
project ("Start what Nova asks of CosmoOS"). Nova's C8 builds a kernel
module whose state, `init` and `shutdown` come from Nova. Its acceptance
(Nova's `docs/cosmoos/c8-report.md`, C8b) needs that module loaded and
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

## Validation

The matrix on `4331948f` (AGENTS.md), 2026-10-26, 07:08 to 07:56 local.
Every step passed:

| step | x86_64 s | aarch64 s |
| --- | --- | --- |
| host-test / fuzz | 92 / 36 | |
| analyze | 221 | 218 |
| debug test QEMU_SMP=1 / 2 / 4 | 169 / 170 / 181 | 172 / 177 / 174 |
| test-smp2 | 180 | 186 |
| test-chaos | 216 | 209 |
| test-harness-retry | 205 | 209 |
| release test | 28 | 30 |

Branch CI is checked on the PR.

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
  concurrency).
- N1's box ticked in `docs/roadmap-1.2.md`.
