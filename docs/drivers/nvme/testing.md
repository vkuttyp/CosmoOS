# NVMe driver: testing

## The QEMU configuration

`scripts/qemu-run.sh` attaches `-device nvme,drive=nvme0,serial=cosmo-nvme0`
over a raw 8 MiB file (`QEMU_NVMEDISK`, a fresh
`boot-test.log.nvme.img` per harness run) on both `q35` and `virt`. The
controller reports 256 namespace slots, one active, `MQES` 2047, MSI-X
with 65 vectors; the driver creates one queue per CPU (four in the
default run, one with `QEMU_SMP=1`).

## Boot markers (`tests/boot/run_boot_test.py`)

```text
[ INFO] module: loaded nvme 1.0
[ INFO] blk: nvme0n1: 16384 sectors of 512 bytes
[ INFO] nvme0: ... 1 namespace(s) of N, Q I/O queue(s) of depth 32
```

## Self-test `nvme` (`kernel/device/devtest.c`)

Skips with a log line when `nvme0n1` is absent. Otherwise, through the
block layer only:

- geometry: 512-byte sectors, 16384 of them, at least one queue, at
  least eight segments, `max_sectors >= 64`;
- 64 KiB written and read back byte for byte, a flush, a read past the
  end refused;
- a two-segment, four-page write (PRP1 plus a PRP list) read back flat
  and compared segment by segment, completed through a `done` callback
  from interrupt context;
- one thread pinned to each online CPU issuing eight reads: all succeed,
  the completions total `8 × CPUs`, and with a queue per CPU at least
  90 % complete on the issuing CPU (`completed_local`);
- `dma_stats.maps − unmaps` unchanged by all of the above while `maps`
  grew;
- cosmofs: `mkdir /mnt-nvme`, format, mount, write 12 000 bytes to a
  file, `fsync`, unmount, mount again, read back and compare, unmount,
  `rmdir`.

Log: `selftest: nvme: 4 queue(s); 32 of 32 completions on the issuing
CPU; cosmofs mounted and read back`.

## Related tests

`blk-segments` and `blk-timeout` (`docs/kernel/device/testing.md`)
cover the block-layer mechanisms the driver relies on; `dma` covers the
map/unmap balance on the virtio disk. The `iommu` self-test
(`docs/kernel/iommu/testing.md`) drives the controller through the
driver's `ops->debug_dma` hook — an Identify Controller aimed at an
address in the controller's domain that nothing maps — to prove the
unit refuses a DMA the driver did not ask for, and that the controller
still serves a read afterwards.

## Gaps

- No timeout, abort or reset under QEMU (its controller answers).
- No removal with I/O in flight; the module is not unloaded by a test.
- No second controller, no second namespace, no 4 KiB-block namespace.
