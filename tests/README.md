# tests

Host unit tests, kernel integration tests, QEMU boot tests, property and fuzz tests. Every subsystem must have tests.

- `host/`: native ASan/UBSan tests of kernel algorithms (`make host-test`):
  `test_buddy`, `test_slab`, `test_crypto` (SHA-512, Ed25519 and CRC32C
  vectors), `test_modelf` (module ELF validation on synthetic images),
  `test_cosmofs` (cosmofs on-disk layout and extent mapping), `test_libc`,
  `test_pkg`, `test_linux` (Linux ABI conversions), `test_hv` (the
  nested page table builder, the I/O exit decoder, the VMCB and UAPI
  layouts) and `test_reloc_aarch64` (the `R_AARCH64` relocation
  encodings and range limits of the AArch64 module loader). All of them
  build on every host regardless of `ARCH`.
- `boot/`: the QEMU boot-test harness (`make test`, `make test-crash`)
  for both architectures (`COSMO_ARCH`, set by the Makefile, selects the
  banner and panic markers; `docs/kernel/arch/aarch64/testing.md`).
  Since Phase 6 it creates a fresh 8 MiB scratch disk
  (`boot-test.log.testdisk.img`) for the virtio-blk and cosmofs
  self-tests (the disk is formatted during the run), routes
  the virtio console to `boot-test.log.vcon`, and requires the
  `boot complete` line there as well as the driver-module load lines.
- `modules/`: kernel module fixtures packed into the boot archive under
  `tests/` and loaded only by the module self-tests: `cosmotest`
  (exports, all section groups), `cosmotest_dep` (depends on cosmotest),
  `cosmotest_fail` (init fails on purpose).
- `linux/`: freestanding raw-Linux-ABI programs (`lxhello`, `lxtest`) and
  the musl `hello_musl`, run under the Linux personality by `/etc/rc.test`
  (`docs/compat/linux/testing.md`). x86-64 only: the `ARCH=aarch64` build
  leaves them out and `rc.test` prints `LINUXTEST: skipped`.
- `hv/`: the six flat guest images of the virtualization tests
  (`guest_pio`, `guest_irq`, `guest_cpuid`, `guest_pm`, `guest_shutdown`,
  `guest_spin`; `hv.mk`), packed into the boot archive under `tests/hv/`
  for the kernel self-tests and `vmctl`
  (`docs/kernel-services/virtualization/testing.md`). x86-64 only: the
  `ARCH=aarch64` build leaves them out and `rc.test` prints
  `HVTEST: skipped`.
