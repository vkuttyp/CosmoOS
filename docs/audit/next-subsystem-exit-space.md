# NEXT SUBSYSTEM — an exited process keeps its memory until its last reference drops

> **Status: built (PR #253).** As designed, with these specifics:
>
> - **The teardown at exit:** `process_last_thread_gone` destroys the
>   space and sets `p->space = NULL` before anything else. The second
>   hold point (`process_test_hold_exiting`) sits right after it, before
>   `EXITED` is published. The reap hold now takes a point (0: a zombie,
>   before the reaper's put; 1: exiting).
> - **The dying teardown:** `dying_range_teardown` (`vmm.c`) is used only
>   by `vm_space_destroy`, which invalidates the tag first and keeps the
>   final invalidate. `vm_space_destroy_counted` returns the pages
>   queried. `arch_mmu_absent_span` is added on both architectures; on
>   aarch64 it claims nothing for the wrong half of the address space,
>   as `arch_mmu_query` does.
> - **P33 blames once:** `selftest_leftover_reported`, and a set of up to
>   64 pids (the snapshot's own limit), pruned from each locked snapshot.
>   The runner remembers every pid a check returns, whether or not it
>   names it in the log. `kill_module`
>   drops its reference before any check. `elf-txtbsy`'s copy loop
>   became `write_program_copy`, which `exit-space-order` shares.
> - **Tests:** `vm-teardown-absent` (memtest; queries exactly the 4
>   populated pages), `p33-once` and `exit-space-order` (debug builds).
> - **Measured unforced** with the probe on the built tree: **0 of 900
>   rewrites refused on each architecture.**
> - **Speed, side by side:** `main` and the branch, booted at the same
>   time on the same architecture, twice each, show no measurable
>   change in the suite's total (aarch64 109.5 and 104.1 s against 106.4
>   and 104.0; x86-64 101.5 and 97.4 against 108.8 and 95.6).
>   `process-user` on aarch64 was faster (7.3 and 8.0 s against 6.3 and
>   6.0). The 12–14% below was measured with the probe's 900 extra
>   spawns in both arms, which is where a cheaper teardown shows.
>
> Mutations, each alone, with its boot confirmed:
>
> | mutation | result |
> |---|---|
> | the teardown back at release | `exit-space-order` FAIL: "writing at the exiting hold returned -26" (x86-64 and aarch64) |
> | the teardown after `EXITED` is published | `exit-space-order` FAIL, the same (x86-64) |
> | the absent-span skip disabled | `vm-teardown-absent` FAIL: "queried 2052 pages" |
> | blame-once forgotten | `p33-once` FAIL: "a process already reported was reported again" |
> | the first invalidate removed | every test passes on both architectures, as the design said; recorded in `docs/kernel/memory/testing.md`, "The destroy-path invalidates have no test" |

## Problem

A process exits in two steps. The reaper runs `process_last_thread_gone`,
which marks the process `EXITED`, closes its handles, completes its exit
and wakes its parent. The process is *released* (`process_release`) when
its last reference drops, and only the release tears down its address
space (`vm_space_destroy`). The last reference is often the reaper's
own, dropped just after `process_last_thread_gone` returns
(`thread_put`), and sometimes the waiter's.

So between the exit and the release, an exited process still has its
address space: its frames stay allocated, and its text mapping still
holds its program file busy (`-ETXTBSY`, the interlock the ELF
shared-text unit built from the file's mapping list). The native
`waitpid` (`process_wait_child`) reaps a child as soon as it is
`EXITED`. A program that waits for a child and then rewrites the
child's binary can therefore be refused. That is ordinary for a build
or install step, and it is exactly what `elf-txtbsy` does in the kernel.
Linux releases the address space in `do_exit`, before the parent is told
(`exit_mm` before `exit_notify`).

The proc-settle unit found this by forcing the window with a reap hold
(`docs/audit/next-subsystem-proc-settle.md`). This report measures it
**unforced**, from a user program, and measures a fix.

### Measured

`tools/exit-space-probe.py` adds `init --txtbsy-race N`. It copies
`/boot/init` to `/tmp/txbsy`, then N times spawns that copy (with
`--exit0`, which returns at once), `waitpid`s it, and rewrites the
file's last byte with its own value. It counts the rewrites refused with
`ETXTBSY` and times how long the file stays busy. A kernel test runs it
(registered REPS times). `--parts` times every `vm_space_destroy` by
part. `--fix` and `--fast` are the candidates below. Debug builds, each
boot confirmed.

**Unforced**, 4,200 rounds (300 and 900 per boot, plain and chaos):

| boot | refused after `waitpid` |
|---|---|
| aarch64, plain | 0 of 300; 6 of 900 |
| x86-64, plain | 1 of 300; 0 of 900 |
| aarch64, chaos | 2 of 900 |
| x86-64, chaos | 1 of 900 |

**10 of 4,200**, on both architectures, with the file busy for **0.23 to
2.3 ms** after `waitpid` had returned. This is not a test artefact: a
user program meets it.

**Where a teardown goes** (`--parts`, every `vm_space_destroy` in a
boot, means; two boots per architecture, each counted per space):

| | aarch64 (616, 617 teardowns) | x86-64 (601, 602) |
|---|---|---|
| regions / pages walked / frames freed / chunks | 13.0 / 2073–2104 / 49 / 76 | 13.3 / 2122–2138 / 59–60 / 77–78 |
| walk (a query per page, and the unmap, under the lock) | 5.9–9.0 ms | 1.2–1.6 ms |
| shootdowns (one cross-CPU round per 32-page chunk) | 4.5–7.4 ms | 2.8–4.4 ms |
| frees | 1.4–1.5 ms | 1.5–1.7 ms |
| **all** | **11.8–18.1 ms** | **5.7–7.9 ms** |

The timing itself slows each teardown, and the window widens with it:
the second aarch64 `--parts` boot refused 7 of 60 rewrites, with the file
busy for up to 16 ms. The `--parts` boots are not counted above.

About 97% of the pages walked are empty. The 8 MB stack reservation
(`USER_STACK_SIZE`) is 2,048 of them, and each is queried from the root.
Every chunk is shot down on the CPUs in the space's mask, though no CPU
is running a space being destroyed. `vm_space_destroy` already
invalidates the space's whole tag at the end.

**The candidates** (900 rounds per boot, per architecture):

| candidate | refused | `process-user` (a64 / x86) | suite total (a64 / x86) |
|---|---|---|---|
| none (the probe alone) | 6 / 0 | 7.0 s / 5.4 s | 128.9 s / 122.3 s |
| `--fix` after the state is published: the space torn down in `process_last_thread_gone` after `EXITED` is set | **1 / 2** | 6.3 s / 5.4 s | 123.6 s / 109.5 s |
| `--fix` first thing, before `EXITED` is published | x86 **0** | x86 **10.5 s**; the boot ran past its 180 s timeout | -- |
| `--fast` alone: a dying space skips absent stretches and per-chunk shootdowns | 133 / 1 | 6.3 s / 5.6 s | 141.6 s / 126.9 s |
| **`--fast --fix`** (first thing) | **0 / 0** | **5.5 s / 5.0 s** | **110.8 s / 107.3 s** |

Four results decide the design:

1. **The teardown must come before `EXITED` is published.** Placed
   after it, one rewrite in 900 was still refused on each
   architecture: `waitpid` reaps on the state, and a parent already
   looking reaps mid-teardown.
2. **At the old cost, moving it is too slow.** Every exit then waits for
   its teardown before anyone is told, on the one reaper thread.
   `process-user` doubled on x86-64 and the boot timed out. (The
   aarch64 boot of this row failed earlier, on the known `--block` flake
   in `process-spawn`; see below.)
3. **Made cheap, it is faster than today.** With a dying space's absent
   stretches skipped and its per-chunk shootdowns dropped, a teardown
   is 3.6 ms on aarch64 and 1.5 ms on x86-64 (medians 2.8 and 0.6 ms).
   With the teardown at exit, the suite then ran 12–14% faster than
   baseline, and no rewrite was refused.
4. **`--fast` alone is not a fix.** It changes which of the reaper and
   the parent drops the last reference, and on aarch64 the window
   *widened* (133 of 900). The window's width depends on timing; only
   the order closes it.

All four `--fast` boots passed, with the debug build's page poisoning.

### A cascade in P33, found on the way

In the aarch64 `--fix` boot, `process-spawn`'s `kill_module` found its
`--block` child already exited after 50 ms: the console-read flake the
ELF tests' old comment describes. Its `CHECK` returned without
`process_put`, leaking the reference. The process therefore never left
the table, and the runner's P33 check then failed **every later test,
19 of them**, each for the same pid. P33 should blame the test that
left a process once, not every test after it.

### Why it matters

- A user program that rewrites a binary it just ran can be refused,
  about once in 400 runs, for a millisecond or two.
- An exited process's memory stays allocated until an unrelated
  reference drops. The proc-settle probe saw that take a whole test
  (`dev-tty`'s process, released during `dev-tty-none`).
- The teardown walks every page of every region, and shoots down each
  chunk of a space no CPU runs. That costs 5.7–18.1 ms per process
  exit, on the single reaper thread or in whichever thread drops the
  last reference.

## Current implementation

- `process_last_thread_gone` (`kernel/process/process.c`): the state to
  `EXITED` first, then children re-parented, the session's terminal,
  handles and the cwd released, `complete(&p->exited)`, the parent woken.
- `process_release`: handles again, `vm_space_destroy(p->space)`, the
  directories, the namespaces, the parent reference.
- `vm_space_destroy` (`kernel/memory/vmm.c`): for each region,
  `user_range_teardown` in 32-page chunks, each with an
  `arch_mmu_query` per page, `arch_mmu_unmap`, `user_shootdown` and the
  frees. Then `arch_mmu_invalidate_asid`, `asid_release` and the
  context destroyed.
- Readers of `p->space` other than the process's own threads: none.
  `procfs` does not read it. Every syscall uses `process_current()`,
  and `setrlimit` changes only the caller's. The clear-child-tid futex
  wake at thread exit (`thread_clear_tid`) already checks for `NULL`,
  and `process_release` checks too.

## Design

### 1. The address space goes at exit

`process_last_thread_gone` tears the space down first, before it takes
any lock or publishes `EXITED`, and sets `p->space = NULL`:

- the text mapping, and with it `-ETXTBSY`, is gone before `waitpid`
  can reap the child or the exit completes;
- the frames are free before anyone is told;
- `process_release` keeps its `if (p->space)` for the processes that
  never reach exit (a failed create).

It runs where it already can today. `process_last_thread_gone` is
called from `thread_put`'s last-reference path, where `vm_space_destroy`
already runs whenever the reaper's reference is the last. No CPU has
the space loaded: its threads have all switched out, and a switch to
any other thread loads that thread's space (`kernel_space` for a kernel
thread). `vm_space_destroy`'s `KASSERT` states exactly that.

### 2. A dying space's teardown is cheap

`vm_space_destroy` tears down a space nothing runs, so:

- **absent stretches are skipped whole.** A new arch function,
  `arch_mmu_absent_span(ctx, va)`, walks to where the translation stops
  and returns how many bytes from `va` are certainly unmapped: the rest
  of the absent table's span, or 0 if `va` is mapped. It is eight lines
  in each architecture, over the walk both already have. The empty
  stack reservation is skipped a table at a time;
- **no chunk is shot down.** The space's tag is invalidated once, at the
  *start* of the teardown (`arch_mmu_invalidate_asid`), so no CPU holds
  a translation of it. Nothing can create a new one, because no CPU runs
  the space. The frames can then be freed chunk by chunk with no
  shootdown. (The prototype skipped the shootdowns without moving the
  invalidate; the design moves it, so that no frame is freed while a
  CPU could still hold a translation of it.) The invalidate at the end
  stays, for the reason its comment gives;
- a live space's `munmap` and friends are unchanged: they keep
  `user_range_teardown` as it is.

### 3. P33 blames once

The runner remembers the pids it has already reported as left, and
skips them in later checks. A test that leaks a process fails, and the
tests after it are not failed for the same process. `kill_module`'s
early returns drop their reference before returning.

### 4. The tests

- **`exit-space-order`**: a native child spawned from a writable copy
  of `init` exits. A second debug-only hold, armed by pid like the
  proc-settle unit's, parks the reaper after the teardown and before
  `EXITED` is published, and the kernel checks that the file is already
  writable and the child not yet reapable. (The proc-settle unit's hold
  stays where it is, for its own tests.) The test runs on every debug
  boot and proves the order, not a rate.
- `elf-txtbsy` is unchanged; its assertion is now the semantics.
- **`vm-teardown-absent`**: a space with a large sparse region is
  destroyed, and the teardown walks only its populated stretches. That
  is asserted from a count of the arch walks, not from time.
- **`p33-once`**: a process leaked by one test fails that test only.

### 5. The record

`invariants.md` (memory, process): an exited process has no address
space, and P33's blame-once rule. `flakes.md`: the second
`sched-migrate-refuses` sighting (plain x86-64, 2026-09-28: its spinner
refused as "affinity", as in the first), and two boots today with the
same four host-networking failures (`net-hostinput`, `net-hoststate`,
`net-flows-fw`, `net-output`). The inventory's §3 row struck through.

### 6. The §70 gate

**Correctness.** `waitpid` returns after the child's memory and text
are gone. Nothing reads a zombie's space: every reader was listed above.

**Concurrency.** The teardown runs where it already can, in
`thread_put`'s last-reference path. The tag invalidate moves before the
frees, so a skipped shootdown cannot leave a translation to a freed
frame.

**Ownership and lifetime.** `p->space` is `NULL` from exit on;
`process_release` and `thread_clear_tid` already accept that.

**Security.** A freed frame is never reachable through a stale
translation: the invalidate precedes the frees.

**Failure.** Unchanged: `vm_space_destroy` cannot fail.

**Performance.** 3.6 ms instead of 11.8–18.1 ms per teardown on
aarch64, 1.5 ms instead of 5.7–7.9 ms on x86-64, and 12–14% off the self-test suite
in the probe's boots. It is paid at exit, on the reaper, before the
parent is told.

## Affected files

| file | change |
|---|---|
| `kernel/process/process.c` | the teardown first in `process_last_thread_gone`; a second hold point after it |
| `kernel/memory/vmm.c` | a dying space's teardown: absent spans skipped, invalidate first, no chunk shootdowns |
| `kernel/include/arch/mmu.h`, `kernel/arch/{aarch64,x86_64}/mmu.c` | `arch_mmu_absent_span` |
| `kernel/core/selftest.c` | P33 blames once |
| `kernel/process/proctest.c`, `kernel/memory/*test*.c` | the three tests; `kill_module`'s early returns |
| `docs/kernel/memory/`, `docs/kernel/process/`, `docs/testing/flakes.md`, the inventory, `README.md` | the record |

## APIs

`arch_mmu_absent_span` (arch-internal). No syscall or ABI change.
`waitpid`'s return now implies the child's memory is free.

## Migration plan

One PR.

## Tests

As in §4. Mutations, each alone, with its boot confirmed:
- the teardown back in `process_release`: `exit-space-order` fails;
- the teardown after `EXITED` is published: `exit-space-order` fails;
- the absent-span skip disabled: `vm-teardown-absent` fails;
- the invalidate moved back to the end: a test that reads a freed frame
  through a stale translation (if one can be built deterministically;
  otherwise recorded as untested);
- the blame-once memory removed: `p33-once` fails.

The probe's 900-round boots, unforced, on both architectures, must show
0 refusals. Also `gmake host-test`, `gmake analyze`, and debug, release,
GIC and chaos boots on both.

## Benchmarks

The probe's `--parts` means before and after, and the suite's total
time, on both architectures.

## Risks

- **The reaper does every teardown.** It already did most of them.
  Cheap, they cost less than today's teardowns in the waiter.
- **A reader of `p->space` added later** would find `NULL` for a zombie.
  The invariant says so.
- **The per-chunk shootdown is skipped only for a dying space**, whose
  tag was just invalidated; `munmap` keeps it.

## Alternatives considered

- **Keep the teardown in the release, and drop only the text mappings
  at exit.** It would close the `-ETXTBSY` window, but keep the frames
  allocated past `waitpid`, and it splits a space's teardown in two.
- **Make `waitpid` wait for the release.** That needs a new wait point
  for the release, and it would still include whatever the last
  reference's holder was doing. The order is the fix.
- **The teardown at exit without making it cheap.** Measured: it
  doubled `process-user` on x86-64 and timed the boot out.
