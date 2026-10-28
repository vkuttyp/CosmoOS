# ash.sh: ps-nice read the shell before the exec, 2026-10-28

Main's CI run 40703059019, on the merge of #477 (`f87a808f`, roadmap 1.2
A5's third PR), failed `busybox test (aarch64)`:

```
test-busybox: FAIL: dynamic scripted test: BBTEST: FAIL ps-nice: want [5315 7] got [5315 0]; BBTEST: FAIL
```

The same job passed on the branch's own run (40700526693). The check is
A5's second PR's (`tests/busybox/ash.sh`); the kernel's nice value is
not at fault. Main red, so fixed now rather than recorded.

## Scope and baseline

Branch `ps-nice-race` from main at `46f5d939`. One test script and a
probe; no kernel, library or harness change. `tools/ps-nice-probe.py
--old` reverse-applies the fix commit's change to `ash.sh` in a
throwaway worktree.

## The mechanism

```sh
nice -n 7 sleep 64 &
np=$!
expect ps-nice "$np 7" "$(ps -o pid,nice | awk ...)"
```

`$!` is the shell ash forks for the background command. That process is
ash, at the parent's nice 0, until it has execed `nice`, which calls
`setpriority` and then execs `sleep`. The script goes on to `ps` at
once; when the child has not yet run `nice`, ps reports nice 0. Nothing
orders the two, so the check passed whenever the child won the race,
which it usually does.

The next check shares the window: `renice -n 2 -p $np` on the shell
leaves it at 2, and `nice -n 7` then sets 9 after renice-add has read 2.
CI's sighting was short enough to reach only ps-nice.

## The fix

Before ps-nice, wait until ps names the process `sleep` (ten 0.1 s
steps, the idiom the script already uses at its stop and pgrep waits).
By then `nice` has set the value and execed; every later check reads
the process as it is meant to be. No assertion changed.

## Proof

`tools/ps-nice-probe.py` holds the window open by the mechanism: in
both modes the background command becomes `{ sleep 0.3; exec nice -n 7
sleep 64; }`, the same process kept as the shell for 0.3 s before it
runs `nice`. Release `make test-busybox`, as CI runs it:

| arch | mode | result |
|---|---|---|
| x86-64 | fixed | test-busybox passes |
| x86-64 | --old | ps-nice `want [419 7] got [419 0]`, renice-add `want [9] got [2]`, nothing else |
| AArch64 | fixed | test-busybox passes |
| AArch64 | --old | the same two failures, nothing else |

## Validation

A test-script change confined to the BusyBox boot: the probe's four
runs above, and branch CI on both architectures. The device-lifecycle
matrix's kernel boots do not run `ash.sh`, and this change touches no
code they build, so the matrix was not run.

## Follow-ups

None. A5's other checks in the block (`nice-self`, `nice-exec`,
`renice-set`) run in the foreground or after the wait.
