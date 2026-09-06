/*
 * arm_smmuv3.c - ARM SMMUv3 stage-2 DMA remapping for the virt machine
 * (docs/kernel/iommu/design.md §5). A linear stream table of 256 entries,
 * stage-2 translation through the generic walker (LPAE descriptors), a
 * polled command queue, an event queue on a wired SPI.
 */

#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <kernel/iommu.h>
#include <kernel/irq.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/string.h>
#include <kernel/vmm.h>
#include <arch/cpu.h>

#include <kernel/iommu_pt.h>

#if defined(ARCH_AARCH64)
/* The unit is described by the ACPI IORT, a static table (no AML): its
 * register base and the event and global-error interrupts come from
 * there, so a machine without an SMMU is recognised before a single
 * register is touched — an unassigned MMIO read is an external abort on
 * this architecture, not a bus value of ones. The virt machine's
 * defaults, for reference: two 64 KiB pages at 0x09050000, SPIs 74-77 =
 * INTIDs 106-109 (eventq, priq, cmdq-sync, gerror). The MSI doorbell the
 * devices write to is the MADT's GIC MSI frame. */
#define VIRT_SMMU_EVENTQ_INTID 106u            /* only if the IORT leaves the GSIV zero */
#define VIRT_SMMU_GERROR_INTID 109u
#define VIRT_GICV2M_BASE       0x08020000ull   /* only if the MADT has no MSI frame */
#define IORT_NODE_SMMUV3       4u

#define SMMU_IDR0        0x000
#define SMMU_IDR1        0x004
#define SMMU_IDR5        0x014
#define SMMU_CR0         0x020
#define SMMU_CR0ACK      0x024
#define SMMU_CR1         0x028
#define SMMU_CR2         0x02c
#define SMMU_GBPA        0x044
#define SMMU_IRQ_CTRL    0x050
#define SMMU_IRQ_CTRLACK 0x054
#define SMMU_GERROR      0x060
#define SMMU_GERRORN     0x064
#define SMMU_STRTAB_BASE 0x080
#define SMMU_STRTAB_BASE_CFG 0x088
#define SMMU_CMDQ_BASE   0x090
#define SMMU_CMDQ_PROD   0x098
#define SMMU_CMDQ_CONS   0x09c
#define SMMU_EVENTQ_BASE 0x0a0
#define SMMU_EVENTQ_PROD 0x100a8
#define SMMU_EVENTQ_CONS 0x100ac

#define IDR0_S2P     (1u << 0)
#define IDR0_VMID16  (1u << 18)
#define CR0_SMMUEN   (1u << 0)
#define CR0_EVENTQEN (1u << 2)
#define CR0_CMDQEN   (1u << 3)
#define GBPA_UPDATE  (1u << 31)
#define GBPA_ABORT   (1u << 20)
#define IRQ_CTRL_GERROR_IRQEN (1u << 0)
#define IRQ_CTRL_EVENTQ_IRQEN (1u << 2)
#define Q_RA_WA      (1ull << 62)

#define CMD_CFGI_STE     0x03
#define CMD_CFGI_ALL     0x04
#define CMD_TLBI_S2_IPA  0x2a
#define CMD_TLBI_S12_VMALL 0x28
#define CMD_TLBI_NSNH_ALL 0x30
#define CMD_SYNC         0x46

#define STE_V            (1ull << 0)
#define STE_CFG_S2       (6ull << 1)
#define STE_S2AA64       (1ull << 51)
#define STE_S2R          (1ull << 58)
/* S2VTCR without T0SZ and PS (from IDR5.OAS at probe): SL0 2 (start at
 * level 0), IRGN0/ORGN0 write-back, SH0 inner, TG0 4 KiB. */
#define STE_S2VTCR_FIXED ((uint64_t)((2 << 6) | (1 << 8) | (1 << 10) | (3 << 12) | (0 << 14)) << 32)

#define STRTAB_LOG2      8u           /* stream ids 0..255: every function of bus 0 */
#define STRTAB_ENTRIES   (1u << STRTAB_LOG2)
#define QUEUE_LOG2       8u
#define QUEUE_ENTRIES    (1u << QUEUE_LOG2)

/* Stage-2 LPAE descriptors. */
#define S2_TABLE         0x3ull
#define S2_PAGE_RW       (0x3ull | (0xfull << 2) | (3ull << 6) | (3ull << 8) | (1ull << 10))
#define S2_PAGE_RO       (0x3ull | (0xfull << 2) | (1ull << 6) | (3ull << 8) | (1ull << 10))
#define S2_ADDR_MASK     0x0000fffffffff000ull

struct smmu {
    struct iommu_unit unit;
    volatile uint8_t *regs;
    paddr_t strtab, cmdq, eventq;
    uint32_t cmdq_prod;
    uint32_t eventq_cons;
    uint32_t vmid_bits[65536 / 32];
    unsigned vmid_max;
    unsigned oas_bits, ps;       /* the output address size IDR5 reports and its PS encoding */
    uint64_t msi_frame;          /* the MSI doorbell every domain identity-maps */
    spinlock_t lock;
};

/* What the IORT says about the unit: its registers and its interrupts. */
struct smmu_desc {
    paddr_t base;
    unsigned event_intid, gerror_intid;
};

static struct smmu g_smmu;
static bool g_present;

static inline uint32_t rd32(struct smmu *u, unsigned off) { return *(volatile uint32_t *)(u->regs + off); }
static inline void wr32(struct smmu *u, unsigned off, uint32_t v) { *(volatile uint32_t *)(u->regs + off) = v; }
static inline void wr64(struct smmu *u, unsigned off, uint64_t v) { *(volatile uint64_t *)(u->regs + off) = v; }

static bool wait_eq(struct smmu *u, unsigned off, uint32_t want)
{
    for (unsigned i = 0; i < 1000000; i++)
        if (rd32(u, off) == want)
            return true;
    return false;
}

/* --- the command queue (u->lock held) --- */

/* False when the queue never made room: the command was NOT written, and
 * the caller may not treat whatever it asked for as done. */
static bool cmd_issue(struct smmu *u, uint64_t w0, uint64_t w1)
{
    uint32_t mask = (QUEUE_ENTRIES << 1) - 1;   /* index plus the wrap bit */
    bool space = false;
    for (unsigned spin = 0; spin < 1000000 && !space; spin++) {
        uint32_t cons = rd32(u, SMMU_CMDQ_CONS) & mask;
        space = !((u->cmdq_prod & (QUEUE_ENTRIES - 1)) == (cons & (QUEUE_ENTRIES - 1)) &&
                  ((u->cmdq_prod ^ cons) & QUEUE_ENTRIES) != 0);
    }
    if (!space) {
        kwarn("iommu: %s: command queue full; command dropped", u->unit.name);
        return false;
    }
    uint64_t *e = (uint64_t *)((uint8_t *)phys_to_virt(u->cmdq) + (u->cmdq_prod & (QUEUE_ENTRIES - 1)) * 16);
    e[0] = w0;
    e[1] = w1;
    arch_dma_barrier();
    u->cmdq_prod = (u->cmdq_prod + 1) & mask;
    wr32(u, SMMU_CMDQ_PROD, u->cmdq_prod);
    return true;
}

/* False when the queue did not drain: the commands before it may not
 * have taken effect. */
static bool cmd_sync(struct smmu *u)
{
    if (!cmd_issue(u, CMD_SYNC, 0))
        return false;
    uint32_t mask = (QUEUE_ENTRIES << 1) - 1;
    for (unsigned spin = 0; spin < 1000000; spin++)
        if ((rd32(u, SMMU_CMDQ_CONS) & mask) == u->cmdq_prod)
            return true;
    kwarn("iommu: %s: command queue did not drain", u->unit.name);
    return false;
}

/* --- the page-table format --- */

static uint64_t s2_table(paddr_t next) { return (next & S2_ADDR_MASK) | S2_TABLE; }
static uint64_t s2_leaf(paddr_t pa, unsigned prot)
{
    return (pa & S2_ADDR_MASK) | ((prot & IOMMU_PROT_WRITE) ? S2_PAGE_RW : S2_PAGE_RO);
}
static bool s2_present(uint64_t e) { return (e & 1) != 0; }
static paddr_t s2_addr(uint64_t e) { return e & S2_ADDR_MASK; }
static const struct iommu_pt_fmt g_fmt = { s2_table, s2_leaf, s2_present, s2_addr };

/* --- iommu_ops --- */

static struct smmu *of(struct iommu_unit *iu) { return (struct smmu *)iu; }

static bool smmu_covers(struct iommu_unit *iu, uint32_t sid)
{
    (void)iu;
    (void)sid;
    return true;   /* the virt machine's whole PCI root complex sits behind it */
}

/* A device's MSI is a write to the GIC's MSI frame, translated like any
 * other: every domain maps that page to itself. */
static unsigned smmu_reserved(struct iommu_unit *iu, struct iommu_range *out, unsigned max)
{
    if (max == 0)
        return 0;
    out[0] = (struct iommu_range){ .base = of(iu)->msi_frame, .len = PAGE_SIZE, .identity = true };
    return 1;
}

static int smmu_domain_init(struct iommu_unit *iu, struct iommu_domain *d)
{
    struct smmu *u = of(iu);
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    unsigned id = 0;
    for (unsigned i = 1; i <= u->vmid_max; i++) {
        if (!((u->vmid_bits[i / 32] >> (i % 32)) & 1u)) {
            u->vmid_bits[i / 32] |= 1u << (i % 32);
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
        u->vmid_bits[id / 32] &= ~(1u << (id % 32));
        spin_unlock_irqrestore(&u->lock, s);
        return -ENOMEM;
    }
    d->id = id;
    d->root = root;
    return 0;
}

static void smmu_domain_fini(struct iommu_unit *iu, struct iommu_domain *d)
{
    struct smmu *u = of(iu);
    iommu_pt_free(d->root, &g_fmt);
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    u->vmid_bits[d->id / 32] &= ~(1u << (d->id % 32));
    spin_unlock_irqrestore(&u->lock, s);
}

static int smmu_attach(struct iommu_unit *iu, struct iommu_domain *d, uint32_t sid)
{
    struct smmu *u = of(iu);
    if (sid >= STRTAB_ENTRIES)
        return -ERANGE;
    uint64_t *ste = (uint64_t *)((uint8_t *)phys_to_virt(u->strtab) + sid * 64);
    ste[1] = 0;
    /* T0SZ = 64 - the output size (the input space the SMMU accepts),
     * PS = the output size: both from IDR5. */
    uint64_t vtcr = STE_S2VTCR_FIXED | ((uint64_t)(64 - u->oas_bits) << 32) | ((uint64_t)u->ps << (32 + 16));
    ste[2] = (uint64_t)d->id | vtcr | STE_S2AA64 | STE_S2R;
    ste[3] = d->root & 0x000ffffffffffff0ull;
    ste[4] = ste[5] = ste[6] = ste[7] = 0;
    arch_dma_barrier();
    ste[0] = STE_V | STE_CFG_S2;
    arch_dma_barrier();
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    bool ok = cmd_issue(u, CMD_CFGI_STE | ((uint64_t)sid << 32), 1 /* leaf */);
    ok = cmd_sync(u) && ok;
    spin_unlock_irqrestore(&u->lock, s);
    return ok ? 0 : -EIO;
}

static int smmu_detach(struct iommu_unit *iu, struct iommu_domain *d, uint32_t sid)
{
    struct smmu *u = of(iu);
    if (sid >= STRTAB_ENTRIES)
        return 0;
    uint64_t *ste = (uint64_t *)((uint8_t *)phys_to_virt(u->strtab) + sid * 64);
    ste[0] = 0;
    arch_dma_barrier();
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    bool ok = cmd_issue(u, CMD_CFGI_STE | ((uint64_t)sid << 32), 1);
    ok = cmd_issue(u, CMD_TLBI_S12_VMALL | ((uint64_t)d->id << 32), 0) && ok;
    ok = cmd_sync(u) && ok;
    spin_unlock_irqrestore(&u->lock, s);
    return ok ? 0 : -EIO;   /* the stream may still be translating */
}

static int smmu_map(struct iommu_domain *d, uint64_t iova, paddr_t pa, size_t pages, unsigned prot)
{
    int rc = iommu_pt_map(d->root, &g_fmt, iova, pa, pages, prot);
    arch_dma_barrier();
    return rc;
}

/* -EIO when the invalidation was not confirmed: the entries are gone from
 * the tables, but the unit may still be translating from its TLB, so the
 * caller may not let anything reuse the addresses or the pages. */
static int smmu_unmap(struct iommu_domain *d, uint64_t iova, size_t pages)
{
    struct smmu *u = of(d->unit);
    if (iommu_pt_unmap(d->root, &g_fmt, iova, pages) == 0)
        return 0;
    arch_dma_barrier();
    bool ok = true;
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    if (pages > 32) {
        ok = cmd_issue(u, CMD_TLBI_S12_VMALL | ((uint64_t)d->id << 32), 0);
    } else {
        for (size_t k = 0; k < pages; k++)
            ok = cmd_issue(u, CMD_TLBI_S2_IPA | ((uint64_t)d->id << 32),
                           ((iova + (uint64_t)k * PAGE_SIZE) & 0x000ffffffffff000ull) | 1 /* leaf */) &&
                 ok;
    }
    ok = cmd_sync(u) && ok;
    spin_unlock_irqrestore(&u->lock, s);
    return ok ? 0 : -EIO;
}

static bool smmu_lookup(struct iommu_domain *d, uint64_t iova, paddr_t *pa)
{
    return iommu_pt_lookup(d->root, &g_fmt, iova, pa);
}

static const struct iommu_ops g_ops = {
    .name = "arm-smmuv3",
    .covers = smmu_covers,
    .reserved = smmu_reserved,
    .domain_init = smmu_domain_init,
    .domain_fini = smmu_domain_fini,
    .attach = smmu_attach,
    .detach = smmu_detach,
    .map = smmu_map,
    .unmap = smmu_unmap,
    .lookup = smmu_lookup,
};

/* --- interrupts --- */

static void smmu_event_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct smmu *u = arg;
    uint32_t mask = (QUEUE_ENTRIES << 1) - 1;
    uint32_t prod = rd32(u, SMMU_EVENTQ_PROD) & mask;
    while (u->eventq_cons != prod) {
        const uint64_t *e = (const uint64_t *)((const uint8_t *)phys_to_virt(u->eventq) +
                                               (u->eventq_cons & (QUEUE_ENTRIES - 1)) * 32);
        unsigned type = (unsigned)(e[0] & 0xff);
        uint32_t sid = (uint32_t)(e[0] >> 32);
        bool write = ((e[1] >> 6) & 1) != 0;   /* RnW: 0 = write */
        iommu_note_fault(&u->unit, sid, e[2], type, !write);
        u->eventq_cons = (u->eventq_cons + 1) & mask;
    }
    wr32(u, SMMU_EVENTQ_CONS, u->eventq_cons);
}

static void smmu_gerror_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct smmu *u = arg;
    uint32_t err = rd32(u, SMMU_GERROR);
    kwarn("iommu: %s: global error 0x%x", u->unit.name, err);
    wr32(u, SMMU_GERRORN, err);   /* acknowledge everything reported */
}

/* --- bring-up --- */

static paddr_t alloc_zeroed(size_t bytes)
{
    unsigned order = 0;
    while (((size_t)PAGE_SIZE << order) < bytes)
        order++;
    struct page *pg = pmm_alloc_pages(order, PMM_FLAGS_ZERO);
    return pg ? page_to_phys(pg) : 0;
}

/* The IORT's first SMMUv3 node (type 4): base address at offset 16 of
 * the node, the event and global-error GSIVs at 44 and 52. Fields are
 * read with memcpy: nothing in the table is guaranteed aligned. */
static bool iort_find_smmuv3(struct smmu_desc *out)
{
    const struct acpi_sdt_header *h = acpi_find_table("IORT");
    if (h == NULL || h->length < 48)
        return false;
    const uint8_t *t = (const uint8_t *)h;
    uint32_t count, off;
    memcpy(&count, t + 36, 4);
    memcpy(&off, t + 40, 4);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t len;
        if (off + 16 > h->length)
            return false;
        memcpy(&len, t + off + 1, 2);
        if (len < 16 || (uint64_t)off + len > h->length)
            return false;
        if (t[off] == IORT_NODE_SMMUV3 && len >= 56) {
            uint64_t base;
            uint32_t ev, gerr;
            memcpy(&base, t + off + 16, 8);
            memcpy(&ev, t + off + 44, 4);
            memcpy(&gerr, t + off + 52, 4);
            out->base = (paddr_t)base;
            out->event_intid = ev ? ev : VIRT_SMMU_EVENTQ_INTID;
            out->gerror_intid = gerr ? gerr : VIRT_SMMU_GERROR_INTID;
            return base != 0;
        }
        off += len;
    }
    return false;
}

void arm_smmuv3_init(void)
{
    struct smmu *u = &g_smmu;
    struct smmu_desc desc;
    if (!iort_find_smmuv3(&desc))
        return;   /* no SMMU on this machine: nothing may touch its registers */
    struct acpi_gic gic;
    u->msi_frame = acpi_madt_gic(&gic) && gic.v2m_base ? gic.v2m_base : VIRT_GICV2M_BASE;
    vaddr_t va = vm_map_phys(desc.base, 2 * 65536, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0)
        return;
    u->regs = (volatile uint8_t *)va;
    uint32_t idr0 = rd32(u, SMMU_IDR0);
    if (!(idr0 & IDR0_S2P)) {
        kwarn("iommu: smmuv3 offers no stage-2 translation; not used");
        vm_unmap_phys(va);
        return;
    }
    uint32_t idr1 = rd32(u, SMMU_IDR1);
    unsigned sidsize = idr1 & 0x3f, cmdqs = (idr1 >> 21) & 0x1f, eventqs = (idr1 >> 16) & 0x1f;
    if (cmdqs < QUEUE_LOG2 || eventqs < QUEUE_LOG2) {
        kwarn("iommu: smmuv3 queues too small (cmdq 2^%u, eventq 2^%u); not used", cmdqs, eventqs);
        vm_unmap_phys(va);
        return;
    }
    u->unit.ops = &g_ops;
    strlcpy(u->unit.name, "arm-smmuv3", sizeof(u->unit.name));
    spinlock_init(&u->lock, "smmu");
    u->vmid_max = (idr0 & IDR0_VMID16) ? 65535u : 255u;
    static const unsigned oas_table[8] = { 32, 36, 40, 42, 44, 48, 52, 48 };
    u->ps = rd32(u, SMMU_IDR5) & 7;
    if (u->ps > 5)
        u->ps = 5;   /* 48 bits: what the 4-level walker covers */
    u->oas_bits = oas_table[u->ps];

    /* Off, then tables and queues, then on. */
    wr32(u, SMMU_CR0, 0);
    wait_eq(u, SMMU_CR0ACK, 0);
    wr32(u, SMMU_CR1, (3u << 10) | (1u << 8) | (1u << 6) | (3u << 4) | (1u << 2) | 1u);   /* inner-shareable, write-back */
    wr32(u, SMMU_CR2, 0);
    u->strtab = alloc_zeroed(STRTAB_ENTRIES * 64);
    u->cmdq = alloc_zeroed(QUEUE_ENTRIES * 16);
    u->eventq = alloc_zeroed(QUEUE_ENTRIES * 32);
    if (!u->strtab || !u->cmdq || !u->eventq) {
        kerror("iommu: smmuv3: no memory for its tables");
        return;
    }
    wr64(u, SMMU_STRTAB_BASE, (u->strtab & 0x000ffffffffffc0ull) | Q_RA_WA);
    wr32(u, SMMU_STRTAB_BASE_CFG, STRTAB_LOG2);   /* linear */
    wr64(u, SMMU_CMDQ_BASE, (u->cmdq & 0x000fffffffffffe0ull) | Q_RA_WA | QUEUE_LOG2);
    wr32(u, SMMU_CMDQ_PROD, 0);
    wr32(u, SMMU_CMDQ_CONS, 0);
    u->cmdq_prod = 0;
    wr64(u, SMMU_EVENTQ_BASE, (u->eventq & 0x000fffffffffffe0ull) | Q_RA_WA | QUEUE_LOG2);
    wr32(u, SMMU_EVENTQ_PROD, 0);
    wr32(u, SMMU_EVENTQ_CONS, 0);
    u->eventq_cons = 0;
    /* Streams the table does not name abort (GBPA governs while SMMUEN
     * is 0; with it on, an invalid STE aborts and records an event). */
    wr32(u, SMMU_GBPA, GBPA_UPDATE | GBPA_ABORT);
    for (unsigned i = 0; i < 1000000 && (rd32(u, SMMU_GBPA) & GBPA_UPDATE); i++)
        ;

    wr32(u, SMMU_CR0, CR0_CMDQEN);
    wait_eq(u, SMMU_CR0ACK, CR0_CMDQEN);
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    (void)cmd_issue(u, CMD_CFGI_ALL, 31);   /* every STE: range 2^31 */
    (void)cmd_issue(u, CMD_TLBI_NSNH_ALL, 0);
    (void)cmd_sync(u);   /* nothing is attached yet: a failure here shows up at the first attach */
    spin_unlock_irqrestore(&u->lock, s);
    wr32(u, SMMU_CR0, CR0_CMDQEN | CR0_EVENTQEN);
    wait_eq(u, SMMU_CR0ACK, CR0_CMDQEN | CR0_EVENTQEN);

    if (irq_request(desc.event_intid, smmu_event_irq, u, "smmu-eventq", IRQ_TRIGGER_EDGE, 0) == 0)
        irq_enable(desc.event_intid);
    else
        kwarn("iommu: smmuv3: cannot request the event queue interrupt");
    if (irq_request(desc.gerror_intid, smmu_gerror_irq, u, "smmu-gerror", IRQ_TRIGGER_EDGE, 0) == 0)
        irq_enable(desc.gerror_intid);
    wr32(u, SMMU_IRQ_CTRL, IRQ_CTRL_GERROR_IRQEN | IRQ_CTRL_EVENTQ_IRQEN);
    wait_eq(u, SMMU_IRQ_CTRLACK, IRQ_CTRL_GERROR_IRQEN | IRQ_CTRL_EVENTQ_IRQEN);

    wr32(u, SMMU_CR0, CR0_CMDQEN | CR0_EVENTQEN | CR0_SMMUEN);
    if (!wait_eq(u, SMMU_CR0ACK, CR0_CMDQEN | CR0_EVENTQEN | CR0_SMMUEN)) {
        kerror("iommu: smmuv3: enable not acknowledged");
        return;
    }
    g_present = true;
    iommu_register_unit(&u->unit);
    kinfo("iommu: %s at %p: stream ids %u bits (%u tabled), VMIDs to %u, stage 2, %u-bit addresses, 4 KiB granule; translation on",
          u->unit.name, (void *)(uintptr_t)desc.base, sidsize, STRTAB_ENTRIES, u->vmid_max, u->oas_bits);
}

#else
void arm_smmuv3_init(void) {}
#endif
