# ACPI Tables: invariants

Prefix `AC`.

**AC1. No table is recorded or exposed without a valid checksum.**
`map_table` returns NULL unless the sum of `length` bytes is zero, and
only its results enter `g_tables`; the RSDP's 20-byte checksum (and the
extended one on the XSDT path) is verified before its addresses are
used. Check: `acpi` self-test (`acpi_find_table("APIC")` succeeds on
QEMU's firmware). Gap: no test feeds a table with a bad checksum; the
rejection path is reviewed, not driven.

**AC2. Every MADT entry is read only within the table and within its own
declared length.** `parse_madt` stops at an entry whose `length` is
below 2 or runs past `madt->hdr.length`, and each `case` checks a
minimum entry length before its `memcpy`s; a MADT shorter than its
fixed fields (`hdr.length < sizeof(struct acpi_madt)`, 44 bytes) is
ignored with a warning before `lapic_address` is read (roadmap M6).
Check: review. Gap: no malformed-table test or fuzzer exists (host
fuzzing of ACPI tables is open work in [docs/plan.md](../../plan.md)).

**AC3. The decoded arrays never exceed their capacities.** Every append
to `g_cpus`, `g_ioapics`, `g_overrides` and `g_tables` is guarded by a
comparison with `ACPI_MAX_CPUS`, `ACPI_MAX_IOAPICS`,
`ACPI_MAX_OVERRIDES` and `ACPI_MAX_TABLES`. Check: review; the guards
are unconditional. Gap: x2APIC CPU, override and table overflow are
dropped without a message (LAPIC/GICC CPU and I/O APIC overflow log a
`kwarn`).

**AC4. Written once, read-only afterwards.** All state is written only
inside `acpi_init`, which runs once on the boot CPU before `irq_init`,
`smp_init` and the first thread; accessors therefore take no lock.
Check: review (the only call is in `kernel_main`). Gap: nothing asserts
against a second call.

**AC5. A pointer handed out stays valid for the life of the kernel.**
Direct-map pointers cover memory the allocator never frees (`pmm.c` and
`bootmem.c` take free memory only from `USABLE` and
`LOADER_RECLAIMABLE` ranges), and `vm_map_phys` windows are never
unmapped. Check: review. Gap: none tested; a future reclaim of
`ACPI_RECLAIMABLE` memory would break this and every
`acpi_find_table` caller.

**AC6. A machine without an RSDP or a MADT does not boot.** `acpi_init`
panics rather than continue, so `acpi_available()` is true whenever any
other code runs. Check: `acpi` self-test asserts `acpi_available()`;
every boot reaches `acpi_init`. Gap: the panic paths are not exercised.

**AC7. Only usable processors are reported.** A processor entry (type
0, 9 or 11) enters `g_cpus` only when its flags say enabled (bit 0) or
online capable: bit 1 in a LAPIC or x2APIC entry
(`MADT_CPU_ONLINE_CAPABLE`), bit 3 in a GICC entry
(`MADT_GICC_ONLINE_CAPABLE`, ACPI 6.5 5.2.12.14, where bit 1 is the
performance interrupt's trigger mode); `smp_init` starts every reported
CPU except the boot CPU.
Check: `smp-online` self-test (`online == reported` unless more than
`CONFIG_MAX_CPUS` were reported), run at `QEMU_SMP` 1, 2 and 4.
Gap: until roadmap M6 a GICC entry was tested against bit 1 like a
LAPIC entry, so a disabled GICC entry with an edge-triggered performance
interrupt was reported; QEMU marks every GICC entry enabled, so no test
reaches either version (inventory, section 3).

**AC8. The boot CPU is identified by the same id space the MADT uses.**
`acpi_madt_cpu.apic_id` holds the APIC/x2APIC id on x86-64 and the
packed MPIDR affinity on AArch64, the same encoding
`arch_smp_boot_hw_id()` returns, so `smp_init` skips exactly one entry.
Check: `smp-online` (a mismatch would start the boot CPU twice or miss
an AP, changing the online count). Gap: none.
