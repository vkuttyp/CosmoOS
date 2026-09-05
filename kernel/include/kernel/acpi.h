/*
 * acpi.h - Static ACPI table access (RSDP, XSDT/RSDT, MADT).
 *
 * Initialised once on the boot CPU after vmm_init; read-only and
 * lock-free afterwards. See docs/drivers/acpi/architecture.md.
 */

#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <kernel/compiler.h>
#include <kernel/types.h>

#define ACPI_MAX_CPUS      64u
#define ACPI_MAX_IOAPICS   8u
#define ACPI_MAX_OVERRIDES 24u

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __packed;

struct acpi_madt_cpu {
    uint32_t apic_id;
    uint32_t acpi_id;
    bool     x2apic;    /* entry came from an x2APIC record */
};

struct acpi_madt_ioapic {
    uint8_t  id;
    paddr_t  address;
    uint32_t gsi_base;
};

/* AArch64: the MADT's GIC description (types 11-13). */
struct acpi_gic {
    unsigned version;        /* GICD entry: 0 unknown, 2, 3, 4 */
    paddr_t gicd_base;
    paddr_t gicc_base;       /* the first CPU interface's physical base (GICv2) */
    paddr_t v2m_base;        /* GIC MSI frame, 0 if none */
    unsigned v2m_spi_base, v2m_spi_count;   /* valid when the frame overrides its TYPER */
};

struct acpi_madt_override {
    uint8_t  bus;       /* 0 = ISA */
    uint8_t  source;    /* ISA IRQ */
    uint32_t gsi;
    uint16_t flags;     /* MPS INTI flags: polarity bits 0-1, trigger bits 2-3 */
};

/* Parse tables. Panics if there is no usable RSDP or no MADT. */
void acpi_init(void);
bool acpi_available(void);

/* Table by 4-character signature, checksum-verified, or NULL. The pointer
 * is a kernel virtual address valid for the life of the kernel. */
const struct acpi_sdt_header *acpi_find_table(const char *signature);

paddr_t acpi_madt_lapic_base(void);
size_t  acpi_madt_cpus(const struct acpi_madt_cpu **out);
size_t  acpi_madt_ioapics(const struct acpi_madt_ioapic **out);
size_t  acpi_madt_overrides(const struct acpi_madt_override **out);
/* false when the MADT carries no GIC distributor entry. */
bool    acpi_madt_gic(struct acpi_gic *out);

/* Map a physical table range to a kernel virtual pointer: direct map when
 * it is RAM, otherwise a fresh uncached-free WB window. */
const void *acpi_map(paddr_t pa, size_t len);

#endif /* KERNEL_ACPI_H */
