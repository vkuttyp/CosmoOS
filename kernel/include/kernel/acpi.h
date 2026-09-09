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
    /*
     * GICv3 and later. A redistributor per CPU replaces the single CPU
     * interface, and it is described one of two ways: a GICR entry
     * giving a contiguous window (base + length) that the kernel walks
     * by stride, or a per-CPU base in each GICC entry. Firmware may use
     * either; QEMU's virt uses the GICR entry. Both are recorded, and a
     * zero in each says the MADT did not offer it.
     */
    paddr_t gicr_base;       /* GICR entry: discovery window base, 0 if none */
    uint64_t gicr_length;    /* its length, so the walk knows where to stop */
    paddr_t gicc_gicr_base;  /* the first GICC entry's own redistributor base, 0 if none */
    paddr_t its_base;        /* GIC ITS entry: translator base, 0 if none */
    unsigned its_id;         /* the ITS's own id, for the command it is given */
    /*
     * Virtualisation. A GICv2 hypervisor drives guests through two more
     * frames: GICH, which it programs, and GICV, which it maps where the
     * guest expects its CPU interface. A GICv3 replaces both with EL2
     * system registers and leaves the two addresses zero -- measured on
     * QEMU's virt, which reports GICH 0x8030000 / GICV 0x8040000 under
     * `gic-version=2` and 0 / 0 under `gic-version=3`.
     *
     * The maintenance interrupt is not part of that split: it is a PPI
     * either way, and both machines report GSIV 25. So `gich_base` is
     * what says which kind of hypervisor interface this is; the GSIV
     * only says which line it raises.
     */
    paddr_t gicv_base;       /* GICC entry: the guest's CPU interface, 0 if none */
    paddr_t gich_base;       /* the hypervisor control frame, 0 if none */
    unsigned maint_gsiv;     /* the VGIC maintenance interrupt, 0 if none */
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
