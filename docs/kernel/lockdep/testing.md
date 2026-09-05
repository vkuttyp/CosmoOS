# Lock discipline and lockdep: testing

## What runs where

| Level | What | Command |
|---|---|---|
| Every debug boot | the checker runs on every acquisition through boot, all self-tests, the userland test script and the network harness; any report is a panic, so `make test` fails | `make test`, `QEMU_SMP=1 make test`, `make ARCH=aarch64 test` |
| Host (ASan/UBSan) | `test_lockdep`: the class table, edges and reachability, the decision procedure on an ABBA, a three-lock cycle and subclass nodes | `make host-test` |
| Target self-tests (debug builds) | `lockdep-order`, `lockdep-recursion`, `lockdep-irq`, `lockdep-sleep`, `lockdep-mutex` (the detector fires on constructed violations); `vfs-concurrency` (the fixed races, on two CPUs) | `make test` |
| Release | the checker is compiled out; `might_sleep`'s panic half stays | `make BUILD=release test` |

Every test above ran and passed on x86-64 (4 CPUs and 1 CPU) and AArch64 (4
CPUs) when this milestone was verified. The debug boot test takes 16.4 s on
x86-64 against 15.5 s before the checker (about 100 000 checked
acquisitions per boot), 18.8 s against 17.8 s on AArch64.

## Host test (`tests/host/test_lockdep.c`)

| Case | What it checks |
|---|---|
| `classes` | one name is one class; the same name with the other kind is another; the table fills to `LOCKDEP_MAX_CLASSES` and then returns -1; node arithmetic at both ends |
| `edges-and-cycles` | add and duplicate edges; reachability direct and through a chain with the returned path; an unconnected node; a 21-node chain truncated to the last 8 entries; the highest node |
| `decision` | the kernel's rule replayed: an ABBA is refused, a three-lock cycle is refused through the chain, edges are added from every held lock, subclass nodes are distinct |

## Checker self-tests (`kernel/core/lockdeptest.c`)

Each arms one expectation with `lockdep_expect`, provokes the violation on
private locks, and checks that exactly one report of that kind was counted
(`lockdep_expected_hits`); the operation proceeds, so every lock is
released normally.

| Test | Steps | Proves |
|---|---|---|
| `lockdep-order` | A → B twice (the second time runs no search: the edge is known); B → A is an inversion; releasing out of order is legal; `lockdep_is_held` tracks the stack | L1, L11 |
| `lockdep-recursion` | two spinlocks from one init site nested is a recursion report; the same with `spin_lock_nested(…, 1)` is silent | L2 |
| `lockdep-irq` | a lock first taken with `spin_lock` and interrupts enabled, then taken inside a real self-IPI handler (`arch_vector_alloc`, `interrupt_register`, `arch_ipi_bind`, `arch_ipi_send`): the interrupt-context acquisition is the IRQ report | L3 |
| `lockdep-sleep` | `might_sleep()` under a spinlock is a report; with nothing held it is silent | L4 |
| `lockdep-mutex` | mutexes M1 → M2 with a spinlock under them is legal; M2 → M1 is an inversion on the per-thread stack; a mutex taken under a spinlock is a sleep report | L1 (mutexes), L4, L11 |

## VFS concurrency (`kernel-services/vfs/vfstest.c`, `vfs-concurrency`)

Two threads pinned to CPU 0 and CPU 1 (or both on CPU 0 with one CPU):

1. For 200 ms one thread renames `/tmp/vc/a/x` into `/tmp/vc/a/b/y` and
   back while the other removes and recreates `/tmp/vc/a/b`. Every result
   is one of the legitimate outcomes (`-ENOENT` when `b` is gone or `y` has
   moved, `-ENOTEMPTY` when `y` is inside `b`, `-EEXIST`); afterwards `x` is
   in exactly one of its two places. Under the old address-order rename
   this was the audit's ABBA against rmdir; under the checker any inversion
   would also have panicked.
2. For 200 ms both threads open and close `/tmp/vc/shared`; afterwards the
   vnode count equals the created files exactly (ramfs pins its vnodes, so
   a duplicate vnode for one inode would show as one extra). Measured:
   1273 rename rounds against 2579 rmdir/mkdir rounds and 14 665 open/close
   calls on x86-64; 494 / 1327 / 18 487 on AArch64.

The test leaves the namespace as it found it (vnode count back to the
start).

## The recorded lock order

`selftest_run_all` ends with `lockdep_dump_graph()`, so every debug boot log
contains the edges the run recorded (`lockdep: edge …`, `a -> b` meaning b
was taken while a was held; 133 classes and 415 edges on x86-64 with 4
CPUs). The parts the documents make claims about:

**Scheduler (S2, S4).** No edge leaves `spin 'runqueue'`: it is a leaf on
both architectures (the AArch64 GIC lock no longer follows it). Its
predecessors are every wait queue (`'sleep'`, `'g_worker_wq'`,
`'thread-exit'`, `'smp_call'`, …), `spin 'process'`, `spin 'tty'`,
`spin 'socket'`, `mutex 'socket'` and the VFS mutexes: what S4 now says.

**VFS (V7).**

```
mutex 'mounts'      -> mutex 'vnode'#0, mutex 'cosmofs', spin 'mount-hash', spin 'mounts'
mutex 'rename'      -> mutex 'vnode'#0, mutex 'vnode'#1, mutex 'pagecache', mutex 'cosmofs', spin 'mount-hash'
mutex 'vnode'#0     -> mutex 'vnode'#1, mutex 'vnode'#2, mutex 'pagecache', mutex 'cosmofs', spin 'mount-hash'
mutex 'vnode'#1     -> mutex 'pagecache', mutex 'cosmofs', spin 'mount-hash'
mutex 'vnode'#2     -> mutex 'cosmofs'
mutex 'file'        -> mutex 'vnode'#0, mutex 'pagecache', mutex 'cosmofs'
mutex 'pagecache'   -> mutex 'cosmofs', spin 'pagecache', spin 'pagecache-stats'
mutex 'mount-sync'  -> mutex 'vnode'#0, mutex 'pagecache', mutex 'cosmofs', spin 'mount-hash'
mutex 'cosmofs'     -> spin 'blk-sync', spin 'virtio-blk', spin 'virtq', spin 'virtio-pci', spin 'mount-hash'
```

`spin 'mount-hash'` has no successor except the allocator locks: a leaf.
Nothing takes a vnode lock under `mount-hash`, `pagecache` or `cosmofs`.

**Network.**

```
mutex 'socket'      -> spin 'tcp', spin 'udp', spin 'arp', spin 'netifs', spin 'net-rxq', spin 'socket',
                       spin 'g_worker_wq', spin 'timer_queue', spin 'random', spin 'virtq', spin 'virtio-pci'
spin 'tcp'          -> spin 'timer_queue', spin 'random'
spin 'udp'          -> spin 'udp-rxq'
mutex 'devices', mutex 'modules' -> spin 'netifs', spin 'netif'
```

The documented order (`sock->lock` → protocol spinlock → `arp`/`nd` →
`netif->lock` → driver → mbuf) is consistent with this: the protocol
spinlocks never nest `arp`, `nd` or `netif` (ARP resolution and
transmission run after the protocol lock is dropped), and the driver locks
appear only under the socket mutex through `transmit`.

To regenerate the lists: `grep 'lockdep: edge' out/x86_64-debug/boot-test.log | sort -u`.

## Running

```sh
make test                          # debug: checker live, 90 self-tests
QEMU_SMP=1 make test
make ARCH=aarch64 test
make host-test                     # test_lockdep among the host tests
make BUILD=release all test        # checker compiled out
```

## Gaps

- The graph covers what the boot runs exercise; a lock order only a
  production workload takes is recorded only when that workload runs on a
  debug build.
- No kernel-level futex race test; the lost-wake argument for `wake_seq` is
  by construction and exercised by the musl tests.
- The `vfs-concurrency` stress found no fault before the fixes were applied
  under the checker's own boots because the checker already refuses the old
  rename order; the pre-fix ABBA was reproduced only by review.
- Trylock acquisitions are pushed but never checked; a trylock cannot
  deadlock, but an order it implies is not recorded either.
