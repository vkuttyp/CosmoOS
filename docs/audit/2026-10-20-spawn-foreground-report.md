# A foreground job has the terminal at its spawn

Date: 2026-10-20. Base: `main` at `50ef1f4d` (after #421-#423). A defect
fix taken ahead of roadmap 1.2 B4 because it turned main red (AGENTS.md,
scope rules): CI on #420's merge, `01c85167`, a documentation-only
commit, failed `busybox test (x86_64)`.

## The failure

`test-busybox: FAIL: busybox: QEMU exited (code 35) waiting for 'exit'`,
after the scripted test passed (run 39397473633, job 118543818694). The
serial log: for the ash-history step the harness types `sh` at the
console shell, `cosmo-sh`, and gets `[1]+  Stopped  sh` at once; the
following `history` reaches `cosmo-sh`, which has no such command, and
its `exit` ends the boot. The same signature on AArch64 on 2026-10-18
(run 39114435454, `v1.0.5-notes`) had not been recorded. Both are in
`docs/testing/flakes.md`, "ash history: the nested shell stopped".

## The mechanism

A boot that starts and leaves a nested `sh` at the console in a loop
(release image of main, x86-64) stopped 1 time in 300. A build logging
every `tcsetpgrp`, every `tcgetpgrp` and every stop signal stopped 2 in
400, and both times showed the same order:

```text
process: pid 34 'sh' created
DBG tcgetpgrp pid 34 pgid 34 rc 0 fg 16
DBG tcsetpgrp pid 16 pgid-now 16 fg 16 -> 34
DBG signal_send sig 21 to pid 34 from pid 34
[1]+  Stopped                  sh
```

`cosmo-sh` (pid 16) spawned the job into a group of its own and then made
that group the terminal's foreground with `tcsetpgrp`. The job, BusyBox
ash, starts job control by asking `tcgetpgrp` and, while the answer is
not its own group, sending itself `SIGTTIN` (`setjobctl`). When the job
asked before the shell's `tcsetpgrp` landed, it stopped. A fork-based
shell does not have this race, because its child hands the terminal over
before `exec`; `cosmo-sh` spawns, so nothing ran in the child's place.

## The fix

- `COSMO_SPAWN_FOREGROUND` (`uapi/cosmo/syscall.h`, with `SETPGID` only):
  when the caller's group is the foreground of its session's controlling
  terminal, the child's group becomes the foreground before the child's
  first instruction (`process.c`, `tty_hand_foreground` in `tty.c`). A
  caller in the background changes nothing and asks with `tcsetpgrp`, as
  before. Tty invariant T21.
- libc `spawnve_pgrp_fg`, `spawnvp_pgrp_fg`.
- `cosmo-sh` spawns a foreground job's last stage with
  `spawnvp_pgrp_fg`, so the group already holds every stage when it takes
  the terminal (and `^C`); a lone command is its own last stage. Its
  `tcsetpgrp` after the spawn stays, for a job whose last stage did not
  start. The earlier stages of a pipeline start in the background for
  the moment the spawns take: backlog USR-6.
- Self-test `spawn-foreground` (`init --probe spawn-foreground`): the
  spawner never calls `tcsetpgrp`, which is the race lost every time,
  and the job's first act is `tcgetpgrp(0) == getpgrp()`; the control
  without the flag must find itself in the background; a spawn from the
  background must hand nothing over.
- `tools/spawn-foreground-probe.py --old` removes the hand-over in a
  throwaway worktree (the flag accepted and doing nothing, as before).

## Measured (2026-10-20)

- Probe `--old`: `spawn-foreground` fails at check 7 ("the job spawned
  to the foreground found another group there") on x86-64 and AArch64;
  the fixed mode passes on AArch64, and the x86-64 debug `make test`
  passes it (156.4 s).
- The nested-shell loop on this branch's x86-64 release image: 0 stopped
  in 400 (main: 1 in 300 and 2 in 400).
- The AGENTS matrix on this branch, every step PASS on both
  architectures: host-test, fuzz (x86-64), analyze, debug boots at
  `QEMU_SMP=1`, 2 and 4, `test-smp2`, `test-chaos`, `test-harness-retry`,
  release test; and release `make test-busybox`, whose ash-history step
  is where main failed, on x86-64 and AArch64.

## Review (#424)

- Qodo, Codex: a pipeline whose first stage did not start spawned the
  stage that made the group without the flag. Codex: handing over at the
  first stage let `^C` reach only the stages spawned so far. Both fixed
  by handing over at the last stage spawned, when the group holds every
  stage; a lone command is its own last stage. The residue -- earlier
  stages start in the background for the moment the spawns take -- is
  backlog USR-6 (a start barrier would close it).
- Codex: the groups were read outside the process table lock, so a
  `setpgid` by another of the caller's threads could land in between.
  The groups are read and the terminal handed over under that lock.
- Qodo: a probe's comment had been left above the new probe; moved back.

After the fixes: x86-64 debug `make test` PASS 154.4 s, AArch64 PASS
170.1 s (lockdep on, the new table-lock-then-tty nesting); release `make
test-busybox` PASS; `make analyze` clean on both; the probe's `--old`
still fails `spawn-foreground` at check 7 on both architectures.
