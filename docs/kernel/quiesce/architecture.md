# Kernel object lifetime and quiescence: architecture

The post-roadmap audit (`docs/audit/2026-09-post-roadmap-audit.md`,
section 20) traced eleven HIGH findings in five subsystems to one gap:
objects reachable from interrupt handlers, timer callbacks, the network
worker and other CPUs were freed by code that could not know whether such
a reader was still active. Reference counts existed on some of those
objects but had no owning release (`device`, `blkdev`); others had none
(`netif`, `tcp_pcb`); and for the two most important asynchronous readers,
interrupt handlers and timer callbacks, nothing could say "no invocation
is still running". This subsystem closes that gap with one primitive and
one rule, and converts the objects the audit named.

## Where it sits

```text
 kernel/core/quiesce.c          epoch-based reclamation: read-side sections, synchronize_quiesce,
   kernel/quiesce.h             call_quiesce (deferred free), the quiescent points
        ▲            ▲
        │            └── quiescent points: schedule() (kernel/scheduler/sched.c), the idle loop,
        │                interrupt return (arch trap tails, x86-64 trap.c / aarch64 trap.c)
        │
   kernel/interrupt/interrupt.c   synchronize_irq(): unregister + grace period
   kernel/timer/timer.c           timer_cancel_sync(): cancel + wait for a running callback
   kernel/module/module.c         unload protocol: stop, shutdown, quiesce, wait for objects, free
   kernel/object/object.c         release is the owner; module-owned kobjects are counted
   kernel/device, kernel/block    device/blkdev release frees the driver's container
   kernel-services/network        netif is a kobject; tcp_pcb timers cancelled synchronously;
                                  accept attaches under the lock; UDP wakes under a reference
   drivers/virtio                 remove paths put references instead of freeing
```

The primitive is generic (`kernel/quiesce.h` has no architecture code);
the architectures contribute exactly one call each, `quiesce_note_quiescent()`
at the point where an interrupt returns to a context that holds no
spinlock and had interrupts enabled.

## Purpose

An object may be reclaimed only after every CPU that could still hold a
reference to it has passed through a provably safe quiescent state. The
subsystem provides that guarantee as a callable operation, makes it cheap
for readers (a read-side section is a preemption-disabled region: no
atomic, no barrier beyond what `preempt_disable` already implies), and
gives every asynchronous callback path (interrupt handler, timer, deferred
work) a way to be waited for.

## Responsibilities

- Define the read-side critical section (`quiesce_read_lock/unlock`) and
  the quiescent state, and mark the quiescent points in the scheduler, the
  idle loop and the interrupt-return path of each architecture.
- `synchronize_quiesce()`: a sleeping wait for one grace period over the
  online CPUs, with the memory ordering that makes a reader's last access
  happen-before the reclaimer's first free.
- `call_quiesce()`: deferral of a callback to after a grace period, from
  any context including interrupt handlers, batched through one worker.
- `synchronize_irq()` and `interrupt_unregister_sync()`: a handler that
  was running when it was unregistered has returned before the caller
  continues.
- `timer_cancel_sync()`: after it returns the callback is not running and
  will not run.
- The kobject rule: memory reachable from an interrupt handler, a timer
  callback or another CPU without a lock is freed only from a kobject
  release or a `call_quiesce` callback. `device`, `blkdev`, `virtio_device`,
  `pci_device` and `netif` gain releases that own their containers.
- Module unload: stop new users, `shutdown()`, one grace period, wait for
  the module's live kobjects to drain (bounded), then free.
- Debug instrumentation: reader depth, quiescent transitions, epoch
  transitions, synchronize callers and wait times, the CPU that delays a
  grace period, IRQ and timer synchronisation waits, objects per module.

## Non-responsibilities

- Replacing reference counting or mutexes where they suffice. Quiescence
  is used where a lock-free reader exists (interrupt slot table, module
  list, netif registry readers) or where a callback must be waited for.
  Every user is listed in `design.md` with what it protects and why a
  reference count alone was not enough.
- A general RCU API (`rcu_dereference`, `rcu_assign_pointer`, list
  helpers). Publication uses `__atomic_store_n(..., RELEASE)` on the
  pointer and `__atomic_load_n(..., ACQUIRE)` at the reader, written out.
- Lock-order checking (a separate milestone), uaccess fixups, region
  split/merge, or any change to the scheduler's policy.
- CPU hotplug. The design accounts for the online mask at the start of a
  grace period and marks a CPU quiescent when it comes online; taking a
  CPU offline is future work and the `design.md` says what it must do.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `quiesce_read_lock/unlock`, `synchronize_quiesce`, `call_quiesce`, `quiesce_note_quiescent`, `quiesce_stats` | `kernel/quiesce.h` | interrupt, timer, module, netif, tests, arch trap tails |
| `interrupt_unregister_sync`, `synchronize_irq` | `kernel/interrupt.h` | `irq_release`, `irq_release_msi`, drivers |
| `timer_cancel_sync` | `kernel/timer.h` | TCP pcbs, tests |
| `struct device.release`, `struct blkdev.release`, `struct netif` as a kobject | `kernel/device.h`, `kernel/blk.h`, `kernel/netif.h` | drivers |
| `module_unload` protocol, `module_owner_of` | `kernel/module.h` | module loader, kobject core |

Tests: host `test_quiesce` (a simulated multi-CPU epoch model), kernel
self-tests `quiesce-grace`, `quiesce-call`, `irq-sync`, `timer-cancel-sync`,
`quiesce-stress`, `net-accept-race`, `module-unload-busy`; details in
`testing.md`.
