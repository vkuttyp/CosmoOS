# Roadmap 1.2's first unit, second PR: LX-1 and PKG-1, 2026-10-27

The second PR of the "Always, whichever direction" unit (the first, #464:
TEST-1, TEST-2, TEST-3, TEST-5). Two correctness lines from the backlog.

## Scope and baseline

Branch `always-correctness` from main at `6df2fce8`. Each defect has a
test commit that fails on main's code on every boot, then the fix.
`tools/always-correctness-probe.py --old` reverts the two fixes in a
throwaway worktree, keeping the tests.

## LX-1: `signal-stop`'s check 9

`signal-stop`'s child exited as soon as it was continued, and check 9
then waited with `WCONTINUED` for the continue. A parent slower than the
child found it exited and got its exit status (four sightings, all
AArch64, three at `QEMU_SMP=1`, all on loaded hosts; flakes.md).

**The kernel's order was right, not the test.** POSIX's `WCONTINUED`
reports a continued *process*; one that has exited is reported by its
exit. Linux's `wait_consider_task` checks for a zombie before it looks at
a continue, the same order as `child_event_locked`. Changing the kernel
to report the continue first would have diverged from Linux.

- **Test (failing first):** the parent sleeps 100 ms after `SIGCONT`, so
  the child has exited by the wait; check 9 fails on every boot.
- **Fix:** the test's child (`signal-stop-child-wait`) exits only when its
  parent, having seen `WIFCONTINUED`, sends `SIGUSR1`. A new self-test,
  `signal-stop-exit`, pins the order: a child continued and gone before
  the wait is reported by its exit, and its continue is not reported
  afterwards. The process design says the exit outranks an unreported stop
  or continue.

## PKG-1: whole-database walks past 128 installed records

Six of `pkg`'s walks over the installed database -- a file owned by
another package, `remove`'s dependants, `list`, `upgrade`, `rollback`,
`verify` -- read the names into a 128-entry stack array and passed over
the rest silently, and `upgrade` also planned at most 128 requests.

- **Test (failing first):** `rc.test` makes 130 installed records by hand
  (BusyBox's `sh`, since the shell test has no loops), requires `pkg list`
  to show all 130 (`pkg-many: N of 130 listed`), and removes them. On main
  `pkg list` shows 128.
- **Fix:** `installed_all` reads every name into a heap array, doubling
  until the directory is read whole, and replaces the six arrays; `upgrade`
  plans as many requests as packages. Out of memory, the command stops
  rather than walk part of the database. The shared-library view keeps its
  explicit, refusing limit (`PKG_LIB_SCAN`). The `pkg` API and design docs
  no longer list a 128-package limit.

## Validation (2026-10-27, macOS host)

| Check | x86_64 | aarch64 |
|---|---|---|
| `tools/always-correctness-probe.py --old` | the two defects: `signal-stop` FAIL, check 9; `pkg-many: 128 of 130`; no `signal-stop-exit` line (the fix adds it) | the same |
| `tools/always-correctness-probe.py` (debug `make test`) | PASS: `signal-stop`, `signal-stop-exit` ok, `pkg-many: 130 of 130`; the boot PASS (197.3 s) | PASS (196.7 s) |
| `BUILD=release make test` | PASS (19.5 s) | PASS (21.7 s) |
| `make analyze` | clean: 7 baselined, 0 unexpected | clean, the same |
| `make host-test`, `make fuzz` | PASS, PASS | (host) |

The probe's first `--old` runs expected a `signal-stop-exit` line, which
the reverted fix commit had added; the boots showed both defects as
intended, and the criterion was corrected to expect no line.

## Backlog

LX-1 and PKG-1 are done. The unit's next PRs: NET-6 and SEC-4; BUILD-1.
