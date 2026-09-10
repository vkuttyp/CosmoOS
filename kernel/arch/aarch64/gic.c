/*
 * gic.c - GICv2 distributor and CPU interface, GICv2m MSI, SGIs as IPIs,
 * the vector map (docs/kernel/arch/aarch64/design.md, "Vector numbering").
 *
 * One implementation of `struct aarch64_irqc_ops`, reached only through
 * `aarch64_gicv2_ops`; irqc.c decides whether this machine gets it. All
 * of the state below is this driver's alone.
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
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>
#include <kernel/vmm.h>
#include <arch/cpu.h>
#include <arch/irqc.h>
#include <aarch64/gicv2m.h>
#include <aarch64/irqc.h>
#include <aarch64/platform.h>
#include <aarch64/sysreg.h>
#include <aarch64/trapframe.h>

/* Distributor */
#define GICD_CTLR       0x000
#define GICD_TYPER      0x004
#define GICD_IGROUPR    0x080
#define GICD_ISENABLER  0x100
#define GICD_ICENABLER  0x180
#define GICD_ISPENDR    0x200
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
#define PRIORITY_DEFAULT 0x80u

static volatile uint32_t *g_gicd, *g_gicc;
static paddr_t g_gicd_pa, g_gicc_pa;
static unsigned g_nr_lines;                      /* from TYPER */
static struct gicv2m g_v2m;

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

static void gicv2_init_cpu(void);

static void gicv2_init(const struct acpi_gic *acpi)
{
    struct acpi_gic gic = *acpi;
    g_gicd_pa = gic.gicd_base ? gic.gicd_base : VIRT_GICD_BASE;
    g_gicc_pa = gic.gicc_base ? gic.gicc_base : VIRT_GICC_BASE;
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

    gicv2m_init(&g_v2m, &gic, g_nr_lines);
    gicv2_init_cpu();
    kinfo("gic: GICv2 at 0x%llx/0x%llx, %u lines, MSI %s (SPIs %u+%u)", (unsigned long long)g_gicd_pa,
          (unsigned long long)g_gicc_pa, g_nr_lines, g_v2m.spi_count ? "via GICv2m" : "unavailable",
          g_v2m.spi_base, g_v2m.spi_count);
}

static void gicv2_init_cpu(void)
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

static int gicv2_vector_alloc(void)
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

static void gicv2_vector_free(unsigned vector)
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

static int gicv2_route(unsigned gsi, unsigned vector, unsigned cpu, unsigned flags)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    int rc = route_locked(gsi, vector, cpu, flags);
    spin_unlock_irqrestore(&g_lock, s);
    return rc;
}

static int gicv2_mask(unsigned gsi)
{
    if (gsi >= g_nr_lines)
        return -EINVAL;
    gicd_wr(GICD_ICENABLER + (gsi / 32) * 4, 1u << (gsi % 32));
    return 0;
}

static int gicv2_unmask(unsigned gsi)
{
    if (gsi >= g_nr_lines)
        return -EINVAL;
    gicd_wr(GICD_ISENABLER + (gsi / 32) * 4, 1u << (gsi % 32));
    return 0;
}

static int gicv2_msi_compose(unsigned vector, unsigned cpu, uint32_t devid, uint64_t *addr,
                          uint32_t *data)
{
    (void)devid;   /* a frame raises an SPI; which device wrote to it does not matter */
    if (!vector_is_dynamic(vector))
        return -EINVAL;
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
            kwarn("gic: MSI frame SPI %u is wired to a device; not offering it", intid);
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

static void gicv2_eoi(unsigned vector)
{
    if (vector >= VEC_SYNC_BASE && vector < VEC_DYNAMIC_BASE)
        return;   /* synchronous exceptions have no controller state */
    if (vector == VEC_SPURIOUS)
        return;
    gicc_wr(GICC_EOIR, g_cur_intid[arch_cpu_id()]);
}

static bool gicv2_msi_doorbell(paddr_t *pa, size_t *len)
{
    if (g_v2m.spi_count == 0)
        return false;
    *pa = gicv2m_setspi_addr(&g_v2m) & ~(paddr_t)(PAGE_SIZE - 1);
    *len = PAGE_SIZE;
    return true;
}

static unsigned gicv2_gsi_count(void)
{
    return GIC_INTID_COUNT;
}

static unsigned gicv2_spurious_vector(void)
{
    return VEC_SPURIOUS;
}

static unsigned gicv2_current_intid(void)
{
    return g_cur_intid[arch_cpu_id()];
}

/* PPI helpers for the timer (banked per CPU, routed once). */
static void gicv2_bind_ppi(unsigned intid, unsigned vector)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    KASSERT(intid >= GIC_PPI_BASE && intid < GIC_SPI_BASE && vector_is_dynamic(vector));
    g_vector_of[intid] = (uint16_t)vector;
    g_intid_of[vector - VEC_DYNAMIC_BASE] = (uint16_t)intid;
    g_routed_ppi_mask |= 1u << intid;
    spin_unlock_irqrestore(&g_lock, s);
}

static void gicv2_enable_local(unsigned intid)
{
    gicd_wr(GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
}

static void gicv2_disable_local(unsigned intid)
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

static void gicv2_ipi_bind(unsigned vector)
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

static void gicv2_ipi_send(unsigned cpu, unsigned vector)
{
    KASSERT(cpu < CONFIG_MAX_CPUS);
    int sgi = sgi_for_vector(vector);
    dsb_ishst();
    gicd_wr(GICD_SGIR, ((uint32_t)g_cpu_iface_mask[cpu] << 16) | (uint32_t)sgi);
}

static void gicv2_ipi_broadcast_others(unsigned vector)
{
    int sgi = sgi_for_vector(vector);
    dsb_ishst();
    gicd_wr(GICD_SGIR, (1u << 24) | (uint32_t)sgi);   /* TargetListFilter 01: all but self */
}

void aarch64_timer_ack(unsigned intid);

static void gicv2_dispatch(struct arch_trap_frame *frame)
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
    gicv2_eoi(vector);
}

/* The distributor's highest line: QEMU's virt describes more lines than
 * it wires devices to, so the last one is free to route anywhere and
 * raise by hand. */
static int gicv2_test_spare_gsi(void)
{
    return g_nr_lines > GIC_SPI_BASE ? (int)(g_nr_lines - 1) : -1;
}

static void gicv2_test_raise(unsigned gsi)
{
    dsb_ishst();
    gicd_wr(GICD_ISPENDR + (gsi / 32) * 4, 1u << (gsi % 32));
}

static int gicv2_test_enabled(unsigned gsi)
{
    return (int)((gicd_rd(GICD_ISENABLER + (gsi / 32) * 4) >> (gsi % 32)) & 1u);
}

/* The line the MSI allocator would hand out next, while it is still
 * unbound: see the ops table. */
static int gicv2_test_msi_overlap_gsi(void)
{
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

static bool gicv2_test_msi_per_device(void)
{
    return false;   /* a frame raises its SPI whoever wrote to it */
}

const struct aarch64_irqc_ops aarch64_gicv2_ops = {
    .name = "GICv2",
    .init = gicv2_init,
    .init_cpu = gicv2_init_cpu,
    .vector_alloc = gicv2_vector_alloc,
    .vector_free = gicv2_vector_free,
    .route = gicv2_route,
    .mask = gicv2_mask,
    .unmask = gicv2_unmask,
    .eoi = gicv2_eoi,
    .msi_compose = gicv2_msi_compose,
    .msi_doorbell = gicv2_msi_doorbell,
    .gsi_count = gicv2_gsi_count,
    .spurious_vector = gicv2_spurious_vector,
    .current_intid = gicv2_current_intid,
    .bind_ppi = gicv2_bind_ppi,
    .enable_local = gicv2_enable_local,
    .disable_local = gicv2_disable_local,
    .ipi_bind = gicv2_ipi_bind,
    .ipi_send = gicv2_ipi_send,
    .ipi_broadcast_others = gicv2_ipi_broadcast_others,
    .dispatch = gicv2_dispatch,
    .test_spare_gsi = gicv2_test_spare_gsi,
    .test_raise = gicv2_test_raise,
    .test_enabled = gicv2_test_enabled,
    .test_msi_overlap_gsi = gicv2_test_msi_overlap_gsi,
    .test_msi_per_device = gicv2_test_msi_per_device,
};
