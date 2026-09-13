# NEXT SUBSYSTEM — the wait, written once

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
**nothing in it is implemented**.

**Subsystem: a condition variable.** The library gives a thread a way to
*start*, to *end*, to *join* and to *exclude* — and no way to **wait for
something another thread will do**. Every program that needs one writes
it by hand, out of the raw futex or out of a yield loop, and there are
five such hand-rolls in the tree already. This unit writes it once, in
the library, and converts **four** of the five. The fifth — `vmctl`'s
park/run handshake — is examined last and converted **only if it comes
out simpler**, because it is concurrent code a review has already
corrected twice; the scope is stated the same way in the affected-files
table and in the migration plan, and "four, with the fifth conditional"
is the commitment.

## Problem

`cosmo/thread.h` is four functions and a mutex:

```c
int  cosmo_thread_start(cosmo_thread_t *, void *(*)(void *), void *, size_t);
int  cosmo_thread_join(cosmo_thread_t *, void **);
void cosmo_thread_finish(void *);
cosmo_tid_t cosmo_thread_id(void);
void cosmo_mutex_lock(cosmo_mutex_t *);
void cosmo_mutex_unlock(cosmo_mutex_t *);
int  cosmo_mutex_trylock(cosmo_mutex_t *);
```

A mutex answers "not at the same time as you". It does not answer "not
until you have done the thing", and that second question is most of what
concurrent code asks. The one program in this system that is genuinely
threaded — `vmctl`, with a thread per vCPU since
`docs/audit/next-subsystem-vcpu-threads.md` — asks it three times, and
each time reaches **past** the library to `SYS_futex_wait` directly.

That is the tell. A program reaching past a library for a primitive the
library was built to provide means the library stopped one function short.

## Current implementation

**The pieces are all there; only the composition is missing.** The futex
landed in `docs/audit/next-subsystem-threads.md`:

```c
SYS_futex_wait  83  /* (uint32_t *word, uint32_t val, uint64_t timeout_ns) -> 0 while *word == val */
SYS_futex_wake  84  /* (uint32_t *word, unsigned n) -> threads woken */
```

`timeout_ns == 0` means *no timer*, so an untimed wait is the same call.
`futex_wake`'s `n` is unbounded in `kernel/ipc/futex.c`, so `UINT_MAX` is
a broadcast. The mutex above them (`libc/src/thread.c:192`) is the
standard three-state one, and its comment records that it **already had a
lost-wakeup bug** — it took the lock with 1 instead of 2 on the contended
path, stranding a sleeper whenever three threads contended, and a review
found it. That bug is the exact class this report is about, and the tree
is now hand-rolling the same class at five more sites.

### The five hand-rolls

| where | what it is | how it waits |
| --- | --- | --- |
| `userland/system/vmctl.c:705` | a `CPU_ON` waiting for its target's first run to *return* | `futex_wait(&m->ran[c], 0, deadline - now)` in a loop with its own deadline arithmetic and a `stopping` re-check |
| `userland/system/vmctl.c:674, 727` | `machine_release` / `machine_quit_all` waking a parked vCPU thread | `futex_wake(&m->park[c], 1)` beside a hand-written state machine |
| `userland/system/vmctl.c:1256` | the supervisor's shutdown wait for `m.live` to reach zero | `futex_wait(&m.live, live, MACHINE_DRAIN_INTERVAL_NS)`, re-armed each pass |
| `userland/tests/thrtest.c:104, 565` | step 3, each side waiting for the other's flag | `for (i = 0; i < 2000000 && !flag; i++) if ((i & 0xfff) == 0xfff) cosmo_yield();` |
| `userland/tests/thrtest.c:487` | a worker waiting to be told to stop | `while (!worker_stop) cosmo_yield();` |

Two of these are worth reading closely, because they are *not* sloppy —
they are careful code that a missing primitive made long.

`vmctl`'s `CPU_ON` wait carries a comment explaining why it re-checks
`stopping` before sleeping, why the answer stays `SUCCESS` when the
machine is powering off, and why the wake and the QUIT are both needed
and neither is sufficient. Every one of those sentences is about the
*mechanics of waiting*, not about PSCI. A condition variable would leave
only the PSCI sentence.

`thrtest`'s step 3 bounds its wait with **a loop count standing in for a
duration** — 2,000,000 iterations, yielding every 4,096. This tree has
been bitten by that substitution before: `docs/audit/next-subsystem-threads.md`
replaced exactly such a bound after review ("a yield-count bound raced the
retry budget"), and `timer-test-smp1-flake` is a family of flakes with the
same root. The count is here because there was nothing better to reach
for.

### What the yield loops cost

`cosmo_yield()` in a wait loop is not merely inelegant. The terminal-modes
unit established the rule the hard way: **the thing being waited for needs
the CPU the polling loop is spinning on.** On a single-CPU boot — which is
how several self-tests run — a spinning waiter and its waker are the same
CPU's time, and the wait is paid for out of the progress it is waiting
for. `thrtest.c:487`'s `while (!worker_stop) cosmo_yield();` is unbounded,
so it spins for the whole life of the step.

### What already exists elsewhere, and why it does not cover this

- **The kernel** has waitqueues of its own; they are the kernel's, behind
  `SYS_*`, and not reachable from a program.
- **The Linux door** has `FUTEX_REQUEUE` and `FUTEX_CMP_REQUEUE`
  (`compat/linux/syscalls.c:1320`) because glibc's `pthread_cond_broadcast`
  uses them. A Linux binary therefore already gets a working condition
  variable **from its own libc**. A native program gets nothing. The two
  doors are unequal in the one place a threaded program notices first.

## Why it matters

1. **Every hand-rolled wait is a lost-wakeup bug waiting to be written.**
   The pattern has exactly one hard part: you must publish the thing you
   are waiting for, and check it, in an order that cannot let a wake fall
   between the check and the sleep. The futex is designed so this is
   *possible* — the compare-and-sleep is atomic against `wake_seq` — but
   the caller must still get the order right at every site. The mutex got
   it wrong once, in the library, written by someone thinking about
   nothing else. Five copies at five call sites is five chances.

2. **It is the missing fourth of a four-part arc that is otherwise done.**
   Native threads and a futex (#116), a thread pointer and `errno` per
   thread (#120), a thread per vCPU (#122), `__thread` and the TLS image
   (#124). A program can now have threads, per-thread `errno`, per-thread
   variables and a mutex — and still cannot wait for a queue to become
   non-empty without writing a futex protocol.

3. **It makes the tests that exist honest.** Two of the five hand-rolls
   are in `thrtest`, the suite that certifies the threading story. A test
   that waits with a loop count is measuring the host's speed as much as
   the property; that is the `settle()`-then-count family, and this is the
   primitive that retires it.

4. **The native door should not be the poor relation of the Linux door.**
   A Linux guest binary gets `pthread_cond_wait`. A native CosmoOS program
   gets a `while` loop. Whatever the native ABI is for, it is not for
   being the worse of the two.

## Design

### One structure, one word

```c
/* libc/include/cosmo/thread.h */
typedef struct { unsigned seq; } cosmo_cond_t;
#define COSMO_COND_INIT { 0 }
```

A sequence number and nothing else. No waiter count, no associated-mutex
field, no allocation, no destroy — the same shape as `cosmo_mutex_t`,
which is one word and a static initialiser, and for the same reason: a
primitive that cannot fail to be created cannot fail to be created *at
startup*, which is where these live.

### The protocol

```c
void cosmo_cond_wait(cosmo_cond_t *c, cosmo_mutex_t *m);
int  cosmo_cond_timedwait(cosmo_cond_t *c, cosmo_mutex_t *m, uint64_t timeout_ns);
void cosmo_cond_signal(cosmo_cond_t *c);
void cosmo_cond_broadcast(cosmo_cond_t *c);
```

```
wait(c, m):                       signal(c):
    seq = load(c->seq)                fetch_add(c->seq, 1)
    unlock(m)                         futex_wake(&c->seq, 1)
    futex_wait(&c->seq, seq, t)
    lock(m)                       broadcast(c):
                                      fetch_add(c->seq, 1)
                                      futex_wake(&c->seq, UINT_MAX)
```

**Why this cannot lose a wakeup, stated as the argument and not as a
claim.** `seq` is read while the caller still holds `m`. A signaller must
hold `m` to change the predicate the waiter is waiting on — that is the
contract, and it is the same contract every condition variable has. So
any signal that happens after the waiter's read of `seq` happens after the
`unlock`, and it *increments* `seq`. `futex_wait` compares the word to the
value it was given and returns `-EAGAIN` without sleeping if it differs.
Therefore a signal that lands in the window between the read and the sleep
does not vanish: it is the reason the sleep does not happen.

The remaining window — between `unlock(m)` and `futex_wait` — is the one
the futex's own `wake_seq` covers, inside `kernel/ipc/futex.c`: a waiter
that is between its compare and its enqueue observes the bump and retries.
That mechanism is already built and already tested; this design depends on
it rather than reinventing it.

### Spurious wakeups are permitted, and that is load-bearing

`cosmo_cond_wait` may return without any signal having occurred. **The
caller must loop on its predicate**, always:

```c
cosmo_mutex_lock(&m);
while (!ready)
    cosmo_cond_wait(&c, &m);
cosmo_mutex_unlock(&m);
```

This is not an apology for the implementation; it is the interface. It is
what lets `seq` be one word with no waiter bookkeeping, and it is what
every other condition variable in the world specifies, so a reader who
knows one knows this one. The header will say it in those words, because
a caller who writes `if` instead of `while` has written a bug that will
survive every test on an unloaded machine.

### What `timedwait` returns

`0` if it was woken or woke spuriously; `-ETIMEDOUT` if the timeout
expired. **Not** "the predicate is true" — this function does not know the
predicate. A caller whose loop must give up computes its own deadline and
re-checks, which is the same shape `vmctl` already writes by hand and will
keep, minus the futex.

`timeout_ns` is relative nanoseconds, matching `cosmo_futex_wait`, and the
kernel's existing bounds apply unchanged (a timeout with the top bit set
is `-EINVAL`; `0` means no timer, so `cosmo_cond_wait` is
`cosmo_cond_timedwait(c, m, 0)` and is implemented as exactly that).

### What this unit does *not* add

Named and deferred, so the scope is a decision and not an accident:
`cosmo_once`, a reader/writer lock, a barrier, and `SYS_futex_requeue` on
the native door. The first three are each a unit's worth of their own
argument; the fourth is a *performance* change to broadcast and is in
Benchmarks below, where it can be justified by a number instead of by
taste.

### The §70 gate

**Correctness.** The argument above is the whole of it: `seq` read under
the mutex, bumped by every signaller holding the mutex, compared by the
kernel atomically against its own `wake_seq`. Internally consistent
because it adds no new synchronisation — it composes two primitives whose
own proofs are already written and tested.

**Concurrency.** Any number of waiters and signallers. The structure is
touched only by atomic read-modify-write and by the futex syscalls; there
is no critical section of its own, which is why it needs no lock of its
own.

**Ownership.** The caller owns the `cosmo_cond_t`, exactly as it owns its
mutex. There is no table, no handle and no kernel object: the kernel knows
only the address, per address space, for as long as a thread is inside
`futex_wait` on it.

**Lifetime.** A `cosmo_cond_t` may be destroyed when no thread is waiting
on it, which is the caller's knowledge and not the library's — the same
rule as the mutex, stated in the header. There is nothing to free.

**Failure.** Nothing here allocates, so nothing here fails for want of
memory. `futex_wait` can return `-EFAULT` or `-EINVAL` only for an address
or a timeout the *caller* got wrong, and those are programming errors on a
structure the caller owns; `cosmo_cond_wait` returns `void` for that
reason, and `cosmo_cond_timedwait` returns only `0` or `-ETIMEDOUT`. An
unexpected futex error is treated as a spurious wakeup, which is safe
precisely because the caller loops.

**Security.** No new syscall and no new kernel state, so no new trust
boundary. The futex's existing rules — a 4-byte-aligned address inside the
caller's own space, and buckets keyed by `struct vm_space` so two
processes cannot reach each other's — are unchanged and are what confine
this.

The test seam is the one thing here that deserves a sentence rather than
a dismissal: `__cosmo_cond_probe` is a **writable function pointer in
every process's data**, and a writable function pointer is a control-flow
target. The honest weighing is that an attacker who can write it can
already write anything else in the process — there is no privilege
boundary inside a process, and libc has other indirect calls — so it
widens no boundary that was not already open. It is called on a path
that is about to enter the kernel anyway, and it is the difference
between a tested lost-wakeup guarantee and an untested one. If that
trade is judged wrong, the fallback in Tests case 2 is the answer, and
it costs the test rather than the security.

**Performance.** The uncontended signal with no waiter is one atomic
increment and one syscall that finds an empty bucket. Making *that* free
(by keeping a waiter count and skipping the syscall) is a real
optimisation and is deliberately not in the first version: see Benchmarks.

**Scalability.** Broadcast wakes every waiter, which then contend for the
mutex — the thundering herd `FUTEX_REQUEUE` exists to avoid. With the
waiter counts this tree has (two to eight), the herd is cheaper than the
second syscall that would avoid it. This is a number, not an opinion, and
Benchmarks says which number would change the decision.

## Affected files

| file | change |
| --- | --- |
| `libc/include/cosmo/thread.h` | `cosmo_cond_t`, `COSMO_COND_INIT`, the four functions, and the `while`-not-`if` contract stated where a caller will read it |
| `libc/src/thread.c` | the four functions, beside the mutex they compose with; and `__cosmo_cond_probe`, the NULL-by-default test seam at the sleep window (Tests, case 2) |
| `libc/src/libc.h` | the probe's declaration, since it is libc's and not a program's |
| `userland/system/vmctl.c` | the `CPU_ON` wait and the supervisor drain become `cosmo_cond_*`. **The park/run state machine is conditional**: it keeps its states either way, and loses its futex calls only if step 5 finds the result simpler — see the migration plan, which is the one place this is decided |
| `userland/tests/thrtest.c` | the two yield-spin waits become condition waits; **new steps** for the primitive itself |
| `docs/libc/invariants.md` | L8's neighbourhood: what a threaded program may now wait on, and the `while` contract as an invariant |
| `docs/libc/architecture.md` | the `cosmo/thread.h` row gains the condition variable |
| `docs/audit/next-subsystem-threads.md` | its "named and deferred" list is stale in three ways this unit can fix while it is here: **a per-thread `errno`** and **the `vmctl` conversion** are built, and **futex requeue** is taken up and re-deferred with a measurement below. It does *not* defer a condition variable -- I assumed it did and checked; the list names requeue, not this |
| `README.md` | Status entry |

**No kernel file changes.** `SYS_COUNT` stays 89, no structure grows, and
`kernel/ipc/futex.c` is used exactly as it already is — `futex_wake` with
`n = UINT_MAX` is already legal and already does the right thing. A unit
that adds a userland primitive and touches no kernel file is the shape
this one should be, and if the implementation finds itself editing
`kernel/` that is a signal the design was wrong.

## New APIs

Four functions, one typedef, one macro, in a header that already exists.
**No new syscall.** No new structure crossing the kernel boundary, and
therefore no versioning question of the kind `struct cosmo_procinfo` raised
two units ago.

One internal symbol that is not an API: `__cosmo_cond_probe`, the test
seam. It is declared in `libc/src/libc.h` rather than a public header
because no program may set it, it is `__`-prefixed, and it is documented
as libc's. It is nonetheless **in the shipped binary**, which is
deliberate — a seam compiled out of production proves things about a
binary nobody runs — and the cost of that is priced in Risks.

## Migration plan

1. **The primitive and its tests.** `cosmo_cond_t` and the four functions,
   with `thrtest` steps that exercise them directly: a signal seen, a
   broadcast seen by every waiter, a timed wait that times out, a timed
   wait that does not, and a signal delivered *before* the wait begins
   (which must not be lost, and is the lost-wakeup case stated as a test).
2. **`thrtest`'s own two waits converted**, which is the smallest real
   caller and removes a loop-count bound this tree has already been bitten
   by. Step 3's 2,000,000-iteration bound goes.
3. **`vmctl`'s supervisor wait**, the simplest of its three: wait for
   `live` to reach zero.
4. **`vmctl`'s `CPU_ON` wait**, the one with the deadline and the
   `stopping` re-check — the interesting one, and the one whose comment
   should shrink to the PSCI sentence.
5. **`vmctl`'s park/run handshake**, last because it is a state machine
   the vCPU-threads unit reasoned about carefully and a review corrected
   twice; converting it is a rewrite of working concurrent code and earns
   its own step, with the guest tests (`guest_offspin`, `guest_psci_race`)
   as the regression.
6. **The documents**, including the threads report's deferral list.

Steps 3–5 are separable and each is independently revertible. **Steps 1–4
are the unit**; step 5 is a judgement made with the code in front of us,
and the banner and the affected-files table say so too rather than
promising all five. **A conversion that makes the code longer is a
conversion that should not happen**, and this plan expects to be told so
at step 5 rather than to discover it after.

If step 5 does not land, the report is converted as-built to say the
park/run handshake keeps its futex calls **and why** — a hand-rolled wait
that survived review twice and reads better than its replacement is a
finding worth recording, not an embarrassment to bury.

## Tests

In `thrtest`, which is where the threading story is certified. The thread
bound step must stay last among thread-creating steps, so these are
inserted before it.

1. **A signal is seen, and the mutex comes back.** One waiter on a
   predicate, one signaller that sets it under the mutex and signals; the
   waiter returns with the predicate true.

   **The predicate is not the assertion that matters here**, because the
   signaller made it true before the wait returned — an implementation
   that forgot to re-acquire the mutex would pass on the predicate alone.
   So the waiter, immediately on return, calls
   `cosmo_mutex_trylock(&m)` and requires **`-EBUSY`**: the mutex is not
   recursive, so a thread that holds it cannot take it again, and a
   thread that does *not* hold it takes it successfully. One call, one
   value, no timing — it is exactly the ownership assertion the
   re-acquisition contract needs, and without it this test is vacuous
   for half of what it claims.

2. **A signal delivered inside the sleep window is not lost.** This is
   the lost-wakeup case, it is the reason the unit exists, and **two
   drafts of this test failed to test it.** The third takes a design
   decision rather than a cleverer arrangement of threads.

   The window is a few instructions wide: between the waiter's read of
   `seq` and its `futex_wait`. Nothing a *second thread* can do reaches
   inside it. The second draft tried — a signaller blocked on the mutex,
   released by the `unlock` inside `cosmo_cond_wait` — and a review
   dismantled it: unlocking makes that thread **runnable, not running**.
   The waiter is not preempted and carries on into `futex_wait` before
   the signaller is scheduled, so the signal lands on a sleeper, the wake
   works, and a read-after-unlock implementation passes. Repeating the
   same scheduling sequence several hundred times repeats the same
   outcome; it is not a probability that improves with attempts.

   **So the library provides the seam.** A single function pointer,
   NULL in every real program, called at the window:

   ```c
   /* libc/src/thread.c -- test seam, NULL except under thrtest */
   void (*__cosmo_cond_probe)(void);
   ...
       unsigned seq = __atomic_load_n(&c->seq, __ATOMIC_RELAXED);
       cosmo_mutex_unlock(m);
       if (__cosmo_cond_probe) __cosmo_cond_probe();   /* <- the window */
       cosmo_futex_wait(&c->seq, seq, timeout_ns);
   ```

   The test sets the probe to a function that performs the whole signal
   synchronously — take the mutex, set the predicate, `cosmo_cond_signal`,
   release — so the signal happens **inside the window, on the waiter's
   own thread**, with no scheduler involved. A correct implementation read
   `seq` before the unlock, so the probe's increment makes `futex_wait`
   return `-EAGAIN` and the `while` sees the predicate. An implementation
   that reads `seq` after the unlock reads it *after* the probe has run,
   sleeps on the current value, and hangs. **Deterministic in both
   directions, on one CPU, with no timing.**

   **What it costs, stated rather than waved past.** One load and one
   predictable not-taken branch, immediately before a syscall — and the
   pointer is compiled in unconditionally, *not* behind `#ifdef`, so the
   code path the tests exercise is the code path that ships. A seam that
   exists only in a test build proves things about a binary nobody runs.
   The symbol is `__`-prefixed and documented as libc's, not a program's.

   If review prefers no seam in the library at all, the fallback is
   explicit and worse: this property becomes **correct by construction,
   proved only by the widened-window mutation** below, and the test
   section says so in the shape `tests/hv/aarch64/guest_psci_race.S`
   already uses — a "what this does not prove" paragraph naming the gap
   rather than a test that quietly does not cover it. That is a real
   option and it is how this tree has handled an unreachable window
   before; the seam is proposed because this window *can* be reached, and
   cheaply.

3. **A broadcast reaches every waiter.** Four waiters, one broadcast, all
   four return; a `signal` in the same position releases exactly one, which
   is what distinguishes the two calls.
4. **A timed wait times out**, returning `-ETIMEDOUT` after at least the
   requested interval — bounded below by the clock, not by a loop count.
5. **A timed wait that is signalled returns 0** before its deadline.
6. **Spurious wakeups do not break a correct caller**: a test that
   broadcasts repeatedly at a waiter whose predicate stays false, and
   asserts the waiter is still waiting and the predicate still false —
   i.e. that the `while` contract is what makes the caller correct.
7. **`vmctl` keeps its guests**: `guest_offspin` and `guest_psci_race`
   unchanged and still passing, which is the regression for steps 3–5.

**Bug-proofs**, one per property, each expected to fail *for its own
stated reason*:

- `seq` read **after** the unlock instead of before → test 2 hangs on its
  first iteration, because the probe runs between the unlock and the read
  and the waiter then sleeps on the value the probe already published. No
  sleep, no repetition and no second CPU are needed: the seam makes this
  the ordinary execution, which is the whole reason for it.
- `signal` not incrementing `seq`, only waking → test 2 hangs the same
  way (the waiter sleeps on a value nothing changes), while test 1 still
  passes — which is what makes the two tests different rather than
  redundant.
- **the probe left NULL by the test** → test 2 passes against *both* the
  correct and the broken implementation, which is the proof that the
  seam is what carries this test and not an ornament on it.
- `broadcast` waking 1 instead of `UINT_MAX` → test 3 fails with three
  waiters still blocked.
- `timedwait` returning 0 on timeout → test 4 fails.
- **the mutex not re-acquired before returning** → test 1's
  `cosmo_mutex_trylock` returns 0 instead of `-EBUSY`. Deterministic, and
  the reason that assertion exists: the predicate alone cannot fail for
  this bug, since the signaller already made it true.

**On vacuity**, because this tree has been caught by it: a wait test that
passes when the wait does nothing is the default failure mode here, since
a waiter that never sleeps and a waiter that is correctly woken both
finish. Test 2 is the one that distinguishes them, and its bug-proof must
show the *timeout*, not a wrong value. Each proof restores the source
byte-identically, verified with `cmp`.

## Benchmarks

Not "is a condvar fast" — it is two atomics and a syscall, and there is
nothing to tune in the first version. The measurements that matter are the
two that decide the deferred work:

1. **Broadcast cost against waiter count** (2, 4, 8, 16 waiters): time
   from `broadcast` to the last waiter holding the mutex. If this grows
   worse than linearly at 8 — the tree's largest real count, `TAP_MAX_GUESTS` in `kernel-services/network/tap.c:147`
   — then `SYS_futex_requeue` on the native door is justified, and the
   kernel code for it already exists and is exercised through the Linux
   door. If it does not, requeue stays deferred and this report says why
   with a number.
2. **The uncontended signal** (no waiter): whether the syscall is worth
   skipping with a waiter count. The number to beat is one `futex_wake`
   into an empty bucket; if that is already cheap relative to the mutex
   operations around it, the waiter count is complexity for nothing.

Both are `thrtest`-shaped and neither needs new scaffolding.

## Risks

- **Converting `vmctl`'s park/run handshake is a rewrite of working
  concurrent code**, and that code was corrected twice by review during
  the vCPU-threads unit (a `CPU_ON` that wrote registers before claiming;
  a `CPU_OFF` that left the vCPU unstartable). The mitigation is that it
  is the *last* step, separable, and guarded by two guest tests whose
  failure mode is a hang the boot deadline catches. If it does not come
  out simpler, it should not land.
- **A caller writing `if` instead of `while`** is the classic misuse, and
  it passes every test on an unloaded machine. The mitigation is the
  header saying so at the point of use, an invariant in
  `docs/libc/invariants.md`, and test 6 existing specifically to make the
  contract a tested property rather than a comment.
- **The one-word design cannot support `pthread_cond_t`'s full semantics**
  if a POSIX layer is ever wanted — in particular a clock attribute and
  the requeue optimisation. That is a real constraint and the reason to
  state it now: this is `cosmo_cond_t`, the native primitive, and a POSIX
  layer would sit on top of it or beside it rather than being retrofitted
  into it.
- **The test seam is a writable function pointer in libc.** Named in the
  §70 gate above with the argument for it; the counter-argument is that a
  process that ships an indirect call nobody needs has shipped a gadget,
  and "an attacker could already do worse" is the reasoning that
  accumulates them one at a time. The mitigation if that view wins is
  stated where the decision is — Tests case 2 — and is a weaker test
  rather than a hidden one.
- **Timeouts are relative, and a waiter that loops re-computes its
  deadline.** A caller that passes the same relative timeout each time
  round the loop waits longer than it meant to. This is inherent to a
  relative-timeout interface, it is the same shape `cosmo_futex_wait`
  already has, and the header must show the deadline pattern rather than
  leave each caller to rediscover it.

## Alternatives considered

- **Leave it, and let each program hand-roll.** What the tree does today.
  It is defensible exactly until the second program does it, and `vmctl`
  plus `thrtest` are already the second. The mutex's own lost-wakeup bug
  is the argument against: this is not code that is easy to get right in
  passing.
- **Expose `SYS_futex_requeue` natively and build the condvar the way
  glibc does.** The kernel function exists and is tested. Rejected for the
  *first* version because it adds a syscall to the native ABI to optimise
  a case (broadcast to many waiters) that this tree does not yet have, and
  because a correct wake-all condvar is a strictly simpler thing to prove.
  Named in Benchmarks with the measurement that would reverse the decision.
- **A waiter count in the structure, to skip the syscall when nobody
  waits.** A real optimisation and a second field, and the field brings the
  question of when it may be read without the mutex. Deferred to a number
  rather than settled by taste.
- **Build it on the mutex's own futex word instead of a separate `seq`.**
  Smaller, and wrong: a waiter sleeping on the mutex's word cannot be
  distinguished from a waiter blocked *acquiring* the mutex, so an unlock
  would wake condition waiters and a signal would wake lock waiters. The
  two waits are different queues and need different words.
- **A channel or a queue instead** — a higher-level primitive that carries
  values. A better fit for some callers and a worse fit for `vmctl`, whose
  waits are on state it already keeps rather than on messages. A channel
  is naturally *built on* a condition variable, so this is the lower layer
  either way.

---

Named and deferred by this unit: `cosmo_once`, a reader/writer lock, a
barrier, `SYS_futex_requeue` on the native door, a waiter count, and a
POSIX `pthread_cond_*` layer.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01FtzXcfogMnEqCnyAVzZYFj
