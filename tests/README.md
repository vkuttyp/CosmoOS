# tests

Host unit tests, kernel integration tests, QEMU boot tests, property and fuzz tests. Every subsystem must have tests.

- `host/`: native ASan/UBSan tests of kernel algorithms (`make host-test`):
  `test_buddy`, `test_slab`, `test_crypto` (SHA-512 and Ed25519 vectors),
  `test_modelf` (module ELF validation on synthetic images).
- `boot/`: the QEMU boot-test harness (`make test`, `make test-crash`).
  Since Phase 6 it creates a fresh 8 MiB scratch disk
  (`boot-test.log.testdisk.img`) for the virtio-blk self-test, routes
  the virtio console to `boot-test.log.vcon`, and requires the
  `boot complete` line there as well as the driver-module load lines.
- `modules/`: kernel module fixtures packed into the boot archive under
  `tests/` and loaded only by the module self-tests: `cosmotest`
  (exports, all section groups), `cosmotest_dep` (depends on cosmotest),
  `cosmotest_fail` (init fails on purpose).
