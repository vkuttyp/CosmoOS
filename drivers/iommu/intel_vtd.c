/*
 * intel_vtd.c - Intel VT-d DMA remapping (docs/kernel/iommu/design.md §5).
 *
 * Every DRHD of the ACPI DMAR table is one unit: a root table, context
 * tables per bus, second-level 4-level paging through the generic walker,
 * register-based context and IOTLB invalidation, the fault event as an
 * MSI. Interrupt remapping is not enabled (QEMU: intremap=off).
 */

#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <kernel/iommu.h>
#include <kernel/irq.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/vmm.h>
#include <arch/cpu.h>

#include <kernel/iommu_pt.h>

#if defined(ARCH_X86_64)

/* Registers (VT-d specification chapter 11). */
#define VTD_VER     0x00
#define VTD_CAP     0x08
#define VTD_ECAP    0x10
#define VTD_GCMD    0x18
#define VTD_GSTS    0x1c
#define VTD_RTADDR  0x20
#define VTD_CCMD    0x28
#define VTD_FSTS    0x34
#define VTD_FECTL   0x38
#define VTD_FEDATA  0x3c
#define VTD_FEADDR  0x40
#define VTD_FEUADDR 0x44

#define VTD_GCMD_TE   (1u << 31)
#define VTD_GCMD_SRTP (1u << 30)
#define VTD_GSTS_TES  (1u << 31)
#define VTD_GSTS_RTPS (1u << 30)
#define VTD_CCMD_ICC  (1ull << 63)
#define VTD_CCMD_GLOBAL (1ull << 61)
#define VTD_CCMD_DOMAIN (2ull << 61)
#define VTD_IOTLB_IVT   (1ull << 63)
#define VTD_IOTLB_GLOBAL (1ull << 60)
#define VTD_IOTLB_DOMAIN (2ull << 60)
#define VTD_FSTS_PPF  (1u << 1)
#define VTD_FSTS_CLEAR 0x7fu
#define VTD_FRCD_F    (1ull << 63)    /* high half, bit 127 of the record */

#define VTD_CAP_CM(c)    (((c) >> 7) & 1)
#define VTD_CAP_SAGAW(c) (((c) >> 8) & 0x1f)
#define VTD_CAP_ND(c)    ((c) & 7)
#define VTD_CAP_FRO(c)   ((unsigned)(((c) >> 24) & 0x3ff) * 16)
#define VTD_CAP_NFR(c)   ((unsigned)(((c) >> 40) & 0xff) + 1)
#define VTD_ECAP_IRO(e)  ((unsigned)(((e) >> 8) & 0x3ff) * 16)
#define VTD_ECAP_C(e)    ((e) & 1)

#define VTD_SL_R (1ull << 0)
#define VTD_SL_W (1ull << 1)
#define VTD_ADDR_MASK 0x000ffffffffff000ull

#define VTD_MAX_UNITS 4

struct vtd_unit {
    struct iommu_unit unit;
    volatile uint8_t *regs;
    uint64_t cap, ecap;
    unsigned fro, nfr, iro;
    paddr_t root_table;
    paddr_t ctx_table[256];      /* per bus, allocated at the first attach */
    uint32_t did_bits[4096 / 32];
    unsigned ndomains;
    bool include_all;
    unsigned nr_scopes;
    uint32_t scope_sid[16];
    spinlock_t lock;             /* register sequences and the context tables */
    int fault_vector;
};

static struct vtd_unit g_units[VTD_MAX_UNITS];
static unsigned g_nunits;

static inline uint32_t rd32(struct vtd_unit *u, unsigned off) { return *(volatile uint32_t *)(u->regs + off); }
static inline uint64_t rd64(struct vtd_unit *u, unsigned off) { return *(volatile uint64_t *)(u->regs + off); }
static inline void wr32(struct vtd_unit *u, unsigned off, uint32_t v) { *(volatile uint32_t *)(u->regs + off) = v; }
static inline void wr64(struct vtd_unit *u, unsigned off, uint64_t v) { *(volatile uint64_t *)(u->regs + off) = v; }

static bool wait32(struct vtd_unit *u, unsigned off, uint32_t mask, bool set)
{
    for (unsigned i = 0; i < 1000000; i++) {
        bool s = (rd32(u, off) & mask) != 0;
        if (s == set)
            return true;
    }
    return false;
}

static bool wait64_clear(struct vtd_unit *u, unsigned off, uint64_t bit)
{
    for (unsigned i = 0; i < 1000000; i++)
        if ((rd64(u, off) & bit) == 0)
            return true;
    return false;
}

/* --- the page-table format: second-level entries --- */

static uint64_t sl_table(paddr_t next) { return (next & VTD_ADDR_MASK) | VTD_SL_R | VTD_SL_W; }
static uint64_t sl_leaf(paddr_t pa, unsigned prot)
{
    return (pa & VTD_ADDR_MASK) | VTD_SL_R | ((prot & IOMMU_PROT_WRITE) ? VTD_SL_W : 0);
}
static bool sl_present(uint64_t e) { return (e & (VTD_SL_R | VTD_SL_W)) != 0; }
static paddr_t sl_addr(uint64_t e) { return e & VTD_ADDR_MASK; }
static const struct iommu_pt_fmt g_fmt = { sl_table, sl_leaf, sl_present, sl_addr };

/* --- invalidation (register based) --- */

static void invalidate_context(struct vtd_unit *u, uint64_t scope)
{
    wr64(u, VTD_CCMD, VTD_CCMD_ICC | scope);
    if (!wait64_clear(u, VTD_CCMD, VTD_CCMD_ICC))
        kwarn("iommu: %s: context invalidation did not complete", u->unit.name);
}

static void invalidate_iotlb(struct vtd_unit *u, uint64_t scope)
{
    wr64(u, u->iro + 8, VTD_IOTLB_IVT | scope);
    if (!wait64_clear(u, u->iro + 8, VTD_IOTLB_IVT))
        kwarn("iommu: %s: IOTLB invalidation did not complete", u->unit.name);
}

/* --- iommu_ops --- */

static struct vtd_unit *of(struct iommu_unit *iu) { return (struct vtd_unit *)iu; }

static bool vtd_covers(struct iommu_unit *iu, uint32_t sid)
{
    struct vtd_unit *u = of(iu);
    if (u->include_all)
        return true;
    for (unsigned i = 0; i < u->nr_scopes; i++)
        if (u->scope_sid[i] == sid)
            return true;
    return false;
}

/* Writes to 0xFEEx_xxxx from a device are interrupt messages to the
 * remapping hardware, not memory accesses: no IOVA may fall there. */
static unsigned vtd_reserved(struct iommu_unit *iu, struct iommu_range *out, unsigned max)
{
    (void)iu;
    if (max == 0)
        return 0;
    out[0] = (struct iommu_range){ .base = 0xFEE00000ull, .len = 0x100000, .identity = false };
    return 1;
}

static int vtd_domain_init(struct iommu_unit *iu, struct iommu_domain *d)
{
    struct vtd_unit *u = of(iu);
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    unsigned id = 0;
    for (unsigned i = 1; i < u->ndomains; i++) {   /* 0 stays unused */
        if (!((u->did_bits[i / 32] >> (i % 32)) & 1u)) {
            u->did_bits[i / 32] |= 1u << (i % 32);
            id = i;
            break;
        }
    }
    spin_unlock_irqrestore(&u->lock, s);
    if (id == 0)
        return -ENOSPC;
    paddr_t root = iommu_pt_alloc_table();
    if (root == 0) {
        s = spin_lock_irqsave(&u->lock);
        u->did_bits[id / 32] &= ~(1u << (id % 32));
        spin_unlock_irqrestore(&u->lock, s);
        return -ENOMEM;
    }
    d->id = id;
    d->root = root;
    return 0;
}

static void vtd_domain_fini(struct iommu_unit *iu, struct iommu_domain *d)
{
    struct vtd_unit *u = of(iu);
    iommu_pt_free(d->root, &g_fmt);
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    u->did_bits[d->id / 32] &= ~(1u << (d->id % 32));
    spin_unlock_irqrestore(&u->lock, s);
}

static int vtd_attach(struct iommu_unit *iu, struct iommu_domain *d, uint32_t sid)
{
    struct vtd_unit *u = of(iu);
    unsigned bus = (sid >> 8) & 0xff, devfn = sid & 0xff;
    if (u->ctx_table[bus] == 0) {
        paddr_t ct = iommu_pt_alloc_table();
        if (ct == 0)
            return -ENOMEM;
        u->ctx_table[bus] = ct;
        uint64_t *root = phys_to_virt(u->root_table);
        root[bus * 2] = ct | 1;   /* present, context-table pointer */
        root[bus * 2 + 1] = 0;
    }
    uint64_t *ctx = phys_to_virt(u->ctx_table[bus]);
    /* Context entry: high = AW 010 (48-bit, 4-level) | domain id << 8;
     * low = second-level table | translation type 0 | present. */
    ctx[devfn * 2 + 1] = ((uint64_t)d->id << 8) | 2u;
    arch_dma_barrier();
    ctx[devfn * 2] = (d->root & VTD_ADDR_MASK) | 1u;
    arch_dma_barrier();
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    invalidate_context(u, VTD_CCMD_GLOBAL);
    invalidate_iotlb(u, VTD_IOTLB_GLOBAL);
    spin_unlock_irqrestore(&u->lock, s);
    return 0;
}

static void vtd_detach(struct iommu_unit *iu, struct iommu_domain *d, uint32_t sid)
{
    struct vtd_unit *u = of(iu);
    unsigned bus = (sid >> 8) & 0xff, devfn = sid & 0xff;
    if (u->ctx_table[bus] == 0)
        return;
    uint64_t *ctx = phys_to_virt(u->ctx_table[bus]);
    ctx[devfn * 2] = 0;
    ctx[devfn * 2 + 1] = 0;
    arch_dma_barrier();
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    invalidate_context(u, VTD_CCMD_GLOBAL);
    invalidate_iotlb(u, VTD_IOTLB_DOMAIN | ((uint64_t)d->id << 32));
    spin_unlock_irqrestore(&u->lock, s);
}

static int vtd_map(struct iommu_domain *d, uint64_t iova, paddr_t pa, size_t pages, unsigned prot)
{
    int rc = iommu_pt_map(d->root, &g_fmt, iova, pa, pages, prot);
    arch_dma_barrier();   /* CAP.CM is 0: new entries need no invalidation, only visibility */
    return rc;
}

static void vtd_unmap(struct iommu_domain *d, uint64_t iova, size_t pages)
{
    struct vtd_unit *u = of(d->unit);
    if (iommu_pt_unmap(d->root, &g_fmt, iova, pages) == 0)
        return;
    arch_dma_barrier();
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    invalidate_iotlb(u, VTD_IOTLB_DOMAIN | ((uint64_t)d->id << 32));
    spin_unlock_irqrestore(&u->lock, s);
}

static bool vtd_lookup(struct iommu_domain *d, uint64_t iova, paddr_t *pa)
{
    return iommu_pt_lookup(d->root, &g_fmt, iova, pa);
}

static const struct iommu_ops g_ops = {
    .name = "intel-vtd",
    .covers = vtd_covers,
    .reserved = vtd_reserved,
    .domain_init = vtd_domain_init,
    .domain_fini = vtd_domain_fini,
    .attach = vtd_attach,
    .detach = vtd_detach,
    .map = vtd_map,
    .unmap = vtd_unmap,
    .lookup = vtd_lookup,
};

/* --- faults --- */

static void vtd_fault_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct vtd_unit *u = arg;
    uint32_t fsts = rd32(u, VTD_FSTS);
    if (fsts & VTD_FSTS_PPF) {
        unsigned fri = (fsts >> 8) & 0xff;
        for (unsigned n = 0; n < u->nfr; n++) {
            unsigned i = (fri + n) % u->nfr;
            unsigned off = u->fro + i * 16;
            uint64_t hi = rd64(u, off + 8);
            if (!(hi & VTD_FRCD_F))
                break;
            uint64_t lo = rd64(u, off);
            uint32_t sid = (uint32_t)(hi & 0xffff);
            unsigned reason = (unsigned)((hi >> 32) & 0xff);
            bool write = ((hi >> 30) & 1) == 0;   /* T: 0 = write, 1 = read */
            iommu_note_fault(&u->unit, sid, lo & ~0xfffull, reason, write);
            wr64(u, off + 8, VTD_FRCD_F);   /* clear the record */
        }
    }
    wr32(u, VTD_FSTS, fsts & VTD_FSTS_CLEAR);
}

/* --- DMAR parsing and bring-up --- */

struct dmar_header {
    struct acpi_sdt_header sdt;
    uint8_t host_address_width;
    uint8_t flags;
    uint8_t reserved[10];
} __packed;

struct dmar_drhd {
    uint16_t type, length;
    uint8_t flags;
    uint8_t size;
    uint16_t segment;
    uint64_t base;
} __packed;

static void parse_scopes(struct vtd_unit *u, const uint8_t *p, const uint8_t *end)
{
    while (p + 6 <= end) {
        uint8_t type = p[0], len = p[1], start_bus = p[5];
        if (len < 6 || p + len > end)
            break;
        if ((type == 1 || type == 2) && len >= 8 && u->nr_scopes < 16) {
            uint8_t dev = p[6], fn = p[7];   /* the first path entry names the device on start_bus */
            u->scope_sid[u->nr_scopes++] = ((uint32_t)start_bus << 8) | ((uint32_t)dev << 3) | fn;
        }
        p += len;
    }
}

static bool vtd_unit_init(struct vtd_unit *u, const struct dmar_drhd *drhd, const uint8_t *end)
{
    vaddr_t va = vm_map_phys((paddr_t)drhd->base, PAGE_SIZE, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0)
        return false;
    u->regs = (volatile uint8_t *)va;
    u->unit.ops = &g_ops;
    ksnprintf(u->unit.name, sizeof(u->unit.name), "intel-vtd%u", g_nunits);
    spinlock_init(&u->lock, "vtd");
    u->include_all = (drhd->flags & 1) != 0;
    parse_scopes(u, (const uint8_t *)drhd + sizeof(*drhd), end);
    u->cap = rd64(u, VTD_CAP);
    u->ecap = rd64(u, VTD_ECAP);
    if (!(VTD_CAP_SAGAW(u->cap) & 4)) {
        kerror("iommu: %s: no 48-bit (4-level) second-level paging (SAGAW 0x%x); not used", u->unit.name,
               (unsigned)VTD_CAP_SAGAW(u->cap));
        return false;
    }
    u->fro = VTD_CAP_FRO(u->cap);
    u->nfr = VTD_CAP_NFR(u->cap);
    u->iro = VTD_ECAP_IRO(u->ecap);
    u->ndomains = 1u << (4 + 2 * VTD_CAP_ND(u->cap));
    if (u->ndomains > 4096)
        u->ndomains = 4096;
    u->root_table = iommu_pt_alloc_table();
    if (u->root_table == 0)
        return false;

    /* The root table, then translation on with no context present. */
    wr64(u, VTD_RTADDR, u->root_table);
    wr32(u, VTD_GCMD, VTD_GCMD_SRTP);
    if (!wait32(u, VTD_GSTS, VTD_GSTS_RTPS, true)) {
        kerror("iommu: %s: root table pointer not accepted", u->unit.name);
        return false;
    }
    invalidate_context(u, VTD_CCMD_GLOBAL);
    invalidate_iotlb(u, VTD_IOTLB_GLOBAL);

    /* The fault event as an MSI on CPU 0. */
    struct irq_msi_msg msg;
    u->fault_vector = irq_request_msi(vtd_fault_irq, u, "vtd-fault", 0, &msg);
    if (u->fault_vector >= 0) {
        wr32(u, VTD_FEDATA, msg.data);
        wr32(u, VTD_FEADDR, (uint32_t)msg.addr);
        wr32(u, VTD_FEUADDR, (uint32_t)(msg.addr >> 32));
        wr32(u, VTD_FECTL, 0);   /* unmask */
    } else {
        kwarn("iommu: %s: no vector for the fault event; faults are counted only when polled", u->unit.name);
    }

    wr32(u, VTD_GCMD, VTD_GCMD_TE);
    if (!wait32(u, VTD_GSTS, VTD_GSTS_TES, true)) {
        kerror("iommu: %s: translation enable not acknowledged", u->unit.name);
        return false;
    }
    kinfo("iommu: %s at %p: version %u.%u, %u domains, %u fault records, %s, coherent %u, caching %u; translation on",
          u->unit.name, (void *)(uintptr_t)drhd->base, rd32(u, VTD_VER) >> 4, rd32(u, VTD_VER) & 0xf, u->ndomains,
          u->nfr, u->include_all ? "all PCI devices" : "listed devices only", (unsigned)VTD_ECAP_C(u->ecap),
          (unsigned)VTD_CAP_CM(u->cap));
    return true;
}

void intel_vtd_init(void)
{
    const struct dmar_header *dmar = (const struct dmar_header *)acpi_find_table("DMAR");
    if (dmar == NULL)
        return;
    const uint8_t *p = (const uint8_t *)dmar + sizeof(*dmar), *end = (const uint8_t *)dmar + dmar->sdt.length;
    while (p + 4 <= end && g_nunits < VTD_MAX_UNITS) {
        const struct dmar_drhd *h = (const struct dmar_drhd *)p;
        if (h->length < 4 || p + h->length > end)
            break;
        if (h->type == 0 && h->length >= sizeof(*h) && h->segment == 0) {
            struct vtd_unit *u = &g_units[g_nunits];
            if (vtd_unit_init(u, h, p + h->length)) {
                g_nunits++;
                iommu_register_unit(&u->unit);
            }
        }
        p += h->length;
    }
}

#else
void intel_vtd_init(void) {}
#endif
