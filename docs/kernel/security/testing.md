# Security: testing (access control and resource limits)

| Level | What | Command |
|---|---|---|
| Host | `test_cred` (the `setres*` rules) | `make host-test` |
| Kernel, debug | `rlimit` (address-space, memory and handle limits on a private space and table), `cache-limits` (ramfs budget, global limit with reclaim), `cache-budget-race` (two writers against one budget, a sampler watching the peak), `process-rlimit` (three `init --probe` runs), `process-nproc` (two spawners against one `NPROC` limit, a sampler watching the peak), `hv-npt` (a VM's cap from its creator) | `make test` |
| User mode | `init --unpriv-test` (the permission boundary, `docs/kernel-services/vfs/invariants.md` V14); `init --probe rlimit-root`, `rlimit-unpriv`, `mem-limit`, `uid-is:N` | run by the kernel tests |
| Kernel, M1 | `random` (output differs, self-test input credits 0), `random-seed` (seeded: the wait returns at once, at least 256 bits credited; unseeded: a 20 ms wait times out after 20 ms, an uncredited input changes nothing), `cosmofs-crypt` (unseeded: encrypted format `-EAGAIN` after at least 5 s, device untouched) | `make test`, `make test-entropy` |
| Boots, M1 | the ordinary boots (virtio-rng, no CPU source: seeded by devices); `test-entropy`'s no-entropy boot (`QEMU_RNG=0`, default CPU: the `WARN`, unseeded markers, boot completes) and CPU-source boot (`QEMU_RNG=0 QEMU_HWRNG=1`, `qemu64,+rdrand,+rdseed` / `neoverse-v1`: seeded with `cpu` credit only). The harness requires and forbids the lines in design.md §6 per configuration | `make test`, `make test-entropy` (CI job "entropy boots") |
| Linux | `lxtest`: `getrandom` flags (`RANDOM|INSECURE` and an undefined bit `-EINVAL`; `INSECURE` answers; seeded: `0`, `RANDOM`, `NONBLOCK` answer; unseeded: `NONBLOCK` and a zero-length `NONBLOCK` are `-EAGAIN`) and the line naming the pool's state;  `getrlimit`, `prlimit64`, `setrlimit` lowering and raising, `EMFILE` at the eighth handle, `cur > max` `-EINVAL`, another pid `-EPERM`, an unbounded resource read as infinity and its set ignored | `make test` (x86-64) |

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

## `make test-security`, stage `priv` (roadmap M5)

`tests/boot/security_test.py` boots the signed release image and runs
`/boot/tests/security/priv.sh` with BusyBox ash as root from init's
shell; the script prints `KEY=VALUE` lines the harness checks, each
refusal printing EPERM by name:

| Check | Expected |
|---|---|
| `priv` as root | `mount sys_admin net proc cred device` |
| `/sbin/mount none /tmp/m5-mnt ramfs` as root | 0 |
| the same under `priv -d mount` | 1, `Operation not permitted`; the child's `priv` lacks `mount` |
| `priv -d mount -- priv -k all -- mount ...`, and the same through `sh -c` | 1: nothing gives a privilege back |
| under `priv -k mount` (that privilege alone) | 0, then `umount` 0 |
| as uid 1000 (`priv -u 1000`) | 1; its `priv` says `none` (leaving uid 0 drops all) |
| `sysctl kernel.hostname=...` as root / `-d sys_admin` / `-k sys_admin` | 0 / 1 (EPERM) / 0, and the name is the last one set |
| kill of a uid-1000 process: native `kill` under `-d proc`, ash's builtin under `-d proc`, ash's as uid 1001, native under `-k proc` | refused, refused, refused, 0 (the victim's status 143) |
| SysV shm: a uid-1000 `0600` segment (`tests/security/shmprobe`) attached and removed by uid 0 under `-d sys_admin`, then with every privilege | `EACCES`, `EPERM`, then `ok`, `ok` |
| uid 1000: `dmesg`; `cat /proc/*/status /proc/*/limits; ls -l /proc/*/fd; sysctl -a` | refused; at least 1000 bytes read and no `ffff` followed by twelve hex digits |

`test-net-login` adds acceptance e: the root session over ssh prints `net
cred` from `priv` and its `mount` is EPERM (the dropbear service's
`privileges net cred`). `tests/host/test_cred.c` holds the rules
(`cred_has`, `cred_drop`, `cred_ids_changed`, `setres*`, `setgroups`,
`cred_may_signal` with and without each privilege).
