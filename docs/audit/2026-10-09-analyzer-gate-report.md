# Static analyzer gate

## Scope and baseline

Work started 2026-10-09 from main `55b1825ebc28c7f5cdb948477019808dc8ed5517`
(PR #336), on branch `analyzer-diagnostic-gate`. PR A inventories target
analyzer diagnostics, repairs proven defects and removable reports, and
gates remaining diagnostics against reviewed architecture baselines.
Device-lifecycle Unit 2 is a separate PR after this PR's merge CI passes.

Fresh baseline commands use GNU make, debug defaults and distinct OUT trees:
`gmake -j4 ARCH=<arch> OUT=out/analyze-gate-baseline-<arch> analyze`.
Logs: `out/analyze-gate-baseline-{x86_64,aarch64}.log`.

The archived AArch64 runs contain 19 reports on October 3 and 28 on
October 8. Ten reports appeared and one epoll null-dereference report
disappeared: the increase of nine is a net change. Investigation starts
with those ten reports. The old target discards diagnostics, touches
`.analyzed` stamps and unconditionally prints `static analysis: clean`.
Consequently, unchanged files do not report again on an incremental run.

## Gate design

Every `analyze` invocation analyzes every enumerated target translation
unit and writes a Clang plist report under OUT. The gate validates all
expected reports and compares each diagnostic with the selected architecture's
checked-in baseline. Keys include translation unit, checker, diagnostic
file, function and message; line and column are display information only.
Duplicate keys are counted so an additional identical report cannot hide
behind an existing entry. Every baseline entry requires a specific reason.
Missing or malformed reports and unreviewed diagnostics fail the target.
The success message states both the reviewed count and zero unexpected
diagnostics. No analyzer checker is disabled.

Full analysis avoids stale diagnostics after header, configuration or
compiler changes. CI runs the same `analyze` target. The probe introduces
a diagnostic in a throwaway worktree and compares the original target's
false success with the new gate's rejection on each architecture.

## Investigation and validation

Results will be recorded as the checks run; completion requires the full
AGENTS.md matrix on both architectures and branch and merge CI.
