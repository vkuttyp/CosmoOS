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
| Network removal: completed and outstanding buffer ownership, map balance, RX accounting, reset-time callbacks | `vnet-remove-pending`, `vnet-remove-late`: debug synthetic transport over real queues, DMA and irq_poll; `tools/vnet-remove-probe.py --old --arch=<arch>` restores main's removal body in a throwaway worktree |
| Persistent RNG callback after reset | `VRNG-RESET-REPOST`: `vrng_completed` reaches the real post gate after the stopping boundary; `tools/virtio-rng-repost-probe.py --old --arch=<arch>` restores the old gate in a throwaway worktree |
| Legacy transport through an I/O BAR, the legacy net header (`MRG_RXBUF`), a 1024-entry legacy queue | x86-64 `make test-smp2`: a legacy-only virtio-net with a 1024-entry receive queue (`QEMU_VNET_OPTS=,disable-modern=on,rx_queue_size=1024`) is eth0 and the whole network suite runs over it; the harness requires `virtio-pci: ...: virtio device 1 as virtioN (N MSI-X vectors; legacy, registers in I/O space)` |
| Legacy transport through a memory BAR | Parallels Desktop 27 only (its network adapter): DHCP, DNS and `wget` on the live and the installed system (`docs/audit/2026-10-13-parallels-devices-report.md`); no QEMU device puts legacy registers in memory |
| virtio-gpu: display info, a resource in two backing chunks, scanout, transfer and flush, the console moved onto it | `make BUILD=release test-display` (`tests/boot/display_test.py`; CI on both architectures): a `virtio-gpu-pci` at 1920x1080, alone and behind the firmware's framebuffer (Parallels' shape), read back from QEMU's `screendump` as text with the kernel's own face; on x86-64 also the crash-test kernel's panic on the screen (the panic flush), and a debug `CRASH_TEST=3` kernel's, raised with lockdep's own lock owned by another CPU (the panic reports the owner word read back, which must name another CPU). The marker is cut to the screen's columns; `DISPLAY_SIZE=640x480` passes, and below that QEMU's firmware prints nothing before the loader on either architecture (400x300 on x86-64 too), so no smaller size is tested. The surface switch a resize makes: selftest `fb-switch` (`docs/kernel/diagnostics/testing.md`) |
| virtio-gpu display changes (`VIRTIO_GPU_EVENT_DISPLAY`) | `make BUILD=release test-display`'s check f, both architectures: a VNC client on the GPU's display (`-vnc unix:...,display=gpu0`) sends SetDesktopSize, which QEMU forwards to the device as the display event; the driver must follow it to 1024x768 and the new screen must read back with the marker under its command (with the event ignored, "the driver did not follow the display to 1024x768"). Parallels Desktop: resizing the VM's window moved the console to 1407x768, 1129x768 and back to 1024x768 (`docs/audit/2026-10-14-virtio-gpu-report.md`) |
| Module dependency handling | boot: the four drivers declare `deps = "virtio"` and load after it |
| Split-ring logic against a hostile device | `tests/host/test_virtq.c` (`make host-test`, ASan/UBSan): the real `virtqueue.c` over a fake transport; the test acts as the peer and writes descriptor links that self-loop, form a two-element loop, point out of range or at a free descriptor; used elements with an id out of range, never posted, not a head, completed twice; lengths beyond the buffers, a legacy device's length on a read-only chain ignored (`virtq-readonly-chain-length`), legacy queues of the device's size up to 1024 (`virtq-legacy-queue-size`); empty, oversized, full-table (boundary descriptor) and normal multi-descriptor chains; 500 rounds of out-of-order completions |

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

- No negative tests on the target: a legacy device without
  `MRG_RXBUF` or with a queue larger than 1024, a legacy device in I/O
  space on AArch64, feature rejection, notify offsets outside the window, probe failure inside a
  driver, console dead state (the ring logic itself is covered on the
  host by `test_virtq`). ~~Unload with requests in flight~~ is covered
  for virtio-blk since the virtio-removal unit:
  `virtio-remove-inflight` removes a live device with its slot table
  full and re-probes it (`docs/kernel/device/testing.md`); what remains
  is the same for the *module* rather than the device.
- Network removal tests cover held RX/TX completions and a two-segment TX
  chain, including RX buffers with no used entry. They do not inject
  probe failure, park an active worker on another CPU, or unload the module.
- Indirect descriptors and event index are never negotiated and so never tested.
- MSI affinity: every vector targets CPU 0.
