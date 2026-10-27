# Roadmap 1.2's first unit, first PR: four test and CI defects, 2026-10-27

The roadmap 1.2 document asks every direction to take its "Always,
whichever direction" lines first, as one unit of at most five PRs. This is
that unit's first PR: TEST-1, TEST-2, TEST-3 and TEST-5, four defects in
tests and CI, three of which reached CI as flakes. CFS-1 and CFS-3 were
already done by B1 and B2, and BUILD-2 was done by #463 (the release key in
the `release` environment).

## Scope and baseline

Branch `always-tests` from main at `6df2fce8` (#463). Each self-test
defect gets two commits: a control that makes the failure certain, on
main's code, then the fix. `tools/always-tests-probe.py --old` reverts the
three fixes in a throwaway worktree, keeping the controls, and shows each
test failing on its control's own check.

## The defects

- **TEST-1, `wait-multi-prepare`** (flakes.md, two AArch64 CI sightings).
  After the wake, a `schedule()` -- a tick, or a preemption point inside the
  second `waitqueue_prepare` -- can pick the test thread again: the
  `next == prev` path sets it RUNNING without counting a switch, and the
  test's guard ("this CPU switched no thread") judged a state the scheduler
  had legitimately changed. **Control:** a `schedule()` right after the
  wake on the pinned CPU, which re-picks the thread on every boot.
  **Fix:** `sched_pick_count` (`CONFIG_SELFTEST` only) counts every pass
  through the pick, and the test judges a run only when its CPU made no
  pick meanwhile.
- **TEST-2, `module-unload-busy`** (flakes.md, two x86-64 sightings).
  `module_unload` waits on its deadline clock, which on a machine whose
  clock is not common is the machine-wide tick, up to a tick behind the
  per-CPU clock the test measured the wait with. **Control:**
  `module_unload`'s 48 ms wait replayed on a pinned CPU whose `clock_now_ns`
  is injected 40 ms back (`clock_test_set_cpu_offset_ns`, which the deadline
  clock never reads); the old measurement reads about 8 ms of it. A first
  control that relied on tick timing (the clock forced not common, the wait
  started 3.5 ms after a tick) failed on AArch64 but passed on x86-64, whose
  ticks under TCG ran late enough to cover the gap; the injected offset does
  not depend on when ticks arrive. **Fix:** the wait is judged by a deadline
  on the same clock (`clock_deadline_ns` before, `clock_deadline_passed`
  after).
- **TEST-3, `net-neigh-down-race`** (flakes.md, one x86-64 sighting). ARP's
  and ND's park hooks parked the first caller on any interface, so eth0's
  own ARP work could take the park while the test's resolve on `ndown0` ran
  unparked. **Control:** armed for `ndown0`, a resolve on a decoy interface
  (`ndecoy0`) must pass the hook. **Fix:** `arp_test_hold_lock_entry` and
  `nd_test_hold_lock_entry` take the interface to park on (NULL disarms);
  the unarmed path still does one relaxed load and no read-modify-write.
- **TEST-5, `test-display`'s failure upload.** The upload took the whole
  directory, which holds the boots' QMP sockets; `upload-artifact` refuses
  a socket and then uploads nothing (#400's run, 2026-10-17). An exclusion
  pattern leaves out `*.sock`. A CI step, so no probe: the next failing
  `test-display` shows it.

## Validation (2026-10-27, macOS host)

| Check | x86_64 | aarch64 |
|---|---|---|
| `tools/always-tests-probe.py --old` | PASS: the three tests fail on their controls (`the control's re-pick was not seen`; `control_passed`; `!arp_decoy`) | PASS, the same |
| `tools/always-tests-probe.py` (debug `make test` of the branch) | PASS: the three `ok`; the boot PASS (186.5 s); no lockdep report beyond the self-test's expected ones | PASS (195.7 s), the same |
| `BUILD=release make test` | PASS (19.4 s) | PASS (21.1 s) |
| `make analyze` | clean: 7 baselined, 0 unexpected | clean, the same |
| `make host-test`, `make fuzz` | PASS, PASS | (host) |

## Backlog

TEST-1, TEST-2, TEST-3 and TEST-5 are done. The unit's next PRs: LX-1 and
PKG-1; NET-6 and SEC-4; BUILD-1.
