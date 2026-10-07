# epoll readiness by callback, and nesting

Date: 2026-10-07. Branch `epoll-callback` from `main` at `95c635e2` (the
epoll-close merge, PR #324). Plan §8, "epoll lifetime, readiness by
callback, nesting"; the epoll-close report's recorded reason for refusing
nesting.

## 1. The defect, and the baseline

epoll v1 and its two follow-ups kept the aio ring's multi-queue wait: every
`epoll_wait` snapshotted every registered member, pinned each with a
reference, parked a wait entry of the waiter's own on each member's queue,
evaluated, slept, then finished every entry and dropped every pin. The
cost of a wait was the number of members registered -- what epoll exists
not to pay -- and a member's wake reached only the member's queue, which
is why a set could not be a member of a set (an outer set asleep on an
inner set's queue would sleep through the inner's members' events).

The baseline is `epoll-scale` on the tree before the unit (`main` at
`95c635e2` with the benches added, `tools/epoll-callback-probe.py
--baseline`): 1, 16, 256 and 1024 eventfds registered, one readable; the
median of twenty non-blocking waits that find it, and of twenty blocking
waits woken by a writer thread (write to return). §4 has the figures.

## 2. Design

**The callback entry** (`docs/kernel/scheduler/design.md`, "Wait queues").
`struct wait_entry` gains `fn`: an entry is a sleeping thread (`fn` NULL,
woken) or a callback (`fn` set, run). A wake runs every callback entry
under the queue's spinlock, in the waker's context -- an interrupt, a
timer, another CPU -- and `wake_one` runs past them to a thread. The
callback may take spinlocks ordered after the queue's and must not sleep.
`waitqueue_add_callback` registers one; `waitqueue_remove_callback` takes
it off under the lock, so on return no callback of the entry is running
or can start; `waitqueue_detach_callbacks` is for a queue's owner about to
free it (every callback entry unlinked and told `WAIT_CB_FREED` under the
lock, Linux's POLLFREE), after which the owner waits a grace period
(`synchronize_quiesce`) so an entry's owner reading the queue pointer in
a read-side section never follows it into freed memory. The split wake
(`waitqueue_lock_nested`, `waitqueue_wake_all_locked`, `waitqueue_unlock`)
is for a set woken from inside another set's wake. The `wake_gen` counter
the EPOLLET unit added -- the pull-side substitute for a callback -- goes.

**The set** (`docs/kernel/io/design.md`, "epoll"). Each item owns a
callback entry on each queue its member's requested directions wake (one;
two for an `O_RDWR` FIFO), registered at ADD and re-registered at MOD,
removed by DEL, by the set's release and by the last-close removal before
the item is freed. The callback (`hook_wake`) links the item onto the set's
ready list under a new spinlock `ep->rlock` unless it is there, notes a
wake of an item a walker holds (`rewake`), and wakes `ep->wait`.
`epoll_obj_wait` prepares on `ep->wait` alone, walks the ready list under
`ep->lock` (moved to a transfer list under `rlock`, evaluated with no
spinlock held), and sleeps only when the list is empty after the walk.

| Semantics | Rule |
|---|---|
| level | a reported item is re-queued at the tail (Linux re-queues level items), so the next wait re-evaluates it; found not ready, it is dropped |
| edge | a reported item is not re-queued; its member's next wake re-queues it -- the wake is the edge, as the generation counter's change was |
| one-shot | disabled on report until MOD; MOD re-hooks, re-arms and re-polls, re-queuing a ready member |
| ADD | hooked, then polled: ready now, or with no queue at all (a plain file), straight onto the list |
| re-arm (a copy failure) | the item goes back on the list, a one-shot's suppression lifted |
| a wake during the walk | `rewake` under `rlock`: re-queued whatever the walker decided |
| the set's own readiness | the ready list is not empty -- a spinlock check, so a nested set is polled without mutex recursion; it may read readable where the walk then finds nothing (a drain's wake), as Linux's epoll descriptor may |

**Which lock the callback runs under.** The member queue's spinlock,
interrupts off. Inside it the callback takes `ep->rlock` and then
`ep->wait`'s spinlock; the order is member queue -> `epoll-ready` -> `epoll`,
and nothing takes them the other way: `ep->lock` (mutex) is never held when
a waker takes the spinlocks, `collect` evaluates readiness (which may take
a member's internal lock) with no spinlock held, and the set's readiness
check is `rlock` alone. For a set woken from inside a member set's wake
the two classes nest in themselves along the chain, so the forwarding
callback takes the outer's locks with a lockdep subclass equal to the chain
depth below the outer: the inner set records `nests` (its subclass plus
one) under its queue lock while it wakes, and the outer's callback reads
it there (Linux's `ep_poll_safewake`). A plain member's wake forwards at
subclass 0, so a chain of four sets uses 0..3 and never two equal ones.
`docs/kernel/lockdep/invariants.md` L5 carries the annotation. Every
debug boot checks the order.

**Item lifetime.** The entries are the item's and the queues the member's,
which the item's reference keeps alive until the unhook; the unhook under
each queue's lock is what makes "a callback in flight on another CPU
cannot touch a freed item" true (A10). The survey of members: pipe ends,
sockets, eventfds, timerfds and the aio ring own their queues; a FIFO's
live in its vnode, which the file holds; a tap's in the tap, which the
open holds; the console's and the terminal's in the one static `struct
tty`; a plain file has none. The one queue not owned by the member is a
signalfd's, which is its *process's*: `process_release` detaches every
callback entry and, when it detached any, waits a grace period before
freeing. Nothing else an item hooks is outlived by the item.

**Nesting** (A11). A set may be a member of a set; `EPOLL_MAX_NESTS` = 4
sets in a chain, counting both ends. The loop check runs under
`g_watch_lock` and no set lock, over per-set `subsets` lists (the items
whose member is a set, kept under the watch lock) and the members'
`watchers` lists: a set added to itself is `-EINVAL`, a set from which the
outer is reachable or that would make a longer chain `-ELOOP`. The bound is
four, not Linux's five: lockdep has four subclasses, and its order graph is
a bitmap of nodes squared -- 512 KiB static at four subclasses, 2 MiB at
eight. Raising `LOCKDEP_SUBCLASSES` is the knob if five is ever wanted.

**The watch lock** (§"The never-registered object's close" in the design).
The previous unit took `g_watch_lock` on every last close to close a race
between an add and a close. `kobject.watched` closes it without the lock:
the add stores the flag under the lock before its handle-count check, with
a `seq_cst` fence; the close reads it after its decrement, with a `seq_cst`
fence; store-buffering, so at least one side sees the other -- the add
refuses, or the close takes the lock and finds the item.
`tests/litmus/epoll/watched.litmus` proves it under RC11: the bad outcome
`Never`, each conjunct reachable, and `Sometimes` with either fence removed
(two controls). The common close -- every pipe, file and socket a program
closes -- is a fence and a byte read.

**`poll()` and the aio ring** keep their multi-queue wait: one call over a
few caller-named objects, the shape thread entries serve.

## 3. What changed

| Where | Change |
|---|---|
| `kernel/include/kernel/wait.h`, `kernel/scheduler/wait.c` | the callback kind (`fn`, `WAIT_CB_FREED`), `waitqueue_add_callback`/`remove_callback`/`detach_callbacks`, the split wake; `wake_gen` removed |
| `kernel/io/epoll.c` | the ready list, `hook_wake`, hook/unhook at ADD/MOD/DEL/release/last close, the walk, nesting with the loop check and `nests`, the `watched` fast path |
| `kernel/include/kernel/epoll.h` | `EPOLL_MAX_NESTS`, `epoll_obj_max_nests`, the add's new results |
| `kernel/include/kernel/object.h`, `kernel/object/object.c`, `kernel/object/handle.c` | `kobject.watched`; the close's fence and flag read |
| `kernel/process/process.c` | the signalfd queue's callback entries detached, a grace period when any were |
| `compat/linux/syscalls.c` | the door no longer refuses a set as a member |
| `kernel/include/kernel/module.h`, `docs/kernel/module/` | module ABI 6 |
| `userland/init/init.c`, `docs/userland/` | `--block` reads a pipe of its own |
| `kernel/io/epolltest.c`, `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | `epoll-scale`, `epoll-wake-race`, `epoll-nest`, `epoll-close-bench` |
| `tests/linux/lxtest.c`, `tests/linux/epoll_musl.c` | `LXEPOLLNEST`; the musl program's nested case |
| `tests/litmus/epoll/watched.litmus`, `tests/litmus/run_litmus.py` | the proof; the runner over two directories |
| `tools/epoll-callback-probe.py` | `--old no-unhook`, `--old no-loop-check`, `--baseline`, `--measure`, `--lockdep0` |
| `docs/kernel/io/design.md` ("epoll"), `invariants.md` (A10, A11, gaps), `testing.md`; `docs/kernel/scheduler/design.md`; `docs/kernel/lockdep/invariants.md`; `docs/compat/linux/testing.md`; `docs/kernel/quiesce/testing.md` | the design, the invariants, the tests |

## 4. The measurements

All figures are from `tools/epoll-callback-probe.py --baseline` (the tree
before the unit, `main` at `95c635e2`, with this branch's two benches added)
and `--measure` (this branch), four CPUs under QEMU TCG on the macOS host at
the default priority, debug builds with lockdep on and with `LOCKDEP=0`
(lockdep costs about half of every debug figure). `epoll-scale` registers
1, 16, 256 and 1024 eventfds with the first readable; medians of twenty.

### A wait against the number of registered members

Microseconds, 1/16/256/1024 members; the non-blocking wait that finds the
one ready member, and the blocking wait woken by a writer (write to return).

| Tree, arch, build | non-blocking | woken |
|---|---|---|
| before, x86-64, debug | 38/187/1314/4087 | 209/735/2933/1699 (a first run: 302/655/2490/13325) |
| before, x86-64, LOCKDEP=0 | 6/27/434/947 | 80/179/1524/2336 |
| this branch, x86-64, debug | 42/56/42/23 | 186/218/189/87 |
| this branch, x86-64, LOCKDEP=0 | 10/12/16/10 | 86/122/159/84 |
| before, AArch64, debug | 27/138/1069/2293 | 156/559/2046/10398 |
| before, AArch64, LOCKDEP=0 | 10/48/590/1083 | 114/321/1904/2603 |
| this branch, AArch64, debug | 45/60/40/25 | 303/372/266/158 |
| this branch, AArch64, LOCKDEP=0 | 11/16/16/11 | 112/172/150/102 |

Before, both figures grow with the count -- about a hundredfold from 1 to
1024 members for the non-blocking wait (every item walked and polled) and
ten- to sixtyfold for the woken one (every member pinned, an entry parked
on each queue, then finished and unpinned after the wake; its 1024-member
figure varies by a factor of eight between two runs, the snapshot's
allocation and 2048 queue operations being at the mercy of the host). On
this branch both are flat: the 1024-member wait is the cheapest of the
four, since the one ready item is the whole walk. The cost of the flat
line is paid at one member: a non-blocking wait with one member takes two
spinlock pairs it did not before (the ready list is moved to the transfer
list and the level item re-queued), 10 against 6 us with lockdep off on
x86-64 and 42 against 38 us in debug, where lockdep weighs every
acquisition. From sixteen members up the new wait is cheaper everywhere.
`epoll-scale` bounds the 1024-member figures against the 1-member ones
(eight times plus 20 us; four times plus 200 us) and fails on the baseline
tree at its first bound, 4087 against 324 us.

### The close path

Closes per second, four CPUs each in a loop of 20 000 rounds (2 000 for
the removal shape): the last close of an object never registered; a close
that is not the last (a slot elsewhere); install, ADD to a set of the
worker's own, last close (the removal).

| Tree, arch, build | last close, never registered | a non-last close | the removal |
|---|---|---|---|
| before, x86-64, debug | 22 764 | 488 025 | 8 109 |
| before, x86-64, LOCKDEP=0 | 123 073 | 3 009 709 | 40 427 |
| this branch, x86-64, debug | 554 856 | 553 858 | 9 193 |
| this branch, x86-64, LOCKDEP=0 | 2 306 512 | 2 259 893 | 29 746 |
| before, AArch64, debug | 32 818 | 779 529 | 10 781 |
| before, AArch64, LOCKDEP=0 | 63 762 | 2 175 568 | 25 339 |
| this branch, AArch64, debug | 555 023 | 554 581 | 8 358 |
| this branch, AArch64, LOCKDEP=0 | 2 046 768 | 2 081 057 | 24 219 |

Before, the last close of a never-registered object took the global watch
lock, and four CPUs closing at once contended a mutex: 23-33 thousand a
second in debug, 64-123 thousand with lockdep off -- against a non-last
close, which took no lock on either tree, at half a million to three
million. On this branch the last close reads the `watched` flag and runs
at the non-last close's speed: 17-24 times the baseline in debug, 19-32
times with lockdep off. This is §2's "show the improvement". The removal
path -- the one case that must take the lock and walk the watchers -- is
within noise of the baseline in debug and 25 % slower with lockdep off on
x86-64 (an ADD now hooks a callback entry on the member's queue and the
removal unhooks it, two spinlock pairs more than a snapshot wait design
needed), the same on AArch64.


## 5. Validation

On the branch head, one chain, one QEMU at a time at the default
priority (no zsh `&`), 439 self-tests in every debug boot:

| Step | x86-64 | AArch64 |
|---|---|---|
| `make host-test` | PASS | PASS |
| `make analyze` | clean | clean |
| debug boot, 4 CPUs | PASS 137.0 s; 135.6 s on the review follow-up | PASS 145.4 s; 146.8 s on the follow-up |
| debug boot, 2 CPUs | PASS 143.6 s | PASS 147.1 s |
| debug boot, 1 CPU | PASS 132.5 s; 134.1 s on the review follow-up | PASS 124.1 s; 125.6 s on the follow-up |
| `make test-smp2` | PASS 143.3 s | PASS 151.2 s |
| `make test-chaos` | PASS 146.8 s | PASS 138.8 s |
| release build and boot | PASS 16.6 s | PASS 20.3 s |
| `tools/epoll-callback-probe.py` (fixed, both tests) | `ok=1` twice | `ok=1` twice |
| `tools/epoll-callback-probe.py --old no-unhook` | `epoll-wake-race` fails at one of its two checks, as required | `epoll-wake-race` fails at one of its two checks, as required |
| `tools/epoll-callback-probe.py --old no-loop-check` | `epoll-nest` fails at `add(inner, ho, outer, ...) == -ELOOP`, as required | the same |
| `make litmus` | herd7 is not on the development host: CI's litmus job (§"CI") | |

Three boots of the chain were repeated after a fix each, and the figures
above are the repeats' where one was needed:

- The first one-CPU boots of both architectures hung at the hang watchdog
  inside `epoll-wake-race`: its drain of the eventfd after the writer
  stopped was a blocking read, and on one CPU the count was zero there.
  The eventfd is non-blocking for the test.
- The first AArch64 measurement boot of this tree ran `epoll-close-bench`
  past the 8 s per-test budget: the removal shape costs tens of
  microseconds a round under TCG. Each shape has its own round count and
  the watchdog is kicked between shapes.
- The first x86-64 `--old no-unhook` boot made `epoll-wake-race` fail at
  the race loop's own check (the leaked item's callback was reported under
  the writer) rather than the deterministic check after the loop the probe
  named; both are the protection, and the probe accepts either.

Qodo's review of the PR found four things worth fixing and one worth
recording, all in one follow-up commit: a second waiter could sleep with
events pending (the walk re-queued items without waking the set's queue:
it wakes now, Linux's `ep_done_scan`); a fired one-shot's member wakes
re-queued the disabled item and made the set readable to `poll()` and to
an outer set (the callback skips a disabled item, as Linux's returns for
cleared events; `epoll-nest` checks the nested one-shot shape); the loop
check walked every path of a layered graph of sets under the global lock
(each set is visited once per check, a generation stamp as Linux's
`loop_check_gen`); and `wake_one` stopped at its thread with callbacks
behind it unrun (it walks on for them). The fifth, a signalfd inherited by
another process not waking that process's wait, is Linux's behaviour too
and is recorded as a gap in `invariants.md`.

No x86-64 boot approached the 180 s budget locally (the longest,
`test-chaos`, 146.8 s). CI (PR #325, run 37648352208, on the review
follow-up `6a793655`): the x86-64 job's debug boots took 151.2-154.8 s
against the 180 s budget (release 16.9 s, the panic-path boot 139.7 s) --
the highest of this series' three units (140.4-149.4 s for the epoll-close
PR, 119.6-124.2 s for the delayed-ACK one, on runners of their own), with
about 25 s of margin; no timeout, the flakes record unchanged, and the
figure is recorded here for the budget question should it narrow further.
The AArch64 job's 134.4-141.2 s against its 240 s (release 19.0 s), well
below the previous unit's 171-186 s, which says more about the runner than
about the tree. `epoll_musl` with its nested case compiled and ran on the
x86-64 runner; the litmus job's `epoll/watched.litmus` verdicts are
`Never`, both witnesses `Sometimes`, both controls `Sometimes`.

## 6. Plan

Plan §8's epoll item is complete; inventory §7.7 and the history carry the
unit; the epoll unit's risks list strikes nesting.
