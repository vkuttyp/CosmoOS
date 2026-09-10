/*
 * gicv3_vdist.c - The virtual GICv3 distributor a guest finds
 * (docs/kernel/arch/aarch64/design.md, "The guest's distributor").
 *
 * Registers are decoded from the offset within a window and the size and
 * direction ESR_EL2 reported. The file is a 32-bit register file over a
 * small fixed array of per-interrupt state; a byte or halfword access is
 * a masked read-modify-write of its word, an eight-byte one is the two
 * words side by side, except for the registers the architecture makes
 * 64 bits wide (GICD_IROUTER, GICR_TYPER), which are handled as such.
 *
 * Locking: `lock` covers every field. It is taken by a guest's MMIO from
 * whichever vCPU thread made it, and by the EL2 backend when it asks
 * what a vCPU should be given next. Nothing here touches a list
 * register: the distributor decides, the vCPU's own run thread places.
 */
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <aarch64/gicv3_vdist.h>

/* GICD: identification and control. */
#define GICD_CTLR        0x0000u
#define GICD_TYPER       0x0004u
#define GICD_IIDR        0x0008u
#define GICD_PIDR2       0xFFE8u
#define GICD_CTLR_KEEP   0x13u          /* EnableGrp0, EnableGrp1, ARE: what a write may set */
#define GICD_CTLR_DS     (1u << 6)      /* one security state: reads as 1 */

/* GICR, RD_base frame. */
#define GICR_CTLR        0x0000u
#define GICR_IIDR        0x0004u
#define GICR_TYPER       0x0008u        /* 64-bit */
#define GICR_WAKER       0x0014u
#define GICR_PIDR2       0xFFE8u
#define GICR_WAKER_PS    (1u << 1)
#define GICR_WAKER_CA    (1u << 2)
#define GICR_TYPER_LAST  (1ull << 4)
#define GICR_SGI_BASE    0x10000u

/* An implementer code, and the architecture revision a driver checks. */
#define VDIST_IIDR       0x0000043Bu
#define VDIST_PIDR2      0x30u          /* ArchRev 3: "this is a GICv3" */

/* GICR, SGI_base frame: the private interrupts' configuration. */
#define GICR_IGROUPR0    0x0080u
#define GICR_ISENABLER0  0x0100u
#define GICR_ICENABLER0  0x0180u
#define GICR_ISPENDR0    0x0200u
#define GICR_ICPENDR0    0x0280u
#define GICR_IPRIORITYR  0x0400u        /* 32 bytes */
#define GICR_ICFGR0      0x0C00u        /* SGIs: edge, read-only */
#define GICR_ICFGR1      0x0C04u        /* PPIs */

/* GICD: the shared interrupts' configuration. Under affinity routing
 * the words for INTIDs 0..31 are dead here -- they live in the GICR. */
#define GICD_IGROUPR     0x0080u
#define GICD_ISENABLER   0x0100u
#define GICD_ICENABLER   0x0180u
#define GICD_ISPENDR     0x0200u
#define GICD_ICPENDR     0x0280u
#define GICD_IPRIORITYR  0x0400u        /* one byte per INTID */
#define GICD_ICFGR       0x0C00u        /* two bits per INTID */
#define GICD_IROUTER     0x6000u        /* 64 bits per SPI, indexed by INTID */
#define IROUTER_KEEP     0x000000FF80FFFFFFull   /* Aff3, IRM, Aff2, Aff1, Aff0 */

/* The idle priority: nothing can be higher than the CPU interface's
 * running priority when it is idle, so an interrupt at 0xFF is never
 * signalled. The GICv3 architecture's rule, kept here by name. */
#define VDIST_PRIO_IDLE  0xFFu

#define NR_WORDS         (VDIST_NR_LINES / 32u)
#define NR_PRIVATE       32u

struct vdist_private {
    bool present;
    uint32_t waker;          /* ProcessorSleep as the guest left it; ChildrenAsleep follows it */
    uint32_t group, enable, pending;   /* one bit per SGI or PPI */
    uint32_t cfg1;           /* ICFGR1: the PPIs' trigger bits, kept and returned */
    uint8_t prio[NR_PRIVATE];
};

struct gicv3_vdist {
    spinlock_t lock;
    uint32_t ctlr;
    uint32_t group[NR_WORDS], enable[NR_WORDS], pending[NR_WORDS];   /* SPIs: words 1.. */
    uint32_t cfg[NR_WORDS * 2];
    uint8_t prio[VDIST_NR_LINES];
    uint64_t irouter[VDIST_NR_LINES];   /* SPIs only; 0..31 stay zero */
    struct vdist_private priv[VDIST_GICR_FRAMES];
    uint64_t reads, writes, sgis;
};

/* A word of a set-or-clear register: the bit in `val & mask` that is set
 * is the request; a 1 sets (or clears), a 0 is "leave alone". */
static void set_bits(uint32_t *reg, uint32_t val, uint32_t mask)
{
    *reg |= val & mask;
}

static void clear_bits(uint32_t *reg, uint32_t val, uint32_t mask)
{
    *reg &= ~(val & mask);
}

static void rw_bits(uint32_t *reg, uint32_t val, uint32_t mask)
{
    *reg = (*reg & ~mask) | (val & mask);
}

/* Four priority bytes as the word a driver reads or writes them as. */
static uint32_t prio_word(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void prio_word_write(uint8_t *p, uint32_t val, uint32_t mask)
{
    for (unsigned b = 0; b < 4; b++) {
        uint32_t lane = 0xFFu << (b * 8);
        if (mask & lane)
            p[b] = (uint8_t)((val & lane) >> (b * 8));
    }
}

struct gicv3_vdist *vdist_create(void)
{
    struct gicv3_vdist *d = kzalloc(sizeof(*d));
    if (d == NULL)
        return NULL;
    spinlock_init(&d->lock, "vdist");
    for (unsigned i = 0; i < VDIST_GICR_FRAMES; i++)
        d->priv[i].waker = GICR_WAKER_PS;   /* asleep until the guest wakes it, as hardware is */
    return d;
}

void vdist_destroy(struct gicv3_vdist *d)
{
    if (d == NULL)
        return;
    if (d->reads || d->writes || d->sgis)
        kdebug("vdist: %llu register read(s), %llu write(s), %llu SGI target(s)", (unsigned long long)d->reads,
               (unsigned long long)d->writes, (unsigned long long)d->sgis);
    kfree(d);
}

void vdist_vcpu_present(struct gicv3_vdist *d, unsigned i, bool present)
{
    if (d == NULL || i >= VDIST_GICR_FRAMES)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    d->priv[i].present = present;
    spin_unlock_irqrestore(&d->lock, s);
}

/* Aff0 = i, in the one cluster this machine has; bit 31 is RES1. */
uint64_t vdist_mpidr(unsigned i)
{
    return (1ull << 31) | (i & 0xFFu);
}

static bool last_frame(const struct gicv3_vdist *d, unsigned i)
{
    for (unsigned j = i + 1; j < VDIST_GICR_FRAMES; j++)
        if (d->priv[j].present)
            return false;
    return true;
}

/* GICR_TYPER: the frame's affinity at 63:32 (Aff0 at 32), its processor
 * number at 23:8, Last on the last present frame, and no LPIs. */
static uint64_t gicr_typer(const struct gicv3_vdist *d, unsigned i)
{
    return ((vdist_mpidr(i) & 0xFFFFFFull) << 32) | ((uint64_t)i << 8) |
           (last_frame(d, i) ? GICR_TYPER_LAST : 0);
}

/* --- the 32-bit register file ---------------------------------------- */

static uint32_t gicd_rd(struct gicv3_vdist *d, unsigned off)
{
    switch (off) {
    case GICD_CTLR:
        return d->ctlr | GICD_CTLR_DS;
    case GICD_TYPER:
        /* ITLinesNumber for 288 lines; ten INTID bits; no 1-of-N, no LPIs. */
        return (VDIST_NR_LINES / 32u - 1u) | (9u << 19) | (1u << 25);
    case GICD_IIDR:
        return VDIST_IIDR;
    case GICD_PIDR2:
        return VDIST_PIDR2;
    default:
        break;
    }
    unsigned n = (off & 0x7Fu) / 4u;                    /* word within a 32-word bank */
    if (off >= GICD_IGROUPR && off < GICD_IGROUPR + NR_WORDS * 4)
        return n ? d->group[n] : 0;
    if (off >= GICD_ISENABLER && off < GICD_ISENABLER + NR_WORDS * 4)
        return n ? d->enable[n] : 0;
    if (off >= GICD_ICENABLER && off < GICD_ICENABLER + NR_WORDS * 4)
        return n ? d->enable[n] : 0;
    if (off >= GICD_ISPENDR && off < GICD_ISPENDR + NR_WORDS * 4)
        return n ? d->pending[n] : 0;
    if (off >= GICD_ICPENDR && off < GICD_ICPENDR + NR_WORDS * 4)
        return n ? d->pending[n] : 0;
    if (off >= GICD_IPRIORITYR && off < GICD_IPRIORITYR + VDIST_NR_LINES) {
        unsigned intid = off - GICD_IPRIORITYR;
        return intid >= NR_PRIVATE ? prio_word(&d->prio[intid]) : 0;
    }
    if (off >= GICD_ICFGR && off < GICD_ICFGR + NR_WORDS * 8) {
        unsigned m = (off - GICD_ICFGR) / 4u;
        return m >= 2 ? d->cfg[m] : 0;
    }
    if (off >= GICD_IROUTER && off < GICD_IROUTER + VDIST_NR_LINES * 8) {
        unsigned intid = (off - GICD_IROUTER) / 8u;
        uint64_t r = intid >= NR_PRIVATE ? d->irouter[intid] : 0;
        return (off & 4u) ? (uint32_t)(r >> 32) : (uint32_t)r;
    }
    return 0;
}

static void gicd_wr(struct gicv3_vdist *d, unsigned off, uint32_t val, uint32_t mask)
{
    if (off == GICD_CTLR) {
        d->ctlr = ((d->ctlr & ~mask) | (val & mask)) & GICD_CTLR_KEEP;
        return;
    }
    unsigned n = (off & 0x7Fu) / 4u;
    if (n == 0 && off >= GICD_IGROUPR && off < GICD_ICPENDR + NR_WORDS * 4)
        return;   /* INTIDs 0..31: the redistributor's, not this register's */
    if (off >= GICD_IGROUPR && off < GICD_IGROUPR + NR_WORDS * 4)
        rw_bits(&d->group[n], val, mask);
    else if (off >= GICD_ISENABLER && off < GICD_ISENABLER + NR_WORDS * 4)
        set_bits(&d->enable[n], val, mask);
    else if (off >= GICD_ICENABLER && off < GICD_ICENABLER + NR_WORDS * 4)
        clear_bits(&d->enable[n], val, mask);
    else if (off >= GICD_ISPENDR && off < GICD_ISPENDR + NR_WORDS * 4)
        set_bits(&d->pending[n], val, mask);
    else if (off >= GICD_ICPENDR && off < GICD_ICPENDR + NR_WORDS * 4)
        clear_bits(&d->pending[n], val, mask);
    else if (off >= GICD_IPRIORITYR && off < GICD_IPRIORITYR + VDIST_NR_LINES) {
        unsigned intid = off - GICD_IPRIORITYR;
        if (intid >= NR_PRIVATE)
            prio_word_write(&d->prio[intid], val, mask);
    } else if (off >= GICD_ICFGR && off < GICD_ICFGR + NR_WORDS * 8) {
        unsigned m = (off - GICD_ICFGR) / 4u;
        if (m >= 2)
            rw_bits(&d->cfg[m], val, mask);
    } else if (off >= GICD_IROUTER && off < GICD_IROUTER + VDIST_NR_LINES * 8) {
        unsigned intid = (off - GICD_IROUTER) / 8u;
        if (intid < NR_PRIVATE)
            return;
        uint64_t m64 = (off & 4u) ? ((uint64_t)mask << 32) : (uint64_t)mask;
        uint64_t v64 = (off & 4u) ? ((uint64_t)val << 32) : (uint64_t)val;
        d->irouter[intid] = ((d->irouter[intid] & ~m64) | (v64 & m64)) & IROUTER_KEEP;
    }
    /* anything else: read-only, or nothing this model keeps */
}

static uint32_t gicr_rd(struct gicv3_vdist *d, unsigned i, unsigned off)
{
    struct vdist_private *p = &d->priv[i];
    if (!p->present)
        return 0;
    switch (off) {
    case GICR_IIDR:
        return VDIST_IIDR;
    case GICR_TYPER:
        return (uint32_t)gicr_typer(d, i);
    case GICR_TYPER + 4:
        return (uint32_t)(gicr_typer(d, i) >> 32);
    case GICR_WAKER:
        /* The children are asleep exactly while the processor is. */
        return p->waker | ((p->waker & GICR_WAKER_PS) ? GICR_WAKER_CA : 0);
    case GICR_PIDR2:
        return VDIST_PIDR2;
    default:
        break;
    }
    if (off < GICR_SGI_BASE)
        return 0;
    unsigned sgi = off - GICR_SGI_BASE;
    switch (sgi) {
    case GICR_IGROUPR0:
        return p->group;
    case GICR_ISENABLER0:
    case GICR_ICENABLER0:
        return p->enable;
    case GICR_ISPENDR0:
    case GICR_ICPENDR0:
        return p->pending;
    case GICR_ICFGR0:
        return 0xAAAAAAAAu;   /* SGIs are edge-triggered and say so */
    case GICR_ICFGR1:
        return p->cfg1;
    default:
        break;
    }
    if (sgi >= GICR_IPRIORITYR && sgi < GICR_IPRIORITYR + NR_PRIVATE)
        return prio_word(&p->prio[sgi - GICR_IPRIORITYR]);
    return 0;
}

static void gicr_wr(struct gicv3_vdist *d, unsigned i, unsigned off, uint32_t val, uint32_t mask)
{
    struct vdist_private *p = &d->priv[i];
    if (!p->present)
        return;
    if (off == GICR_WAKER) {
        p->waker = ((p->waker & ~mask) | (val & mask)) & GICR_WAKER_PS;
        return;
    }
    if (off < GICR_SGI_BASE)
        return;
    unsigned sgi = off - GICR_SGI_BASE;
    switch (sgi) {
    case GICR_IGROUPR0:
        rw_bits(&p->group, val, mask);
        return;
    case GICR_ISENABLER0:
        set_bits(&p->enable, val, mask);
        return;
    case GICR_ICENABLER0:
        clear_bits(&p->enable, val, mask);
        return;
    case GICR_ISPENDR0:
        set_bits(&p->pending, val, mask);
        return;
    case GICR_ICPENDR0:
        clear_bits(&p->pending, val, mask);
        return;
    case GICR_ICFGR1:
        rw_bits(&p->cfg1, val, mask);
        return;
    default:
        break;
    }
    if (sgi >= GICR_IPRIORITYR && sgi < GICR_IPRIORITYR + NR_PRIVATE)
        prio_word_write(&p->prio[sgi - GICR_IPRIORITYR], val, mask);
}

/* --- routing ------------------------------------------------------------ */

#define GICD_CTLR_ENABLE_G1 (1u << 1)
#define IROUTER_IRM         (1ull << 31)   /* "any": to the lowest-numbered present vCPU */

/* The affinity vCPU i has, in IROUTER's shape: Aff0..2 low, Aff3 at 32. */
static uint64_t route_of(unsigned i)
{
    return vdist_mpidr(i) & 0xFFFFFFull;
}

static unsigned first_present(const struct gicv3_vdist *d)
{
    for (unsigned j = 0; j < VDIST_GICR_FRAMES; j++)
        if (d->priv[j].present)
            return j;
    return 0;
}

/* Is `intid`, right now, one that vCPU i should be given? Caller holds the lock. */
static bool deliverable_locked(const struct gicv3_vdist *d, unsigned i, unsigned intid, uint8_t *prio)
{
    if (i >= VDIST_GICR_FRAMES || intid >= VDIST_NR_LINES)
        return false;
    const struct vdist_private *p = &d->priv[i];
    if (!p->present || (p->waker & GICR_WAKER_PS) || !(d->ctlr & GICD_CTLR_ENABLE_G1))
        return false;
    if (intid < NR_PRIVATE) {
        uint32_t bit = 1u << intid;
        if (!(p->pending & p->enable & p->group & bit))
            return false;
        *prio = p->prio[intid];
        return true;
    }
    unsigned n = intid / 32u;
    uint32_t bit = 1u << (intid % 32u);
    if (!(d->pending[n] & d->enable[n] & d->group[n] & bit))
        return false;
    uint64_t r = d->irouter[intid];
    if (r & IROUTER_IRM) {
        if (i != first_present(d))
            return false;
    } else if ((r & ~IROUTER_IRM) != route_of(i)) {
        return false;
    }
    *prio = d->prio[intid];
    return true;
}

int vdist_pending_for(struct gicv3_vdist *d, unsigned i, uint8_t *prio)
{
    if (d == NULL || i >= VDIST_GICR_FRAMES)
        return -1;
    int best = -1;
    uint8_t best_prio = VDIST_PRIO_IDLE;
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    const struct vdist_private *p = &d->priv[i];
    /* Private first, then the SPIs; the candidate words are cheap to
     * test before walking their bits. Lower value is higher priority;
     * on a tie the lower INTID, as the architecture orders them. A
     * candidate has to beat the idle priority to be signalled at all --
     * `pr < best_prio` against VDIST_PRIO_IDLE is that rule, not an
     * accident of the starting value: an interrupt a guest sets to 0xFF
     * stays pending and is never forwarded, on hardware or here. */
    uint32_t cand = p->pending & p->enable & p->group;
    for (unsigned b = 0; cand; b++, cand >>= 1) {
        uint8_t pr;
        if ((cand & 1u) && deliverable_locked(d, i, b, &pr) && pr < best_prio) {
            best = (int)b;
            best_prio = pr;
        }
    }
    for (unsigned n = 1; n < NR_WORDS; n++) {
        uint32_t w = d->pending[n] & d->enable[n] & d->group[n];
        for (unsigned b = 0; w; b++, w >>= 1) {
            uint8_t pr;
            unsigned intid = n * 32u + b;
            if ((w & 1u) && deliverable_locked(d, i, intid, &pr) && pr < best_prio) {
                best = (int)intid;
                best_prio = pr;
            }
        }
    }
    spin_unlock_irqrestore(&d->lock, s);
    if (best >= 0)
        *prio = best_prio;
    return best;
}

bool vdist_deliverable(struct gicv3_vdist *d, unsigned i, unsigned intid)
{
    if (d == NULL)
        return false;
    uint8_t prio;
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    bool ok = deliverable_locked(d, i, intid, &prio);
    spin_unlock_irqrestore(&d->lock, s);
    return ok;
}

void vdist_ack(struct gicv3_vdist *d, unsigned i, unsigned intid)
{
    if (d == NULL || i >= VDIST_GICR_FRAMES || intid >= VDIST_NR_LINES)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    if (intid < NR_PRIVATE)
        d->priv[i].pending &= ~(1u << intid);
    else
        d->pending[intid / 32u] &= ~(1u << (intid % 32u));
    spin_unlock_irqrestore(&d->lock, s);
}

void vdist_raise_private(struct gicv3_vdist *d, unsigned i, unsigned intid)
{
    if (d == NULL || i >= VDIST_GICR_FRAMES || intid >= NR_PRIVATE)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    d->priv[i].pending |= 1u << intid;
    spin_unlock_irqrestore(&d->lock, s);
}

/*
 * ICC_SGI1R_EL1: Aff3 at 48, RS at 44, IRM at 40, Aff2 at 32, the INTID
 * at 24, Aff1 at 16, and a sixteen-bit list over Aff0 within that cluster
 * -- the same shape the host's own driver composes (gicv3.c). This
 * machine's guests have one cluster with Aff1..3 zero, so a target is
 * an Aff0 in the list, offset by sixteen times the range selector.
 */
unsigned vdist_sgi(struct gicv3_vdist *d, unsigned from, uint64_t sgi1r)
{
    if (d == NULL)
        return 0;
    unsigned sgi = (unsigned)((sgi1r >> 24) & 0xFu);
    bool irm = (sgi1r >> 40) & 1u;
    unsigned aff1 = (unsigned)((sgi1r >> 16) & 0xFFu);
    unsigned aff2 = (unsigned)((sgi1r >> 32) & 0xFFu);
    unsigned aff3 = (unsigned)((sgi1r >> 48) & 0xFFu);
    unsigned rs = (unsigned)((sgi1r >> 44) & 0xFu);
    uint32_t list = (uint32_t)(sgi1r & 0xFFFFu);
    unsigned hit = 0;
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    for (unsigned i = 0; i < VDIST_GICR_FRAMES; i++) {
        if (!d->priv[i].present)
            continue;
        bool target;
        if (irm) {
            target = i != from;
        } else {
            unsigned aff0 = (unsigned)(vdist_mpidr(i) & 0xFFu);
            target = aff1 == 0 && aff2 == 0 && aff3 == 0 && aff0 / 16u == rs &&
                     (list & (1u << (aff0 % 16u))) != 0;
        }
        if (target) {
            d->priv[i].pending |= 1u << sgi;
            hit++;
        }
    }
    d->sgis += hit;
    spin_unlock_irqrestore(&d->lock, s);
    return hit;
}

/* --- access decode ---------------------------------------------------- */

enum window { WIN_NONE, WIN_GICD, WIN_GICR };

static enum window locate(uint64_t gpa, unsigned *frame, unsigned *off)
{
    if (gpa >= VDIST_GICD_BASE && gpa < VDIST_GICD_BASE + VDIST_GICD_SIZE) {
        *frame = 0;
        *off = (unsigned)(gpa - VDIST_GICD_BASE);
        return WIN_GICD;
    }
    if (gpa >= VDIST_GICR_BASE && gpa < VDIST_GICR_BASE + VDIST_GICR_STRIDE * VDIST_GICR_FRAMES) {
        uint64_t rel = gpa - VDIST_GICR_BASE;
        *frame = (unsigned)(rel / VDIST_GICR_STRIDE);
        *off = (unsigned)(rel % VDIST_GICR_STRIDE);
        return WIN_GICR;
    }
    return WIN_NONE;
}

static uint32_t word_rd(struct gicv3_vdist *d, enum window w, unsigned frame, unsigned off)
{
    return w == WIN_GICD ? gicd_rd(d, off) : gicr_rd(d, frame, off);
}

static void word_wr(struct gicv3_vdist *d, enum window w, unsigned frame, unsigned off, uint32_t val,
                    uint32_t mask)
{
    if (w == WIN_GICD)
        gicd_wr(d, off, val, mask);
    else
        gicr_wr(d, frame, off, val, mask);
}

bool vdist_mmio(struct gicv3_vdist *d, unsigned vcpu, uint64_t gpa, unsigned size, bool write,
                uint64_t *val)
{
    (void)vcpu;   /* GICD is not banked under affinity routing; GICR is addressed by frame */
    unsigned frame, off;
    enum window w = locate(gpa, &frame, &off);
    if (w == WIN_NONE)
        return false;
    if (size != 1 && size != 2 && size != 4 && size != 8)
        size = 4;
    if (off & (size - 1))
        off &= ~(size - 1);   /* an unaligned access lands on its aligned register */

    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    if (write)
        d->writes++;
    else
        d->reads++;
    if (size == 8) {
        unsigned lo = off & ~7u;
        if (write) {
            word_wr(d, w, frame, lo, (uint32_t)*val, 0xFFFFFFFFu);
            word_wr(d, w, frame, lo + 4, (uint32_t)(*val >> 32), 0xFFFFFFFFu);
        } else {
            *val = (uint64_t)word_rd(d, w, frame, lo) |
                   ((uint64_t)word_rd(d, w, frame, lo + 4) << 32);
        }
    } else {
        unsigned word = off & ~3u;
        unsigned shift = (off & 3u) * 8u;
        uint32_t mask = (size == 4) ? 0xFFFFFFFFu : (((1u << (size * 8u)) - 1u) << shift);
        if (write) {
            word_wr(d, w, frame, word, (uint32_t)(*val << shift), mask);
        } else {
            *val = (word_rd(d, w, frame, word) & mask) >> shift;
        }
    }
    spin_unlock_irqrestore(&d->lock, s);
    return true;
}
