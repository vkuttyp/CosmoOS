# NEXT SUBSYSTEM — the two thread calls the native door still lacks

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it.

**README.md:1485-1486 names three things native threads were left
without: `SYS_mprotect`, futex requeue and per-thread signal
targeting.** The first shipped as the previous unit. The other two are
the same shape as it was — **built, tested, and reachable only
through the Linux door** — and `docs/kernel/process/design.md:1135-1136`
lists them as the native thread ABI's remaining items in so many
words: *"requeue (the Linux door already exposes it); per-thread
signal targeting (a native `tgkill`)"*.

**Takes up** the inventory's §1.2 entries **"native futex requeue"**
and **"per-thread signal targeting"** (both README.md:1486), as one
unit, because they are one gap: the native thread ABI stopped two
calls short of the personality that sits beside it.

## What is established

**`futex_requeue` exists and is careful** (`kernel/ipc/futex.c:159`).
It takes two words, wakes up to `nr_wake` waiters on the first and
moves up to `nr_requeue` more onto the second without waking them;
with `cmp` it first checks the first word against `cmpval` and
returns `-EAGAIN` if it has moved. The compare cannot run under the
bucket spinlock (the user copy may fault), so it notes the bucket's
`queue_seq`, compares unlocked, takes both bucket locks — lower
address first, always, the second annotated as nested for lockdep —
and acts only if nothing touched the bucket in between, else
compares again. That is the same shape as `futex_wait`'s
compare-then-enqueue, and it is what makes the compare atomic with
respect to the other operations, as on Linux.

**The Linux door reaches it** (`compat/linux/syscalls.c:1466-1478`):
`LX_FUTEX_REQUEUE` and `LX_FUTEX_CMP_REQUEUE` both call it and both
report woken + requeued, as the Linux kernel does.

**The native door has wait and wake only.** `SYS_futex_wait` (83) and
`SYS_futex_wake` (84) — `sys_futex_wait` and `sys_futex_wake` at
`kernel/syscall/native.c:187` and `:203` — and nothing else.

**libc's broadcast is the herd, and says so.** `cosmo_cond_broadcast`
(`libc/src/thread.c:308-318`) bumps `seq` and calls
`cosmo_futex_wake(&c->seq, ~0u)`: every waiter wakes, every one
contends for the mutex, all but one go straight back to sleep on it.
The comment beside it: *"the herd `FUTEX_REQUEUE` exists to avoid …
whether the second syscall a requeue would cost is worth saving is a
measurement the report defers rather than a guess made here."* This
is that report, and it does not get to defer the measurement again.

**`signal_send_thread` exists** (`kernel/process/signal.c:373`) and
queues a signal on one thread rather than a process; `lx_tgkill`
(`compat/linux/syscalls.c:2119`) and the fault path use it. Threads
have a per-thread `sig_blocked` (`kernel/include/kernel/thread.h:85`,
inherited at creation "as on Linux"), so masking is already per
thread. The native `sys_kill` (`native.c:1282`) takes a pid and routes
to the process. A native thread knows its own id —
`cosmo_thread_id()` (`libc/include/cosmo/thread.h:96`) — and a
creator knows the ids of the threads it made (`cosmo_thread_t::tid`),
so the addresses exist; only the call does not.

**The `mutex` is a three-state futex** (`libc/src/thread.c`): 0 free,
1 held with no waiter recorded, 2 held with a waiter possible. A
contended `lock` exchanges 2 in and waits on the word with expected
value 2; `unlock` decrements, and **wakes one waiter only if it found
2** (`thread.c:232-238`). This is the fact that decides how requeue
can be used, below.

**The fuzzer covers none of the thread calls.** `allowed[]`
(`userland/init/init.c:3983`) lists neither `SYS_futex_wait`,
`SYS_futex_wake` nor `SYS_thread_create`. That predates this unit and
is named here; the two new calls go in with constraints, and the
three old ones are an open question this report puts to the
implementer rather than answers.

## The problem

### The same door, still half open

The previous unit's argument applies unchanged and is not repeated at
length: a program compiled against Linux headers can requeue waiters
and signal one thread of itself, and the same program compiled
natively cannot. Every native library that wants a condition variable
without a thundering herd, or a way to interrupt one thread's blocking
call, has to be told the kernel cannot do it — when it can.

### The herd is real and measurable, and nobody has measured it

With *N* waiters on a condition and one broadcast, the native
`cosmo_cond_broadcast` produces *N* wakeups, *N* attempts on the
mutex, and *N − 1* immediate re-sleeps on it: *2N − 1* futex
transitions and *N − 1* wasted context switches, every time, scaling
with the number of waiters. Requeue makes it one wakeup and *N − 1*
silent moves, then one wake per unlock as the mutex hands over.

The libc comment defers the question of whether the second syscall
(the requeue itself, on the broadcaster's side) is worth it. It is
worth it exactly when *N* is more than one, which is the only case a
broadcast exists for; but that is an argument, and the unit will
carry the number instead. See Tests.

### Requeue is not a drop-in, because of what `unlock` believes

A waiter moved from `c->seq` onto `m->state` sleeps on the mutex's
word — but `cosmo_mutex_unlock` only issues a wake **if it found the
state at 2**, and `cosmo_mutex_lock`'s fast path takes a free mutex
with **1**. Three things follow, and each is a hang, not an error:

1. **The broadcaster's own unlock.** Holding the mutex at state 1
   (no other waiter recorded), it requeues *N − 1* threads onto a
   word whose unlock wakes nobody. They sleep for ever.
2. **The handoff.** Say the word *was* 2, so the unlock wakes one. That
   waiter returns from its `cosmo_futex_wait` inside
   `cosmo_cond_timedwait` and calls `cosmo_mutex_lock` from the top:
   the mutex is free, `cas(0, 1)` succeeds, it holds at **1**. Its
   unlock finds 1 and wakes nobody. With three or more waiters the
   chain breaks after the first link and the rest sleep for ever — the
   same strand `cosmo_mutex_lock` already fixed for its own contended
   path, arriving again through a different door.
3. **The broadcaster that does not hold the mutex.** POSIX permits it.
   `cosmo_mutex_t` is one word with no owner, so libc cannot tell
   "held by me" from "held by someone else", and a mark made before
   the requeue can be undone by that someone's unlock before the
   requeue lands: the waiters are moved onto a free word and no unlock
   is coming.

And the fourth thing, which is not a hole but a fact:
`cosmo_cond_broadcast(cosmo_cond_t *)` **has no mutex to requeue
onto**. `cosmo_cond_t` is one word by design (`cosmo/thread.h`: "no
associated mutex"), so the target of the requeue has to come from
somewhere.

So the design has a layout change, three rules and one measurement,
not a one-line swap.

## Design

**Two syscalls, 94 and 95; `SYS_COUNT` 94 → 96.**

```c
#define SYS_futex_requeue 94  /* (uint32_t *w1, uint32_t *w2, unsigned nr_wake, unsigned nr_requeue, uint32_t val) -> woken + requeued */
#define SYS_thread_kill   95  /* (cosmo_tid_t tid, int sig) -> 0 */
```

**`SYS_futex_requeue` is the compare form only.** The native ABI is
new and need not carry Linux's history: the non-comparing
`FUTEX_REQUEUE` has the lost-wakeup race glibc abandoned it for, and
offering it would be offering a footgun for compatibility with nothing.
`val` is compared against `*w1` atomically with the bucket, and
`-EAGAIN` says "the word moved; decide again". Both words page-aligned
to 4 and inside the user window, as the wait and wake already
require; the count returned is woken + requeued, as the Linux door
reports. It is `futex_requeue(space, w1, w2, nr_wake, nr_requeue,
true, val)` and nothing else — the door translates.

**`cosmo_cond_t` records the mutex it is waited on with.**
`cosmo_cond_timedwait` stores `m` into a second word, `c->mutex`,
before it unlocks; `cosmo_cond_broadcast` reads it. The signature of
`cosmo_cond_broadcast` does not change — every existing caller keeps
compiling — and the cost is one word and one relaxed store on the
wait path. POSIX already makes waiting on one condition with two
different mutexes at once undefined, so the word has one value that
matters, and the header says so. `COSMO_COND_INIT` becomes `{ 0, 0 }`;
a static `cosmo_cond_t` is still a static `cosmo_cond_t`, and libc is
`libc.a`, so a layout change is a rebuild, not an ABI break. The
header comment that says "one word ... no associated mutex" is
rewritten, not left to contradict the struct beneath it. A `mutex` of
`NULL` means nobody has ever waited: there is nothing to requeue, and
the broadcast wakes all (of nobody) as today.

**`cosmo_cond_broadcast` requeues under three rules.** The mutex
protocol they serve is the one `cosmo_mutex_lock`'s own comment
states: *while a sleeper exists on the mutex word, a holder holds it
at 2*, because only an unlock that finds 2 wakes.

1. **A condition waiter relocks through the contended path.** After
   its `cosmo_futex_wait` returns — woken, requeued-and-woken, timed
   out or `-EAGAIN`, it cannot tell which — the waiter takes the mutex
   by **exchanging 2 in**, never by `cas(0, 1)`. This is the loop
   `cosmo_mutex_lock` already runs once it has seen contention,
   entered from the top: `cosmo_mutex_lock_contended(m)`, a static
   helper the two share. It is what makes the handoff a chain: each
   woken waiter holds at 2, so its unlock wakes the next. The cost is
   the one `cosmo_mutex_lock` already accepted for the same reason —
   one `futex_wake` with nobody there, for the last waiter in the
   chain. The bug-proof is the fast path put back: three waiters, one
   broadcast, a bounded join reports the two that never return.
2. **After the requeue, make the word reachable.** `seq++`, then
   `futex_requeue(&c->seq, &m->state, 1, ~0u, seq)` — wake one, move
   the rest — and *then* the broadcaster looks at `m->state` and acts
   on what it sees, in a loop until one of three things is true:
   it read **2** (a holder will wake on unlock; done); it read **1**
   and its `cas(1, 2)` succeeded (same); it read **0** and it woke one
   sleeper on `m->state` itself (that sleeper relocks at 2 by rule 1,
   and its unlock carries the chain). A CAS that fails because the
   word moved goes round again. This is done *after* the requeue
   rather than before because a mark made before can be undone by an
   unlock that lands in between; done after, every interleaving ends
   with either a holder at 2 or a wake already issued, and the
   ordinary mutex protocol takes it from there. It is rule 2 that
   makes rule 3 unnecessary for correctness.
3. **The broadcaster need not hold the mutex.** Rule 2 does not ask
   who holds it, only what the word says, so a broadcast without the
   mutex requeues like any other and the waiters drain one per
   unlock — which is still the serialised handoff the requeue exists
   for, since each waiter needs the mutex next anyway. There is no
   wake-all fallback in the design, because a fallback is a second
   path to keep correct and rule 2 already covers the case.

A requeued waiter returns from its `cosmo_futex_wait` on the cond word
only when the mutex's unlock (or rule 2's wake) reaches it, and takes
the mutex by rule 1. `cosmo_cond_signal` is unchanged: a single wake
is already optimal, and its waiter also relocks by rule 1, which is
the one place the rule costs a wake that today's code does not pay —
once per signalled wait, on an uncontended mutex. The measurement
below reports that too, so the trade is a number and not a claim.

**`SYS_thread_kill` is `tgkill` with the process implied.** The
target is a thread of the **calling process** — a tid from another
process is `-ESRCH`, never a cross-process delivery, because the
native `kill` is the process-scoped door and this one is not a second
way through it. `sig` 0 probes; `-EINVAL` for a bad signal, `-ESRCH`
for a tid that is not a live thread of this process. It is
`signal_send_thread(t, sig, &info)` after the lookup, with the sender
recorded as the native `kill` records it. Delivery honours the target
thread's own mask, which already exists per thread; the handler runs
on the targeted thread's frame, which is what `native_signal.c`
already does for faults.

**"Gone" is eventual, and the test knows it.** `lxtest` learned this
the hard way earlier this week (`docs/testing/flakes.md`,
"`lxtest`'s tgkill-after-join"): a joined thread still resolves by tid
until `thread_exit` has run, because the joiner is woken deliberately
early. A native test that asserts `-ESRCH` the instant `join` returns
would flake the same way. It waits for the condition.

**libc**: `cosmo_futex_requeue` in `cosmo/syscall.h`,
`cosmo_thread_kill(cosmo_tid_t, int)` in `cosmo/thread.h`.

**The fuzzer** gets both. `SYS_futex_requeue` with words on the
fuzzer's own scratch pages (a requeue between two words nobody waits
on is harmless); `SYS_thread_kill` with **signal 0 only** — a random
signal to a random tid of the fuzzer's own process is the fuzzer
killing itself, which is the `setrlimit` and `mprotect` lesson a
third time — and tids drawn from `cosmo_thread_id()`, garbage, and
the process's own pid, which is a valid tid for the first thread.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h` | `SYS_futex_requeue` 94, `SYS_thread_kill` 95, `SYS_COUNT` → 96 |
| `kernel/syscall/native.c` | `sys_futex_requeue`, `sys_thread_kill`, dispatch entries |
| `libc/include/cosmo/syscall.h` | the two raw wrappers |
| `libc/include/cosmo/thread.h` | `cosmo_thread_kill`; `cosmo_cond_t` gains `cosmo_mutex_t *mutex`, `COSMO_COND_INIT` becomes `{ 0, 0 }`, the "one word, no associated mutex" comment is rewritten; the contract of `cosmo_cond_broadcast` gains its three rules and the one-mutex-per-condition requirement |
| `libc/src/thread.c` | `cosmo_cond_timedwait` records the mutex and relocks through `cosmo_mutex_lock_contended`, split out of `cosmo_mutex_lock`; `cosmo_cond_broadcast` requeues under the three rules; the comment that deferred the measurement is replaced by the measurement |
| `userland/tests/thrtest.c`, `userland/tests/cwdtest.c` | the cases below, with the herd measured; both programs' static `COSMO_COND_INIT` conditions rebuild against the new layout unchanged |
| `userland/init/init.c` | both calls in `allowed[]`, constrained as above |
| `docs/kernel/syscall/api.md` | two table rows, and the count paragraph (94 → 96) |
| `docs/kernel/process/design.md` | line 1135-1136's "remaining items" loses two of three; per-thread targeting documented as native |
| `docs/kernel/process/invariants.md` | the tid-resolution invariant at 309-310 gains the native check beside `lxtest`'s |
| `docs/kernel/ipc/*` | requeue is reachable from both doors |
| `docs/libc/api.md`, `docs/libc/testing.md` | `cosmo_thread_kill`; the broadcast's rules and its measured cost |
| `docs/audit/2026-09-deferred-work-inventory.md` | strike both §1.2 entries |
| `README.md` | Status entry; line 1485-1486 says all three are done |

## Tests

**The measurement the libc comment owes** (`thrtest`): *N* = 8 waiters
on one condition, and a counter of how many times a thread **slept on
the mutex word** — calls to `cosmo_futex_wait(&m->state, …)` — from
the broadcast to the last waiter's return. (Not "took the contended
path": rule 1 sends every condition waiter through that path by
construction, so that count says nothing. A sleep is the herd; the
exchange that finds 0 is not.) One broadcast. Before this unit that
number is *N − 1* by construction — every waiter but the winner wakes
at once and sleeps again on the mutex. After it, the waiters are woken
one per `unlock` onto a mutex that unlock just freed, so the exchange
finds 0: the count is **0**, or a small number if the scheduler runs
a woken thread before the unlock that freed it. The test asserts it is
below *N − 1*, prints both figures together with the number of empty
wakes rule 1 issued, and the report's as-built carries all three. That
is the measurement, and the bug-proof is the old broadcast: put
`cosmo_futex_wake(~0u)` back and the sleep count returns to *N − 1*.

| case | what it establishes |
| --- | --- |
| broadcast, mutex held (state 1), three or more waiters | every waiter returns — rule 2 found 1 and marked 2, and rule 1 carried the chain. Two bug-proofs, each a bounded join reporting a hang: remove rule 2's loop (the requeued waiters are never woken); relock through `cosmo_mutex_lock`'s fast path instead of rule 1 (the first waiter returns, the rest never do) |
| broadcast, mutex held with a recorded waiter (state 2) | same, and rule 2 reads 2 and does nothing — no double-marking, no extra wake |
| broadcast, mutex **not** held | every waiter returns: rule 2 found 0 and issued the wake that starts the chain. Bug-proof: make rule 2 break on 0 without waking, and they hang |
| broadcast while **another** thread holds the mutex | the holder unlocks at an instrumented moment (the `__cosmo_cond_probe` hook, which exists for exactly this: between the requeue and rule 2's read); every waiter returns. This is the interleaving that a mark-before-requeue design loses, and the test is written from that mechanism, not from a stopwatch |
| broadcast on a condition nobody has waited on | `mutex` is `NULL`; returns, no requeue, no fault |
| `SYS_futex_requeue` with a stale `val` | `-EAGAIN`, nobody moved |
| a requeued waiter and a timeout | `cosmo_cond_timedwait` on a requeued thread still times out, on the mutex word now |
| `thread_kill` to a sibling with a handler | the handler runs **on that thread** — it records `cosmo_thread_id()` — and on no other |
| `thread_kill` to a sibling that has the signal blocked | pending on it, delivered when unblocked, never on another thread |
| `thread_kill` to the process's first thread by its pid-as-tid | delivered |
| `thread_kill` to a tid of another process | `-ESRCH`, and that process saw nothing |
| `thread_kill(tid, 0)` after `join` | `-ESRCH` **eventually** — a bounded wait, per the `lxtest` lesson, not one shot |
| `thread_kill` with a bad signal | `-EINVAL` |

## Risks

**A requeue that strands waiters is a hang, not a failure.** Rules 1
and 2 above are load-bearing, and their failure mode is a thread that
never returns. The tests use bounded joins so that a stranded waiter
is a reported failure rather than a hung suite — the same discipline
as `thrtest`'s heap barrier.

**Rule 1 costs a wake on the signal path that today's code does not
pay.** One `futex_wake` with nobody there, per signalled wait on an
uncontended mutex. The measurement reports it; if it turns out to
matter, the alternative is a per-waiter "was I requeued" flag the
broadcaster sets, which is more state and a second thing to get
right, and the report prefers the number first.

**A condition waited on with two mutexes at once now has a word that
is wrong for one of them.** POSIX makes that undefined already; the
header says so, and the debug build can assert it (`c->mutex` is
`NULL` or `m`) at a cost of one compare on the wait path.

**Two syscalls per broadcast, not one.** The measurement is what
decides whether that is a cost or a saving; the report predicts a
saving for every *N* > 1 and will be corrected by the number if wrong.

**`thread_kill` widens what a thread can do to its siblings.** Only
within the process, and only what `kill` already lets it do to the
whole process; the new capability is aim, not reach. Cross-process is
refused by construction and tested.

**The three older thread calls stay unfuzzed.** Adding them is one
line each, but a fuzzed `SYS_thread_create` with random arguments
needs its own constraints (a bad stack pointer kills the fuzzer, which
is the recurring lesson). Named, not done: the implementer should
decide with the cases in front of them.

## Alternatives considered

**Expose the non-comparing requeue too, for symmetry with Linux.**
Rejected: it has the race glibc abandoned it for, and the native door
has no legacy to serve.

**Give `cosmo_cond_broadcast` a mutex parameter instead of a second
word.** Rejected: it changes every caller's signature for a value the
condition already saw at wait time, and a broadcaster that passes the
wrong mutex — or one it does not hold — has the same problems rule 2
solves, with less information to solve them.

**Mark the mutex 2 before the requeue and fall back to wake-all when
it reads 0** — the first draft of this report. Rejected on review:
the mark can be undone by another holder's unlock before the requeue
lands, and the woken waiter's fast-path relock breaks the chain
regardless. The pre-mark solved the broadcaster's own case and no
other; the post-requeue loop and the contended relock solve all of
them, and the fallback then has no case left to serve.

**Require broadcasters to hold the mutex.** Rejected: POSIX permits
broadcasting unlocked, and a rule a caller can violate silently into a
hang is worse than one more read of the word.

**A process-wide `kill` with a thread argument, extending
`SYS_kill`.** Rejected: `kill` is the process-scoped door with its own
permission story (uids, signal 0 as liveness probe); a new number that
cannot address another process is simpler to reason about than a flag
on one that can.

**Only one of the two.** They share the README line, the design-doc
line, the door, and the test program; splitting them makes two
reports for one gap.
