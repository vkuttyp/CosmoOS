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
state at 2**. If the broadcaster holds the mutex with no other waiter
recorded (state 1), it requeues *N − 1* threads onto a word whose
unlock will never wake them. **They sleep for ever.** And if the
broadcaster does not hold the mutex at all (state 0 — POSIX permits
broadcasting unlocked), there is no unlock coming at all.

So the design has two rules and one measurement, not a one-line swap.

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

**`cosmo_cond_broadcast` uses it, under two rules.**

1. **Mark the mutex contended before requeueing onto it.** If the
   broadcaster holds the mutex (state 1), CAS it to 2 first, so the
   eventual `unlock` finds 2 and wakes. If it is already 2, nothing to
   do. This is the rule that keeps the moved waiters reachable, and it
   is exactly what glibc does for the same reason.
2. **If the mutex is not held, do not requeue.** State 0 at broadcast
   time means no unlock is coming; fall back to waking every waiter,
   as today. Broadcasting without the mutex is legal and rare, and the
   herd is the correct price for it.

Then `seq++` and `futex_requeue(&c->seq, &m->state, 1, ~0u, seq)`:
wake one, move the rest. A requeued waiter returns from its
`cosmo_futex_wait` on the cond word only when the mutex's unlock
wakes it, goes straight to `cosmo_mutex_lock(m)`, and takes the mutex
through the ordinary contended path — the same code as today, entered
later. `cosmo_cond_signal` is unchanged: a single wake is already
optimal.

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
| `libc/include/cosmo/thread.h` | `cosmo_thread_kill`; the contract of `cosmo_cond_broadcast` gains its two rules |
| `libc/src/thread.c` | `cosmo_cond_broadcast` requeues under the two rules; the comment that deferred the measurement is replaced by the measurement |
| `userland/tests/thrtest.c` | the cases below, with the herd measured |
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
on one condition, holding a counter of how many times
`cosmo_mutex_lock` took its contended path. One broadcast. Before this
unit that number is *N − 1* by construction — every waiter but the
winner sleeps again on the mutex. After it, the waiters are woken one
per `unlock`, so each takes the mutex uncontended: the count is **0**,
or a small number if the scheduler runs a woken thread before the
unlock that freed it. The test asserts it is below *N − 1*, prints
both figures, and the report's as-built carries them. That is the
measurement, and the bug-proof is the old broadcast: put
`cosmo_futex_wake(~0u)` back and the count returns to *N − 1*.

| case | what it establishes |
| --- | --- |
| broadcast, mutex held (state 1) | every waiter returns; the mutex was marked 2 before the requeue — remove that CAS and the requeued waiters **hang**, which a bounded join reports |
| broadcast, mutex held with a recorded waiter (state 2) | same, no double-marking |
| broadcast, mutex **not** held | every waiter returns (the wake-all fallback); remove the fallback and they hang |
| `SYS_futex_requeue` with a stale `val` | `-EAGAIN`, nobody moved |
| a requeued waiter and a timeout | `cosmo_cond_timedwait` on a requeued thread still times out, on the mutex word now |
| `thread_kill` to a sibling with a handler | the handler runs **on that thread** — it records `cosmo_thread_id()` — and on no other |
| `thread_kill` to a sibling that has the signal blocked | pending on it, delivered when unblocked, never on another thread |
| `thread_kill` to the process's first thread by its pid-as-tid | delivered |
| `thread_kill` to a tid of another process | `-ESRCH`, and that process saw nothing |
| `thread_kill(tid, 0)` after `join` | `-ESRCH` **eventually** — a bounded wait, per the `lxtest` lesson, not one shot |
| `thread_kill` with a bad signal | `-EINVAL` |

## Risks

**A requeue that strands waiters is a hang, not a failure.** Rule 1
above is load-bearing, and its failure mode is a thread that never
returns. The tests use bounded joins so that a stranded waiter is a
reported failure rather than a hung suite — the same discipline as
`thrtest`'s heap barrier.

**The "not held" fallback is a policy, and a caller may not expect the
herd there.** Documented in the header: broadcast with the mutex held
if you want the requeue.

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

**Requeue unconditionally and require broadcasters to hold the
mutex.** Rejected: POSIX permits broadcasting unlocked and the
fallback costs one comparison. A rule a caller can violate silently
into a hang is worse than a slower path.

**A process-wide `kill` with a thread argument, extending
`SYS_kill`.** Rejected: `kill` is the process-scoped door with its own
permission story (uids, signal 0 as liveness probe); a new number that
cannot address another process is simpler to reason about than a flag
on one that can.

**Only one of the two.** They share the README line, the design-doc
line, the door, and the test program; splitting them makes two
reports for one gap.
