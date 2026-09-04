# VirtIO: testing

## What exercises the stack

| Path | Exercised by |
|---|---|
| Capability walk, BAR mapping, MSI-X (config + queue vectors), device id mapping | every boot: three virtio functions bind to `virtio-pci` |
| Initialisation sequence, feature negotiation | every boot: `virtio: virtioN: id X, features offered ..., negotiated ...` |
| Interrupt-driven virtqueue (add/kick/pop through an MSI-X vector) | `virtio_blk` I/O in the `blk` self-test; `virtio_rng` completions before the `random` self-test |
| Polled virtqueue | `virtio_console` transmit on every log line; the harness reads the result back |
| Block request format, slot pool, splitting | `blk` self-test: `max_sectors + 1` sector transfers, overwrite, flush, statistics |
| Entropy feed and budget | `random` self-test: `random_source_bytes() > 0` (4104 bytes reach the pool on a normal boot: 64 × 65 completions) |
| Console sink registration | `virtio-console` self-test and the boot marker `virtio-console: virtioN: registered as a console sink` |
| Network device: receive posting, transmit chains, header handling | Phase 8 `net-arp` (ARP through `eth0`) and `net-harness` (TCP and UDP echo with the host over QEMU user-mode networking); boot marker `virtio-net: virtioN is eth0` |
| Module dependency handling | boot: the four drivers declare `deps = "virtio"` and load after it |

Details of the self-tests are in `docs/kernel/device/testing.md`.

## Boot markers

`tests/boot/run_boot_test.py` requires the module load lines for
`virtio`, `virtio_blk`, `virtio_rng`, `virtio_console`, `virtio_net`,
the `blk: vda: 16384 sectors of 512 bytes` line, the `net: eth0
registered` line, the console sink line, and the `boot
complete` line inside the virtio console output file
(`boot-test.log.vcon`). A regression that silently breaks notification
or the used ring therefore fails the boot test even in release builds,
where self-tests are off.

## Running

```sh
make test                         # debug, 4 CPUs
make BUILD=release test           # drivers without self-tests, markers still checked
QEMU_SMP=1 make test
make run                          # watch the [DEBUG] virtio: lines; console copy in out/<arch>-<build>/vcon.log
```

To try a different device set, append to `QEMU_EXTRA`, for example
`QEMU_EXTRA="-device virtio-net-pci"`: the transport binds it
(`virtio device 1 as virtioN`), no driver claims it, and it stays
`unbound` in `device_dump()`.

## Gaps

- No host unit test of `virtqueue.c` with a fake transport (the ring
  code is host-compilable; this is the next test to add).
- No negative tests: legacy-only device, feature rejection, hostile used
  ring, notify offsets outside the window, probe failure inside a
  driver, unload with requests in flight, console dead state.
- `virtio_net` has no driver; indirect descriptors and event index are
  never negotiated and so never tested.
- MSI affinity: every vector targets CPU 0.
