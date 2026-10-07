# epoll interest removal on the final close

Date: 2026-10-07. Branch `epoll-close-removal` from `main` at `6c5403dd`
(after PR #323, the first of this pair). Plan §8, "epoll lifetime and
nesting"; the epoll unit's one recorded deviation
([risks](next-subsystem-epoll.md)).

## 1. The defect, and the test that shows it

epoll v1 ([report](next-subsystem-epoll.md), PR #291) gave each interest
entry a reference to its member and dropped it on `EPOLL_CTL_DEL` and at
the set's release -- and nowhere else. Linux removes an entry when the
last descriptor referring to its open file description is closed, and
every event loop written for Linux relies on that: it closes the socket
and moves on. On CosmoOS the registration kept the closed socket alive,
so no FIN went and the set went on evaluating a descriptor the program no
longer had.

`epoll-close` (`kernel/io/epolltest.c`) shows it on current `main` with a
handle table of its own: a pipe's read end registered and closed is still
held by the registration (a second reference where the test's own must be
the last; released, the end's writer would see `-EPIPE`), a readable
eventfd closed is still reported, and a TCP client socket registered and
closed is still held, so its loopback peer stays `ESTABLISHED` where
`CLOSE_WAIT` -- the FIN received -- is required. `tools/epoll-close-probe.py
--old` boots that state and fails at the first of those checks on both
architectures (§5).

## 2. Design

**Identity.** Linux keys the registration on the open file description.
Here that is the kobject in the handle slot: `dup`, `dup2/3` and a spawn's
handle map install the same object; a file's per-open state (offset,
flags) is its own kobject (`struct file`), so two `open`s of one path
are two descriptions here as there. The rule therefore maps onto a count
of slots per object.

**The count.** `struct kobject` gains `handles`: the number of
handle-table slots, in every process, that hold it, raised in
`install_slot` (both installs) and lowered in `handle_close`. It fits the
structure's padding. `handle_close` reads the decrement's result under
the table lock and, when it reached zero, calls
`epoll_last_handle_closed(obj)` after dropping the table lock and before
its own `kobject_put`: the removal takes mutexes, and the object must
still be alive while its registrations go.

**The watchers list.** Each `epoll_item` is linked on its member's
`kobject.watchers` (a pointer; items chain through `obj_next`) and knows
its set (`it->ep`). The last close walks that list: for each item, under
the set's `ep->lock`, the item leaves the set's list, `ep->nr` drops,
and the set's queue is woken; the items' references are put outside every
lock. Objects never registered pay one pointer and an uncontended mutex
at their last close: the decision that there is nothing to remove is made
under the lock the add publishes under (an unlocked look at the list was
the first version, and review found the add that had passed its check
and not yet linked -- a registration that would have outlived the last
descriptor).

**Locking.** One global mutex, `g_watch_lock` (Linux's `epmutex`),
guards every `watchers` list and the handle-count check. The order is
`g_watch_lock` outside `ep->lock`, always: `epoll_obj_add`, `epoll_obj_del`,
`epoll_release` and `epoll_last_handle_closed` take it first;
`epoll_obj_wait` takes `ep->lock` alone and is never in the order. The
lock is initialised at boot (`epoll_init`, after `futex_init`).

**Races closed.**

| Race | Rule |
|---|---|
| the last close against an add in flight: the add passed its handle-count check under the watch lock and has not linked its item yet | the removal decides under the same lock, so it either sees the item or runs before the add's check (which then sees no handle) |
| ADD against the last close: the door's lookup saw a slot, another thread emptied it before the add, the removal found no item | the add re-reads `handles` under the watch lock and refuses an object with none (`-EBADF`); Linux returns 0 and removes at the final fput, which cannot happen here because the item itself would hold the object |
| a descriptor reappears: a handle riding in a unix message is installed at the receiver between `handle_close`'s decrement and the removal's lock | the removal re-reads `handles` under the lock and keeps the registrations when it is back |
| the set closed while it holds entries, then a member's last close | `epoll_release` unlinks its items from their members' lists under the watch lock before freeing them |
| `epoll_wait` asleep on the member during the close | the waiter holds a snapshot reference and its own wait entry on the member's queue (the epoll unit's rule); the removal wakes the set's queue, the waiter finishes, drops its pin and re-snapshots without the member; the member's release follows the drop, not its next event |

**Not a descriptor.** A handle in flight in an unread unix message holds
a reference and no slot. An object whose only remaining reference rides
in a message loses its registrations when its last slot closes; Linux
keeps them until the message is received and that descriptor closed. A
gap, recorded in `docs/kernel/io/invariants.md`.

**Nesting: kept refused.** `-EINVAL` for an epoll as a member, as v1. A
member's events wake the member's queue, and a waiter parks its own
entries there; the set's own queue is woken only by `ctl`. An outer set
sleeping on an inner set's queue would sleep through every event of the
inner set's members -- a silent lost wake, the worst kind. Linux's
`ep_poll_callback` forwards a member's wake to the set's queue through
an item-owned wait entry, and its loop detection (`-ELOOP`, depth 5) and
the wait-queue lock nesting it implies are built on that: a redesign of
`struct wait_entry` (a callback kind) and of the set's wake path, which
the EPOLLET unit's wake-generation counter does not substitute for. Until
then a program that nests gets a loud error.

## 3. What changed

| Where | Change |
|---|---|
| `kernel/include/kernel/object.h`, `kernel/object/object.c` | `kobject.handles`, `kobject.watchers`; both zero at init |
| `kernel/object/handle.c` | `install_slot` raises the count; `handle_close` lowers it and calls `epoll_last_handle_closed` at zero, outside the table lock, before the put |
| `kernel/include/kernel/epoll.h`, `kernel/io/epoll.c` | `epoll_init`, `epoll_last_handle_closed`; items carry `ep` and `obj_next`; `g_watch_lock`; add refuses an object with no slot; add/del/release link and unlink the watchers list |
| `kernel/core/main.c` | `epoll_init()` after `futex_init()` |
| `kernel/io/epolltest.c`, `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h`, `kernel/kernel.mk` | `epoll-close`, eight cases (§4) |
| `tests/linux/lxtest.c` | `LXEPOLLCLOSE`: eventfd, dup, socket pair, the set closed first |
| `tests/linux/epoll_musl.c`, `tests/linux/linux.mk`, `userland/etc/rc.linux`, `tests/boot/run_boot_test.py` | the static musl program and its marker (x86-64, where CI has `musl-gcc`) |
| `tools/epoll-close-probe.py` | `--old` restores v1 |
| `docs/kernel/io/design.md` ("epoll"), `invariants.md` (A9, two gaps), `testing.md`; `docs/kernel/object/architecture.md`; `docs/compat/linux/testing.md`; `next-subsystem-epoll.md` (risks) | the design, the invariant, the tests |

No Linux door changed: `lx_epoll_ctl` already returned the add's result,
and `-EBADF` is a new result only in the racing case.

## 4. Tests

| Case | What is required |
|---|---|
| pipe read end registered, closed | count back to the test's creator reference; wait reports nothing; the test's own put releases the end and the writer's write returns `-EPIPE` |
| eventfd readable, registered, closed | nothing reported; released |
| TCP client to a loopback server that accepts and holds, registered, closed | count back to the test's reference; nothing reported; the test's own put is then the close and the server's connection reaches `CLOSE_WAIT` within 2 s |
| two slots, registered under the first; first closed, then second | event still reported after the first close; nothing after the second; released |
| slot closed, then add under it | `-EBADF` |
| waiter blocked (`FOREVER`) on a pipe end and an eventfd; the pipe end's slot closed | the end's count returns to the test's reference within 1 s while the waiter still sleeps; the eventfd's write ends the wait with one event, the eventfd's |
| a second set with a member; the set's slot closed first, then the member's | both plain closes; counts as expected |
| a registered member in a table that is destroyed | nothing reported; released |
| `lxtest` `LXEPOLLCLOSE` (both architectures) | eventfd closed: not reported; `dup` keeps it, last close removes it, `DEL` then `-ENOENT`; a non-blocking socket pair's registered end closed: the peer reads 0 where it read `EAGAIN`; a set closed first then its member |
| `epoll_musl` (x86-64 CI) | pipe reader closed: `EPIPE` (`SIGPIPE` ignored); socket pair end closed: EOF; `dup` keeps, last close removes |
| `tools/epoll-close-probe.py --old` | `epoll-close` fails at `kobject_refcount(rd) == 1` after the baseline close, both architectures |

## 5. Validation

On the branch head, one chain, one QEMU at a time at the default
priority (no zsh `&`), 435 self-tests in every debug boot:

| Step | x86-64 | AArch64 |
|---|---|---|
| `make host-test` | PASS | PASS |
| `make analyze` | clean | clean |
| debug boot, 4 CPUs | PASS 136.5 s | PASS 135.5 s |
| debug boot, 2 CPUs | PASS 134.3 s | PASS 145.5 s |
| debug boot, 1 CPU | PASS 125.0 s | PASS 124.8 s |
| `make test-smp2` | PASS 142.4 s | PASS 132.4 s |
| `make test-chaos` | PASS 146.0 s | PASS 148.6 s |
| release build and boot | PASS 16.8 s | PASS 20.3 s |
| `tools/epoll-close-probe.py` (this branch) | `EPOLLCLOSEPROBE: ok=1` | the same |
| `tools/epoll-close-probe.py --old` | `ok=0 reason=check failed: kobject_refcount(rd) == 1 at line 137`, as required | the same |

`epoll-close` takes about 120 ms. `epoll_musl` is built only where
`musl-gcc` exists -- the x86-64 CI runner -- so its first run is CI's;
the development host has none. The one earlier boot of the branch that
failed did so at the test's own first baseline check, because the test
held its creator reference across the close and asserted the writer's
`-EPIPE` where only the count could yet show the removal; the check order
is the one in §4 now, and the module ABI marker the runner requires
moved to v5 with the structure. No x86-64 boot approached the 180 s
budget locally (the longest, `test-chaos`, 146.0 s). The PR's CI boot
times are added here when its run completes.

## 6. Plan

Plan §8's "epoll lifetime and nesting" is complete as to the lifetime and
decided as to nesting; inventory §2.6 and §7.6 and the history carry the
unit. The epoll unit's risks list strikes the deviation.
