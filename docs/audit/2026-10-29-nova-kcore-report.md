# Nova's kernel library in a debug boot, 2026-10-29

Roadmap 2.0's Nova prerequisites N1 (global and per-CPU state) and N3 (a
kernel library) are Nova's M14 K1 and K3. Both shipped in novac 0.2.0,
pinned here since #488. K3's acceptance is that the library builds in
this tree and its self-tests pass in a debug boot on both architectures.
This PR is that test.

## Scope and baseline

Branch `nova-kcore` from main at `9be071c4` (#488).

## Audit (main at `9be071c4`, read, not copied)

| Item | Where | What it means |
|---|---|---|
| Each port is compiled alone, freestanding, without floats | `scripts/nova-gen.py` | the library must be asked for per port |
| `SPINLOCK_INIT(name)` has no self-pointers | `kernel/include/kernel/spinlock.h:39` | a spin lock can be a compile-time static |
| A mutex's wait list points to itself; there is no `MUTEX_INIT` | `kernel/include/kernel/mutex.h`, `wait.h` (`WAITQUEUE_INIT(name)`) | no static mutex: a C constant initialiser has no name for itself (Nova ADR 0025 §7) |
| `arch_irq_save`/`restore` inline to `arch_irq_*_hw`, and `preempt_enable` is not exported | `kernel/include/kernel/arch.h`, `ksym` exports | `IrqGuard`, `PreemptGuard`, `PerCpu` only in code built into the kernel |
| Lockdep keys a class by name and kind; the table holds `LOCKDEP_MAX_CLASSES` | `kernel/include/kernel/lockdep_core.h` | every new lock name is a class |
| A debug boot on main peaks at about 500 of 512 classes and warns at 90% | `lockdep.c` (`class table peak`) | the table was already nearly full |

## What changed

- **The test, in Nova** (`kernel/nova/kcore_test.nova`, 153 lines;
  generated `kcore_test.c`). It returns 0 or the number of the first
  failed check, and `kcore_selftest.c` names it as the self-test
  `nova-kcore`. The checks:
  - a table under a static `SpinLock`, built at compile time with
    `SPINLOCK_INIT`: two inserts, their slots and values;
  - a static `IrqSpinLock`;
  - this CPU's `PerCpu<u64>` counter, lent under an `IrqGuard`, twice;
  - a `SpinLock` and a `Mutex` in kernel-heap boxes;
  - a `KRef` whose clone takes a count, and whose last drop releases the
    object, dropping its data once (counted under a third static lock);
  - `Quiesce.synchronize()`, and a `deferDrop` of a value never published.
- **`scripts/nova-gen.py`**: each `PORTS` line says whether the port uses
  the library; that port is built with `novac --kernel cosmoos`. The
  runtime it generates is the same as the other ports'.
- **Lockdep's class table, 512 → 640.** With the test's classes the boot
  peaked at 506 (AArch64) and 507 (x86-64). On x86-64 a lock taken later
  in the boot, lxtest's `sigsuspend`, then found the table full: "KERNEL
  PANIC: lockdep: held-lock stack overflow ... lock class table full".
  Main was already past the 90% warning, so any new lock would have done
  the same. At 640 both boots peak at 79%. The lock-order matrix grows
  from 2,560² to 3,200² bits (0.82 MB to 1.28 MB).
- **`lockdep-graph-bench`'s budget, 20 s → 30 s.** Its sizes follow the
  table: in local boots it took 3.2 → 4.8 s (AArch64) and 3.4 → 5.1 s
  (x86-64). The budget keeps its margin over CI's slowest figure scaled
  by that 1.5×.
- **The analyzer baseline** (`tools/analysis/*.json`): CI's static
  analysis found four "Potential memory leak" reports in `kcore_test.c`,
  one per `Box.create` in a `KernelHeap` (`nvB_…` functions). Each block
  leaves as the `Ok` payload of a `Result` returned by value, which the
  analyzer stops tracking; the caller's box frees it on drop, and the
  test creates and drops each. They are baselined with that reason, for
  both compilers and both architectures.
- `docs/build/nova.md` describes the library; roadmap 2.0's N1, N3 and N6
  rows say what is done.

## Tests (run 2026-10-29, macOS host, QEMU)

| Test | Result |
|---|---|
| `make nova-check` (novac 0.2.0) | 7 generated files match |
| `gmake ARCH=aarch64 test` (debug: lockdep on) | PASS in 197.5 s; `nova-kcore ... ok (3 ms)`; 523 self-tests; class peak 506 of 640 |
| `gmake ARCH=x86_64 test` | PASS in 193.6 s; `nova-kcore ... ok (2 ms)`; 523 self-tests; class peak 507 of 640 |
| the same at 512 classes (control) | AArch64 PASS; x86-64 FAIL: the class-table overflow above |
| `gmake host-test` | PASS |
| `gmake check-docs` | PASS |
| `gmake analyze`, both architectures | clean after the baseline (11 baselined, 0 unexpected) |

No lockdep report names a Nova lock. The test leaves its table empty and
its boxes freed. Its static locks, the per-CPU counter and the drop count
keep their values, which no later test reads.

## Not covered

- Concurrency: the test runs on one thread. Nova's C7 stress test (Nova
  `integrations/cosmoos/c7.py`) took these locks from four CPUs under
  lockdep as a module. Converted subsystems (SA2, SA3) will do it in tree.
- `ObjectCache<T>` and a static `Mutex` do not exist yet (Nova ADR 0025 §7).
- Nova's inline atomics (Nova ADR 0026) are on Nova's main and come with
  the next novac.
