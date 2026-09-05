# Security: testing (access control and resource limits)

| Level | What | Command |
|---|---|---|
| Host | `test_cred` (the `setres*` rules) | `make host-test` |
| Kernel, debug | `rlimit` (address-space, memory and handle limits on a private space and table), `cache-limits` (ramfs budget, global limit with reclaim), `cache-budget-race` (two writers against one budget, a sampler watching the peak), `process-rlimit` (three `init --probe` runs), `process-nproc` (two spawners against one `NPROC` limit, a sampler watching the peak), `hv-npt` (a VM's cap from its creator) | `make test` |
| User mode | `init --unpriv-test` (the permission boundary, `docs/kernel-services/vfs/invariants.md` V14); `init --probe rlimit-root`, `rlimit-unpriv`, `mem-limit`, `uid-is:N` | run by the kernel tests |
| Linux | `lxtest`: `getrlimit`, `prlimit64`, `setrlimit` lowering and raising, `EMFILE` at the eighth handle, `cur > max` `-EINVAL`, another pid `-EPERM`, an unbounded resource read as infinity and its set ignored | `make test` (x86-64) |

## The probes (`userland/init/init.c`, `probe()`)

`rlimit-root` (exit 0, a nonzero exit names the step): the defaults
(`AS` 2 GiB, `NOFILE` 64, `NPROC` 128); an unknown resource and
`NOFILE` 65 are `-EINVAL`; `AS` 16 MiB makes a 32 MiB `mmap` `-ENOMEM`
and a 1 MiB one succeed, and root raises it back; `NOFILE` 4 makes
`pipe` `-EMFILE` (0, 1, 2 open; room for one), 64 makes it succeed;
`NPROC` 1 makes `spawn` `-EAGAIN` (the caller counts), 128 lets it
through; `spawnve_as(…, 1000, 1000)` runs `uid-is:1000` to exit 0;
`VMEM` 1 MiB: a VM created through `/dev/vmm` refuses 2 MiB and takes
1 MiB (skipped where no backend exists).

`rlimit-unpriv` (exit 0): drops to 1000:1000; lowering `NOFILE` to 60
succeeds, raising to 64 is `-EPERM`, `MEM` likewise; `spawnve_as` to
0:0 and to 1000:0 are `-EPERM`, to 1000:1000 succeeds; every `procinfo`
record carries uid 1000; 80 `log` calls: at least 16 succeed and at
least one is `-EAGAIN`.

`mem-limit` (exit 139): `MEM` 1 MiB, then a page-by-page touch of a
4 MiB mapping. `hold` (exit 0): sleeps 30 ms, so `process-nproc`'s
children stay alive long enough to be counted.

## The admission races

Greptile's review of the milestone found two check-then-act windows: the
`NPROC` count was taken and released before the process was registered,
and the ramfs budget was read before the page was charged. Both are now
one atomic step (`docs/kernel/security/invariants.md` S4, S6), and
`process-nproc` and `cache-budget-race` sample the invariants under two
concurrent actors on four CPUs; a regression would show as a peak above
the limit.

## Results (2026-09-05, QEMU TCG, x86-64 and AArch64)

```
selftest: rlimit: address-space, resident-memory and handle limits bind where they are enforced
selftest: cache-limits: ramfs budget refused 3 misses; N clean pages reclaimed under the global limit
selftest: process-rlimit: limits inherited, lowered, raised only by root; SETCRED flows down; a memory limit ends the toucher
selftest: process-nproc: 4 admitted, 12 refused across two spawners; peak 4 of a limit of 4
selftest: cache-budget-race: 167 pages admitted, 313 refused across two writers; peak 4 of a budget of 4
SELFTEST: PASS (105 tests)
```

`rlimit` under 1 ms, `cache-limits` about 60 ms, `process-rlimit` about
20 ms, `process-nproc` about 100 ms, `cache-budget-race` about 15 ms on
x86-64. The syscall fuzzer exercises `getrlimit` and leaves
`setrlimit` out (a random low memory limit would end the fuzzer itself).

## Gaps

- No test of `NPROC` across two unprivileged users, or of a user
  reaching `NPROC × MEM` in aggregate (limits are per process).
- The global cache limit is not tested under concurrent readers, and
  reclaim's `mutex_trylock` skip path (a busy victim) is not driven
  (the budget race test exercises concurrent misses, not reclaim).
- `procinfo` visibility is tested for the unprivileged view only; that
  root sees every process is exercised by `ps` in the shell test.
- No `chmod`/`chown`, no group-permission test (unchanged from V14).
