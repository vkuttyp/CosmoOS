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

TBD-MEASUREMENTS

## 5. Validation

TBD-VALIDATION

## 6. Plan

Plan §8's epoll item is complete; inventory §7.7 and the history carry the
unit; the epoll unit's risks list strikes nesting.
