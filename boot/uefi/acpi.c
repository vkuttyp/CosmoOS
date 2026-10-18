/*
 * acpi.c - What the loader reads from the firmware's ACPI tables
 * (docs/boot/design.md, "The serial console").
 *
 * The kernel cannot read a table before it has a console, and it cannot
 * have a console before it knows where the serial port is. The loader runs
 * with the firmware's identity map and can read any table, so it finds the
 * SPCR (Serial Port Console Redirection) here and records the port in the
 * boot info; its own output after ExitBootServices goes there too.
 *
 * It also prints, while the firmware console still works, the tables the
 * firmware offers and what the kernel will take from them (the GIC, the
 * timer interrupts, the PSCI conduit): on a machine that then stays silent
 * that is the evidence a screen shows.
 *
 * Tables are checksum-checked and every read is bounded by the table's
 * length, as in the kernel's walker (drivers/acpi/acpi.c).
 */

#include "loader.h"
#include "cosmoboot.h"

struct sdt {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

#define SDT_HDR 36u
/* No table this loader reads is near this long; a length past it is a
 * corrupt header, refused before the checksum walks off the table. */
#define SDT_MAX (1u << 20)
#define MAX_TABLES 64u

static bool sum_ok(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++)
        s = (uint8_t)(s + b[i]);
    return s == 0;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* The root table's entries, as table pointers; 0 when there is none. */
static const struct sdt *g_tables[MAX_TABLES];
static unsigned g_ntables;
static char g_oem[7];
/* The end of the highest RSDP, root table or table collected: the
 * direct map must reach it (direct_map_size). */
static uint64_t g_end;

static void note_end(uint64_t addr, uint64_t len)
{
    if (addr + len > g_end)
        g_end = addr + len;
}

static void collect(uint64_t rsdp_addr)
{
    const uint8_t *rsdp = (const uint8_t *)(uintptr_t)rsdp_addr;
    if (rsdp == NULL || memcmp(rsdp, "RSD PTR ", 8) != 0 || !sum_ok(rsdp, 20))
        return;
    memcpy(g_oem, rsdp + 9, 6);
    uint8_t rev = rsdp[15];
    uint64_t root;
    unsigned width;
    uint32_t rlen = rd32(rsdp + 20);
    if (rev >= 2 && rlen >= 36 && rlen <= 4096 && sum_ok(rsdp, rlen) && rd64(rsdp + 24) != 0) {
        root = rd64(rsdp + 24);
        width = 8;
    } else {
        root = rd32(rsdp + 16);
        width = 4;
    }
    note_end(rsdp_addr, width == 8 ? rlen : 20);
    const struct sdt *r = (const struct sdt *)(uintptr_t)root;
    if (r == NULL || r->length < SDT_HDR || r->length > SDT_MAX || !sum_ok(r, r->length))
        return;
    note_end(root, r->length);
    const uint8_t *e = (const uint8_t *)r + SDT_HDR;
    unsigned n = (r->length - SDT_HDR) / width;
    for (unsigned i = 0; i < n; i++) {
        uint64_t a = width == 8 ? rd64(e + i * 8) : rd32(e + i * 4);
        const struct sdt *t = (const struct sdt *)(uintptr_t)a;
        if (t == NULL || t->length < SDT_HDR || t->length > SDT_MAX || !sum_ok(t, t->length))
            continue;
        if (g_ntables == MAX_TABLES) {
            /* As the kernel's walker (ACPI_MAX_TABLES): said, not silent. */
            lprintf("acpi: more than %u tables; the rest are not read\n", MAX_TABLES);
            break;
        }
        g_tables[g_ntables++] = t;
        note_end(a, t->length);
    }
}

uint64_t acpi_tables_end(void)
{
    return g_end;
}

static const uint8_t *find(const char *sig, uint32_t *len)
{
    for (unsigned i = 0; i < g_ntables; i++) {
        if (memcmp(g_tables[i]->signature, sig, 4) == 0) {
            *len = g_tables[i]->length;
            return (const uint8_t *)g_tables[i];
        }
    }
    return NULL;
}

/* The SPCR interface types, which the DBG2 reuses as serial subtypes. */
static uint32_t console_kind(uint8_t type, uint8_t space)
{
    switch (type) {
    case 0x03:   /* ARM PL011 */
    case 0x0D:   /* ARM SBSA generic UART, 32-bit access only */
    case 0x0E:   /* ARM SBSA generic UART */
        return space == 0 ? COSMOBOOT_CONSOLE_PL011 : COSMOBOOT_CONSOLE_NONE;
    case 0x00:   /* full 16550 */
    case 0x01:   /* 16550 subset */
    case 0x12:   /* 16550-compatible, with the descriptor saying how */
        return space == 0 ? COSMOBOOT_CONSOLE_16550_MMIO : space == 1 ? COSMOBOOT_CONSOLE_16550_PORT
                                                                       : COSMOBOOT_CONSOLE_NONE;
    default:
        return COSMOBOOT_CONSOLE_NONE;
    }
}

static const char *kind_name(uint32_t kind)
{
    return kind == COSMOBOOT_CONSOLE_PL011        ? "PL011"
           : kind == COSMOBOOT_CONSOLE_16550_MMIO ? "16550 (MMIO)"
           : kind == COSMOBOOT_CONSOLE_16550_PORT ? "16550 (port)"
                                                  : "not a console this kernel drives";
}

/* The register layout from the port's address descriptor (GAS: space,
 * bit width, bit offset, access size, address): the access width from its
 * access size, the register spacing from its bit width; each 0 when the
 * descriptor leaves it undefined. A 16550 the drivers cannot reach (an
 * access of 2 or 8 bytes) is no console. */
static uint32_t console_layout(struct cosmoboot_info *info, uint32_t kind, const uint8_t *gas)
{
    uint8_t width = gas[1], access = gas[3];
    info->console_access = access >= 1 && access <= 4 ? 1u << (access - 1) : 0u;
    info->console_stride = width == 8 || width == 16 || width == 32 || width == 64 ? width / 8u : 0u;
    if (kind == COSMOBOOT_CONSOLE_16550_MMIO && info->console_access != 0 && info->console_access != 1 &&
        info->console_access != 4) {
        lprintf("acpi: a 16550 accessed %u bytes at a time is not one this loader drives\n", info->console_access);
        info->console_access = info->console_stride = 0;
        return COSMOBOOT_CONSOLE_NONE;
    }
    return kind;
}

/*
 * DBG2 (the Debug Port Table), when there is no SPCR: the firmware's debug
 * serial port. Its device entries follow an offset at 36 and a count at
 * 40; each has a length at 1, a count of address descriptors at 3, the
 * port type at 12 (0x8000: serial), the subtype at 14 (the SPCR's
 * interface types) and the offset of its first address descriptor at 18.
 * It names no interrupt, so the kernel polls it.
 */
static bool dbg2(struct cosmoboot_info *info)
{
    uint32_t len;
    const uint8_t *d = find("DBG2", &len);
    if (d == NULL || len < 44)
        return false;
    uint64_t off = rd32(d + 36);   /* 64-bit sums: a corrupt offset must not wrap past the checks */
    uint32_t n = rd32(d + 40);
    for (uint32_t i = 0; i < n && off >= 44 && off + 22 <= len; i++) {
        const uint8_t *e = d + off;
        uint16_t elen, type, sub, baro;
        memcpy(&elen, e + 1, 2);
        memcpy(&type, e + 12, 2);
        memcpy(&sub, e + 14, 2);
        memcpy(&baro, e + 18, 2);
        if (elen < 22 || off + elen > len)
            break;
        if (type == 0x8000 && e[3] > 0 && (uint32_t)baro + 12u <= elen) {
            uint8_t space = e[baro], access = e[baro + 3];
            uint64_t base = rd64(e + baro + 4);
            uint32_t kind = base != 0 && sub <= 0xff ? console_kind((uint8_t)sub, space) : COSMOBOOT_CONSOLE_NONE;
            lprintf("acpi: DBG2 serial port %u: subtype 0x%x, space %u, base 0x%llx, access %u, no interrupt -> %s\n",
                    i, sub, space, (unsigned long long)base, access, kind_name(kind));
            if (kind != COSMOBOOT_CONSOLE_NONE)
                kind = console_layout(info, kind, e + baro);
            if (kind != COSMOBOOT_CONSOLE_NONE) {
                info->console_type = kind;
                info->console_base = base;
                info->console_gsiv = 0;
                info->console_flags = COSMOBOOT_CONSOLE_FROM_DBG2;
                return true;
            }
        }
        off += elen;
    }
    return false;
}

/* SPCR: interface type at 36; the base address descriptor (GAS: space,
 * bit width, bit offset, access size, address) at 40; the interrupt's
 * GSIV at 54. */
static void spcr(struct cosmoboot_info *info)
{
    uint32_t len;
    const uint8_t *t = find("SPCR", &len);
    if (t == NULL || len < 58) {
        if (!dbg2(info))
            lputs("acpi: no SPCR and no DBG2 serial port: no serial console\n");
        return;
    }
    uint8_t type = t[36], space = t[40], access = t[43], itype = t[52];
    uint64_t base = rd64(t + 44);
    /* The interrupt type at 52: bit 3 says the GSIV is a GIC interrupt.
     * Anything else (a PC-AT IRQ, an I/O APIC's) is not one the kernel can
     * request on an ARM machine: the port is then polled. */
    uint32_t gsiv = (itype & 0x08) ? rd32(t + 54) : 0;
    uint32_t kind = base != 0 ? console_kind(type, space) : COSMOBOOT_CONSOLE_NONE;
    lprintf("acpi: SPCR: interface type 0x%x, space %u, base 0x%llx, access %u, GSIV %u -> %s\n", type, space,
            (unsigned long long)base, access, gsiv, kind_name(kind));
    if (kind != COSMOBOOT_CONSOLE_NONE)
        kind = console_layout(info, kind, t + 40);
    info->console_type = kind;
    if (kind != COSMOBOOT_CONSOLE_NONE) {
        info->console_base = base;
        info->console_gsiv = gsiv;
    }
}

/* What the kernel will take from the MADT, MCFG, GTDT and FADT. */
static void summary(void)
{
    uint32_t len;
    const uint8_t *m = find("APIC", &len);
    if (m != NULL && len >= 44) {
        unsigned gicc = 0, gicr = 0, its = 0, msi = 0, lapic = 0;
        uint64_t gicd = 0, gicr0 = 0;
        unsigned gicv = 0;
        for (uint32_t off = 44; off + 2 <= len;) {
            uint8_t ty = m[off], el = m[off + 1];
            if (el < 2 || off + el > len)
                break;
            if ((ty == 0 || ty == 9))
                lapic++;
            else if (ty == 11)
                gicc++;
            else if (ty == 12 && el >= 24) {
                gicd = rd64(m + off + 8);
                gicv = m[off + 20];
            } else if (ty == 13)
                msi++;
            else if (ty == 14 && el >= 16) {
                if (gicr++ == 0)
                    gicr0 = rd64(m + off + 4);
            } else if (ty == 15)
                its++;
            off += el;
        }
        if (gicc != 0 || gicd != 0)
            lprintf("acpi: MADT: %u GICC, GICD 0x%llx version %u, %u GICR (first 0x%llx), %u ITS, %u MSI frame\n",
                    gicc, (unsigned long long)gicd, gicv, gicr, (unsigned long long)gicr0, its, msi);
        else
            lprintf("acpi: MADT: %u local APIC(s)\n", lapic);
    } else {
        lputs("acpi: no MADT\n");
    }
    const uint8_t *c = find("MCFG", &len);
    if (c != NULL && len >= 44) {
        for (uint32_t off = 44; off + 16 <= len; off += 16)
            lprintf("acpi: MCFG: segment %u, buses %u-%u, ECAM 0x%llx\n", c[off + 8] | (c[off + 9] << 8),
                    c[off + 10], c[off + 11], (unsigned long long)rd64(c + off));
    }
    const uint8_t *g = find("GTDT", &len);
    if (g != NULL && len >= 72)
        lprintf("acpi: GTDT: EL1 physical GSIV %u, virtual GSIV %u\n", rd32(g + 56), rd32(g + 64));
    const uint8_t *f = find("FACP", &len);
    if (f != NULL && len >= 131) {
        uint16_t arm;
        memcpy(&arm, f + 129, 2);
        if (arm != 0)
            lprintf("acpi: FADT: ARM boot flags 0x%x (PSCI %s, conduit %s)\n", arm, (arm & 1) ? "yes" : "no",
                    (arm & 2) ? "HVC" : "SMC");
    }
}

void acpi_scan(struct cosmoboot_info *info)
{
    collect(info->acpi_rsdp);
    if (g_ntables == 0) {
        lputs("acpi: no usable RSDP/XSDT: no serial console\n");
        return;
    }
    lprintf("acpi: OEM '%s', %u tables:", g_oem, g_ntables);
    for (unsigned i = 0; i < g_ntables; i++)
        lprintf(" %c%c%c%c", g_tables[i]->signature[0], g_tables[i]->signature[1], g_tables[i]->signature[2],
                g_tables[i]->signature[3]);
    lputs("\n");
    spcr(info);
}

/* After ExitBootServices: on the serial port acpi_scan chose (the screen
 * belongs to the firmware's console until then, and printing this twice
 * would double it wherever the firmware mirrors that console to serial). */
void acpi_report(void)
{
    if (g_ntables != 0)
        summary();
}
