# NEXT SUBSYSTEM — native threads and a futex

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: the front door gets what the side door already has. This
kernel runs threads, schedules them across every online CPU, gives each
its own registers, stack, thread pointer, signal mask and pending set, and
wakes them on a user-space word through `futex_wait`/`futex_wake` — and a
**native** program can reach none of it. There is no native syscall that
creates a thread and none that waits on a futex; the only door to either is
the Linux personality's `clone(CLONE_THREAD)`. So a Linux binary running
under CosmoOS can use every CPU in the machine and a CosmoOS binary cannot,
which inverts the rule this project has held since the compat layer was
built: the personality is a translation of what the machine offers, never a
superset of it. The unit closes that with five syscalls and no new
mechanism — `SYS_thread_create` (a `struct cosmo_thread` request, as
`SYS_spawn` takes a `struct cosmo_spawn`), `SYS_thread_exit`,
`SYS_thread_self`, `SYS_futex_wait` and `SYS_futex_wake` — because `process_add_thread`,
`process_thread_start`, `process_thread_abandon`, `process_thread_exit` and
the futex are all already there, already used, and already tested through
the other door. **Joining is not a syscall**: a thread names a word to be
zeroed and futex-woken when it exits, which is the mechanism the Linux side
already relies on (`clear_child_tid`), so a native `join` is a futex wait
in a library. The motivating consumer is the project's own `vmctl`: its
machine mode runs a guest's vCPUs **on one thread, a tick each**, and the
fairness rule and 64-turn power-off grace that unit needed exist only
because it cannot give each vCPU a thread — which the kernel's own
`el2-guest-uart-race` does, in-kernel, two vCPUs on two threads.**

## Problem

- **A native program cannot use a second CPU.** The scheduler is SMP, per
  CPU, with threads that migrate; `sched_getaffinity` under the Linux
  personality answers "every online CPU". A native program gets one thread
  and therefore one CPU's worth of work, whatever the machine has. Its only
  concurrency is more processes — separate address spaces, `SYS_pipe` or a
  socket between them, and a copy of every byte they share.
- **The Linux personality is more capable than the native one.** `lx_clone`
  accepts `CLONE_THREAD` and returns a tid; `lx_futex` waits and wakes. A
  `musl` binary with `pthread_create` works. The equivalent CosmoOS program
  cannot be written. Every earlier unit treated the compat layer as a
  *second door onto the same room* — the repository's own lesson is that a
  check enforced in `native.c` alone is half-enforced, because
  `compat/linux` reaches the same objects — and this is that asymmetry
  pointing the other way, which is worse: the front door is the one missing
  a feature.
- **`vmctl` pays for it in design debt.** Machine mode runs N vCPUs in one
  thread with `COSMO_VCPU_RUN_ONE_TICK` and a turn boundary after every
  PSCI call. Two units in this repository exist because of that: the
  round-robin itself, and the bounded power-off hold (`#112`) that had to
  be invented when a `SYSTEM_OFF` could outrun a sibling's first
  instruction. Neither would be needed if each vCPU had a thread, which is
  how the kernel's own two-vCPU test runs.
- **Everything a threaded program needs already exists except the door.**
  Per-thread registers, stack, `tls_base`, kernel stack, `sig_pending`,
  `sig_blocked`, `clear_child_tid`, a tid, `PROCESS_MAX_THREADS` (256) as a
  bound, `nr_live`/`nr_threads` accounting, `process_exit` versus
  `process_thread_exit` as two distinct functions, and a futex with wait,
  wake and requeue. The unit is an interface, not an implementation.

## Current implementation

**The machinery.** `kernel/include/kernel/process.h:326-330`:

```c
int process_add_thread(struct process *p, const struct arch_user_regs *regs, uintptr_t tls, struct thread **out);
void process_thread_start(struct thread *t);
void process_thread_abandon(struct thread *t);
struct thread *process_find_thread(struct process *p, uint32_t lx_tid);
```

`process_add_thread` links a thread into the process — `process_find_thread`
can see it — but leaves it un-runnable until `process_thread_start`, so the
creator can finish whatever must be true before the child's first
instruction and `process_thread_abandon` can undo a half-made thread. That
two-phase shape is exactly what a native creator needs, and it exists
because `lx_clone` needed it.

**`struct thread`** (`kernel/include/kernel/thread.h`) already carries
`tls_base` (restored on every switch to user), `sig_pending` and
`sig_blocked` **per thread** under `proc->lock`, `clear_child_tid` ("zeroed
and futex-woken at exit"), and `lx_tid`, "the Linux view of the id".

**The futex** (`kernel/include/kernel/futex.h`) offers `futex_wait(space,
uaddr, val, timeout_ns)`, `futex_wake(space, uaddr, n)` and a requeue,
keyed on the address space, initialised from `main.c`.

**Exit is already two operations.** `process_exit(status)`
(`process.c:780`) ends the process; `process_thread_exit(status)` (`:804`)
ends the calling thread and, when it is the last, sets the process's exit
status — via `p->pers->thread_exit`, a personality hook. Native `SYS_exit`
calls `process_exit`; the Linux `exit` calls `process_thread_exit` and
`exit_group` calls `process_exit`. **So the native meaning of `SYS_exit` is
already right** — the process, as POSIX `exit()` — and nothing about it
changes.

**The only door.** `compat/linux/syscalls.c:1457`, `lx_clone`: it refuses
anything but `CLONE_THREAD` (`-ENOSYS` for a fork-like clone, there being
no address-space copy), requires Linux's `CLONE_SIGHAND`+`CLONE_VM`
combination, copies the caller's registers, sets the result to 0, takes the
new stack and TLS from the arguments, writes the tid words *before*
starting the child so a joiner cannot read a stale value, and starts it.
`lx_sched_setaffinity` is accepted and ignored; `lx_sched_getaffinity`
answers `cpu_online_mask()`.

**What has no native syscall**: thread creation, thread exit, a thread's own
id, futex wait, futex wake. `SYS_COUNT` is 82 and the native table
(`kernel/syscall/native.c:1497`) has no entry for any of them.

## Why it matters

- **It is the one place the compat layer is a superset.** Not a missing
  translation — a missing *primitive*. A project whose personality layer is
  defined as "a second door onto the same objects" cannot leave the front
  door narrower, and no amount of care in `native.c` fixes it.
- **The machine's parallelism is unreachable from its own userland.** The
  SMP work — per-CPU run queues, migration, IPIs, the ASID/PCID unit, the
  locking audit — is exercised by the kernel and by Linux binaries. The
  system's own programs cannot touch it.
- **It removes design debt rather than adding surface.** `vmctl`'s
  round-robin, its `ONE_TICK` turns, its freshness tracking and its 64-turn
  power-off grace are all a single-threaded monitor's workarounds. They stay
  correct and stay tested; but the next unit can make them a *fallback*
  rather than the only way to run a machine.
- **It is five syscalls over tested mechanism.** The risk is concentrated in
  the interface decisions, not the implementation, which is the cheapest
  kind of unit to get right — and the most expensive to get wrong, since a
  syscall's shape is permanent.

## Proposed design

### Five syscalls, and one that is deliberately absent

```c
#define SYS_thread_create 82  /* (const struct cosmo_thread *req) -> tid */
#define SYS_thread_exit   83  /* (int status) -> does not return */
#define SYS_thread_self   84  /* () -> tid */
#define SYS_futex_wait    85  /* (uint32_t *word, uint32_t val, uint64_t timeout_ns) -> 0 */
#define SYS_futex_wake    86  /* (uint32_t *word, unsigned n) -> threads woken */
```

`SYS_COUNT` 82 → 87.

```c
struct cosmo_thread {
    uint64_t entry;        /* first instruction; receives `arg` in the first argument register */
    uint64_t arg;
    uint64_t stack_top;    /* the stack pointer the thread starts with; the caller owns the mapping */
    uint64_t tls;          /* thread pointer (x86-64 FS base, AArch64 TPIDR_EL0); 0 = none */
    uint64_t clear_tid;    /* a user word: the kernel writes the tid into it before the thread
                              runs and zeroes and futex-wakes it when the thread exits; 0 = none */
    unsigned flags;        /* 0 */
    uint32_t reserved;
};
```

A struct rather than five positional arguments, for the reason `SYS_spawn`
takes one: the fields will grow (a name, a priority, a CPU hint), and
`cosmo_spawn`'s pattern — new fields appended, read only when a flag asks
for them, so a caller built against an older header passes an older struct
and the kernel never reads past what it gave — is this repository's answer
to that and should not be reinvented per syscall.

**`SYS_thread_create`** validates the request, builds the register set from
`entry`/`arg`/`stack_top` (rather than copying the caller's, which is
`clone`'s shape and wrong for a native call — a native thread starts at a
function, not in the middle of a syscall), calls `process_add_thread`, sets
`clear_child_tid`, **writes the tid into the `clear_tid` word**, and only
then calls `process_thread_start`.

That write is the kernel's, not libc's, and it is ordered before the start
for the reason `lx_clone`'s own comment gives: if the caller wrote the word
after the syscall returned, a child that ran and exited first would zero
and wake it, and the caller's write would then leave a stale non-zero value
that a join waits on for ever. The kernel writing it before the child can
run makes the word's two states — the tid, then zero — the only two a
joiner can observe, and the interface self-contained: libc writes nothing.
A `clear_tid` the kernel cannot write is `-EFAULT` with the half-made
thread abandoned, which is what the two-phase creation is for. It returns the tid, or
`-EAGAIN` at `PROCESS_MAX_THREADS` or while the process is exiting,
`-EFAULT` for a request or a `clear_tid` word outside the caller's space,
`-EINVAL` for an unaligned `clear_tid`, an unknown flag or a `stack_top`
that is not aligned to the architecture's stack alignment.

**The entry-time stack invariant is part of the interface**, because a
thread is entered at a function without a `call` having happened. The
kernel hands the entry exactly what a call would have left:

- **x86-64**: a zero return address is pushed, so the entry sees
  `rsp % 16 == 8` — the alignment SysV promises a function, and the one
  its prologue's aligned spills (`movaps`) depend on. `stack_top` itself
  must be 16-byte aligned; the kernel does the push.
- **AArch64**: `sp` is `stack_top`, 16-byte aligned as the AAPCS requires,
  and `x30` (the link register) is **zero**.

In both cases a `return` from the entry function therefore jumps to address
zero, which is unmapped: a thread that returns dies with a fault at a
recognisable address instead of wandering into whatever the stack held.
Returning is not meant to happen — libc's `thread_create` passes a
trampoline of its own as `entry`, which calls the caller's function and
then `SYS_thread_exit` with its return value, so a native thread ends the
way a `pthread` does — but an interface should say what the machine does
when a program gets it wrong, and "fault at zero" is a better answer than
"undefined".

**`SYS_thread_exit`** calls `process_thread_exit`: this thread ends, its
`clear_tid` word is zeroed and futex-woken, and if it was the last the
process ends with that status. It joins `SYS_exit` and `SYS_sigreturn` in
`native_always_allowed` — a thread that cannot exit cannot be stopped, and
a syscall filter that can trap a thread in the kernel is a denial of
service the filter unit did not intend.

**`SYS_thread_self`** answers the caller's tid. One line, and the
alternative — deriving it from `SYS_procinfo` — asks a program to parse a
table to learn its own name.

**`SYS_futex_wait` / `SYS_futex_wake`** are `futex_wait`/`futex_wake` with
the caller's `vm_space`: wait returns 0 when woken, `-EAGAIN` if the word
does not hold `val` (the race the interface exists to close),
`-ETIMEDOUT`, or `-EINTR` when a signal is delivered; wake returns the
number of threads woken. The word must be 4-byte aligned and in the
caller's space. **Requeue is deferred** — it is a condition-variable
optimisation, it has no native caller until someone writes a condvar, and
the Linux door already exposes it for the binaries that want it.

**`SYS_thread_join` is deliberately absent.** A thread that wants to be
joined names a `clear_tid` word; a joiner reads the word and, if it is
non-zero, waits on it with `SYS_futex_wait`. That is the mechanism the
Linux personality already depends on, it needs no kernel table of
joinable-but-unreaped threads, and it composes: the same word answers "has
it finished?" without a syscall at all. The cost is that a library must
own the convention, which is where a `join` belongs.

### One id, two views

`lx_tid` becomes `tid`, and `process_find_thread`'s parameter with it. The
Linux view and the native view are **the same number** — one thread, one
id, so `/proc`, `SYS_procinfo`, `tgkill` and a native `thread_self` cannot
disagree. This is a rename plus the removal of the word "Linux" from a
comment, and it is the only change this unit makes to the compat layer.

### What is per-thread, and what is shared

| per-thread | shared by the process |
| --- | --- |
| registers, user stack, kernel stack | the address space (`vm_space`) |
| `tls_base` | the handle table |
| `tid` | credentials, groups, rlimits |
| `sig_pending`, `sig_blocked`, `sig_saved_blocked` | signal *dispositions* (`sigaction`) |
| `clear_child_tid` | cwd, root, namespaces, the syscall filter |

Two of those are worth stating because a program can now observe them.
**`SYS_sigprocmask` affects the calling thread** — the mask is already
per-thread, which was invisible with one thread and is now a documented
property. And **the syscall filter is per-process**, so a thread cannot
narrow its own; the filter unit's model is unchanged, which is what makes
it safe for `SYS_thread_create` to be filterable like anything else.

### Where a signal is delivered

Unchanged, and better than this report first described it. A
process-directed signal goes through `signal_send` → `route_locked` into
**`p->sig_shared_pending`** (`process.h:159`, "signals sent to the process,
not yet taken by a thread"), and any thread that does not block it may take
it: `sig_shared_pending & ~t->sig_blocked` (`signal.c:108`). That is
Linux's rule, already implemented, and it means a threaded native program
gets the behaviour it would expect — block a signal in the workers and the
thread that does not block it is the one that handles it — without this
unit designing anything. `recheck_defaults_locked` even covers the other
direction: a default-terminate signal already pending on the process
terminates it as soon as a thread stops blocking it.

(`process_kill` is a different path — it records the default-termination
state and wakes every thread — and this report named it by mistake in an
earlier draft.)

What is *not* here is per-thread signal **targeting**: there is no native
equivalent of `tgkill`, so a native program cannot direct a signal at one
of its own threads. That is a later unit, named and not smuggled in.

### The bound

`PROCESS_MAX_THREADS` (256) stays the bound, and **no new rlimit is
proposed**. The machine is already bounded: 256 threads per process,
processes per uid by `COSMO_RLIMIT_NPROC`, and a thread's stack is the
caller's own mapping and therefore already charged to `COSMO_RLIMIT_AS` and
`COSMO_RLIMIT_MEM`. A `COSMO_RLIMIT_NTHREAD` would add an ABI number and a
knob for a limit that is already enforced twice; if an operator ever needs
to lower it per process, that is the unit that should add it.

### The §70 gate

**Correctness.** Every new syscall is a thin argument check over a function
the Linux door already calls, so the state machine is unchanged. The one
new state a native program can reach is "a process with more than one
thread", and the kernel has been in that state under Linux binaries since
the compat layer was built.

**Concurrency.** Threads of one process run on any CPU, concurrently, in
one address space. `process_add_thread` and `process_thread_exit` take
`p->lock`; the futex has its own; nothing in the new syscalls holds two.

**Ownership.** The thread's kernel-side objects are the process's, freed
when the thread is reaped (`nr_threads` counts until then, `nr_live` until
exit). The thread's *stack* is the caller's mapping: the kernel neither
allocates nor frees it, and a program that unmaps a running thread's stack
faults that thread, exactly as it would corrupt its own.

**Lifetime.** A thread ends at `SYS_thread_exit`, at the process's exit, or
at a fatal signal. The last thread to leave sets the process's status. A
half-made thread is undone with `process_thread_abandon`, which is why
creation is two-phase.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h` | five numbers, `SYS_COUNT` 82→87, `struct cosmo_thread` |
| `kernel/syscall/native.c` | five handlers; `SYS_thread_exit` added to `native_always_allowed` |
| `kernel/include/kernel/thread.h`, `kernel/include/kernel/process.h` | `lx_tid` → `tid`, and the comments that name it |
| `compat/linux/syscalls.c` | the rename's other side; no behaviour change |
| `libc/include/cosmo/thread.h` (new), `libc/src/thread.c` (new) | `thread_create`/`thread_join`/`thread_exit` and a mutex over the futex — the library that owns the join convention, beside `libc/src/signal.c` and `process.c`, which wrap their syscalls the same way |
| `tests/native/` (new), `tests/native/native.mk` | `thrtest`, the userland proof, mirroring `tests/linux`'s shape |
| `userland/etc/rc.test`, `tests/boot/run_boot_test.py` | run it, require its marker |
| `docs/kernel/process/design.md`, `-/testing.md` | the model, the interface, what is per-thread |
| `docs/compat/linux/design.md` | the personality is a translation again, not a superset |
| `README.md` | Status entry |

## New APIs

Five syscalls and one uapi struct, above (`thread_create`, `thread_exit`,
`thread_self`, `futex_wait`, `futex_wake`). No new device, no new ioctl, no
new kernel subsystem. The userland library is new code but not new
interface: it is the convention that makes `clear_tid` a `join`.

## Migration plan

1. **The rename.** `lx_tid` → `tid` across the kernel and the compat layer.
   No behaviour change; the tree stays green.
2. **`SYS_thread_self`** alone: the smallest possible new syscall, which
   proves the table entry, the filter path and the test harness before
   anything can create a thread.
3. **`SYS_futex_wait` / `SYS_futex_wake`**, testable from one thread
   (`-EAGAIN` on a mismatched word, `-ETIMEDOUT` on a timeout, `-EFAULT`
   and `-EINVAL` on bad addresses) before any thread exists to wake.
4. **`SYS_thread_create` / `SYS_thread_exit`**, and with them the first
   native program that runs on two CPUs.
5. **The userland library**: create, exit, join over `clear_tid`, and a
   mutex over the futex.
6. **`thrtest`** in full, then the bug-proofs.
7. **Docs and README**, swept by listing every claim that native userland
   is single-threaded.

## Tests

A kernel selftest cannot create a *user* thread, so the proof is a native
userland program with a marker, as `HVTEST` is — `tests/native/thrtest`,
run from `rc.test`, requiring `THREADTEST: PASS` in
`run_boot_test.py`'s markers.

1. **A thread runs, and is joined**: `thread_create` returns a tid, the
   child writes a word and exits, the parent's join returns and sees it.
2. **The entry conditions are what the ABI promises**: the thread's first
   function reads its own stack pointer and reports it — `rsp % 16 == 8` on
   x86-64 (what a `call` leaves) and `sp % 16 == 0` on AArch64 — and does
   an aligned 16-byte spill, which is the thing that faults if the
   invariant is wrong rather than merely unusual. It also checks it
   received `arg` in the first argument register.
3. **Two CPUs are really used**: two threads each spin until both have
   observed the other's flag set, with a bound. On one CPU this cannot
   complete without preemption; the assertion is that it completes.
   (Under `-smp 1` the same test must still pass, because preemption alone
   suffices — so the test asserts *progress*, not parallelism, and the
   parallel case is what makes it fast rather than what makes it pass.)
4. **The futex closes the race it exists for**: a wait on a word that does
   not hold the expected value returns `-EAGAIN` without sleeping; a wait
   with a timeout returns `-ETIMEDOUT`; a wake returns the number woken;
   a wake with no waiter returns 0.
5. **`clear_tid` is a join**: the word holds the child's **tid** the
   instant `thread_create` returns — written by the kernel before the child
   could run, so the value cannot be a stale one the caller wrote — it is
   zero after the child exits, and a `futex_wait` on it returns when that
   happens. A child that exits before the parent looks is the same test
   with a `thread_exit` first thing, and must behave identically.
6. **Exit semantics**: `SYS_thread_exit` from a worker leaves the process
   running (the parent sees the join complete and keeps going);
   `SYS_exit` from *any* thread ends the whole process, and the harness
   sees the exit status the caller gave.
7. **The mask is per-thread**: a worker blocks a signal, the main thread
   does not, and a signal sent to the process is handled by the main
   thread — the property the table above claims.
8. **The bound holds**: creating threads until `-EAGAIN` gives at most
   `PROCESS_MAX_THREADS`, and the process is still healthy afterwards
   (every thread joins, the next create succeeds).
9. **The argument checks**: a request outside the caller's space, a
   `clear_tid` that is unaligned or unmapped, an unknown flag, an
   unaligned `stack_top` — each `-EFAULT` or `-EINVAL`, and no thread
   created (`SYS_procinfo`'s thread count unchanged).
10. **The filter**: a syscall filter that denies `SYS_thread_create` denies
   it; one that denies `SYS_thread_exit` **cannot**, because it is always
   allowed.

**Bug-proofs** to run against the shipped code, each observed to fail for
its stated reason — eight, each naming the step that catches it:

1. The tid written into `clear_tid` **after** `process_thread_start` rather
   than before → a child that exits first zeroes the word, the late write
   leaves a stale tid, and **step 5**'s join waits on it until the test's
   bound fails. This is the race `lx_clone`'s own comment says it was
   written to avoid.
2. The return address not pushed on x86-64 → **step 2**'s entry sees
   `rsp % 16 == 0` and its aligned spill faults.
3. `futex_wait` not re-checking the word under its lock → **step 4**'s
   `-EAGAIN` becomes a sleep that no wake reaches.
4. `clear_tid` not futex-woken at exit → **step 5**'s join hangs and the
   test's bound fails it.
5. `SYS_thread_exit` calling `process_exit` → **step 6**'s process dies
   when a worker finishes.
6. `SYS_thread_exit` removed from `native_always_allowed` → **step 10**'s
   filtered thread cannot exit.
7. The bound not checked → **step 8** runs past `PROCESS_MAX_THREADS` or
   faults.
8. The tid taken from a per-personality counter rather than shared → a
   native `thread_self` and the Linux view disagree, which **step 1** sees.

## Benchmarks

Thread creation and a futex round trip, measured in the test itself and
reported in the log rather than asserted: a create/join pair, and a
wake→wait handoff, each over enough iterations to be worth reading. The
suite's own timing is the guard against a regression, as in the last three
units. There is no throughput claim to make — the unit adds a capability
rather than changing a path — and the honest comparison, a native program
against the same program built for the Linux personality, is worth printing
once for the record.

## Risks

- **A syscall's shape is permanent.** Five of them at once is the largest
  interface addition since the netctl channel, and unlike a device ioctl
  there is no version field to grow. The mitigation is that four are
  one-argument or two-argument calls over existing functions, and the fifth
  takes `cosmo_spawn`'s extensible-struct pattern.
- **Per-thread state that is not yet per-thread.** The table above is the
  whole of it, and anything found missing while building — a per-thread
  field that should be shared, or the reverse — is a correctness bug in the
  compat layer too, since Linux threads already share exactly this. Finding
  one would be a result, not a setback.
- **Signals to a threaded native program are underspecified by design.**
  Named above, deferred, and worth a warning in the docs rather than a
  half-built targeting rule.
- **A thread's stack is the caller's problem**, including its guard page.
  The library should allocate with a guard; the kernel will not, and a
  program that overflows a thread stack corrupts whatever is below it —
  the same deal a program gets for its own stack, and worth stating.
- **`PROCESS_MAX_THREADS` is per process, not per system.** 256 threads ×
  `NPROC` processes is a large number of kernel stacks. It is the bound
  that exists today for Linux binaries, so this unit does not change the
  exposure, but it does make it reachable by more programs.

## Alternatives considered

- **Expose `clone` natively.** Rejected: its flag matrix is a Linux
  interface, most of it meaningless here (`-ENOSYS` for a fork-like clone
  today), and copying the caller's registers is the wrong start for a
  native thread, which begins at a function.
- **A `SYS_thread_join`.** Rejected above: it needs kernel state for
  unreaped threads and buys nothing the `clear_tid` word does not, while
  the word also answers "has it finished?" without a syscall.
- **Processes and shared memory instead of threads.** The Unix-philosophy
  answer, and the right one for most of this system's programs — which is
  why nothing in this unit discourages it. It is not the answer for a
  hypervisor monitor whose vCPUs must share the guest's address space and
  its own bookkeeping, which is the consumer that motivates the unit.
- **Convert `vmctl` to a thread per vCPU in this unit.** Tempting, since
  it is the motivating case and would retire the fairness machinery. Kept
  out: it changes the hypervisor's userland contract (a vCPU's thread
  affinity, what `SYSTEM_OFF` means when siblings are running on other
  CPUs, how `--machine` reports a fault on one vCPU) and deserves its own
  report and its own proofs. The primitive lands first, with a test of its
  own; the conversion is the named next unit, and the fairness rule stays
  until it is built and proved.
- **A green-threads library in libc.** No kernel change, no second CPU.
  It is what a program can already build over `SYS_aio_*` and
  `SYS_ioready`, and the gap this unit closes is precisely the one it
  cannot: a second CPU.

Named and deferred: futex requeue; per-thread signal targeting
(`tgkill`-shaped); a thread's name and priority in `cosmo_thread`;
`COSMO_RLIMIT_NTHREAD`; `/proc` per-thread entries; and the `vmctl`
conversion, which is the first consumer.
