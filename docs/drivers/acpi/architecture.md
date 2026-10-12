# ACPI Tables: Architecture

## Purpose

Read the static ACPI tables the firmware leaves in memory so the kernel
can enumerate CPUs and interrupt controllers. Nothing more: no AML
interpreter, no power management, no device tree of ACPI objects.

## Position

`drivers/acpi/acpi.c` sits below the interrupt controllers and SMP
bring-up and above the memory subsystem. Its only input is
`cosmoboot_info.acpi_rsdp`. It reads tables through the direct map when
they lie in RAM-typed memory (ACPI reclaim and NVS count as RAM, and the
direct map covers all RAM after `vmm_init`), and through a read-only
`vm_map_phys` window otherwise (`acpi_map`).

## Responsibilities

- Validate the RSDP (signature, checksum, revision) and choose the XSDT
  (revision ≥ 2) or the RSDT.
- Validate every referenced table header checksum before exposing it.
- `acpi_find_table("SIG")` for consumers.
- Decode the MADT into typed arrays: local APIC base (with the 64-bit
  override entry), processor entries (LAPIC and x2APIC, only those
  flagged enabled or online-capable), I/O APICs, ISA interrupt source
  overrides; since Phase 13 also the GIC entries: CPU interfaces (type
  11, the MPIDR affinity fields become the processor `hw_id`, the first
  entry's base is the GICv2 CPU interface), the distributor (type 12,
  base and version) and the MSI frame (type 13, base and optional SPI
  range); for GICv3 the GICR discovery window (type 14), the GICC
  entry's own redistributor base and the ITS (type 15); and the GICC
  virtualisation fields (GICV, GICH, maintenance GSIV). All are exposed
  as `struct acpi_gic` through `acpi_madt_gic()`.

## Non-responsibilities

AML, FADT power fields, HPET, MCFG (PCIe, Phase 6), SRAT (NUMA, later).
The AArch64 backend reads its own tables through `acpi_find_table`
(GTDT for the timer interrupts, SPCR for the console, the FADT's ARM
boot flags for the PSCI conduit); this driver only locates them.

## Interfaces

`kernel/acpi.h`: `acpi_init`, `acpi_find_table`, `acpi_madt_*` accessors
(including `acpi_madt_gic`) and the `struct acpi_madt_*` / `struct
acpi_gic` records.

## Concurrency and ownership

Initialised once on the BSP before any other CPU exists; read-only
afterwards, no locks. Decoded arrays are static with fixed maxima
(`ACPI_MAX_CPUS` 64, `ACPI_MAX_IOAPICS` 8, `ACPI_MAX_OVERRIDES` 24) and
at most 64 tables are recorded; excess entries are dropped, with a
warning for LAPIC/GICC CPUs and I/O APICs and silently for x2APIC CPUs,
overrides and tables.

## Error handling

A missing or invalid RSDP, an unusable root table or a missing MADT is
a panic: this kernel targets APIC (x86-64) and GIC (AArch64) platforms
only. A bad checksum on an individual table skips that table with a
warning.

## Security

Tables come from firmware, part of the trusted computing base, but
table lengths are checked against the header size and every MADT entry
against the table end and its own minimum length before it is read (the
MADT's fixed `lapic_address`/`flags` fields are the exception; see
[invariants.md](invariants.md) AC2).

## Testing

Self-test `acpi`: the MADT reports at least one CPU and either the LAPIC
base (x86-64) or the GIC distributor (AArch64) is present. The
`smp-online` self-test checks that as many CPUs come online as the MADT
reports. See [testing.md](testing.md).
