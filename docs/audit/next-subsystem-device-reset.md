# NEXT SUBSYSTEM — the device model has no generic reset operation

> Constitution §68 report. This PR adds the report and the probe
> (`tools/device-reset-probe.py`); the `reset` operation and the virtio
> handler described under "Design" and the edits in "Affected files" are
> planned work that lands in the implementation PR that follows, gated on CI.
> As committed here, a bound device cannot be reset in place.

## Problem

The device model's `struct device_driver` (`kernel/include/kernel/device.h`)
has `match`, `probe` and `remove`, and nothing else: there is no `reset`
operation, and no `device_reset(dev)` entry point. The only way to
re-initialize a bound device is to tear it down and build it again — the
driver's `remove` followed by a fresh `probe` (`device_test_unbind` /
`device_test_bind`, or `pci_test_remove` / `pci_test_rebind`). That path
unregisters the device's higher-level object and registers a new one: a
virtio-blk `remove`+`rebind` produces a **different `blkdev`**, so any handle
or pointer to the old one is stale.

The pieces a reset needs already exist one layer down. `virtio.c` exports
`virtio_device_reset` (status to zero), `virtio_device_init` (reset and
re-negotiate features, up to `FEATURES_OK`) and `virtio_device_ready`
(`DRIVER_OK`); a driver's `probe` strings these together with its own
`virtq_alloc` to build the virtqueues (`virtio_blk.c`: `virtio_device_init`,
then `virtq_alloc`, then `virtio_device_ready`). All the pieces are there —
but reachable only from inside a driver's own `probe` and `remove`, never as
a model operation. There is no path from the device model to "reset this
device in place and leave it bound and registered."

Prompt #2 §41 names "device reset" among the operations to design
(`docs/audit/2026-09-deferred-work-inventory.md`, the device-model row:
"Open: ... a generic device reset operation"), and Prompt #2 §47 names
"device reset" again as a fault-injection point still to build (the
fault-injection row: "open: ... device reset"). Both are open; this unit is
the operation the second one would inject.

### Measured

`tools/device-reset-probe.py` adds a self-test that re-initializes the
removal disk (`QEMU_RMDISK`) the only way the model allows today (one debug
boot, x86-64):

```
DEVRESET: struct device_driver has no reset op; re-initialising vdb needed remove+reprobe, which replaced the blkdev with a different object (not an in-place reset); virtio_device_reset exists but is private to the driver's remove
```

- **No reset operation.** `device_driver` carries no `reset`, and nothing in
  the model calls `virtio_device_reset`.
- **Re-init is remove+reprobe.** The only in-place recovery is
  `pci_test_remove` + `pci_test_rebind`, which runs the driver's `remove` and
  a fresh `probe`. The probe keeps a reference to the old `blkdev` across the
  cycle (`blk_unregister` waits only for in-flight submits, not references),
  so a pointer comparison shows the re-probe produced a **different** object
  under the same name — the device was destroyed and re-created, not reset.

## Why it matters

- **A wedged device cannot be recovered in place.** When a device's hardware
  state is wrong — a fault, a protocol error, a reset injected by §47 — the
  model offers only remove+rebind, which invalidates every reference to the
  device and its higher-level object instead of returning it to a known
  state while keeping its identity.
- **The fault-injection point cannot be built on nothing.** §47's "device
  reset" injection needs a reset operation to inject; there is none. The
  virtio primitive is there but private to each driver's teardown.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| driver ops | `struct device_driver` (`device.h`) | `match`, `probe`, `remove` — no `reset` |
| bind / unbind | `try_bind` / `unbind` (`device.c`) | `probe` on bind, `remove` on unbind |
| re-init path | `pci_test_remove` + `pci_test_rebind`, `device_test_unbind` + `device_test_bind` | full teardown then fresh probe; a new higher-level object |
| the pieces | `virtio_device_reset`/`init`/`ready` (`virtio.c`, exported) + the driver's `virtq_alloc`/`virtq_free` | status to zero; reset and re-negotiate features; `DRIVER_OK`; the driver builds (`probe`) and tears down (`remove`) its queues — strung together only inside a driver's `probe`/`remove` |

## Design

### 1. A `reset` operation on the driver

`struct device_driver` gains `int (*reset)(struct device *dev)`, and the
model gains `int device_reset(struct device *dev)`: under the model lock, if
`dev` is bound and its driver has a `reset`, call it; `-ENODEV` if unbound,
`-EOPNOTSUPP` if the driver has no `reset`. The device stays registered and
bound across the call — only the driver's hardware state is re-initialized.

### 2. The virtio reset handler

`virtio_blk`'s `reset` operates on the **existing** `vblk` and its `blkdev`:
it does **not** `blk_unregister` the disk (as `remove` does), `blk_register`
a new one or re-allocate the driver's private state (as `probe` does). It
re-does only the device-and-queue initialization those two bracket — the part
between `blk_unregister`+teardown in `remove` and the geometry read+
`blk_register` in `probe`. In order
(`docs/audit/next-subsystem-virtio-remove-inflight.md`):

1. Refuse new submissions (the removal unit's barrier).
2. `virtio_device_reset` — the device drops every request in its rings.
3. `virtq_free` the old queue, which also releases its interrupt (and frees
   the slot: `virtq_alloc` on an already-occupied slot 0 is `-EINVAL`).
4. **Only now**, with the queue and its `vblk_done` handler gone, walk the
   driver's slot table and complete each still-outstanding request `-EIO`
   exactly once. Completing them *before* `virtq_free` would race the
   interrupt handler, which could complete the same `bio` twice and unmap a
   slot twice — the hazard `vblk_remove`'s own comment calls out.
5. `virtio_device_init` (reset and re-negotiate the same features),
   `virtq_alloc` into the same `vb->vq` (the queue with the `vblk_done`
   callback), `virtio_device_ready` for `DRIVER_OK`.

The `blkdev` (and the `vblk` behind it) is the same object throughout, still
registered under the same name — callers keep their reference and their
handle stays valid, which is the whole difference from a remove+rebind. The
pre-allocated slot tables (`inflight`, `maps`) are reused and re-cleared, not
re-allocated, since the slot count does not change. `virtio_device_init` on
its own only resets and negotiates; the queue teardown, the completion of
outstanding requests, the re-alloc and `DRIVER_OK` are the driver's steps,
which is why reset is largely per-driver rather than a single bus default (a
bus helper can wrap the init/ready bookends).

### 3. In-flight requests

A reset, like a removal, must not leave a submitted request pointing into a
torn-down queue, nor complete one twice. It follows the removal order
exactly: refuse new requests, `virtio_device_reset` and `virtq_free` the
queue (releasing its interrupt), and *only then* walk the slot table and
complete each outstanding request `-EIO` — once, with the `vblk_done` handler
already gone. This is the one real hazard and it is already solved for
removal.

## Affected files

| file | change |
|---|---|
| `kernel/include/kernel/device.h` | `reset` op on `struct device_driver`; `device_reset` declaration |
| `kernel/device/device.c` | `device_reset(dev)` — bound + has `reset` → call, else `-ENODEV`/`-EOPNOTSUPP` |
| `drivers/virtio/virtio.c` | an optional bus helper wrapping the `virtio_device_init`/`virtio_device_ready` bookends |
| `drivers/virtio/virtio_blk.c` | the block driver's `reset`: refuse new submits, `virtio_device_reset` + `virtq_free` the old queue, complete outstanding `-EIO` once, then `virtio_device_init` + `virtq_alloc` + `virtio_device_ready`, keeping the `blkdev` |
| `docs/kernel/device/*.md` | the reset operation |
| `README.md` | Status entry |

## APIs

`device_reset(struct device *dev)` is a kernel API; a `reset` op is added to
`struct device_driver`. No user ABI changes in this unit. Exposing reset as
a §47 fault-injection knob (an operator or test triggering a reset under
load) is the follow-on the fault-injection row names, and is not built here.

## Tests

Planned for the implementation.

| test | proves |
|---|---|
| `device-reset` (`QEMU_RMDISK`) | a bound virtio-blk is reset in place: the same `blkdev` (same name, same reference) answers a read of a pattern written before the reset; a request held in flight across the reset completes `-EIO`; `device_reset` on an unbound device is `-ENODEV`, and on a driver with no `reset` is `-EOPNOTSUPP` |

**Planned mutations** (each alone, boot confirmed):
- the reset not re-creating the virtqueues: I/O after the reset never
  completes (the queues are dead), so the post-reset read fails.
- the reset not failing in-flight requests: a request held across the reset
  is lost — the test's held bio neither completes nor errors.
- `device_reset` not checking the bound state: a reset of an unbound device
  dispatches through a NULL driver instead of `-ENODEV`.

## Benchmarks

None.

## Risks

- **In-flight teardown** — the one real hazard, and the removal unit already
  solved it (`virtio-remove-inflight`). The reset reuses its quiesce seam
  rather than inventing a second one.
- **Reset racing submission** — `device_reset` holds the model lock and the
  driver quiesces its own queues, as `remove` does; a submit concurrent with
  a reset either precedes the quiesce (and is failed `-EIO`) or follows the
  re-init (and runs on the new queues).
- **Keeping registration across a hardware reset** — the point of the unit
  (an in-place reset, not a remove+rebind), and what makes it more than a
  rename of the existing teardown path.

## Alternatives considered

- **Call remove+rebind a "reset".** It is what exists, and it is not a
  reset: it destroys the device and its higher-level object and builds new
  ones, invalidating every reference. A reset returns the *same* device to a
  known state.
- **A per-driver ad-hoc reset, each exposed its own way.** Then the §47
  fault-injection point and any generic recoverer would have no uniform
  operation to call; the model is the right place for `reset`, beside
  `probe` and `remove`.
