# Lock discipline and lockdep: invariants

Rules the checker enforces and rules the checker itself keeps. Each has a
**Check** and, where honest, a **Gap**. Changing a rule means changing
this file and the code together.

## Rules enforced on the tree (LOCKDEP=1, default in debug)

**L1. No lock-order inversion.** For every pair of lock classes taken
nested, the tree takes them in one order only; an acquisition that would
close a cycle in the recorded graph panics with both held stacks and the
chain. Check: every boot with LOCKDEP=1 and every self-test run under the checker;
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

**L3. No IRQ-used lock can reach a lock acquired with interrupts enabled.**
A blocking IRQ-context acquisition marks a class IRQ-used. A successful
acquisition with IRQs enabled (including trylock) marks it IRQ-enabled.
The validator checks both usage-after-edges and edges-after-usage across
all subclasses. An IRQ trylock itself is nonblocking and adds no IRQ-used
label; a failed trylock adds no usage or edge. Check: `lockdep-irq` uses
self-IPIs for direct, edge-last, safe-label-last and trylock cases; host
`irq-dependencies` covers a maximum-node path and `irq-oracle` compares
BFS against independent transitive closure. Timer cancellation has a
separate observed-lock-profile check (L15); arbitrary callback completion
and NMI/#MC relationships remain outside these models.

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
| subclass 1 on the second futex bucket | `futex.c` `futex_requeue` (milestone 10) | the two buckets are locked in address order, and no other path takes two futex bucket locks |

`ramfs_release_tree` locks no child: it runs from unmount after the
reference scan proved exclusive access, with the root locked, and a lock
per level would nest the class to arbitrary depth.

**L6. Runqueue successors follow explicit rules.** Pairs nest in increasing
CPU-id order (S24). Address-space switching may take `asid` while the local
runqueue is held (`arch_thread_switch_prepare` → `vm_space_switch` →
`asid_switch_prepare`); the current AArch64 graph observes this edge.
`arch_ipi_send` reads a binding made at `ipi_init` and takes no GIC lock.
Check: inspect outgoing `runqueueN` edges in the final graph (`testing.md`)
for ordered pairs, ASID allocation, and logging from expected diagnostics;
`lockdep-rq-order` verifies that the reversed pair is rejected. The graph
is not expected to have zero outgoing runqueue edges.

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
tables are static (320 classes, 1280 nodes, 24 held per CPU, 8 mutexes per
thread); the raw lock is a word. Exhaustion of any table is a report, not
an overrun. Check: host `test_lockdep` (class table full → -1); review.

**L10. Classes are keyed by name contents and lock kind, with kernel-owned
name storage.** Equal names share a class; a mutex and its internal
spinlock are separate kinds. Names exceeding 63 characters are rejected
without truncation. Check: host `classes` and `metadata-lifetime`, including
freed and reused source buffers, maximum name length and exhaustion;
`module-load` uses a fixture lock in unloadable rodata, checks no new class
on reload, and the final graph dump reads its name after unload.

**L11. Held stacks are per CPU for spinlocks and per thread for mutexes,
and a lock is on a stack exactly while it is owned.** Ownership and the
push, the pop and the release, happen with interrupts masked (LOCKDEP=1),
so no handler observes one without the other. The run-queue lock
handed across a context switch and interrupt-context acquisitions are
therefore tracked correctly without special cases, and a lock still being
waited for (a contended plain `spin_lock` with interrupts enabled, during
which a handler may run) is not seen as held by that handler. Check: every
boot (a mismatch would report an unheld release at the first schedule);
`lockdep-order` (release out of order is legal); `lockdep-contention` (a
timer handler inside a contended wait records no edge from the awaited
lock). The per-CPU stack is read only where the reader cannot move
(scheduler S25): a spinlock's acquisition has preemption off already; a
mutex's acquisition check, and `lockdep_is_held` asked by a preemptible
thread, read it with interrupts off, since a thread moved between the
read and the scan would be scanning another CPU's stack.

**L12. With LOCKDEP=0 the runtime hooks and graph compile out.** Release
defaults to this configuration; it can also be selected in debug. Lock
layouts and the thread held array remain stable across the option, and
`might_sleep` retains its always-on half. Check: disabled debug/release
builds and ELF symbol inspection; `lockdep.c` is `#if CONFIG_LOCKDEP`.

**L13. A returned diagnostic path length never exceeds its buffer.**
Reachability searches all nodes; path output retains at most the caller's
capacity and returns the stored count, including zero for a zero-capacity
request. Check: ASan/UBSan `path-bounds` drives a 1280-node chain and the
printer's loop; `lockdep-order` detects a ten-lock cycle through real hooks.

**L14. `spin_unlock_irqrestore` restores the state saved for its lock and
does not enable interrupts while another spinlock remains held.** Manual
`spin_unlock` followed by `arch_irq_restore` is outside this pair check.
Check: `lockdep-irq` directly probes wrong state and nested-enable cases,
then performs matching real releases. Gap: manual raw save/restore sites
are not paired with lock identities.

**L15. `timer_cancel_sync` does not wait while holding a lock its running
callback has acquired.** A callback records blocking spinlock objects in a
bounded per-callback profile; cancellation checks the caller's held
spinlock and mutex stacks before each wait. Check: `timer-cancel-sync`
holds the callback-needed lock while a real callback blocks on it, then
verifies the expected report returns from the synchronous wait. Gap: only
observed callback paths and up to 16 locks per callback are represented;
other callback/wait relationships remain unmodeled.

**L16. Remote held-stack diagnostics print only a consistent copy.**
The CPU-local writer masks IRQs and brackets atomic stack changes with a
sequence update. The remote reader makes one bounded attempt, rejects odd
or changing sequences and oversized counts, and prints only after a
successful copy. It takes no target-owned lock and does not retry a stuck
writer. Check: the threaded sanitizer model validates whole generations
and immediate refusal of a stopped writer; `lockdep-order` checks actual
push, irqsave flags, out-of-order removal, and empty-stack publication.
