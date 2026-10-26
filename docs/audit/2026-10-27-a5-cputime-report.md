# A5, first PR: CPU time split into user and system time, and the CPU-time timers, in Nova, 2026-10-27

Roadmap 1.2 A5 (OpenSSH and git) absorbs two kernel gaps both suites
touch: LX-13 (nice values) and LX-14 (`ITIMER_VIRTUAL` and `ITIMER_PROF`
are `-EINVAL`). On 2026-10-26 the owner asked for A5's kernel lines to be
written in Nova, the memory-safe language whose GPT parser replaced
`kernel/block/gpt.c` in #459, and chose a weighted share for nice values.
This PR is LX-14. LX-13's interface and the weighted share follow in two
more PRs.

On 2026-10-27 the owner also chose where Nova sources live: in this tree,
beside the C they generate, not in Nova's repository or a third one. The
PR therefore starts by moving `gpt.nova` in and adding `make nova`.

## Scope and baseline

Branch `cputime-itimers` from main at `68925d14` (#459). Main's own
validation is #459's matrix (Nova's `docs/cosmoos/c9-report.md`).

## Defects found on main

Both are shown failing on main by `tools/cputime-probe.py --old`, on both
architectures:

- **The CPU-time clocks read the monotonic clock.** `lx_clock_gettime`
  accepted every id from 0 to 7 and gave all but the realtime ones
  `clock_now_ns`. So `CLOCK_PROCESS_CPUTIME_ID` and
  `CLOCK_THREAD_CPUTIME_ID` were time since boot, and musl's `clock()`
  with them. Through a 200 ms sleep they advanced 203,583 µs (x86-64) and
  204,499 µs (AArch64).
- **A process lost an exited thread's CPU time.** `/proc/<pid>/stat` and
  the native `process_list` summed the threads still on `p->threads`, so
  a thread's time vanished when `thread_put` unlinked it. A thread that
  spun for 300 ms and exited added 0 ticks to `/proc/self/stat`.

## What changed

- **Nova sources in tree** (`4c5fafe2`). `kernel/block/gpt.nova` sits
  beside `gpt.c`. `scripts/nova-gen.py` (`make nova`, `make nova-check`)
  compiles each source with the pinned novac (0.1.1) and writes or
  compares the generated C and Nova's runtime. CI's "Nova sources" job
  installs the pinned release, checked against its SHA-256, and fails on
  a mismatch. Building still needs no Nova compiler (`docs/build/design.md`,
  "Nova sources").
- **CPU time** (`076a8e14`; `docs/kernel/process/design.md`, "CPU time").
  The tick samples the thread it interrupts as user or system time. A
  report splits the exact run time in the samples' proportion, never
  below an earlier report, as Linux's `cputime_adjust` does. `ITIMER_VIRTUAL`
  runs on user time and `ITIMER_PROF` on both. The arithmetic and the
  timers are `kernel/process/cputime.nova`. The process side is C
  (`cpu_account.c`): the tick's hook before `rq->lock`, under the
  process's lock; the fold of a leaving thread's time into its process
  (the second defect); a waited-for child's time reaching its parent.
- **Linux ABI.** `getrusage`, `times` and `wait4`'s `rusage` are new. The
  CPU-time clocks are fixed (the first defect), and `clock_nanosleep`
  refuses them. `setitimer`/`getitimer` take the two timers.
  `/proc/<pid>/stat` fills `utime`, `stime`, `cutime` and `cstime`.

## Validation (2026-10-27, macOS host, this branch; code as of `076a8e14`)

| Check | x86_64 | aarch64 |
|---|---|---|
| `tools/cputime-probe.py --old` | PASS: 29 checks fail, every expected one | PASS, the same |
| `tools/cputime-probe.py` (debug `make test` of the branch) | PASS: `lxtest: cputime ok`, `LINUXTEST: PASS` | PASS |
| `BUILD=release make test` | PASS (19.0 s) | PASS (21.7 s) |
| `make analyze` | clean: 7 baselined, 0 unexpected | clean, the same |
| `make host-test` (`test_cputime` included), `make fuzz` (`fuzz_cputime` included) | PASS, PASS | (host) |
| `make nova-check` with novac 0.1.1 | match | |

On 2026-10-26, before the move, the same code passed the whole matrix on
both architectures. Debug boots showed no lockdep report beyond the
lockdep self-test's expected ones.

Two failures on the way were in the tests, not the code:

- The first debug run read the thread's CPU clock after the process's,
  so the thread's could legitimately be larger. The test now reads the
  thread's first.
- The first fuzz run had `ITIMER_VIRTUAL` fire on a system tick. That is
  correct for a periodic timer shorter than a tick, which fires once per
  tick until it catches up, as Linux's does. The invariant now allows
  exactly that.

## For Nova

This is the first CosmoOS feature written in Nova rather than ported
from C, and the first held to tests written beforehand rather than to a
differential test against a C original. Nova's report is its
`docs/cosmoos/a5-report.md`.

## Backlog

LX-14 is done. LX-13 is next (A5's second PR).
