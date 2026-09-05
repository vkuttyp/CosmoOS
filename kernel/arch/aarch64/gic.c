/*
 * gic.c - GICv2 distributor and CPU interface, GICv2m MSI, SGIs as IPIs,
 * the vector map (docs/kernel/arch/aarch64/design.md, "Vector numbering").
 *
 * GSI = INTID on this architecture. Dynamic vectors (VEC_DYNAMIC_BASE..)
 * are software ids the generic layers allocate; `route` binds one to an
 * INTID, IPIs bind one to an SGI, MSIs to a GICv2m SPI. The IRQ path
 * acknowledges an INTID, maps it to its vector and remembers it for EOI.
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
#include <aarch64/platform.h>
#include <aarch64/sysreg.h>
#include <aarch64/trapframe.h>

/* Distributor */
#define GICD_CTLR       0x000
#define GICD_TYPER      0x004
#define GICD_IGROUPR    0x080
#define GICD_ISENABLER  0x100
#define GICD_ICENABLER  0x180
#define GICD_ICPENDR    0x280
#define GICD_ICACTIVER  0x380
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR  0x800
#define GICD_ICFGR      0xC00
#define GICD_SGIR       0xF00
/* CPU interface */
#define GICC_CTLR 0x000
#define GICC_PMR  0x004
#define GICC_BPR  0x008
#define GICC_IAR  0x00C
#define GICC_EOIR 0x010
/* GICv2m */
#define V2M_MSI_TYPER     0x008
#define V2M_MSI_SETSPI_NS 0x040

#define PRIORITY_DEFAULT 0x80u

static volatile uint32_t *g_gicd, *g_gicc, *g_v2m;
static paddr_t g_gicd_pa, g_gicc_pa, g_v2m_pa;
static unsigned g_nr_lines;                      /* from TYPER */
static unsigned g_v2m_spi_base, g_v2m_spi_count;
static uint64_t g_v2m_used[32];                  /* up to 2048 SPIs */

static spinlock_t g_lock = SPINLOCK_INIT("gic");
static uint16_t g_vector_of[GIC_INTID_COUNT];     /* INTID -> vector (identity when unrouted) */
static uint16_t g_intid_of[VEC_DYNAMIC_COUNT];    /* dynamic vector -> INTID, 0xFFFF none */
static uint64_t g_vector_used[VEC_DYNAMIC_COUNT / 64];
static int g_sgi_of_vector[VEC_DYNAMIC_COUNT];    /* dynamic vector -> SGI id, -1 none */
static int g_sgi_vector[GIC_SGI_COUNT];           /* SGI id -> vector, -1 none */
static uint32_t g_routed_ppi_mask;                /* PPIs to enable on every CPU */
static uint8_t g_cpu_iface_mask[CONFIG_MAX_CPUS]; /* GICD target bit of each CPU */
static unsigned g_cur_intid[CONFIG_MAX_CPUS];
static uint64_t g_spurious;

static inline uint32_t gicd_rd(unsigned off) { return g_gicd[off / 4]; }
static inline void gicd_wr(unsigned off, uint32_t v) { g_gicd[off / 4] = v; }
static inline uint32_t gicc_rd(unsigned off) { return g_gicc[off / 4]; }
static inline void gicc_wr(unsigned off, uint32_t v) { g_gicc[off / 4] = v; }

static inline void gicd_wr8(unsigned off, uint8_t v)
{
    volatile uint8_t *b = (volatile uint8_t *)g_gicd;
    b[off] = v;
}
static inline uint8_t gicd_rd8(unsigned off)
{
    volatile uint8_t *b = (volatile uint8_t *)g_gicd;
    return b[off];
}

static bool vector_is_dynamic(unsigned v)
{
    return v >= VEC_DYNAMIC_BASE && v < VEC_DYNAMIC_BASE + VEC_DYNAMIC_COUNT;
}

static volatile uint32_t *map(paddr_t pa, size_t len, const char *what)
{
    vaddr_t va = vm_map_phys(pa, len, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0)
        panic("gic: cannot map %s at 0x%llx", what, (unsigned long long)pa);
    return (volatile uint32_t *)va;
}

void arch_irqc_init(void)
{
    struct acpi_gic gic;
    if (!acpi_madt_gic(&gic)) {
        kwarn("gic: MADT has no GIC entries; using the virt defaults");
        gic.gicd_base = VIRT_GICD_BASE;
        gic.gicc_base = VIRT_GICC_BASE;
        gic.v2m_base = VIRT_GICV2M_BASE;
        gic.v2m_spi_base = 0;
        gic.v2m_spi_count = 0;
    }
    if (gic.version != 0 && gic.version != 2)
        panic("gic: distributor version %u; only GICv2 is implemented", gic.version);
    g_gicd_pa = gic.gicd_base;
    g_gicc_pa = gic.gicc_base ? gic.gicc_base : VIRT_GICC_BASE;
    g_v2m_pa = gic.v2m_base;
    g_gicd = map(g_gicd_pa, 0x10000, "GICD");
    g_gicc = map(g_gicc_pa, 0x2000, "GICC");

    for (unsigned i = 0; i < GIC_INTID_COUNT; i++)
        g_vector_of[i] = (uint16_t)i;
    for (unsigned i = 0; i < VEC_DYNAMIC_COUNT; i++) {
        g_intid_of[i] = 0xFFFF;
        g_sgi_of_vector[i] = -1;
    }
    for (unsigned i = 0; i < GIC_SGI_COUNT; i++)
        g_sgi_vector[i] = -1;

    gicd_wr(GICD_CTLR, 0);
    g_nr_lines = 32u * ((gicd_rd(GICD_TYPER) & 0x1F) + 1);
    if (g_nr_lines > GIC_INTID_COUNT)
        g_nr_lines = GIC_INTID_COUNT;
    /* Every SPI: group 0, disabled, not pending, default priority, level, CPU 0. */
    for (unsigned i = GIC_SPI_BASE; i < g_nr_lines; i += 32) {
        gicd_wr(GICD_ICENABLER + (i / 32) * 4, 0xFFFFFFFFu);
        gicd_wr(GICD_ICPENDR + (i / 32) * 4, 0xFFFFFFFFu);
        gicd_wr(GICD_ICACTIVER + (i / 32) * 4, 0xFFFFFFFFu);
        gicd_wr(GICD_IGROUPR + (i / 32) * 4, 0);
    }
    for (unsigned i = GIC_SPI_BASE; i < g_nr_lines; i += 4)
        gicd_wr(GICD_IPRIORITYR + i, 0x80808080u);
    for (unsigned i = GIC_SPI_BASE; i < g_nr_lines; i += 16)
        gicd_wr(GICD_ICFGR + (i / 16) * 4, 0);
    gicd_wr(GICD_CTLR, 1);

    if (g_v2m_pa) {
        g_v2m = map(g_v2m_pa, 0x1000, "GICv2m");
        uint32_t typer = g_v2m[V2M_MSI_TYPER / 4];
        g_v2m_spi_base = (typer >> 16) & 0x3FF;
        g_v2m_spi_count = typer & 0x3FF;
        if (gic.v2m_spi_count) {
            g_v2m_spi_base = gic.v2m_spi_base;
            g_v2m_spi_count = gic.v2m_spi_count;
        }
        if (g_v2m_spi_base < GIC_SPI_BASE || g_v2m_spi_base + g_v2m_spi_count > g_nr_lines) {
            kwarn("gic: GICv2m SPI range %u+%u is outside the distributor's %u lines; MSI disabled",
                  g_v2m_spi_base, g_v2m_spi_count, g_nr_lines);
            g_v2m_spi_count = 0;
        }
    }
    arch_irqc_init_cpu();
    kinfo("gic: GICv2 at 0x%llx/0x%llx, %u lines, MSI %s (SPIs %u+%u)", (unsigned long long)g_gicd_pa,
          (unsigned long long)g_gicc_pa, g_nr_lines, g_v2m_spi_count ? "via GICv2m" : "unavailable",
          g_v2m_spi_base, g_v2m_spi_count);
}

void arch_irqc_init_cpu(void)
{
    unsigned cpu = arch_cpu_id();
    /* SGIs and PPIs are banked: disable, clear, set priorities, then enable what is routed. */
    gicd_wr(GICD_ICENABLER, 0xFFFF0000u);
    gicd_wr(GICD_ICPENDR, 0xFFFFFFFFu);
    gicd_wr(GICD_ICACTIVER, 0xFFFFFFFFu);
    gicd_wr(GICD_IGROUPR, 0);
    for (unsigned i = 0; i < 32; i += 4)
        gicd_wr(GICD_IPRIORITYR + i, 0x80808080u);
    gicd_wr(GICD_ISENABLER, 0x0000FFFFu | g_routed_ppi_mask);
    g_cpu_iface_mask[cpu] = gicd_rd8(GICD_ITARGETSR);   /* byte 0: this CPU's interface bit */
    if (g_cpu_iface_mask[cpu] == 0)
        g_cpu_iface_mask[cpu] = (uint8_t)(1u << cpu);
    gicc_wr(GICC_PMR, 0xFF);
    gicc_wr(GICC_BPR, 0);
    gicc_wr(GICC_CTLR, 1);
}

int arch_vector_alloc(void)
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
        if (g_v2m_spi_count && intid >= g_v2m_spi_base && intid < g_v2m_spi_base + g_v2m_spi_count) {
            unsigned k = intid - g_v2m_spi_base;
            g_v2m_used[k / 64] &= ~(1ull << (k % 64));
        }
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

void arch_vector_free(unsigned vector)
{
    KASSERT(vector_is_dynamic(vector));
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
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
    g_vector_of[intid] = (uint16_t)vector;
    g_intid_of[vector - VEC_DYNAMIC_BASE] = (uint16_t)intid;
    if (intid >= GIC_SPI_BASE) {
        gicd_wr8(GICD_IPRIORITYR + intid, PRIORITY_DEFAULT);
        gicd_wr8(GICD_ITARGETSR + intid, g_cpu_iface_mask[cpu]);
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

int arch_irqc_route(unsigned gsi, unsigned vector, unsigned cpu, unsigned flags)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    int rc = route_locked(gsi, vector, cpu, flags);
    spin_unlock_irqrestore(&g_lock, s);
    return rc;
}

int arch_irqc_mask(unsigned gsi)
{
    if (gsi >= g_nr_lines)
        return -EINVAL;
    gicd_wr(GICD_ICENABLER + (gsi / 32) * 4, 1u << (gsi % 32));
    return 0;
}

int arch_irqc_unmask(unsigned gsi)
{
    if (gsi >= g_nr_lines)
        return -EINVAL;
    gicd_wr(GICD_ISENABLER + (gsi / 32) * 4, 1u << (gsi % 32));
    return 0;
}

int arch_irqc_msi_compose(unsigned vector, unsigned cpu, uint64_t *addr, uint32_t *data)
{
    if (!vector_is_dynamic(vector) || g_v2m_spi_count == 0)
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    int found = -1;
    for (unsigned k = 0; k < g_v2m_spi_count; k++) {
        if ((g_v2m_used[k / 64] & (1ull << (k % 64))) == 0) {
            g_v2m_used[k / 64] |= 1ull << (k % 64);
            found = (int)k;
            break;
        }
    }
    if (found < 0) {
        spin_unlock_irqrestore(&g_lock, s);
        return -ENOSPC;
    }
    unsigned intid = g_v2m_spi_base + (unsigned)found;
    int rc = route_locked(intid, vector, cpu, 0);   /* MSIs are edge triggered */
    if (rc) {
        g_v2m_used[found / 64] &= ~(1ull << (found % 64));
        spin_unlock_irqrestore(&g_lock, s);
        return rc;
    }
    gicd_wr(GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
    spin_unlock_irqrestore(&g_lock, s);
    *addr = g_v2m_pa + V2M_MSI_SETSPI_NS;
    *data = intid;
    return 0;
}

void arch_irqc_eoi(unsigned vector)
{
    if (vector >= VEC_SYNC_BASE && vector < VEC_DYNAMIC_BASE)
        return;   /* synchronous exceptions have no controller state */
    if (vector == VEC_SPURIOUS)
        return;
    gicc_wr(GICC_EOIR, g_cur_intid[arch_cpu_id()]);
}

unsigned arch_irqc_gsi_count(void)
{
    return GIC_INTID_COUNT;
}

unsigned arch_irqc_spurious_vector(void)
{
    return VEC_SPURIOUS;
}

unsigned gic_current_intid(void)
{
    return g_cur_intid[arch_cpu_id()];
}

/* PPI helpers for the timer (banked per CPU, routed once). */
void gic_bind_ppi(unsigned intid, unsigned vector)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    KASSERT(intid >= GIC_PPI_BASE && intid < GIC_SPI_BASE && vector_is_dynamic(vector));
    g_vector_of[intid] = (uint16_t)vector;
    g_intid_of[vector - VEC_DYNAMIC_BASE] = (uint16_t)intid;
    g_routed_ppi_mask |= 1u << intid;
    spin_unlock_irqrestore(&g_lock, s);
}

void gic_enable_local(unsigned intid)
{
    gicd_wr(GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
}

void gic_disable_local(unsigned intid)
{
    gicd_wr(GICD_ICENABLER + (intid / 32) * 4, 1u << (intid % 32));
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

void arch_ipi_bind(unsigned vector)
{
    KASSERT(vector_is_dynamic(vector));
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    int sgi = sgi_for_vector_locked(vector);
    spin_unlock_irqrestore(&g_lock, s);
    if (sgi < 0)
        panic("gic: more than %u IPI vectors", GIC_SGI_COUNT);
}

/* Lock-free: the binding was made by arch_ipi_bind before the first send
 * and never changes while the vector is allocated. arch_ipi_send runs
 * under the run-queue lock, which must stay a leaf (S2). */
static int sgi_for_vector(unsigned vector)
{
    KASSERT(vector_is_dynamic(vector));
    int sgi = __atomic_load_n(&g_sgi_of_vector[vector - VEC_DYNAMIC_BASE], __ATOMIC_ACQUIRE);
    if (sgi < 0)
        panic("gic: IPI vector %u sent before arch_ipi_bind", vector);
    return sgi;
}

void arch_ipi_send(unsigned cpu, unsigned vector)
{
    KASSERT(cpu < CONFIG_MAX_CPUS);
    int sgi = sgi_for_vector(vector);
    dsb_ishst();
    gicd_wr(GICD_SGIR, ((uint32_t)g_cpu_iface_mask[cpu] << 16) | (uint32_t)sgi);
}

void arch_ipi_broadcast_others(unsigned vector)
{
    int sgi = sgi_for_vector(vector);
    dsb_ishst();
    gicd_wr(GICD_SGIR, (1u << 24) | (uint32_t)sgi);   /* TargetListFilter 01: all but self */
}

void aarch64_timer_ack(unsigned intid);

void gic_irq_dispatch(struct arch_trap_frame *frame)
{
    unsigned cpu = arch_cpu_id();
    uint32_t iar = gicc_rd(GICC_IAR);
    unsigned intid = iar & 0x3FF;
    if (intid >= GIC_INTID_COUNT) {
        g_spurious++;
        frame->vector = VEC_SPURIOUS;
        return;
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
        gicc_wr(GICC_EOIR, intid);
        return;
    }
    interrupt_dispatch(vector, frame);
    arch_irqc_eoi(vector);
}
