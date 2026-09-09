/*
 * gicv3_its.c - The GICv3 Interrupt Translation Service (aarch64/gicv3_its.h).
 *
 * Everything here is told to the ITS through one queue of 32-byte
 * commands: MAPD gives a device its translation table, MAPC gives a CPU
 * a collection pointing at that CPU's redistributor, MAPTI binds one of
 * the device's events to an LPI in a collection, INV throws away what
 * the redistributor cached about it, and SYNC waits for the lot. The
 * queue is the only way in, so every operation below is "write commands,
 * wait for the reader to pass them".
 *
 * Two simplifications this driver makes, both stated so they can be
 * revisited rather than discovered:
 *
 *   - **EventID is the LPI's index.** Nothing forces a device's event
 *     numbering to be dense or private, and giving each device its own
 *     event allocator would need a bitmap per device. Since the kernel
 *     hands out one LPI per MSI anyway, using the LPI's offset as the
 *     event id means every device's table is the same size and no
 *     second allocator exists to get wrong.
 *   - **The device table is flat and capped.** GITS_TYPER may claim
 *     twenty device-id bits, which is an eight-megabyte flat table; the
 *     architecture's answer is a two-level table, which this does not
 *     implement. It allocates what the id space needs up to the cap and
 *     refuses a device id beyond it, with a warning, rather than
 *     silently mistranslating.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/kmalloc.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vmm.h>
#include <arch/cpu.h>
#include <aarch64/gicv3_its.h>
#include <aarch64/sysreg.h>

#define GITS_CTLR       0x0000
#define GITS_TYPER      0x0008
#define GITS_CBASER     0x0080
#define GITS_CWRITER    0x0088
#define GITS_CREADR     0x0090
#define GITS_BASER0     0x0100
#define GITS_TRANSLATER 0x10040

#define GITS_CTLR_ENABLED   (1u << 0)
#define GITS_CTLR_QUIESCENT (1u << 31)

#define GITS_TYPER_PHYSICAL   (1ull << 0)
#define GITS_TYPER_ITT_SIZE(t) ((unsigned)(((t) >> 4) & 0xF) + 1)
#define GITS_TYPER_IDBITS(t)   ((unsigned)(((t) >> 8) & 0x1F) + 1)
#define GITS_TYPER_DEVBITS(t)  ((unsigned)(((t) >> 13) & 0x1F) + 1)
#define GITS_TYPER_PTA         (1ull << 19)
#define GITS_TYPER_HCC(t)      ((unsigned)(((t) >> 24) & 0xFF))

/* GITS_BASER<n> and GITS_CBASER share their cacheability encoding:
 * inner read-allocate write-allocate write-back, inner shareable, which
 * is what makes the tables the CPU writes visible to the ITS without a
 * cache maintenance dance. */
#define BASER_VALID       (1ull << 63)
#define BASER_INDIRECT    (1ull << 62)
#define BASER_INNER_RaWaWb (7ull << 59)
#define BASER_TYPE(v)     ((unsigned)(((v) >> 56) & 0x7))
#define BASER_ENTRY_SIZE(v) ((unsigned)(((v) >> 48) & 0xFF) + 1)
#define BASER_SHARE_INNER (1ull << 10)
#define BASER_PAGE_4K     (0ull << 8)

#define CBASER_INNER_RaWaWb (7ull << 59)
#define CBASER_SHARE_INNER  (1ull << 10)
#define CBASER_VALID        (1ull << 63)

#define BASER_TYPE_DEVICE     1
#define BASER_TYPE_COLLECTION 4

#define CMD_MAPD    0x08
#define CMD_MAPC    0x09
#define CMD_MAPTI   0x0A
#define CMD_INV     0x0C
#define CMD_INVALL  0x0D
#define CMD_SYNC    0x05
#define CMD_DISCARD 0x0F

#define CMDQ_BYTES   PAGE_SIZE      /* 128 commands: this kernel issues them one at a time */
#define CMD_BYTES    32u
#define ITT_ALIGN    256u
#define DEVTAB_MAX_BYTES (64u * 1024u)

struct its_device {
    struct its_device *next;
    uint32_t devid;
    paddr_t itt;
};

struct gicv3_its {
    volatile uint8_t *regs;
    paddr_t pa;
    uint64_t typer;
    unsigned itt_entry_size;
    unsigned lpi_base, nr_lpis;
    unsigned event_bits;          /* MAPD's Size + 1: how many event ids a device gets */
    uint32_t max_devid;           /* the flat device table's reach */

    paddr_t cmdq_pa;
    uint64_t *cmdq;               /* CMDQ_BYTES of 32-byte commands */
    unsigned cmdq_write;          /* byte offset of the next free slot */

    paddr_t rd_target[CONFIG_MAX_CPUS];   /* what MAPC/SYNC call this CPU */
    bool collection_mapped[CONFIG_MAX_CPUS];

    struct its_device *devices;
    spinlock_t lock;
};

static uint32_t rd32(const struct gicv3_its *its, unsigned off)
{
    return *(volatile uint32_t *)(its->regs + off);
}
static void wr32(struct gicv3_its *its, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(its->regs + off) = v;
}
static uint64_t rd64(const struct gicv3_its *its, unsigned off)
{
    return *(volatile uint64_t *)(its->regs + off);
}
static void wr64(struct gicv3_its *its, unsigned off, uint64_t v)
{
    *(volatile uint64_t *)(its->regs + off) = v;
}

static paddr_t alloc_zeroed(size_t bytes)
{
    unsigned order = 0;
    while (((size_t)PAGE_SIZE << order) < bytes)
        order++;
    struct page *pg = pmm_alloc_pages(order, PMM_FLAGS_ZERO);
    return pg ? page_to_phys(pg) : 0;
}

/* --- the command queue ---
 *
 * Commands are written at CWRITER and the ITS consumes up to it; a
 * command has taken effect when CREADR has passed it. Everything here
 * runs under `its->lock`. */

static void cmd_submit(struct gicv3_its *its, uint64_t dw0, uint64_t dw1, uint64_t dw2, uint64_t dw3)
{
    uint64_t *slot = (uint64_t *)((uint8_t *)its->cmdq + its->cmdq_write);
    slot[0] = dw0;
    slot[1] = dw1;
    slot[2] = dw2;
    slot[3] = dw3;
    its->cmdq_write = (its->cmdq_write + CMD_BYTES) % CMDQ_BYTES;
    dsb_ish();                       /* the command before the pointer that publishes it */
    wr64(its, GITS_CWRITER, its->cmdq_write);
}

/* Wait for the ITS to have read everything written so far. The queue is
 * only ever one command ahead here, so this is a short spin; a stuck ITS
 * would otherwise leave a device with no interrupts and no explanation,
 * so it is bounded and reported. */
static bool cmd_drain(struct gicv3_its *its)
{
    for (unsigned spins = 0; spins < 1000000u; spins++) {
        if ((rd64(its, GITS_CREADR) & 0xFFFE0u) == its->cmdq_write)
            return true;
        arch_cpu_relax();
    }
    kwarn("its: the command queue did not drain (CREADR 0x%llx, CWRITER 0x%x)",
          (unsigned long long)rd64(its, GITS_CREADR), its->cmdq_write);
    return false;
}

static void cmd_sync(struct gicv3_its *its, unsigned cpu)
{
    cmd_submit(its, CMD_SYNC, 0, (uint64_t)its->rd_target[cpu] << 16, 0);
}

/* --- tables --- */

/* Program one GITS_BASER: `entries` of whatever size the ITS says it
 * uses. Returns false when the table type is not implemented (then
 * there is nothing to program) or memory ran out. */
static bool baser_setup(struct gicv3_its *its, unsigned n, unsigned type, size_t entries,
                        size_t cap_bytes, uint32_t *reach_out)
{
    unsigned off = GITS_BASER0 + n * 8;
    uint64_t val = rd64(its, off);
    if (BASER_TYPE(val) != type)
        return false;
    unsigned esz = BASER_ENTRY_SIZE(val);
    size_t bytes = entries * esz;
    if (bytes > cap_bytes)
        bytes = cap_bytes;
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0)
        pages = 1;
    if (pages > 256)
        pages = 256;
    paddr_t pa = alloc_zeroed(pages * PAGE_SIZE);
    if (pa == 0)
        return false;
    val = BASER_VALID | BASER_INNER_RaWaWb | BASER_SHARE_INNER | BASER_PAGE_4K |
          ((uint64_t)type << 56) | ((uint64_t)(esz - 1) << 48) | (pa & 0x0000FFFFFFFFF000ull) |
          (uint64_t)(pages - 1);
    wr64(its, off, val);
    uint64_t back = rd64(its, off);
    if ((back & BASER_VALID) == 0) {
        kwarn("its: the implementation refused a type %u table", type);
        return false;
    }
    if (reach_out)
        *reach_out = (uint32_t)((pages * PAGE_SIZE) / esz);
    return true;
}

struct gicv3_its *its_init(paddr_t base, unsigned lpi_base, unsigned nr_lpis)
{
    struct gicv3_its *its = kzalloc(sizeof(*its));
    if (its == NULL)
        return NULL;
    static const spinlock_t init_lock = SPINLOCK_INIT("its");
    its->lock = init_lock;
    its->pa = base;
    vaddr_t va = vm_map_phys(base, 0x20000, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0) {
        kwarn("its: cannot map the translator at 0x%llx", (unsigned long long)base);
        kfree(its);
        return NULL;
    }
    its->regs = (volatile uint8_t *)va;
    its->typer = rd64(its, GITS_TYPER);
    if ((its->typer & GITS_TYPER_PHYSICAL) == 0) {
        kwarn("its: no physical LPI support; not used");
        kfree(its);
        return NULL;
    }
    its->itt_entry_size = GITS_TYPER_ITT_SIZE(its->typer);
    its->lpi_base = lpi_base;
    its->nr_lpis = nr_lpis;

    /* One event id per LPI, so every device's table is the same size and
     * the event id needs no allocator of its own. */
    its->event_bits = 1;
    while ((1u << its->event_bits) < nr_lpis)
        its->event_bits++;
    if (its->event_bits > GITS_TYPER_IDBITS(its->typer))
        its->event_bits = GITS_TYPER_IDBITS(its->typer);

    /* Quiesce before touching the tables: an enabled ITS may be using
     * whatever the firmware left. */
    wr32(its, GITS_CTLR, 0);
    for (unsigned spins = 0; spins < 1000000u; spins++) {
        if (rd32(its, GITS_CTLR) & GITS_CTLR_QUIESCENT)
            break;
        arch_cpu_relax();
    }

    unsigned devbits = GITS_TYPER_DEVBITS(its->typer);
    size_t want = devbits >= 32 ? 0xFFFFFFFFull : (1ull << devbits);
    bool have_dev = false;
    for (unsigned n = 0; n < 8; n++) {
        uint32_t reach = 0;
        if (baser_setup(its, n, BASER_TYPE_DEVICE, want, DEVTAB_MAX_BYTES, &reach)) {
            its->max_devid = reach ? reach - 1 : 0;
            have_dev = true;
        } else {
            baser_setup(its, n, BASER_TYPE_COLLECTION, CONFIG_MAX_CPUS, PAGE_SIZE, NULL);
        }
    }
    if (!have_dev) {
        kwarn("its: no device table; not used");
        kfree(its);
        return NULL;
    }

    its->cmdq_pa = alloc_zeroed(CMDQ_BYTES);
    if (its->cmdq_pa == 0) {
        kwarn("its: no memory for the command queue");
        kfree(its);
        return NULL;
    }
    its->cmdq = phys_to_virt(its->cmdq_pa);
    wr64(its, GITS_CBASER, CBASER_VALID | CBASER_INNER_RaWaWb | CBASER_SHARE_INNER |
                               (its->cmdq_pa & 0x000FFFFFFFFFF000ull) | (CMDQ_BYTES / PAGE_SIZE - 1));
    wr64(its, GITS_CWRITER, 0);
    its->cmdq_write = 0;
    wr32(its, GITS_CTLR, GITS_CTLR_ENABLED);

    kinfo("its: at 0x%llx, %u device-id bits (table reaches %u), %u event bits, ITT entries %u bytes",
          (unsigned long long)base, devbits, its->max_devid + 1, its->event_bits,
          its->itt_entry_size);
    return its;
}

void its_init_cpu(struct gicv3_its *its, unsigned cpu, paddr_t rd_pa, unsigned rd_index)
{
    if (its == NULL || cpu >= CONFIG_MAX_CPUS)
        return;
    /* GITS_TYPER.PTA says whether a collection names a redistributor by
     * address or by processor number. Getting this wrong points every
     * interrupt at collection zero's CPU, which is why it is read rather
     * than assumed. */
    paddr_t target = (its->typer & GITS_TYPER_PTA) ? (rd_pa >> 16) : (paddr_t)rd_index;

    arch_irq_state_t s = spin_lock_irqsave(&its->lock);
    its->rd_target[cpu] = target;
    cmd_submit(its, CMD_MAPC, 0, (1ull << 63) | (target << 16) | (uint64_t)cpu, 0);
    cmd_sync(its, cpu);
    cmd_drain(its);
    its->collection_mapped[cpu] = true;
    spin_unlock_irqrestore(&its->lock, s);
}

/* Caller holds the lock. */
static struct its_device *device_get(struct gicv3_its *its, uint32_t devid)
{
    for (struct its_device *d = its->devices; d != NULL; d = d->next)
        if (d->devid == devid)
            return d;
    if (devid > its->max_devid) {
        kwarn("its: device id 0x%x is beyond the device table (reaches 0x%x); no MSI for it", devid,
              its->max_devid);
        return NULL;
    }
    size_t itt_bytes = ((size_t)1 << its->event_bits) * its->itt_entry_size;
    if (itt_bytes < ITT_ALIGN)
        itt_bytes = ITT_ALIGN;
    paddr_t itt = alloc_zeroed(itt_bytes);
    if (itt == 0)
        return NULL;
    struct its_device *d = kzalloc(sizeof(*d));
    if (d == NULL)
        return NULL;
    d->devid = devid;
    d->itt = itt;
    d->next = its->devices;
    its->devices = d;
    cmd_submit(its, CMD_MAPD | ((uint64_t)devid << 32), (uint64_t)(its->event_bits - 1),
               (1ull << 63) | (itt & 0x000FFFFFFFFFFF00ull), 0);
    return d;
}

int its_map_event(struct gicv3_its *its, uint32_t devid, uint32_t event, unsigned lpi, unsigned cpu)
{
    if (its == NULL || cpu >= CONFIG_MAX_CPUS)
        return -EINVAL;
    if (event >= (1u << its->event_bits))
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&its->lock);
    if (!its->collection_mapped[cpu]) {
        spin_unlock_irqrestore(&its->lock, s);
        return -EINVAL;   /* that CPU has no collection: it never ran its_init_cpu */
    }
    struct its_device *d = device_get(its, devid);
    if (d == NULL) {
        spin_unlock_irqrestore(&its->lock, s);
        return -ENOSPC;
    }
    cmd_submit(its, CMD_MAPTI | ((uint64_t)devid << 32), (uint64_t)event | ((uint64_t)lpi << 32),
               (uint64_t)cpu, 0);
    cmd_submit(its, CMD_INV | ((uint64_t)devid << 32), event, 0, 0);
    cmd_sync(its, cpu);
    bool ok = cmd_drain(its);
    spin_unlock_irqrestore(&its->lock, s);
    return ok ? 0 : -EIO;
}

void its_unmap_event(struct gicv3_its *its, uint32_t devid, uint32_t event, unsigned cpu)
{
    if (its == NULL || cpu >= CONFIG_MAX_CPUS)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&its->lock);
    cmd_submit(its, CMD_DISCARD | ((uint64_t)devid << 32), event, 0, 0);
    cmd_sync(its, cpu);
    cmd_drain(its);
    spin_unlock_irqrestore(&its->lock, s);
}

paddr_t its_translater(const struct gicv3_its *its)
{
    return its->pa + GITS_TRANSLATER;
}
