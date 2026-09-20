# libc: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| Host | `tests/host/test_libc.c`: the pure parts compiled with the host clang under ASan and UBSan, functions renamed with a `c_` prefix so they do not clash with the host's libc | `make host-test` |
| Target, user mode | `init --selftest` (`userland/init/init.c`): every system-call-backed function through the library, plus `malloc`/`realloc`, `snprintf`, `strtol`, `setenv`/`getenv`, stdio on a file, `opendir`/`readdir`, `inet_pton`/`inet_ntop` | `make test` (self-test builds) |
| Integration | The shell and the utilities are built on the library and exercised by `/etc/rc.test` and the interactive harness | `make test` |
| Threads against the library's shared tables | `userland/tests/thrtest.c`, behind the `THREADTEST: PASS` marker | `make test` |
| The condition variable's broadcast over requeue, and `cosmo_thread_kill` | `userland/tests/thrtest.c` steps 23–30, same marker | `make test` |

## The shared tables under threads (`userland/tests/thrtest.c`)

Eight cases, and the unit that added them measured which prove
something rather than assuming
(`docs/audit/next-subsystem-libc-shared-tables.md`).

| case | what it does | unlocked? |
| --- | --- | --- |
| `env-spawn-under-setenv` | a thread looping `spawnvp` while another grows the environment and a third churns the heap — the reader of `environ` that lives outside `stdlib.c`. Four variants over `spawnvp_flags`'s **three freeing exits** — the function has four returns and only three free the snapshot. The absolute arm is *one* exit serving both a spawn that runs and one that cannot, so two variants aim at it: the second checks the **errno survives the `free`**, which is the only reason that arm saves and restores it. The other two are the `PATH` search finding something and the `PATH` search exhausting every element. The fourth return — `__env_snapshot` failing, where there is nothing to free — is untested and listed in the gaps below. The first build spawned only `/bin/true`, so the PATH-search half — the loop holding the snapshot across repeated attempts, and its frees — ran in no test; review found that, and then found the errno arm still missing | passes: a regression test, not a proof (the window is a few instructions inside a call that then spends milliseconds creating a process). The errno arm **is** a proof, and its mutation dies: drop the save/restore and ten `absolute spawn miss left errno 0, wanted ENOENT` lines appear. It guards something real rather than a defensive habit — `free` calls `munmap` for a large block (`libc/src/malloc.c:187`), and `munmap` sets `errno` |
| `env-grow-under-readers` | three readers in `getenv` against 400 `setenv` growths, **plus a thread churning the heap** so the freed array is reused | **the process dies**: `#GP`, signal 11, three runs of three — reliable, not forced |
| `env-unset-under-readers` | three readers against 200 `unsetenv` removals | passes — it removes no array, so it is the regression test of the set |
| `env-pointer-survives-overwrite` | holds the pointer `getenv` returned, overwrites the name, reuses the heap 400 times **at the freed entry's own size**, and reads it again — the contract the deliberate leak buys, which no other case here would have missed | **freeing the replaced string**. The size matters: the first version churned 64-byte blocks against an 18-byte entry, and passed against a `setenv` that freed it — vacuous, and caught by its own bug-proof |
| `atexit-concurrent` | eight threads registering through a start barrier | the drain loses handlers |
| `atexit-bound` | three threads offering 24 registrations at a full-ish table; the bound counts the verdict handler's slot, which is not in `at_registered` | **accepts 33 into a table of 32** — with `ATEXIT_MAX` raised to 33 the check fails, which it did not before that slot was counted |
| `atexit` from inside the drain | `reentrant_handler` calls `getenv` (the lock `exit` was holding) **and** `atexit` — the drain has popped the flood's slots by then, so the registration succeeds and LIFO runs the new handler next | a drain that walks a snapshot instead of re-reading the list: the late handler never runs |
| the drain's own check | registered **first** so the LIFO order runs it **last**; prints the verdict **and sets the exit status** | `THREADTEST: FAIL`, and `SHTEST: FAIL 1` from `/etc/rc.test` |

The verdict is printed by the last handler rather than by `main`,
because the drain is part of what is under test and a `main` that
printed `PASS` before calling `exit` could not be failed by it.

**The status travels as well as the marker.** `main` ends
`exit(failures ? 1 : 0)`, and since the drain's own checks can raise
`failures` after that status is fixed, the last handler ends
`_exit(1)` when they do — it is the last statement of the last
handler, and stdout is flushed just above it. The first build of the
moved verdict left `exit(0)` behind, so `/etc/rc.test`'s
`/boot/tests/native/thrtest || FAILS=1` saw success on a failing run.
Proving the repair then showed `rc.test` could not print
`SHTEST: FAIL n` either, for an unrelated reason in the shell
(`docs/userland/design.md`, "AND-OR lists are left-associative").

**Two details make the grow case work, and it proved nothing without
them.** The observed name is added *after* the padding, so a reader
walks the part of the array being reallocated instead of finding its
answer at the front; and a churn thread allocates and fills blocks in
the same size class, so the freed array is reused before the reader
reads it. Without the churn the test passes even unlocked, because
`setenv` frees the array and never a string — the stale copy's
pointers are all still correct.

## The native thread door (`userland/tests/thrtest.c` steps 23–30)

`docs/audit/next-subsystem-native-thread-door.md`. The broadcast's old
comment deferred a measurement "to the report"; this is it, as run.

**The herd** (step 23): eight waiters, each holding the mutex for a
millisecond after its wait returns so that the herd is visible by
construction and not by scheduling luck; one broadcast with the mutex
held at 1. Counted from the broadcast to the last return: sleeps on the
mutex word (calls to `futex_wait` on it), not "contended-path entries",
which every condition waiter now makes by rule.

| build | moved by the requeue | sleeps on the mutex word | wake-all would sleep | empty wakes |
| --- | --- | --- | --- | --- |
| x86-64 | 8 | 0 | 7 | 1 |
| AArch64 | 8 | 0 | 7 | 1 |

None, or one: the one waiter the requeue *wakes* (wake one, move the
rest) can find the broadcaster still holding the mutex and sleep on it
once, and did in every run of the first build — whose third rule marked
the word 2 before the woken waiter reached it; without that mark the
runs above found it free. The one empty wake is the last link of the
chain, whose unlock finds 2 and nobody left. The test asserts sleeps
below seven and prints the row; the wake-all broadcast put back gives
seven or eight.

| step | case | bug-proof (each a hang a bounded join reports, unless said) |
| --- | --- | --- |
| 23 | held at 1, eight waiters, every one returns | a waiter relocking through the fast path; the requeue waking none (move all); the wake-all broadcast (the count, not a hang) |
| 24 | held at 2; not held at all | the same two hangs |
| 25 | another thread holds the mutex and unlocks inside the broadcast, before the requeue and, separately, after it — through `__cosmo_cond_bcast_probe`, on the broadcasting thread | none specific: this is the interleaving the report's third rule was written for, and the step is what showed the two rules already cover it |
| 26 | a concurrent broadcaster (the probe, before the requeue) moves `seq` first: `-EAGAIN`, the retry finds nobody; then a concurrent *signal*: one woken, the retry moves the other two | the kernel requeue without its compare; returning on `-EAGAIN` instead of retrying (the signal case hangs) |
| 27 | a requeued timed wait expires on the mutex word, held past its budget | — (the count of moved waiters is the assertion) |
| 28 | a condition nobody has waited on: `mutex` NULL, `seq` bumped | — |
| 29 | the recorded mutex's page unmapped after the last waiter left; broadcast returns (a child: `thrtest stale-mutex`, status 0) | any load through the pointer: the child faults — the report's read-after-requeue did, tried without its guard, before it was removed altogether |
| 30 | `cosmo_thread_kill`: a running sibling's handler runs there and not here; a sibling with the signal blocked sees nothing anywhere until it unblocks; this thread by its pid; another process's thread `-ESRCH` and untouched (a napping child exits 0, not 138); a joined thread `-ESRCH` eventually; bad signals `-EINVAL` | the kernel delivering to the process instead of the thread |

**What the tests wait for.** "Entered" is not "asleep": step 27 moved
nobody one run in three because its waiter was still between its
unlock and its `futex_wait`. The steps now wait until the kernel says
every waiter is asleep — a requeue of the condition's word onto itself,
which the kernel counts without moving (`kernel/ipc/futex.c`; putting
the move back stops the boot at that count, with interrupts off). **A
count must not perturb**: the first CI run failed both architectures
at that wait, because the probe still bumped the bucket's wake
sequence, a waiter between its compare and its enqueue returned
spuriously, went round its loop, and blocked on the mutex the counter
held. The kernel no longer bumps it for a same-word requeue with
nothing to wake, and the wait releases the mutex each pass.

## Host test (`tests/host/test_libc.c`, `make host-test`)

Includes `libc/src/printf.c`, `libc/src/malloc.c` and `libc/src/conv.c`
directly after `#define`-renaming the functions they define (`printf`
becomes `c_printf`, `malloc` becomes `c_malloc`, and so on) and after
supplying the few externals they need: `c_write`/`c_fwrite` sinks that
count bytes, `c_strlen`/`c_strnlen`, an `mmap` macro that carves a
static 1 MiB arena and counts calls, a `munmap` macro that counts calls,
and a `c_abort` that records a failure. The host's own headers are
included first so the renames never touch host declarations.

| Area | Checks |
|---|---|
| `snprintf` | `%d %i %u`; width, `-`, `0`, `+`, space flags; `%x %X %o %#x %#o`; `%s` with precision and width both ways; `%c`, `%%`; `%ld %lld %zu %hd %hhd`; `%p`; `%s` of NULL is `(null)`; truncation to 4 bytes returns the full length 9; NULL buffer with size 0 returns 5; `%*d %-*d %.*d`; `%.0d` of 0 prints nothing; `LLONG_MIN`; `%f` prints `?`; `%q` prints `%q` |
| conversions | `strtol` with leading blanks, sign and trailing text (`end` set); `strtoul` base 0 with `0x`, leading `0` and decimal; base 36; no digits leaves `end == s`; overflow both ways saturates with `ERANGE`; `atoi("+77")`, `atol("-9")` |
| `qsort` | 200 pseudo-random integers sorted; `n` of 0 and 1 |
| allocator | 64 blocks of `i*37+1` bytes, 16-byte aligned, written and read back; even blocks freed, odd blocks grown with `realloc` (contents kept), all freed; a 16000-byte block then fits an existing arena (no new mapping); a 100000-byte block maps once and its `free` unmaps once; `calloc` zeroes; `malloc(1 << 50)` is NULL; `malloc(0)` is a unique block |

Prints `libc  ok` or `libc  FAIL (n)` with one line per failed check.
Result: 6 host binaries pass (`test_buddy`, `test_slab`, `test_crypto`,
`test_modelf`, `test_cosmofs`, `test_libc`).

## Target (`init --selftest`)

`fs_selftest` uses `fopen`/`fprintf`/`fgets`/`feof`/`fclose` on
`/tmp/stdio.txt` and `opendir`/`readdir`/`closedir` on `/tmp/d`;
`net_selftest` uses the socket functions with `struct sockaddr`,
`inet_pton` for `127.0.0.1` and `fe80::1`, `inet_ntop` back;
`proc_selftest` uses `pipe`, `dup`, `dup2`, `fstat`, `isatty`,
`spawnvp`, `spawnve`, `waitpid` (with and without `WNOHANG`), `kill`,
`chdir`, `getcwd` (including `ERANGE` for a 4-byte buffer), `mkdir`,
`rmdir`, `getppid`, `procinfo`, `klog_read`, `sysctl_get`, `malloc`,
`realloc`, `free`, `snprintf`, `strtol`, `strtoul`, `setenv`, `getenv`.
The full list is in `docs/kernel/process/testing.md`. Every failure
prints `USERTEST: check failed: <expression> (errno N)`; the run ends
with `USERTEST: PASS` or `USERTEST: FAIL (n checks)`.

## Gaps and planned tests

- `string.c`, `stdio.c`, `dirent.c`, `unistd.c` have no host test; the
  string functions are exercised only through everything else.
- No fuzzing of `vsnprintf` or `strtol`.
- No test of `atexit` ordering or of `fflush(NULL)` beyond exit.
- **Two new branches have no test and cannot get one from userland**,
  both recorded here rather than left implicit. `spawnvp_flags`
  returns `ENOMEM` when `__env_snapshot` cannot allocate, and
  `cosmo_thread_start`'s `EEXIST` retry has three behaviours worth
  checking — that it retries on `EEXIST`, that it does *not* retry on
  any other errno, and that exhausting `STACK_MAP_ATTEMPTS` returns
  rather than spins. Each needs a failure seam the library does not
  have: a malloc that can be made to fail, and a `MAP_FIXED` that can
  be made to collide. Both were proved by source mutation instead,
  which is this repository's bug-proof convention but leaves nothing
  standing in the suite; the retry's proof is in
  `docs/testing/flakes.md`. Adding the seams is the fix and is not
  done here.
- No leak or fragmentation measurement of the allocator under a
  long-running program (nothing runs long yet).
