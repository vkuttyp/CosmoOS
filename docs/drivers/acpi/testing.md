# ACPI Tables: testing

## Self-test `acpi` (`kernel/scheduler/schedtest.c`)

`selftest_acpi`, registered as `"acpi"` in the table in
`kernel/core/selftest.c`. Runs in every build with `SELFTEST=1` (the
default for `BUILD=debug`, `build/config.mk`). It checks:

- `acpi_available()`;
- `acpi_madt_lapic_base() != 0` or `acpi_madt_gic()` true (a LAPIC on
  x86-64, a GIC distributor on AArch64);
- `acpi_madt_cpus()` reports at least one CPU;
- `acpi_find_table("APIC")` is non-NULL and `acpi_find_table("ZZZZ")`
  is NULL.

Marker: `SELFTEST: acpi` (name padded to 16 columns) `... ok (N ms)`.

## Indirect coverage

- **`smp-online`** (`kernel/scheduler/smptest.c`): the number of online
  CPUs equals `acpi_madt_cpus()`'s count (unless more than
  `CONFIG_MAX_CPUS` were reported). Run at `QEMU_SMP=1`, `2` and `4`,
  this checks that the MADT walk reports exactly the CPUs QEMU's
  firmware lists and that the hardware ids match
  `arch_smp_boot_hw_id()` (invariants AC7, AC8).
- **`irq-route`** (`kernel/scheduler/schedtest.c`, x86-64): routes the
  PIT's ISA IRQ through `irq_legacy_to_gsi`, which reads
  `acpi_madt_overrides()`; QEMU's MADT overrides ISA IRQ 0 to GSI 2, so
  an interrupt arriving proves the override decode. On AArch64 there is
  no ISA source and the test logs a skip.
- **Every boot**, debug or release: `acpi_init` panics without a valid
  RSDP, root table and MADT, and the interrupt controllers, SMP
  bring-up, PCI ECAM (`MCFG`), the IOMMU drivers (`DMAR`, `IORT`) and,
  on AArch64, the timer (`GTDT`) and PSCI conduit (`FACP`) all depend
  on its output; `make test-platforms` checks on AArch64 that the
  platform line identifies QEMU's firmware by its RSDP OEM id and takes
  the timer PPIs from the GTDT. The boot log carries one summary
  line, for example
  `acpi: XSDT rev 2, 6 tables, LAPIC at 0xfee00000, 1 CPUs, 1 IOAPICs, 5 overrides`
  (from `docs/development.md`); `tests/boot/run_boot_test.py` does not
  match it.
- **`make test-gic`** (AArch64): boots QEMU `virt` with
  `gic-version=3`, which exercises the GICR (type 14) and, when the
  machine provides one, ITS (type 15) entries that `gicv3.c` consumes;
  a second boot with `QEMU_MSI=gicv2m` exercises the MSI frame entry
  under a GICv3 when the host's QEMU supports it.

## Running

```sh
gmake test                          # x86-64 debug, 4 CPUs
gmake test QEMU_SMP=1               # and QEMU_SMP=2: AC7/AC8 at other counts
gmake test-smp2
gmake ARCH=aarch64 test             # GICv2 virt machine
gmake ARCH=aarch64 test-gic         # GICv3 (and GICv2m-under-GICv3)
gmake BUILD=release test            # acpi_init on the release build; no self-tests
```

Use GNU make (`gmake` on macOS), as described in
[docs/development.md](../../development.md).

## Gaps

- No host test and no fuzzer covers `acpi.c`. Host-side ACPI table
  fuzzing is listed as remaining work in [docs/plan.md](../../plan.md)
  ("Implementation — missing fuzz/property coverage").
- Nothing drives the failure paths: bad table checksum, bad RSDP,
  truncated or malformed MADT entries, array overflow, the panic on a
  missing MADT. They are covered by review only.
- The RSDT (32-bit) path runs only on firmware with a revision 0 or 1
  RSDP or no XSDT; the x86-64 boot uses the XSDT (the log line above),
  and no test checks which root table AArch64 uses.
- The type 5 (64-bit LAPIC address override) and type 9 (x2APIC)
  entries are not asserted by any test; at the CPU counts the harness
  uses (`QEMU_SMP` at most 4) QEMU describes x86-64 CPUs with type 0
  entries.
- The non-RAM path of `acpi_map` (a `vm_map_phys` window) runs only if
  firmware places a table outside RAM-typed memory; it is not forced by
  any test.
- `gicv_base`, `gich_base`, `maint_gsiv` and `its_id` are logged but
  not asserted by any test.
