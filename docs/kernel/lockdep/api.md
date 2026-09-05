# Lock discipline and lockdep: API

All interfaces are kernel-internal unless marked *(exported)*. The checker
exists in debug builds (`CONFIG_LOCKDEP`, which defaults to `CONFIG_DEBUG`);
in release builds every function below except `might_sleep()`'s always-on
check compiles to nothing. Each entry follows constitution section 52.

## Annotations used by ordinary code

### `void spin_lock_nested(spinlock_t *lock, unsigned subclass)` *(exported)*, `arch_irq_state_t spin_lock_irqsave_nested(spinlock_t *lock, unsigned subclass)` *(exported)*, `void mutex_lock_nested(struct mutex *m, unsigned subclass)` *(exported)*
- Purpose: acquire a lock of a class the caller already holds one of,
  declaring the nesting intended. `subclass` is 1..3 (`LOCKDEP_SUBCLASSES`
  is 4); the (class, subclass) pair is a distinct node in the order graph,
  so the acquisition is checked like any other and the same-class
  recursion report is not raised.
- Uses in the tree: `VNODE_NESTED_PARENT2` (the second parent directory of
  a rename), `VNODE_NESTED_CHILD` (a child vnode under its parent:
  `remove_entry`, `ramfs_rename`'s replaced entry). Every use is listed in
  `invariants.md` L5 with its justification; adding one means adding it
  there.
- Behaviour otherwise identical to the plain forms.

### `might_sleep()`
- Purpose: declare that the calling function may block. Reports (debug) or
  panics (release) when `preempt_count != 0` or `irq_depth != 0`.
- Placed in: `mutex_lock`, `waitqueue_prepare` (every `wait_event`),
  `semaphore_down`, `wait_for_completion`, `synchronize_quiesce`,
  `copy_from_user`, `copy_to_user`, `strncpy_from_user`. A new sleeping
  primitive or a new path that can fault on user memory adds it at entry.
- Cost: two loads and a branch when nothing is held.

### `lockdep_assert_held(lock, kind)`, `lockdep_assert_not_held(lock, kind)`
Debug assertions that the calling CPU (spinlocks, `LOCKDEP_KIND_SPIN`) or
thread (mutexes, `LOCKDEP_KIND_MUTEX`) holds / does not hold `lock`. No-ops
in release builds.

## Checker interface (`kernel/include/kernel/lockdep.h`, `kernel/core/lockdep.c`)

### `void lockdep_acquire_check(uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass, bool irqs_on, uintptr_t ip)`
- Called by the lock primitives before the acquisition waits (a
  deadlocking order is reported rather than hung on). Classifies the lock
  on first use (cached in `*class_slot`), records interrupt-safety usage,
  checks recursion and order against the held set, records edges. Pushes
  nothing: a contended lock is not held.
- Concurrency: any context. Takes the checker's raw (untracked) lock only
  to classify or to record new edges.

### `void lockdep_acquired(const void *lock, uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass, bool trylock, bool irqs_on, uintptr_t ip)`
- Called once the lock is owned: pushes the held entry (trylock callers
  use only this half; a trylock cannot deadlock and records no order).

### `void lockdep_release(const void *lock, unsigned kind, uintptr_t ip)`
Removes the entry (not necessarily the top one); a lock that is not held
is a report.

### `bool lockdep_is_held(const void *lock, unsigned kind)`
Whether `lock` is on this CPU's spinlock stack / this thread's mutex stack.

### `void lockdep_thread_exit(struct thread *t)`
Called by `thread_exit`: a thread exiting with a mutex held is a report.

### `void lockdep_dump_held(void)`
Prints both held stacks (class, subclass, acquiring address, `[irq]`,
`[irqs-on]`, `[try]`). `panic()` calls it after the backtrace, so every
crash report shows what the CPU and thread held.

### `void lockdep_dump_graph(void)`
Prints every recorded edge (`kdebug`), `'a'#n -> 'b'#m` meaning b was taken
while a was held. `selftest_run_all` calls it once at the end of the run so
the debug boot log carries the tree's real lock order; `testing.md`
reproduces the interesting part.

### `void lockdep_get_stats(struct lockdep_stats *out)`
Classes, edges, acquisitions, reachability searches, reports.

### `void lockdep_expect(enum lockdep_report_kind kind)`, `unsigned lockdep_expected_hits(void)`, `const char *lockdep_report_name(kind)`
Self-tests only. `lockdep_expect` arms a one-shot expectation: the next
report of `kind` increments a counter and returns instead of panicking,
and the operation proceeds. `lockdep_expected_hits` returns and clears the
counter. Report kinds: `LOCKDEP_R_INVERSION`, `LOCKDEP_R_RECURSION`,
`LOCKDEP_R_IRQ`, `LOCKDEP_R_SLEEP`, `LOCKDEP_R_OVERFLOW`,
`LOCKDEP_R_UNHELD`, `LOCKDEP_R_EXIT_HELD`.

## The core (`kernel/include/kernel/lockdep_core.h`)

Pure inline functions over `struct lockdep_graph` (class table and edge
bitmaps) and `struct lockdep_scratch`, with no kernel dependency:
`lockdep_core_class` (lookup or create; -1 when full),
`lockdep_core_has_edge`, `lockdep_core_add_edge` (true when new),
`lockdep_core_reaches` (breadth-first reachability with the path). The
host test `tests/host/test_lockdep.c` drives them; `lockdep.c` wraps them.

## Structures changed for the checker

`spinlock_t` and `struct mutex` gained `uint16_t class` (+ padding to keep
8-byte alignment); `struct thread` gained the per-thread mutex stack.
Because `spinlock_t` is embedded in exported structures, the module ABI is
**v3**.

## Fixes shipped with the milestone (interfaces that changed)

- `struct mount.lock` is a spinlock (the vnode hash only); `struct mount`
  gained `rename_lock`, `sync_lock`, `unmounted`. `vnode_put` is a function
  (unhash-before-drop; `docs/kernel-services/vfs/api.md`).
- `arch_ipi_bind(vector)` in `kernel/include/arch/irqc.h`: binds an IPI
  vector to its controller resource once, so `arch_ipi_send` takes no lock
  (the run-queue lock is a leaf). Called by `ipi_init` and by any test that
  raises its own IPI vector.
- `futex_wait` copies the user word with no spinlock held and uses the
  bucket's wake sequence to keep the compare-and-enqueue atomic against
  wakers (`docs/compat/linux/design.md`, "futex").
