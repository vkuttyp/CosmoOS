# Lock discipline and lockdep: invariants

Rules the checker enforces and rules the checker itself keeps. Each has a
**Check** and, where honest, a **Gap**. Changing a rule means changing
this file and the code together.

## Rules enforced on the tree (debug builds, every boot)

**L1. No lock-order inversion.** For every pair of lock classes taken
nested, the tree takes them in one order only; an acquisition that would
close a cycle in the recorded graph panics with both held stacks and the
chain. Check: every debug boot and every self-test run under the checker;
`lockdep-order` and `lockdep-mutex` prove the detector fires on a
constructed ABBA (spinlocks and mutexes); host `test_lockdep` proves the
graph search (direct, transitive, subclass nodes, truncation). Gap: an
order the boot tests never exercise is never recorded; the boot exercises
every subsystem's self-tests, the userland test script and the network
harness, which is the coverage `testing.md` documents.

**L2. No same-class nesting without an annotation.** Taking a lock while
another of the same class is held is a report unless the inner one is a
`*_lock_nested` subclass. Check: `lockdep-recursion`; every annotation is
in L5.

**L3. A lock taken in interrupt context is never held with interrupts
enabled.** The class is marked IRQ-safe at its first interrupt-context
acquisition and held-with-IRQs-on at its first `spin_lock` with interrupts
enabled; both bits is a report naming both sites. Check: `lockdep-irq` (a
class first held with interrupts on, then taken in a real self-IPI
handler). Gap: a class that acquires the two bits in the other order is
reported at the second acquisition too; both are covered by the same code
path.

**L4. No sleeping call in atomic context.** `might_sleep()` at the entry
of every sleeping primitive and every user-memory copy panics when a
spinlock, a `quiesce_read_lock` section or an interrupt is active, in
release builds too. Check: `lockdep-sleep`, `lockdep-mutex` (a mutex under
a spinlock); the fix to `futex_wait` exists because `copy_from_user` now
carries it. Gap: `kmalloc` does not sleep today and carries no annotation;
if it ever can, it gets one.

**L5. The annotated nestings, and why each is safe.**

| Annotation | Site | Justification |
|---|---|---|
| `VNODE_NESTED_CHILD` on `victim->lock` | `vfs.c` `remove_entry` | the parent is locked first; the child cannot be a parent of anything locked here |
| `VNODE_NESTED_CHILD` on `replaced->lock` | `ramfs.c` `ramfs_rename` | both parents are locked (subclass 0 and 1); the replaced entry is a child of the second |
| `VNODE_NESTED_PARENT2` on the second parent | `vfs.c` `vfs_rename` | under `rename_lock` the ancestor is locked first; two unrelated directories are locked in address order, which no other path contradicts (parent-then-child needs an ancestry; other renames are excluded) |

`ramfs_release_tree` locks no child: it runs from unmount after the
reference scan proved exclusive access, with the root locked, and a lock
per level would nest the class to arbitrary depth.

**L6. `runqueue.lock` is a leaf.** Nothing is acquired while it is held;
`arch_ipi_send` reads a binding made by `arch_ipi_bind` at `ipi_init` and
takes no lock (scheduler invariant S2). Check: the recorded graph has no
edge out of `runqueue` (`testing.md`); `lockdep-order`'s dump each boot.

**L7. The VFS order is `g_mounts_lock` → `rename_lock` → `vnode->lock`
(parent, `PARENT2`, `CHILD`) → `pagecache.lock` → filesystem private
locks → block layer, with `mount->lock` (the hash) a spinlock leaf and
`sync_lock` alone before filesystem locks.** `vnode_release` takes no mount
lock: `vnode_put` unhashes under the hash spinlock before the last drop,
so a hashed vnode always has a reference and `vnode_lookup_cached` never
skips one (VFS invariant V7, audit #21). Check: `vfs-concurrency` (two CPUs:
rename against rmdir/mkdir of the destination; open/close of one file,
the vnode count exact afterwards); the recorded graph.

**L8. `futex_wait` holds no spinlock across the user copy, and no wake is
lost.** The bucket's `wake_seq` is read under the lock, the copy runs
unlocked, and the enqueue happens only if the sequence is unchanged;
otherwise the call returns 0 (a permitted spurious wake). Check: the Linux
personality tests (musl mutexes, condition variables, barriers) on every
boot; review of the sequence argument in `design.md`. Gap: no kernel-level
futex race test; the lost-wake argument is by construction.

## Rules the checker keeps

**L9. The checker allocates nothing and takes no tracked lock.** All
tables are static (160 classes, 640 nodes, 24 held per CPU, 8 mutexes per
thread); the raw lock is a word. Exhaustion of any table is a report, not
an overrun. Check: host `test_lockdep` (class table full → -1); review.

**L10. Classes are keyed by the initialisation name pointer and the lock
kind.** One site, one class; a mutex and its internal spinlock are two
classes. Check: host `test_lockdep` (`classes`); the tree's names are
literals (review: `grep spinlock_init\|mutex_init\|SPINLOCK_INIT`).

**L11. Held stacks are per CPU for spinlocks and per thread for mutexes,
and a lock is on a stack only while it is owned.** The run-queue lock
handed across a context switch and interrupt-context acquisitions are
therefore tracked correctly without special cases, and a lock still being
waited for (a contended plain `spin_lock` with interrupts enabled, during
which a handler may run) is not seen as held by that handler. Check: every
boot (a mismatch would report an unheld release at the first schedule);
`lockdep-order` (release out of order is legal); `lockdep-contention` (a
timer handler inside a contended wait records no edge from the awaited
lock).

**L12. Release builds carry no checker code beyond the `class` field and
`might_sleep`'s always-on half.** Check: `BUILD=release` in the
verification chain; `lockdep.c` is `#if CONFIG_LOCKDEP`.
