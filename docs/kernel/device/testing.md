# Device infrastructure: testing

## What runs where

| Level | What | Command |
|---|---|---|
| Target self-tests (debug builds, 4 CPUs) | `device`, `pci`, `dma`, `blk-lifetime`, `fault-blk`, `random`, `blk`, `virtio-console` | `make test` |
| Host fuzz | `fuzz_virtq`: the split virtqueue driven by a program-generated hostile device (`make fuzz`, `docs/verification/`) | `make fuzz` |
| Boot markers | module load lines for the four driver modules, the `blk: vda:` line, the virtio-console registration line, and the `boot complete` line **inside the virtio console file** | `make test` |
| Release | same drivers, no self-tests | `make BUILD=release test` |
| Single CPU | MSI vectors and completions on one CPU | `QEMU_SMP=1 make test` |
| Without an IOMMU | every driver on the identity DMA path | `QEMU_IOMMU=0 make test` |
| Build-time | every driver module is signed and checked (`check-module-elf.py`) | `make modules` |

The host fuzz target `fuzz_virtq` (`tests/fuzz/fuzz_virtq.c`) is the
subsystem's host test: the input is a program of operations (add a chain,
the device completes a used element with any id and length, pop, rewrite
a descriptor, jump the used index, scribble the available ring) and the
target asserts the driver only ever pops cookies it added and has not
reclaimed and never counts more free descriptors than the queue holds.
`fault-blk` (`kernel/core/faulttest.c`) injects `-EIO` at `blk_submit`
and at completion under a cosmofs workload on a RAM block device
(`kernel/block/ramblk.c`, debug builds: a registered block device backed
by kernel memory with a write recorder and snapshots for the
crash-consistency harness).

## The QEMU configuration the tests assume

`scripts/qemu-run.sh` attaches, after the firmware and AHCI boot disk:

- `virtio-blk-pci` backed by `QEMU_TESTDISK` (the harness creates a
  fresh 8 MiB zero file `boot-test.log.testdisk.img` per run, so the
  `blk` test's writes never persist between runs; since Phase 7 the
  `cosmofs-*` self-tests format this disk and leave a filesystem that
  `init --selftest` mounts, see `docs/kernel-services/vfs/testing.md`);
- `virtio-rng-pci`;
- `virtio-serial-pci` with one `virtconsole` whose `chardev` is a file,
  `QEMU_VCON` (`boot-test.log.vcon` under the harness);
- since Phase 8, `virtio-net-pci` on QEMU user-mode networking
  (`docs/kernel-services/network/testing.md`);
- since milestone 9, `nvme` (`serial=cosmo-nvme0`) over `QEMU_NVMEDISK`
  (a fresh 8 MiB `boot-test.log.nvme.img` per harness run;
  `docs/drivers/nvme/testing.md`);
- since the virtio-removal unit, **last of all**, a second
  `virtio-blk-pci` over `QEMU_RMDISK` (a fresh **4 MiB**
  `boot-test.log.rmdisk.img` per harness run): the disk
  `virtio-remove-inflight` removes and re-probes. It is attached after
  every function above so that none of them moves, which makes it
  `vdb` on q35 and `vdc` on `virt` (where the boot image is a
  virtio-blk too) — so the test finds it by its capacity, which no
  other disk in the machine has, rather than by a name that differs
  between the two machines. `QEMU_RMDISK=0` leaves it out and the test
  skips.

Under q35 the functions are, in enumeration order: host bridge
(`00:00.0`), VGA (`00:01.0`), `virtio-blk` for the scratch disk
(`00:02.0`), `nvme` (`00:03.0`), `virtio-rng` (`00:04.0`),
`virtio-serial` (`00:05.0`), `virtio-net` (`00:06.0`), `e1000e`
(`00:07.0`), `xhci` (`00:08.0`), `virtio-blk` for the removal disk
(`00:09.0`), plus the ISA bridge, AHCI and SMBus of the chipset. (This
paragraph said "9 functions, the four virtio functions at
`00:02.0`–`00:05.0`" from Phase 8 until the virtio-removal unit; NVMe
had taken `00:03.0` at milestone 9 and the USB and second NIC came
later, so it had been describing an older machine for some time. The
removal disk's own arrival moved nothing.) The tests do not depend on
the VGA beyond enumerating it.

## Self-tests (`kernel/device/devtest.c`)

`device`: registers a synthetic bus `selftest` with a name-matching
`match`; checks `device_setup` defaults (32-bit mask, unbound), resource
add/lookup, device-then-driver probing (`driver_register` probes the
existing `fake0`, not `fake1`), duplicate registration (`-EEXIST` for
both device and driver), `device_find` reference counting (3 = init +
bus + find), `device_for_each` count, `driver_unregister` unbinding
(`remove` called once, `drvdata` cleared), driver-then-device probing
with a failing probe (`DEV_FAILED`, `probe_error == -EIO`, no `driver`),
recovery to `DEV_UNBOUND` on driver removal, `device_unregister` back to
one reference, and `bus_find` for both the synthetic and the `pci` bus.

`pci`: at least one device; index 0 is the host bridge (`00:00.0`, class
`06.00`, named `pci:00:00.0`, on `pci_bus`, findable through
`device_find`); for every device the 8/16/32-bit config accessors agree
on the id dword, the vendor is not `0xffff`, every implemented BAR has a
power-of-two size and is aligned to it, `pci_find_capability` returns
the cached MSI/MSI-X offsets and 0 for an unknown id; `pci_device_at`
past the end and an unknown vendor/device return NULL; every vendor
`1af4` function has MSI-X and a memory BAR. Logs the device count, the
virtio count, and whether ECAM or legacy access is in use.

`dma`: an 8 KiB `DMA_ZERO` allocation is page aligned, below 4 GiB,
zeroed, in the direct map, and maps to its own address (whole and at an
offset); `dma_set_mask` rejects 23 and 65; a 24-bit device's allocation
lands below 16 MiB; a `kmalloc` buffer maps to `virt_to_phys`; a kernel
arena page, a stack address, and a NULL range map to 0; the statistics
show exactly two allocations, two frees, three maps, three failures,
and no outstanding bytes.

`random`: two `random_u64` results differ; 64 bytes are not all zero;
crediting keeps the count within 512; when a virtio-rng function is
present, `random_source_bytes() > 0` (virtio_rng has fed the pool by
the time self-tests run) and the counts are logged.

`blk`: skipped with a log line if no `vda` exists. Otherwise: geometry
sanity (512-byte sectors, at least 2048 of them, `max_sectors` in
[8, 1024]); a write of `max_sectors + 1` sectors at sector 1000 followed
by a read compares equal (each helper call splits into two bios); the
middle sector is overwritten and the whole span re-read; `blk_flush`
succeeds; a read at `capacity`, a read crossing `capacity`, a zero-sector
read, a read into a stack buffer, and a bio without `done` are all
refused with `-EINVAL` and complete nothing; the statistics show exactly
4 reads, 3 writes, 0 errors more than before.

`virtio-console`: skipped if no virtio console function exists;
otherwise `console_has_sink("virtio-console")` is true and an unknown
name is false.

Every test restores what it changed: the synthetic bus stays registered
(static, empty), test buffers are freed, `blk_find`'s reference is
dropped.

### `blk-lifetime`

A synthetic block device (`kernel/device/devtest.c`): `blk_register`
without `ops->release` is `-EINVAL`; with it the refcount is 2 (creator +
registry), 3 after `blk_find`; a bio completes through the fake `submit`;
`blk_unregister` removes it from the registry, drops the registry's
reference, sets `gone`, and `blk_submit` returns `-ENODEV` without
reaching the driver; the creator's `blkdev_put` does not run the release
while the finder holds it; the finder's put runs it exactly once
(`docs/kernel/quiesce/invariants.md` Q9–Q11).

### `blk-queue`

`kernel/block/blktest.c`, on the RAM block device in deferred mode
(`ramblk_set_deferred`): the pending queue and the bio flags
(`docs/kernel-services/vfs/testing.md`).

### `blk-segments`

`kernel/block/blktest.c`: a three-segment write (half a page from
mid-page, a whole page, half a page) on the RAM device reads back flat
and byte-exact, then back into two whole-page segments; a middle segment
not ending on a page, a later one not starting on one, a wrong total,
nine segments against `max_segments` 8, and a stack buffer are each
`-EINVAL`.

### `blk-timeout`

`kernel/block/blktest.c`: the RAM device in deferred mode with
`ramblk_set_stall` (the worker completes nothing) and a 300 ms
`timeout_ns`: `blk_read` returns `-ETIMEDOUT` between 300 ms and 3 s,
`timeouts` grew by one (the `blk-timeout` thread called the driver's
`timeout`, which completed the bio), and after the stall is lifted a read
completes.

### `nvme`

`kernel/device/devtest.c`, on `nvme0n1` through the block layer only:
`docs/drivers/nvme/testing.md`.

### `dma` (milestone 9 additions)

`dma_unmap` counts, `dma_mappable` agrees with `dma_map`, and sixteen
reads plus a write and a flush on `vda` leave `maps − unmaps` where it
was. `dma_stats.leaked` stays zero: it counts frames an IOMMU refused
to confirm the revocation of. Since the IOMMU unit the same balance is what keeps a device's
domain from filling up: the `iommu` self-test
(`docs/kernel/iommu/testing.md`) covers the mapped path of the same
API, and every test in this file now runs with the devices translating.

## Boot markers (`tests/boot/run_boot_test.py`)

Required in the serial log on every normal run:

```text
[ INFO] module: loaded virtio 1.0
[ INFO] module: loaded virtio_blk 1.0
[ INFO] module: loaded virtio_rng 1.0
[ INFO] module: loaded virtio_console 1.0
[ INFO] blk: vda: 16384 sectors of 512 bytes
[ INFO] virtio-console: virtioN: registered as a console sink
[ INFO] module: loaded nvme 1.0
[ INFO] blk: nvme0n1: 16384 sectors of 512 bytes
[ INFO] nvme0: ... 1 namespace(s) of N, Q I/O queue(s) of depth 32
[ INFO] iommu: intel-vtd0 at 0xfed90000: ...; translation on
[ INFO] iommu: intel-vtd0: pci:00:02.0 (requester 0010) in domain 1
```

(the two `iommu:` lines only when the machine has a unit, i.e. not
under `QEMU_IOMMU=0`)

and, read from `QEMU_VCON` after QEMU exits, `[ INFO] boot complete`:
the console output really went through the virtqueue. The panic run
(`make test-crash`) does not check the console file.

## Running by hand

```sh
make test                              # everything above
QEMU_SMP=1 make test                   # one CPU
make BUILD=release test                # drivers without self-tests
make run                               # interactive; virtio console goes to out/<arch>-<build>/vcon.log
QEMU_TESTDISK=/tmp/disk.img make run   # keep a scratch disk between runs
```

`device_dump()`, `blk_dump()`, `module_dump()` print the model, the
block registry and the module list; they are not wired to a command yet.

### `virtio-remove-inflight`

`docs/audit/next-subsystem-virtio-remove-inflight.md`. The window the
lifetime-windows unit named and narrowed: `vpci_remove` — which only a
module unload had ever run — with requests at the device. Debug builds,
on the removal disk above; skips on one CPU, without the disk, or if
`virtio_blk` published no test seams.

A submitter on another CPU reads the disk continuously from a pool of
96 bios (more than the driver's slots, so the block layer's pending
list is exercised too). The window is held open by construction rather
than by a stopwatch: a driver hook makes `vblk_done` return without
consuming anything, so requests the device has finished stay in the
slot table and `vblk_remove` finds them. Then `pci_test_remove`, and
the assertions are about the protected object:

- every accepted bio completes exactly **once**, with one of three
  statuses and nothing else — `0`, `-EIO` (a slot the remove found,
  completed by its leftover walk) or `-ENODEV` (queued on the pending
  list behind a full table, completed by `blk_unregister`);
- the `-EIO` count **equals** what the remove found in flight: no
  double completion, no stranded slot;
- no completion is stamped after the removal's own boundary stamp,
  which `vblk_remove` takes at the end of its leftover walk from
  `blk_test_tick`'s sequence — inside the removal, because a stamp the
  caller takes after `pci_test_remove` returns can be beaten by a
  callback on another CPU that completes later and numbers itself
  earlier;
- the disk is off the registry, the function is `DEV_UNBOUND` with no
  `driver` and no `drvdata`, and the release runs when the test drops
  the last reference and not before;
- then `pci_test_rebind` brings it back: same name, and the first
  sector still holds what was written before the removal. That is the
  assertion that the removal left the hardware sane — status 0, MSI-X
  disabled, BARs unmapped — and the reason the test leaves the machine
  as it found it.

**A third pass, and the defect it belongs to.** Review of the first
build found that `vblk_remove` read and cleared `vb->inflight` with no
lock while `vblk_done` touches it only under `vb->lock` — and, worse,
that nothing waited for a completion handler at all: the reset stops
the device, not a handler already inside the driver, and the removal
went on to free the virtqueue and the DMA pool. The fix is invariant
**Q11b** (`docs/kernel/quiesce/invariants.md`), and the third pass is
its adversary: a real completion walk is parked *inside* the driver and
the removal must wait for it. As run, the walk parks on cpu0 — where
every MSI-X vector lands — and the removal spins tens to hundreds of
times before proceeding, on both architectures.

Getting that pass to be deterministic took four rounds, and each cause
is worth knowing for the next test of this shape: the submitter and the
removal shared a CPU, so the removal outlasted the park; the park was
armed before the removing thread was created, and creating a thread can
need a TLB shootdown that a parked walk (interrupt context, interrupts
off) cannot acknowledge; the removal drained last, so the park had to
outlast a whole prologue; and once the drain went first the window
became too short for an interrupt to land in. It now arms from the
removing thread, waits there for a walk to be inside, and removes.

The first two passes run twice, held and unheld. **The unheld pass finds
0 requests in flight at the remove, every run, on both architectures**
— a QEMU device answers in microseconds — which is the measurement
that says the hook is necessary and the unheld pass is a regression
guard rather than a proof. Held, x86-64: 89 accepted, 64 found in
flight, 64 `-EIO`, 12 `-ENODEV`, 13 `0`; AArch64: 79, 64, 64, 10, 5.
The driver's seams reach the test through a hook table the module
publishes to the block layer at its init
(`blk_test_driver_hooks_set`), because the kernel image cannot name a
module's symbols.

## Gaps

- No host unit test for the virtqueue ring logic (`virtq_add`/`virtq_pop`
  are pure enough for one with a fake transport); the target tests cover
  it only through real I/O.
- No test unloads a driver *module* with requests in flight (a device
  is removed with them outstanding — `virtio-remove-inflight` — but the
  module stays loaded), unregisters the console sink while other CPUs
  log (invariant D12's gap), or exercises `pci_msi_enable` (every QEMU
  virtio device has MSI-X).
- No PCI hot-plug: the removal test drives `pci_test_remove` and
  `pci_test_rebind`, which are the model's own transitions, not an
  event from the machine (`device_del` over QMP would need a rescan and
  a hotplug interrupt this kernel does not have). No legacy
  configuration access under test (q35 always has an MCFG), no test of
  a probe failure inside a real driver.
- `virtio_net` is not driven; the transport handles the device but no
  driver binds it.
