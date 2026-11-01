/*
 * acpi_parse.c - The ACPI table walker's decoders of what firmware wrote
 * (roadmap 2.0 SA5, the virtq/USB/ACPI family): the checksum, the RSDP's
 * signature and checksums, the root table's entries and the MADT. Moved
 * from acpi.c, the MADT's warnings now counted for acpi.c to log: the C
 * step, to be replaced in CeeSafe once tests/host/test_acpi.c's recorded
 * digest agrees.
 */

#include <kernel/string.h>

#include "acpi_internal.h"

struct madt_entry {
    uint8_t type;
    uint8_t length;
} __packed;

#define MADT_LAPIC          0
#define MADT_IOAPIC         1
#define MADT_ISO            2
#define MADT_LAPIC_OVERRIDE 5
#define MADT_X2APIC         9
#define MADT_GICC           11
#define MADT_GICD           12
#define MADT_GIC_MSI_FRAME  13
#define MADT_GICR           14
#define MADT_GIC_ITS        15

#define MADT_CPU_ENABLED        (1u << 0)
#define MADT_CPU_ONLINE_CAPABLE (1u << 1)   /* LAPIC and x2APIC entries */
#define MADT_GICC_ONLINE_CAPABLE (1u << 3)  /* GICC entries: bit 1 there is the performance
                                             * interrupt's trigger mode (ACPI 6.5, 5.2.12.14) */

uint8_t acpi_checksum(const void *p, size_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + b[i]);
    return sum;
}

int acpi_rsdp_decode(const struct acpi_rsdp *rsdp, paddr_t *root, unsigned *entry_size)
{
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0)
        return ACPI_RSDP_SIGNATURE;
    if (acpi_checksum(rsdp, 20) != 0)
        return ACPI_RSDP_CHECKSUM;
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        if (acpi_checksum(rsdp, rsdp->length < sizeof(*rsdp) ? rsdp->length : sizeof(*rsdp)) != 0)
            return ACPI_RSDP_EXT_CHECKSUM;
        *root = rsdp->xsdt_address;
        *entry_size = 8;
    } else {
        *root = rsdp->rsdt_address;
        *entry_size = 4;
    }
    return ACPI_RSDP_OK;
}

size_t acpi_root_count(const struct acpi_sdt_header *root, unsigned entry_size)
{
    return (root->length - sizeof(*root)) / entry_size;
}

paddr_t acpi_root_entry(const struct acpi_sdt_header *root, unsigned entry_size, size_t i)
{
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    if (entry_size == 8) {
        uint64_t v;
        memcpy(&v, entries + i * 8, 8);
        return v;
    }
    uint32_t v;
    memcpy(&v, entries + i * 4, 4);
    return v;
}

static void add_cpu(struct acpi_madt_state *st, uint32_t acpi_id, uint32_t apic_id, bool x2apic)
{
    if (st->cpu_count < ACPI_MAX_CPUS) {
        st->cpus[st->cpu_count].acpi_id = acpi_id;
        st->cpus[st->cpu_count].apic_id = apic_id;
        st->cpus[st->cpu_count].x2apic = x2apic;
        st->cpu_count++;
    } else {
        st->extra_cpus++;
    }
}

void acpi_madt_decode(const struct acpi_madt *madt, struct acpi_madt_state *st)
{
    /* The fixed fields first: a table shorter than them has none of its
     * own to give, and nothing past its length is read. */
    if (madt->hdr.length < sizeof(*madt)) {
        st->too_short = true;
        return;
    }
    st->lapic_base = madt->lapic_address;

    const uint8_t *p = (const uint8_t *)madt + sizeof(*madt);
    const uint8_t *end = (const uint8_t *)madt + madt->hdr.length;

    while (p + sizeof(struct madt_entry) <= end) {
        const struct madt_entry *e = (const struct madt_entry *)p;
        if (e->length < sizeof(*e) || p + e->length > end) {
            st->stopped = true;
            st->bad_entry_len = e->length;
            break;
        }

        switch (e->type) {
        case MADT_LAPIC:
            if (e->length >= 8) {
                uint32_t flags;
                memcpy(&flags, p + 4, 4);
                if (flags & (MADT_CPU_ENABLED | MADT_CPU_ONLINE_CAPABLE))
                    add_cpu(st, p[2], p[3], false);
            }
            break;
        case MADT_X2APIC:
            if (e->length >= 16) {
                uint32_t apic_id, flags, acpi_id;
                memcpy(&apic_id, p + 4, 4);
                memcpy(&flags, p + 8, 4);
                memcpy(&acpi_id, p + 12, 4);
                if (flags & (MADT_CPU_ENABLED | MADT_CPU_ONLINE_CAPABLE))
                    add_cpu(st, acpi_id, apic_id, true);
            }
            break;
        case MADT_IOAPIC:
            if (e->length >= 12) {
                if (st->ioapic_count < ACPI_MAX_IOAPICS) {
                    uint32_t addr, gsi;
                    memcpy(&addr, p + 4, 4);
                    memcpy(&gsi, p + 8, 4);
                    st->ioapics[st->ioapic_count].id = p[2];
                    st->ioapics[st->ioapic_count].address = addr;
                    st->ioapics[st->ioapic_count].gsi_base = gsi;
                    st->ioapic_count++;
                } else {
                    st->extra_ioapics++;
                }
            }
            break;
        case MADT_ISO:
            if (e->length >= 10) {
                if (st->override_count < ACPI_MAX_OVERRIDES) {
                    uint32_t gsi;
                    uint16_t flags;
                    memcpy(&gsi, p + 4, 4);
                    memcpy(&flags, p + 8, 2);
                    st->overrides[st->override_count].bus = p[2];
                    st->overrides[st->override_count].source = p[3];
                    st->overrides[st->override_count].gsi = gsi;
                    st->overrides[st->override_count].flags = flags;
                    st->override_count++;
                }
            }
            break;
        case MADT_LAPIC_OVERRIDE:
            if (e->length >= 12) {
                uint64_t addr;
                memcpy(&addr, p + 4, 8);
                st->lapic_base = addr;
            }
            break;
        case MADT_GICC:
            /* GICC: CPU interface number 4, ACPI UID 8, flags 12, parking version 16,
             * performance GSIV 20, parked address 24, physical base 32, GICV 40, GICH 48,
             * VGIC maintenance 56, GICR base 60, MPIDR 68 (76 bytes in ACPI 5.1, 80 in 6.x). */
            if (e->length >= 76) {
                uint32_t flags;
                uint64_t base, mpidr;
                memcpy(&flags, p + 12, 4);
                memcpy(&base, p + 32, 8);
                memcpy(&mpidr, p + 68, 8);
                if (st->gic.gicc_base == 0)
                    st->gic.gicc_base = base;
                /* Virtualisation, from the first entry that offers it:
                 * every CPU's frames are the same on the machines this
                 * runs on, and a hypervisor that needed per-CPU frames
                 * would have to walk the entries itself. */
                if (st->gic.gich_base == 0) {
                    uint64_t gicv, gich;
                    uint32_t maint;
                    memcpy(&gicv, p + 40, 8);
                    memcpy(&gich, p + 48, 8);
                    memcpy(&maint, p + 56, 4);
                    st->gic.gicv_base = gicv;
                    st->gic.gich_base = gich;
                    st->gic.maint_gsiv = maint;
                }
                /* GICv3: each CPU's redistributor may be named here
                 * instead of by a GICR entry (offset 60, ACPI 5.1+). */
                if (st->gic.gicc_gicr_base == 0) {
                    uint64_t gicr;
                    memcpy(&gicr, p + 60, 8);
                    st->gic.gicc_gicr_base = gicr;
                }
                if (flags & (MADT_CPU_ENABLED | MADT_GICC_ONLINE_CAPABLE)) {
                    uint32_t acpi_id;
                    memcpy(&acpi_id, p + 8, 4);
                    /* hw_id: the MPIDR affinity fields (Aff0-2 in bits 0-23, Aff3 in 24-31) */
                    add_cpu(st, acpi_id, (uint32_t)((mpidr & 0xFFFFFFull) | ((mpidr >> 8) & 0xFF000000ull)), false);
                }
            }
            break;
        case MADT_GICD:
            if (e->length >= 24) {
                uint64_t base;
                memcpy(&base, p + 8, 8);
                st->gic.gicd_base = base;
                st->gic.version = p[20];
                st->gic_present = true;
            }
            break;
        case MADT_GIC_MSI_FRAME:
            if (e->length >= 24) {
                uint64_t base;
                uint32_t flags;
                uint16_t count, sbase;
                memcpy(&base, p + 8, 8);
                memcpy(&flags, p + 16, 4);
                memcpy(&count, p + 20, 2);
                memcpy(&sbase, p + 22, 2);
                if (st->gic.v2m_base == 0) {
                    st->gic.v2m_base = base;
                    if (flags & 1) {
                        st->gic.v2m_spi_count = count;
                        st->gic.v2m_spi_base = sbase;
                    }
                }
            }
            break;
        case MADT_GICR:
            /* GICR: discovery window base 4, length 12. */
            if (e->length >= 16 && st->gic.gicr_base == 0) {
                uint64_t base;
                uint32_t len;
                memcpy(&base, p + 4, 8);
                memcpy(&len, p + 12, 4);
                st->gic.gicr_base = base;
                st->gic.gicr_length = len;
            }
            break;
        case MADT_GIC_ITS:
            /* ITS: id 4, physical base 8. */
            if (e->length >= 16 && st->gic.its_base == 0) {
                uint32_t id;
                uint64_t base;
                memcpy(&id, p + 4, 4);
                memcpy(&base, p + 8, 8);
                st->gic.its_id = id;
                st->gic.its_base = base;
            }
            break;
        default:
            break;
        }
        p += e->length;
    }
}
