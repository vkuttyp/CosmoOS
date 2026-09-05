# Lock discipline and lockdep: architecture

Milestone 3 of the post-roadmap plan (`docs/audit/2026-09-post-roadmap-audit.md`
§19), the subsystem the lifetime pass named as its successor
(`docs/audit/2026-09-lifetime-quiesce-report.md` §9).

## Where it sits

```
  spin_lock / spin_lock_irqsave / spin_trylock / spin_unlock          (kernel/core/spinlock.c)
  mutex_lock / mutex_trylock / mutex_unlock                            (kernel/scheduler/mutex.c)
  might_sleep()  (mutex, wait_event, semaphore, completion, sleep, uaccess, synchronize_quiesce)
            │
            ▼
  lockdep_acquire / lockdep_release / lockdep_might_sleep              (kernel/core/lockdep.c, debug builds)
            │
            ├── lock classes            one per init-site name, cached in the lock (`class` field)
            ├── held-lock stacks        per CPU for spinlocks (interrupt context nests on top),
            │                           per thread for mutexes (held across sleeps)
            ├── dependency graph        class×subclass bitmap, "A was held when B was taken";
            │                           an acquisition that closes a cycle is a report
            ├── IRQ-safety state        per class: taken in interrupt context / held with
            │                           interrupts enabled; both at once is a report
            └── report                  held stacks with acquire addresses, then panic
                                        (self-tests arm an expectation and count instead)
```

Release builds compile the hooks to nothing; the `class` field stays so the
module ABI has one layout. The always-on checks that predate this milestone
stay always on: a spinlock re-acquired on its CPU panics, a sleeping
primitive entered with `preempt_count != 0` or in interrupt context panics.

## Purpose

Every lock-order rule in the tree was, until now, a sentence in a document
checked by review, and the audit found three of them wrong (scheduler S2/S4,
VFS V7, the network order) and one real ABBA (VFS rename). The lifetime pass
added rules of the same kind: `synchronize_quiesce` never under a spinlock,
`timer_cancel_sync` only under locks the callback never takes, `transmit`
never sleeping. This subsystem turns each into a debug-build panic at the
first violation, on the first boot that runs the path, with the held-lock
stacks in the report. The same milestone fixes what the checker and the
audit found: the rename order, the vnode cache's check-then-get, the futex
copy under a spinlock, `vfs_sync` holding the mount list across a commit,
and the aarch64 IPI path taking the GIC lock under the run-queue lock.

## Responsibilities

- Classify every spinlock and mutex by its initialisation name and cache the
  class in the lock.
- Keep the held-lock stacks: which locks this CPU (spinlocks) and this thread
  (mutexes) hold, with the acquiring instruction and the interrupt state.
- Record and check the dependency graph across (class, subclass) nodes;
  report an acquisition that would create a cycle, a same-node re-acquisition
  without a nesting annotation, and a stack overflow.
- Record and check interrupt safety per class.
- Provide `might_sleep()`, the one annotation for "this may block", and
  place it in every sleeping primitive and every user-memory copy.
- Provide nesting annotations (`spin_lock_nested`, `mutex_lock_nested`) for
  the few legitimate same-class nestings (parent and child vnode, the two
  parents of a rename).
- Provide the self-test hook that turns the next report of an expected kind
  into a counter.
- Report with enough context to act on: both stacks, both instruction
  addresses, the class names, the cycle.

## Non-responsibilities

- It does not detect deadlocks at run time (a hung system); it detects the
  orderings that could deadlock, whether or not they do in this run.
- It does not check lock-free algorithms, memory ordering or the quiescence
  protocol; `docs/kernel/quiesce/` owns those.
- It does not model wait queues, completions or semaphores as locks: a
  thread blocked on one while holding a mutex is legal and outside the
  graph. `might_sleep()` covers the illegal case (blocking under a
  spinlock).
- It does not track locks taken by user-mode code or by guests.
- It is not a performance tool; debug builds only.

## Interfaces at a glance

| Interface | Header | Notes |
|---|---|---|
| `spin_lock_nested(lock, subclass)`, `mutex_lock_nested(m, subclass)` | `kernel/spinlock.h`, `kernel/mutex.h` | same-class nesting annotation, 0..3 |
| `might_sleep()` | `kernel/lockdep.h` | in every sleeping primitive and user copy |
| `lockdep_assert_held(lock)`, `lockdep_assert_not_held(lock)` | `kernel/lockdep.h` | debug assertions on the held stacks |
| `lockdep_expect(kind)`, `lockdep_expected_hits()` | `kernel/lockdep.h` | self-tests only |
| `lockdep_dump_held()` | `kernel/lockdep.h` | from the panic report |
| `lockdep_get_stats()` | `kernel/lockdep.h` | classes, edges, checks, reports |

Documents: `design.md` (data structures, algorithm, the fixes),
`api.md`, `invariants.md`, `testing.md`.
