/*
 * gicv3.c - GICv3 distributor, redistributors and the system-register
 * CPU interface (aarch64/irqc.h, docs/kernel/arch/aarch64/design.md).
 *
 * The other implementation of `struct aarch64_irqc_ops`; irqc.c decides
 * which machine gets it. Three things move relative to GICv2, and they
 * are the whole of the difference:
 *
 *   - The CPU interface is system registers (`ICC_*_EL1`), not an MMIO
 *     window, so acknowledging an interrupt is an `mrs`.
 *   - SGIs and PPIs are configured in a per-CPU redistributor rather
 *     than in the distributor's banked registers.
 *   - An SPI is routed by writing a 64-bit affinity to `GICD_IROUTER`,
 *     and an SGI is sent by writing an affinity to `ICC_SGI1R_EL1`.
 *     Neither has the eight-bit CPU mask that capped GICv2 at eight
 *     CPUs.
 *
 * MSI has an order of preference, decided at init from the MADT: the
 * ITS (`gicv3_its.c`) if firmware described one, a GICv2m frame
 * (`gicv2m.c`, shared with the GICv2 driver) if it described one of
 * those instead, and otherwise -ENODEV -- a decline, since no driver in
 * this tree falls back to INTx. The ITS is what real GICv3 hardware
 * offers; the frame is what QEMU will still give a GICv3 on request,
 * and keeping it is what let the CPU-interface work be tested before
 * the ITS existed.
 */

#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>
#include <kernel/vmm.h>
#include <arch/cpu.h>
#include <arch/irqc.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <aarch64/gicv2m.h>
#include <aarch64/gicv3_its.h>
#include <aarch64/irqc.h>
#include <aarch64/platform.h>
#include <aarch64/sysreg.h>
#include <aarch64/trapframe.h>

/* Distributor. The first half is GICv2's register file unchanged; what
 * is new is IROUTER and the control bits that make it mean anything. */
#define GICD_CTLR       0x0000
#define GICD_TYPER      0x0004
#define GICD_IGROUPR    0x0080
#define GICD_ISENABLER  0x0100
#define GICD_ICENABLER  0x0180
#define GICD_ISPENDR    0x0200
#define GICD_ICPENDR    0x0280
#define GICD_ICACTIVER  0x0380
#define GICD_IPRIORITYR 0x0400
#define GICD_ICFGR      0x0C00
#define GICD_IROUTER    0x6000   /* 64 bits per SPI, indexed by INTID */

#define GICD_CTLR_ENABLE_G1  (1u << 0)
#define GICD_CTLR_ENABLE_G1A (1u << 1)
#define GICD_CTLR_ARE_NS     (1u << 4)   /* affinity routing: IROUTER, not ITARGETSR */
#define GICD_CTLR_RWP        (1u << 31)  /* a previous write is still taking effect */
#define GICD_TYPER_RSS       (1u << 26)  /* SGI target lists have a range selector */
#define ICC_CTLR_RSS         (1ull << 18)

/* Redistributor: two 64 KiB frames per CPU, the second holding what the
 * distributor used to bank. GICv4 adds two more, which is why the
 * stride is read from the first frame rather than assumed. */
#define GICR_CTLR        0x0000
#define GICR_TYPER       0x0008
#define GICR_WAKER       0x0014
#define GICR_TYPER_VLPIS (1ull << 1)
#define GICR_TYPER_LAST  (1ull << 4)
#define GICR_WAKER_PS    (1u << 1)   /* ProcessorSleep */
#define GICR_WAKER_CA    (1u << 2)   /* ChildrenAsleep */
#define GICR_CTLR_RWP    (1u << 3)
#define GICR_STRIDE      0x20000u
#define GICR_STRIDE_VLPI 0x40000u

#define GICR_PROPBASER    0x0070
#define GICR_PENDBASER    0x0078
#define GICR_TYPER_PLPIS  (1ull << 0)
#define GICR_CTLR_ENABLE_LPIS (1u << 0)

#define GICR_SGI_BASE     0x10000
#define GICR_IGROUPR0     (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0   (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0   (GICR_SGI_BASE + 0x0180)
#define GICR_ICPENDR0     (GICR_SGI_BASE + 0x0280)
#define GICR_ICACTIVER0   (GICR_SGI_BASE + 0x0380)
#define GICR_IPRIORITYR   (GICR_SGI_BASE + 0x0400)
#define GICR_ICFGR0       (GICR_SGI_BASE + 0x0C00)

/* ICC_SRE_EL1: SRE selects the system-register interface at all; DFB and
 * DIB stop FIQ and IRQ bypassing the interface when it is disabled. */
#define ICC_SRE_SRE (1ull << 0)
#define ICC_SRE_DFB (1ull << 1)
#define ICC_SRE_DIB (1ull << 2)
/* ICC_CTLR_EL1: CBPR and EOImode both off -- one binary point, and EOI
 * drops the priority and deactivates in one write. */
#define ICC_CTLR_CBPR   (1ull << 0)
#define ICC_CTLR_EOIMODE (1ull << 1)

/* Priorities are the non-secure view's: PMR admits anything numerically
 * below it, so a default of 0xA0 is delivered and 0xF0 masks nothing. */
#define PRIORITY_DEFAULT 0xA0u
#define PMR_UNMASKED     0xF0u

/*
 * LPIs. Interrupt ids from 8192 up, delivered to a redistributor rather
 * than through the distributor, and the only interrupts an ITS can
 * produce. One per dynamic vector is all this kernel can use, but the
 * property table must still cover the whole id space its `IDbits` says
 * it does -- fourteen bits is the architectural minimum -- and the
 * pending table must be 64 KiB aligned, so both are bigger than the 256
 * entries actually in play.
 */
#define LPI_BASE      8192u
#define NR_LPIS       VEC_DYNAMIC_COUNT
#define LPI_ID_BITS   14u
/* The property table needs (2^IDbits - 8192) bytes and GICR_PROPBASER
 * would take a 4 KiB alignment, but every implementation and every
 * other kernel gives it 64 KiB; matching that costs 56 KiB once and
 * removes a class of doubt about hardware this has never run on. */
#define LPI_PROP_BYTES (64u * 1024u)
#define LPI_PEND_BYTES (64u * 1024u)
#define LPI_ENABLED   (1u << 0)

static volatile uint32_t *g_gicd;
static paddr_t g_gicd_pa;
static unsigned g_nr_lines;
static struct gicv2m g_v2m;

static volatile uint8_t *g_gicr_window;    /* the discovery window, mapped once */
static paddr_t g_gicr_pa;
static uint64_t g_gicr_len;
static unsigned g_gicr_stride = GICR_STRIDE;
static bool g_rss;                                  /* SGI target lists reach past Aff0 15 */
static volatile uint8_t *g_gicr[CONFIG_MAX_CPUS];   /* each CPU's own frame */
static uint32_t g_affinity[CONFIG_MAX_CPUS];        /* MPIDR affinity, packed */

static struct gicv3_its *g_its;                   /* NULL: MSI comes from the frame, or not at all */
static uint8_t *g_lpi_prop;                       /* the shared LPI property table */
static paddr_t g_lpi_prop_pa;
static uint16_t g_lpi_vector[NR_LPIS];            /* LPI index -> vector, 0xFFFF none */
static uint16_t g_lpi_of_vector[VEC_DYNAMIC_COUNT];
static uint32_t g_lpi_devid[NR_LPIS];             /* who writes it, for the DISCARD */
static uint8_t g_lpi_cpu[NR_LPIS];
static uint64_t g_lpi_used[NR_LPIS / 64];

static spinlock_t g_lock = SPINLOCK_INIT("gicv3");
static uint16_t g_vector_of[GIC_INTID_COUNT];     /* INTID -> vector (identity when unrouted) */
static uint16_t g_intid_of[VEC_DYNAMIC_COUNT];    /* dynamic vector -> INTID, 0xFFFF none */
static uint64_t g_vector_used[VEC_DYNAMIC_COUNT / 64];
static int g_sgi_of_vector[VEC_DYNAMIC_COUNT];    /* dynamic vector -> SGI id, -1 none */
static int g_sgi_vector[GIC_SGI_COUNT];           /* SGI id -> vector, -1 none */
static uint32_t g_routed_ppi_mask;                /* PPIs to enable on every CPU */
static unsigned g_cur_intid[CONFIG_MAX_CPUS];
static uint64_t g_spurious;

static inline uint32_t gicd_rd(unsigned off) { return g_gicd[off / 4]; }
static inline void gicd_wr(unsigned off, uint32_t v) { g_gicd[off / 4] = v; }
static inline void gicd_wr8(unsigned off, uint8_t v)
{
    volatile uint8_t *b = (volatile uint8_t *)g_gicd;
    b[off] = v;
}
static inline void gicd_wr64(unsigned off, uint64_t v)
{
    volatile uint64_t *q = (volatile uint64_t *)((volatile uint8_t *)g_gicd + off);
    *q = v;
}

static inline uint32_t gicr_rd(unsigned cpu, unsigned off)
{
    return *(volatile uint32_t *)(g_gicr[cpu] + off);
}
static inline void gicr_wr(unsigned cpu, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(g_gicr[cpu] + off) = v;
}

static inline void gicr_wr64(unsigned cpu, unsigned off, uint64_t v)
{
    *(volatile uint64_t *)(g_gicr[cpu] + off) = v;
}
static inline uint64_t gicr_rd64(unsigned cpu, unsigned off)
{
    return *(volatile uint64_t *)(g_gicr[cpu] + off);
}

/* A write to an enable or control register is not in effect until the
 * controller says so; reading back too early sees the old state. */
static void gicd_wait_rwp(void)
{
    while (gicd_rd(GICD_CTLR) & GICD_CTLR_RWP)
        arch_cpu_relax();
}
static void gicr_wait_rwp(unsigned cpu)
{
    while (gicr_rd(cpu, GICR_CTLR) & GICR_CTLR_RWP)
        arch_cpu_relax();
}

static bool vector_is_dynamic(unsigned v)
{
    return v >= VEC_DYNAMIC_BASE && v < VEC_DYNAMIC_BASE + VEC_DYNAMIC_COUNT;
}

static volatile void *map(paddr_t pa, size_t len, const char *what)
{
    vaddr_t va = vm_map_phys(pa, len, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0)
        panic("gicv3: cannot map %s at 0x%llx", what, (unsigned long long)pa);
    return (volatile void *)va;
}

static paddr_t alloc_zeroed(size_t bytes)
{
    unsigned order = 0;
    while (((size_t)PAGE_SIZE << order) < bytes)
        order++;
    struct page *pg = pmm_alloc_pages(order, PMM_FLAGS_ZERO);
    return pg ? page_to_phys(pg) : 0;
}

static uint32_t this_affinity(void)
{
    return (uint32_t)MPIDR_AFFINITY(READ_SYSREG(mpidr_el1));
}

/* The affinity a GICD_IROUTER or ICC_SGI1R_EL1 wants: Aff3 at bit 32,
 * the rest where MPIDR has them. */
static uint64_t irouter_of(uint32_t aff)
{
    return (uint64_t)(aff & 0x00FFFFFFu) | ((uint64_t)(aff >> 24) << 32);
}

/* Walk the discovery window for the frame whose TYPER carries this
 * CPU's affinity. Every CPU walks it; there is no ordering to get wrong
 * because the window is read-only here. */
static volatile uint8_t *find_redistributor(uint32_t aff)
{
    for (uint64_t off = 0; off + g_gicr_stride <= g_gicr_len; off += g_gicr_stride) {
        volatile uint8_t *rd = g_gicr_window + off;
        uint64_t typer = *(volatile uint64_t *)(rd + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == aff)
            return rd;
        if (typer & GICR_TYPER_LAST)
            break;
    }
    panic("gicv3: no redistributor for affinity 0x%x", aff);
}

static void gicv3_init_cpu(void);

static void gicv3_init(const struct acpi_gic *acpi)
{
    g_gicd_pa = acpi->gicd_base ? acpi->gicd_base : VIRT_GICD_BASE;
    g_gicd = (volatile uint32_t *)map(g_gicd_pa, 0x10000, "GICD");

    /* Firmware describes the redistributors either as one window to walk
     * or as a base per GICC entry; QEMU's virt uses the window. With
     * only the per-CPU base we can still walk, because the frames are
     * contiguous in the same order -- we just have to bound the walk
     * ourselves. */
    if (acpi->gicr_base) {
        g_gicr_pa = acpi->gicr_base;
        g_gicr_len = acpi->gicr_length;
    } else if (acpi->gicc_gicr_base) {
        g_gicr_pa = acpi->gicc_gicr_base;
        g_gicr_len = (uint64_t)GICR_STRIDE_VLPI * CONFIG_MAX_CPUS;
    } else {
        panic("gicv3: the MADT describes no redistributors");
    }
    g_gicr_window = (volatile uint8_t *)map(g_gicr_pa, (size_t)g_gicr_len, "GICR");
    if (*(volatile uint64_t *)(g_gicr_window + GICR_TYPER) & GICR_TYPER_VLPIS)
        g_gicr_stride = GICR_STRIDE_VLPI;   /* GICv4: two more frames per CPU */

    for (unsigned i = 0; i < GIC_INTID_COUNT; i++)
        g_vector_of[i] = (uint16_t)i;
    for (unsigned i = 0; i < VEC_DYNAMIC_COUNT; i++) {
        g_intid_of[i] = 0xFFFF;
        g_sgi_of_vector[i] = -1;
    }
    for (unsigned i = 0; i < GIC_SGI_COUNT; i++)
        g_sgi_vector[i] = -1;

    gicd_wr(GICD_CTLR, 0);
    gicd_wait_rwp();
    g_rss = (gicd_rd(GICD_TYPER) & GICD_TYPER_RSS) != 0;
    g_nr_lines = 32u * ((gicd_rd(GICD_TYPER) & 0x1F) + 1);
    if (g_nr_lines > GIC_INTID_COUNT)
        g_nr_lines = GIC_INTID_COUNT;
    /* Every SPI: Group 1, disabled, not pending, default priority, level.
     * Group 1 rather than GICv2's Group 0 because the system-register
     * interface delivers Group 1 as IRQ; Group 0 would arrive as FIQ. */
    for (unsigned i = GIC_SPI_BASE; i < g_nr_lines; i += 32) {
        gicd_wr(GICD_ICENABLER + (i / 32) * 4, 0xFFFFFFFFu);
        gicd_wr(GICD_ICPENDR + (i / 32) * 4, 0xFFFFFFFFu);
        gicd_wr(GICD_ICACTIVER + (i / 32) * 4, 0xFFFFFFFFu);
        gicd_wr(GICD_IGROUPR + (i / 32) * 4, 0xFFFFFFFFu);
    }
    for (unsigned i = GIC_SPI_BASE; i < g_nr_lines; i += 4)
        gicd_wr(GICD_IPRIORITYR + i, 0xA0A0A0A0u);
    for (unsigned i = GIC_SPI_BASE; i < g_nr_lines; i += 16)
        gicd_wr(GICD_ICFGR + (i / 16) * 4, 0);
    gicd_wait_rwp();
    gicd_wr(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1A | GICD_CTLR_ENABLE_G1);
    gicd_wait_rwp();
    /* IROUTER means nothing until ARE is set, so it is programmed after:
     * everything to the boot CPU until something asks otherwise. */
    uint64_t boot = irouter_of(this_affinity());
    for (unsigned i = GIC_SPI_BASE; i < g_nr_lines; i++)
        gicd_wr64(GICD_IROUTER + i * 8, boot);

    for (unsigned i = 0; i < NR_LPIS; i++) {
        g_lpi_vector[i] = 0xFFFF;
        g_lpi_devid[i] = 0;
    }
    for (unsigned i = 0; i < VEC_DYNAMIC_COUNT; i++)
        g_lpi_of_vector[i] = 0xFFFF;

    /* MSI, in the order the report argued for: an ITS if firmware
     * described one, otherwise a GICv2m frame, otherwise none -- and
     * "none" is a decline, not a fallback, because no driver here
     * falls back to INTx. */
    if (acpi->its_base) {
        g_its = its_init(acpi->its_base, LPI_BASE, NR_LPIS);
        if (g_its) {
            g_lpi_prop_pa = alloc_zeroed(LPI_PROP_BYTES);
            if (g_lpi_prop_pa == 0) {
                kwarn("gicv3: no memory for the LPI property table; the ITS is not used");
                g_its = NULL;
            } else {
                g_lpi_prop = phys_to_virt(g_lpi_prop_pa);
            }
        }
    }
    if (g_its == NULL)
        gicv2m_init(&g_v2m, acpi, g_nr_lines);
    gicv3_init_cpu();
    kinfo("gicv3: GICD at 0x%llx, GICR 0x%llx+0x%llx stride 0x%x, %u lines, MSI %s",
          (unsigned long long)g_gicd_pa, (unsigned long long)g_gicr_pa,
          (unsigned long long)g_gicr_len, g_gicr_stride, g_nr_lines,
          g_its ? "via the ITS" : g_v2m.spi_count ? "via GICv2m" : "unavailable");
}

static void gicv3_init_cpu(void)
{
    unsigned cpu = arch_cpu_id();
    uint32_t aff = this_affinity();
    g_affinity[cpu] = aff;
    g_gicr[cpu] = find_redistributor(aff);

    /* Out of sleep first: a sleeping redistributor accepts no writes to
     * the SGI frame and delivers nothing. */
    gicr_wr(cpu, GICR_WAKER, gicr_rd(cpu, GICR_WAKER) & ~GICR_WAKER_PS);
    while (gicr_rd(cpu, GICR_WAKER) & GICR_WAKER_CA)
        arch_cpu_relax();

    gicr_wr(cpu, GICR_ICENABLER0, 0xFFFFFFFFu);
    gicr_wait_rwp(cpu);
    gicr_wr(cpu, GICR_ICPENDR0, 0xFFFFFFFFu);
    gicr_wr(cpu, GICR_ICACTIVER0, 0xFFFFFFFFu);
    gicr_wr(cpu, GICR_IGROUPR0, 0xFFFFFFFFu);
    for (unsigned i = 0; i < 32; i += 4)
        gicr_wr(cpu, GICR_IPRIORITYR + i, 0xA0A0A0A0u);
    gicr_wr(cpu, GICR_ISENABLER0, 0x0000FFFFu | g_routed_ppi_mask);
    gicr_wait_rwp(cpu);

    /* LPIs, if there is anything to deliver them: the property table is
     * shared, the pending table is this redistributor's alone, and
     * EnableLPIs is a one-way switch, so everything it reads must be in
     * place first. */
    if (g_lpi_prop_pa && (gicr_rd64(cpu, GICR_TYPER) & GICR_TYPER_PLPIS) &&
        (gicr_rd(cpu, GICR_CTLR) & GICR_CTLR_ENABLE_LPIS)) {
        /* One-way switch, already thrown: this redistributor is reading
         * whatever table the firmware gave it, not ours, and there is no
         * way to take it back. Say so rather than deliver nothing. */
        kwarn("gicv3: CPU %u had LPIs enabled before the kernel ran; its MSIs may not arrive", cpu);
    } else if (g_lpi_prop_pa && (gicr_rd64(cpu, GICR_TYPER) & GICR_TYPER_PLPIS)) {
        paddr_t pend = alloc_zeroed(LPI_PEND_BYTES);
        if (pend == 0) {
            panic("gicv3: CPU %u has no memory for its LPI pending table", cpu);
        }
        gicr_wr64(cpu, GICR_PROPBASER,
                  (g_lpi_prop_pa & 0x000FFFFFFFFFF000ull) | (7ull << 7) | (1ull << 10) |
                      (uint64_t)(LPI_ID_BITS - 1));
        gicr_wr64(cpu, GICR_PENDBASER,
                  (pend & 0x000FFFFFFFFF0000ull) | (7ull << 7) | (1ull << 10));
        dsb_ish();
        gicr_wr(cpu, GICR_CTLR, gicr_rd(cpu, GICR_CTLR) | GICR_CTLR_ENABLE_LPIS);
    }

    uint64_t sre = READ_SYSREG(icc_sre_el1);
    WRITE_SYSREG(icc_sre_el1, sre | ICC_SRE_SRE | ICC_SRE_DFB | ICC_SRE_DIB);
    isb();
    if ((READ_SYSREG(icc_sre_el1) & ICC_SRE_SRE) == 0)
        panic("gicv3: CPU %u cannot enable the system register interface", cpu);
    WRITE_SYSREG(icc_pmr_el1, PMR_UNMASKED);
    WRITE_SYSREG(icc_bpr1_el1, 0);
    uint64_t ctlr = READ_SYSREG(icc_ctlr_el1);
    WRITE_SYSREG(icc_ctlr_el1, ctlr & ~(ICC_CTLR_CBPR | ICC_CTLR_EOIMODE));
    WRITE_SYSREG(icc_igrpen1_el1, 1);
    isb();

    /* An SGI to this CPU has to be able to name it. Both ends must have
     * the range selector for an Aff0 past 15, and a CPU nobody can send
     * an IPI to would deadlock the first shootdown instead of failing
     * here. */
    if ((aff & 0xFFu) >= 16 && !(g_rss && (READ_SYSREG(icc_ctlr_el1) & ICC_CTLR_RSS)))
        panic("gicv3: CPU %u has Aff0 %u and this GIC has no SGI range selector", cpu, aff & 0xFFu);

    if (g_its) {
        paddr_t rd_pa = g_gicr_pa + (paddr_t)(g_gicr[cpu] - g_gicr_window);
        unsigned rd_index = (unsigned)((gicr_rd64(cpu, GICR_TYPER) >> 8) & 0xFFFF);
        its_init_cpu(g_its, cpu, rd_pa, rd_index);
    }
}

static int gicv3_vector_alloc(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    for (unsigned i = 0; i < VEC_DYNAMIC_COUNT; i++) {
        if ((g_vector_used[i / 64] & (1ull << (i % 64))) == 0) {
            g_vector_used[i / 64] |= 1ull << (i % 64);
            spin_unlock_irqrestore(&g_lock, s);
            return (int)(VEC_DYNAMIC_BASE + i);
        }
    }
    spin_unlock_irqrestore(&g_lock, s);
    return -ENOSPC;
}

static void unbind_locked(unsigned vector)
{
    unsigned i = vector - VEC_DYNAMIC_BASE;
    unsigned intid = g_intid_of[i];
    if (intid != 0xFFFF && intid < GIC_INTID_COUNT) {
        if (intid >= GIC_SPI_BASE)
            gicd_wr(GICD_ICENABLER + (intid / 32) * 4, 1u << (intid % 32));
        g_vector_of[intid] = (uint16_t)intid;
        if (gicv2m_owns(&g_v2m, intid))
            gicv2m_free(&g_v2m, intid);
        if (intid >= GIC_PPI_BASE && intid < GIC_SPI_BASE)
            g_routed_ppi_mask &= ~(1u << intid);
    }
    g_intid_of[i] = 0xFFFF;
    int sgi = g_sgi_of_vector[i];
    if (sgi >= 0) {
        g_sgi_vector[sgi] = -1;
        g_sgi_of_vector[i] = -1;
    }
}

static void gicv3_vector_free(unsigned vector)
{
    KASSERT(vector_is_dynamic(vector));
    unsigned i = vector - VEC_DYNAMIC_BASE;

    /* An LPI is given back by talking to the ITS, which waits on its
     * command queue; that cannot happen under this lock, so the binding
     * is taken out first and the LPI itself released after. */
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    unsigned lpi = g_lpi_of_vector[i];
    uint32_t devid = 0;
    unsigned lpi_cpu = 0;
    if (lpi != 0xFFFF) {
        devid = g_lpi_devid[lpi];
        lpi_cpu = g_lpi_cpu[lpi];
        g_lpi_of_vector[i] = 0xFFFF;
        g_lpi_vector[lpi] = 0xFFFF;
    }
    spin_unlock_irqrestore(&g_lock, s);
    if (lpi != 0xFFFF) {
        g_lpi_prop[lpi] = 0;
        dsb_ish();
        its_unmap_event(g_its, devid, lpi, lpi_cpu);
        s = spin_lock_irqsave(&g_lock);
        g_lpi_used[lpi / 64] &= ~(1ull << (lpi % 64));
        spin_unlock_irqrestore(&g_lock, s);
    }

    s = spin_lock_irqsave(&g_lock);
    unbind_locked(vector);
    g_vector_used[(vector - VEC_DYNAMIC_BASE) / 64] &= ~(1ull << ((vector - VEC_DYNAMIC_BASE) % 64));
    spin_unlock_irqrestore(&g_lock, s);
}

static int route_locked(unsigned intid, unsigned vector, unsigned cpu, unsigned flags)
{
    if (intid >= g_nr_lines || !vector_is_dynamic(vector))
        return -EINVAL;
    if (cpu >= CONFIG_MAX_CPUS || percpu_get(cpu) == NULL)
        return -EINVAL;
    /* One INTID, one vector (invariant A9). Firmware may wire a device
     * to a line that also falls inside the MSI frame's range, and the
     * frame's allocator cannot see that; refusing here is what stops an
     * MSI from silently taking a line something else is using. */
    if (g_vector_of[intid] != intid && g_vector_of[intid] != vector)
        return -EBUSY;
    g_vector_of[intid] = (uint16_t)vector;
    g_intid_of[vector - VEC_DYNAMIC_BASE] = (uint16_t)intid;
    if (intid >= GIC_SPI_BASE) {
        gicd_wr8(GICD_IPRIORITYR + intid, PRIORITY_DEFAULT);
        gicd_wr64(GICD_IROUTER + intid * 8, irouter_of(g_affinity[cpu]));
        uint32_t cfg = gicd_rd(GICD_ICFGR + (intid / 16) * 4);
        unsigned shift = (intid % 16) * 2;
        cfg &= ~(3u << shift);
        if (!(flags & ARCH_IRQ_TRIGGER_LEVEL))
            cfg |= 2u << shift;   /* edge */
        gicd_wr(GICD_ICFGR + (intid / 16) * 4, cfg);
    } else if (intid >= GIC_PPI_BASE) {
        g_routed_ppi_mask |= 1u << intid;
    }
    return 0;
}

static int gicv3_route(unsigned gsi, unsigned vector, unsigned cpu, unsigned flags)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    int rc = route_locked(gsi, vector, cpu, flags);
    spin_unlock_irqrestore(&g_lock, s);
    return rc;
}

/* SGIs and PPIs are the calling CPU's, and live in its redistributor;
 * SPIs are everyone's, and live in the distributor. */
static int gicv3_mask(unsigned gsi)
{
    if (gsi >= g_nr_lines)
        return -EINVAL;
    if (gsi < GIC_SPI_BASE) {
        unsigned cpu = arch_cpu_id();
        gicr_wr(cpu, GICR_ICENABLER0, 1u << gsi);
        gicr_wait_rwp(cpu);
    } else {
        gicd_wr(GICD_ICENABLER + (gsi / 32) * 4, 1u << (gsi % 32));
        gicd_wait_rwp();
    }
    return 0;
}

static int gicv3_unmask(unsigned gsi)
{
    if (gsi >= g_nr_lines)
        return -EINVAL;
    if (gsi < GIC_SPI_BASE)
        gicr_wr(arch_cpu_id(), GICR_ISENABLER0, 1u << gsi);
    else
        gicd_wr(GICD_ISENABLER + (gsi / 32) * 4, 1u << (gsi % 32));
    return 0;
}

/*
 * An ITS translation, rather than a line: take an LPI, enable it in the
 * property table, and tell the ITS that this device's event number maps
 * to it in this CPU's collection. The event number *is* the LPI's index
 * (gicv3_its.c), so the device's table needs no allocator of its own.
 */
static int its_compose(unsigned vector, unsigned cpu, uint32_t devid, uint64_t *addr, uint32_t *data)
{
    unsigned i = vector - VEC_DYNAMIC_BASE;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    if (g_lpi_of_vector[i] != 0xFFFF) {
        spin_unlock_irqrestore(&g_lock, s);
        return -EBUSY;
    }
    int lpi = -1;
    for (unsigned k = 0; k < NR_LPIS; k++) {
        if ((g_lpi_used[k / 64] & (1ull << (k % 64))) == 0) {
            g_lpi_used[k / 64] |= 1ull << (k % 64);
            lpi = (int)k;
            break;
        }
    }
    if (lpi < 0) {
        spin_unlock_irqrestore(&g_lock, s);
        return -ENOSPC;
    }
    g_lpi_vector[lpi] = (uint16_t)vector;
    g_lpi_of_vector[i] = (uint16_t)lpi;
    g_lpi_devid[lpi] = devid;
    g_lpi_cpu[lpi] = (uint8_t)cpu;
    spin_unlock_irqrestore(&g_lock, s);

    /* The redistributor reads this table; the ITS's INV in its_map_event
     * is what makes it re-read this byte. */
    g_lpi_prop[lpi] = (uint8_t)(PRIORITY_DEFAULT | LPI_ENABLED);
    dsb_ish();

    int rc = its_map_event(g_its, devid, (uint32_t)lpi, LPI_BASE + (unsigned)lpi, cpu);
    if (rc) {
        g_lpi_prop[lpi] = 0;
        s = spin_lock_irqsave(&g_lock);
        g_lpi_vector[lpi] = 0xFFFF;
        g_lpi_of_vector[i] = 0xFFFF;
        g_lpi_used[lpi / 64] &= ~(1ull << ((unsigned)lpi % 64));
        spin_unlock_irqrestore(&g_lock, s);
        return rc;
    }
    *addr = its_translater(g_its);
    *data = (uint32_t)lpi;
    return 0;
}

static int gicv3_msi_compose(unsigned vector, unsigned cpu, uint32_t devid, uint64_t *addr,
                          uint32_t *data)
{
    if (!vector_is_dynamic(vector))
        return -EINVAL;
    if (g_its)
        return its_compose(vector, cpu, devid, addr, data);
    if (g_v2m.spi_count == 0)
        return -ENODEV;   /* neither an ITS nor a frame: nothing can deliver an MSI */
    /* The frame's SPI range can overlap lines firmware wired to devices
     * -- on QEMU's virt the SMMU's event and error interrupts sit inside
     * it -- and the frame has no way to know. Walk past any line that is
     * already bound, leaving it marked used so it is never offered
     * again. */
    for (;;) {
        unsigned intid;
        int rc = gicv2m_alloc(&g_v2m, &intid);
        if (rc)
            return rc;
        arch_irq_state_t s = spin_lock_irqsave(&g_lock);
        rc = route_locked(intid, vector, cpu, 0);   /* MSIs are edge triggered */
        if (rc == 0)
            gicd_wr(GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
        spin_unlock_irqrestore(&g_lock, s);
        if (rc == -EBUSY) {
            kwarn("gicv3: MSI frame SPI %u is wired to a device; not offering it", intid);
            continue;   /* the SPI stays taken: it is not ours to hand out */
        }
        if (rc) {
            gicv2m_free(&g_v2m, intid);
            return rc;
        }
        *addr = gicv2m_setspi_addr(&g_v2m);
        *data = intid;
        return 0;
    }
}

static void gicv3_eoi(unsigned vector)
{
    if (vector >= VEC_SYNC_BASE && vector < VEC_DYNAMIC_BASE)
        return;   /* synchronous exceptions have no controller state */
    if (vector == VEC_SPURIOUS)
        return;
    WRITE_SYSREG(icc_eoir1_el1, g_cur_intid[arch_cpu_id()]);
}

static bool gicv3_msi_doorbell(paddr_t *pa, size_t *len)
{
    if (g_its)
        *pa = its_translater(g_its) & ~(paddr_t)(PAGE_SIZE - 1);
    else if (g_v2m.spi_count)
        *pa = gicv2m_setspi_addr(&g_v2m) & ~(paddr_t)(PAGE_SIZE - 1);
    else
        return false;
    *len = PAGE_SIZE;
    return true;
}

static unsigned gicv3_gsi_count(void)
{
    return GIC_INTID_COUNT;
}

static unsigned gicv3_spurious_vector(void)
{
    return VEC_SPURIOUS;
}

static unsigned gicv3_current_intid(void)
{
    return g_cur_intid[arch_cpu_id()];
}

static void gicv3_bind_ppi(unsigned intid, unsigned vector)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    KASSERT(intid >= GIC_PPI_BASE && intid < GIC_SPI_BASE && vector_is_dynamic(vector));
    g_vector_of[intid] = (uint16_t)vector;
    g_intid_of[vector - VEC_DYNAMIC_BASE] = (uint16_t)intid;
    g_routed_ppi_mask |= 1u << intid;
    spin_unlock_irqrestore(&g_lock, s);
}

static void gicv3_enable_local(unsigned intid)
{
    KASSERT(intid < GIC_SPI_BASE);
    gicr_wr(arch_cpu_id(), GICR_ISENABLER0, 1u << intid);
}

static void gicv3_disable_local(unsigned intid)
{
    KASSERT(intid < GIC_SPI_BASE);
    unsigned cpu = arch_cpu_id();
    gicr_wr(cpu, GICR_ICENABLER0, 1u << intid);
    gicr_wait_rwp(cpu);
}

static int sgi_for_vector_locked(unsigned vector)
{
    unsigned i = vector - VEC_DYNAMIC_BASE;
    if (g_sgi_of_vector[i] >= 0)
        return g_sgi_of_vector[i];
    for (int sgi = 0; sgi < (int)GIC_SGI_COUNT; sgi++) {
        if (g_sgi_vector[sgi] < 0) {
            g_sgi_vector[sgi] = (int)vector;
            g_sgi_of_vector[i] = sgi;
            return sgi;
        }
    }
    return -1;
}

static void gicv3_ipi_bind(unsigned vector)
{
    KASSERT(vector_is_dynamic(vector));
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    int sgi = sgi_for_vector_locked(vector);
    spin_unlock_irqrestore(&g_lock, s);
    if (sgi < 0)
        panic("gicv3: more than %u IPI vectors", GIC_SGI_COUNT);
}

/* Lock-free for the same reason GICv2's is: the binding is made once at
 * registration, and the send runs under the run-queue lock, which must
 * stay a leaf (scheduler invariant S2). */
static int sgi_for_vector(unsigned vector)
{
    KASSERT(vector_is_dynamic(vector));
    int sgi = __atomic_load_n(&g_sgi_of_vector[vector - VEC_DYNAMIC_BASE], __ATOMIC_ACQUIRE);
    if (sgi < 0)
        panic("gicv3: IPI vector %u sent before arch_ipi_bind", vector);
    return sgi;
}

/* ICC_SGI1R_EL1: Aff3 at 48, Aff2 at 32, Aff1 at 16, the INTID at 24,
 * and a sixteen-bit list over Aff0 within that cluster. Aff0 can go
 * past 15, and then the list needs the Range Selector at 44 to say
 * which group of sixteen it covers -- an extension (GICv3.1 RSS) that
 * an implementation may not have. `g_rss` records whether this one
 * does, and gicv3_init_cpu refuses a CPU it could never reach rather
 * than leaving the discovery to the first IPI. */
static uint64_t sgi1r_of(uint32_t aff, unsigned sgi)
{
    unsigned aff0 = aff & 0xFFu;
    return ((uint64_t)((aff >> 24) & 0xFF) << 48) | ((uint64_t)((aff >> 16) & 0xFF) << 32) |
           ((uint64_t)((aff >> 8) & 0xFF) << 16) | ((uint64_t)(aff0 / 16) << 44) |
           ((uint64_t)sgi << 24) | (1ull << (aff0 % 16));
}

static void gicv3_ipi_send(unsigned cpu, unsigned vector)
{
    KASSERT(cpu < CONFIG_MAX_CPUS);
    int sgi = sgi_for_vector(vector);
    dsb_ishst();
    WRITE_SYSREG(icc_sgi1r_el1, sgi1r_of(g_affinity[cpu], (unsigned)sgi));
    isb();
}

static void gicv3_ipi_broadcast_others(unsigned vector)
{
    int sgi = sgi_for_vector(vector);
    dsb_ishst();
    /* IRM: every CPU but this one, whatever their affinities are. */
    WRITE_SYSREG(icc_sgi1r_el1, (1ull << 40) | ((uint64_t)sgi << 24));
    isb();
}

void aarch64_timer_ack(unsigned intid);

static void gicv3_dispatch(struct arch_trap_frame *frame)
{
    unsigned cpu = arch_cpu_id();
    unsigned intid = (unsigned)(READ_SYSREG(icc_iar1_el1) & 0xFFFFFFu);
    if (intid >= LPI_BASE && intid < LPI_BASE + NR_LPIS) {
        g_cur_intid[cpu] = intid;
        uint16_t v = g_lpi_vector[intid - LPI_BASE];
        unsigned lpi_vector = v == 0xFFFF ? VEC_SPURIOUS : v;
        frame->vector = lpi_vector;
        if (lpi_vector == VEC_SPURIOUS) {
            g_spurious++;
            WRITE_SYSREG(icc_eoir1_el1, intid);
            return;
        }
        interrupt_dispatch(lpi_vector, frame);
        gicv3_eoi(lpi_vector);
        return;
    }
    if (intid >= GIC_INTID_COUNT) {
        g_spurious++;
        frame->vector = VEC_SPURIOUS;
        return;   /* 1020-1023 are acknowledged by the read itself */
    }
    g_cur_intid[cpu] = intid;
    unsigned vector;
    if (intid < GIC_SGI_COUNT) {
        int v = g_sgi_vector[intid];
        vector = v < 0 ? VEC_SPURIOUS : (unsigned)v;
    } else {
        vector = g_vector_of[intid];
    }
    if (intid >= GIC_PPI_BASE && intid < GIC_SPI_BASE)
        aarch64_timer_ack(intid);
    frame->vector = vector;
    if (vector == VEC_SPURIOUS) {
        g_spurious++;
        WRITE_SYSREG(icc_eoir1_el1, intid);
        return;
    }
    interrupt_dispatch(vector, frame);
    gicv3_eoi(vector);
}

/* See gicv2_test_spare_gsi: the last line the distributor reports. */
static int gicv3_test_spare_gsi(void)
{
    return g_nr_lines > GIC_SPI_BASE ? (int)(g_nr_lines - 1) : -1;
}

static void gicv3_test_raise(unsigned gsi)
{
    dsb_ishst();
    gicd_wr(GICD_ISPENDR + (gsi / 32) * 4, 1u << (gsi % 32));
}

static int gicv3_test_enabled(unsigned gsi)
{
    if (gsi >= g_nr_lines)
        return -1;
    return (int)((gicd_rd(GICD_ISENABLER + (gsi / 32) * 4) >> (gsi % 32)) & 1u);
}

/* The line the MSI allocator would hand out next, while it is still
 * unbound: see the ops table. */
static int gicv3_test_msi_overlap_gsi(void)
{
    if (g_its)
        return -1;   /* an LPI is not a line; there is nothing to collide with */
    for (unsigned k = 0; k < g_v2m.spi_count; k++) {
        unsigned intid = g_v2m.spi_base + k;
        if (!gicv2m_is_free(&g_v2m, intid))
            continue;
        arch_irq_state_t s = spin_lock_irqsave(&g_lock);
        bool unbound = g_vector_of[intid] == intid;
        spin_unlock_irqrestore(&g_lock, s);
        if (unbound)
            return (int)intid;
    }
    return -1;
}

static bool gicv3_test_msi_per_device(void)
{
    return g_its != NULL;
}

const struct aarch64_irqc_ops aarch64_gicv3_ops = {
    .name = "GICv3",
    .init = gicv3_init,
    .init_cpu = gicv3_init_cpu,
    .vector_alloc = gicv3_vector_alloc,
    .vector_free = gicv3_vector_free,
    .route = gicv3_route,
    .mask = gicv3_mask,
    .unmask = gicv3_unmask,
    .eoi = gicv3_eoi,
    .msi_compose = gicv3_msi_compose,
    .msi_doorbell = gicv3_msi_doorbell,
    .gsi_count = gicv3_gsi_count,
    .spurious_vector = gicv3_spurious_vector,
    .current_intid = gicv3_current_intid,
    .bind_ppi = gicv3_bind_ppi,
    .enable_local = gicv3_enable_local,
    .disable_local = gicv3_disable_local,
    .ipi_bind = gicv3_ipi_bind,
    .ipi_send = gicv3_ipi_send,
    .ipi_broadcast_others = gicv3_ipi_broadcast_others,
    .dispatch = gicv3_dispatch,
    .test_spare_gsi = gicv3_test_spare_gsi,
    .test_raise = gicv3_test_raise,
    .test_enabled = gicv3_test_enabled,
    .test_msi_overlap_gsi = gicv3_test_msi_overlap_gsi,
    .test_msi_per_device = gicv3_test_msi_per_device,
};
