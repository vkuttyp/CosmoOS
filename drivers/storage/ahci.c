/*
 * ahci.c - AHCI host bus adapter: SATA disks as block devices
 * (docs/drivers/ahci/design.md, invariants A1-A6).
 *
 * The fourth model of device behind the block layer: a per-port command
 * list of 32 slots, a scatter-gather table per slot, completion by a bit
 * clearing. The disk behind a port is a struct blkdev named
 * ahci<controller>p<port> whose DMA device is the controller -- a SATA
 * port has no identity of its own, so it is not a struct device, which
 * answers the question the USB unit left open (design.md, "What this
 * unit answers").
 *
 * Non-queued commands only: the HBA hands them to the device one at a
 * time while several sit in its slots; NCQ's tag machinery was not
 * written because the benchmark showed nothing for it to buy on this
 * host (docs/drivers/ahci/testing.md).
 */

#include <kernel/blk.h>
#include <kernel/device.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/interrupt.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/mutex.h>
#include <kernel/page.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <drivers/pci.h>

#include "ahci.h"

#define AHCI_MAX_PORTS     32u
#define AHCI_MAX_SLOTS     32u
#define AHCI_MAX_SECTORS   256u                       /* 128 KiB at 512-byte sectors */
#define AHCI_TIMEOUT_NS    (10ull * 1000 * 1000 * 1000)
#define AHCI_SYNC_NS       (5ull * 1000 * 1000 * 1000)
#define AHCI_STOP_MS       500u
#define AHCI_RESET_MS      1000u
#define AHCI_DEBOUNCE_MS   100u
#define AHCI_TABLES_BYTES  (AHCI_MAX_SLOTS * sizeof(struct ahci_cmd_table))   /* 32 KiB */

struct ahci;
struct ahci_port;

/* A synchronous command's waiter: IDENTIFY and the tests' one-offs. */
struct ahci_sync {
    struct completion done;
    int status;
};

struct ahci_slot {
    struct bio *bio;                   /* the bio in this slot, or */
    struct ahci_sync *sync;            /* the waiter */
    dma_addr_t seg_dma[AHCI_PRDT_MAX];
    uint32_t seg_len[AHCI_PRDT_MAX];
    unsigned nsegs;
    enum dma_dir dir;
    bool nomap;                        /* debug_dma: the address was never mapped */
};

struct ahci_disk {
    struct blkdev bd;
    struct ahci_port *port;
    char model[41], serial[21];
    bool lba48, wcache, ncq;
    unsigned qdepth;
};

struct ahci_port {
    struct ahci *hba;
    unsigned index;
    vaddr_t regs;
    bool implemented;

    struct ahci_cmd_header *cl;        /* 32 headers; the received-FIS area follows in the same page */
    dma_addr_t cl_dma;
    void *fis;
    dma_addr_t fis_dma;
    struct ahci_cmd_table *tables;     /* one per slot */
    dma_addr_t tables_dma;

    struct mutex hotplug;              /* attach, detach, probe and reset: one at a time per port (worker, tests, remove) */
    spinlock_t lock;                   /* active, slots[], disk, the flags below */
    uint32_t active;                   /* slots holding a command */
    struct ahci_slot slots[AHCI_MAX_SLOTS];
    struct ahci_disk *disk;            /* the disk attached, NULL when none */
    bool error;                        /* the handler saw a task-file or bus error: the worker recovers */
    unsigned err_slot;                 /* PxCMD.CCS at that moment */
    uint32_t err_ci;                   /* PxCI at that moment: which commands the HBA still held */
    bool recovering;                   /* a restart is in progress: submit refuses (-EAGAIN) until the port runs again */
    bool change;                       /* PCS/PRCS: the worker re-reads the port */
    uint64_t issued, completed, errors, resets;
};

struct ahci {
    struct pci_device *pdev;
    vaddr_t abar;
    unsigned index, nports, nslots;
    uint32_t pi;
    uint16_t version;
    bool s64, sncq;
    int vector;
    bool msix;
    struct ahci_port ports[AHCI_MAX_PORTS];
    struct thread *worker;
    struct waitqueue wq;
    bool stop, wake;
    uint64_t irqs;
};

static unsigned g_next_index;

static uint32_t rd32(vaddr_t a) { return *(volatile uint32_t *)a; }
static void wr32(vaddr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t prd(struct ahci_port *p, unsigned reg) { return rd32(p->regs + reg); }
static inline void pwr(struct ahci_port *p, unsigned reg, uint32_t v) { wr32(p->regs + reg, v); }
static inline void wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }

static bool wait_bits(vaddr_t reg, uint32_t mask, uint32_t want, unsigned ms)
{
    for (unsigned waited = 0; waited <= ms; waited++) {
        if ((rd32(reg) & mask) == want)
            return true;
        thread_sleep_ms(1);
    }
    return (rd32(reg) & mask) == want;
}

/* --- port start/stop (§10.1.2, §10.3) ------------------------------------------------ */

/* Stop command processing only: FIS receive stays on, so the D2H FIS a
 * COMRESET produces reaches the received-FIS area and PxTFD -- with FRE
 * off the signature FIS is held back and the busy bit never clears
 * (§10.4.2; the first version waited a second for exactly that). */
static bool port_stop_cmd(struct ahci_port *p)
{
    uint32_t cmd = prd(p, PX_CMD);
    if (cmd & PXCMD_ST) {
        pwr(p, PX_CMD, cmd & ~PXCMD_ST);
        if (!wait_bits(p->regs + PX_CMD, PXCMD_CR, 0, AHCI_STOP_MS))
            return false;
    }
    return true;
}

/* Stop both command processing and FIS receive: for (re)programming the
 * port's memory and for teardown. */
static bool port_stop(struct ahci_port *p)
{
    if (!port_stop_cmd(p))
        return false;
    uint32_t cmd = prd(p, PX_CMD);
    if (cmd & PXCMD_FRE) {
        pwr(p, PX_CMD, cmd & ~PXCMD_FRE);
        if (!wait_bits(p->regs + PX_CMD, PXCMD_FR, 0, AHCI_STOP_MS))
            return false;
    }
    return true;
}

static void port_start(struct ahci_port *p)
{
    uint32_t cmd = prd(p, PX_CMD);
    pwr(p, PX_CMD, cmd | PXCMD_FRE);
    (void)wait_bits(p->regs + PX_CMD, PXCMD_FR, PXCMD_FR, 100);
    pwr(p, PX_CMD, prd(p, PX_CMD) | PXCMD_ST);
}

/* COMRESET (§10.4.2): the link is re-established and the device answers
 * with its signature; true when a device is present afterwards. */
static bool port_comreset(struct ahci_port *p)
{
    uint32_t sctl = prd(p, PX_SCTL);
    pwr(p, PX_SCTL, (sctl & ~PXSCTL_DET_MASK) | PXSCTL_DET_INIT);
    thread_sleep_ms(2);
    pwr(p, PX_SCTL, sctl & ~PXSCTL_DET_MASK);
    bool up = false;
    for (unsigned ms = 0; ms < AHCI_RESET_MS; ms++) {
        if (PXSSTS_DET(prd(p, PX_SSTS)) == DET_PRESENT) {
            up = true;
            break;
        }
        thread_sleep_ms(1);
    }
    pwr(p, PX_SERR, 0xffffffffu);
    if (up)
        (void)wait_bits(p->regs + PX_TFD, ATA_STS_BSY | ATA_STS_DRQ, 0, AHCI_RESET_MS);
    p->resets++;
    return up;
}

/* --- slots and commands ---------------------------------------------------------------- */

/* p->lock held. */
static int slot_alloc(struct ahci_port *p)
{
    for (unsigned i = 0; i < p->hba->nslots; i++) {
        if (!(p->active & (1u << i))) {
            p->active |= 1u << i;
            memset(&p->slots[i], 0, sizeof(p->slots[i]));
            return (int)i;
        }
    }
    return -1;
}

/* The command FIS and header for `slot`: an ATA command with a 48-bit LBA
 * and a 16-bit count, the PRDT already written by the caller. */
static void cmd_fill(struct ahci_port *p, unsigned slot, uint8_t cmd, uint64_t lba, uint32_t count, bool write,
                     unsigned nprd)
{
    struct ahci_cmd_table *t = &p->tables[slot];
    uint8_t *f = t->cfis;
    memset(f, 0, 20);
    f[0] = FIS_TYPE_REG_H2D;
    f[1] = FIS_H2D_C;
    f[2] = cmd;
    f[3] = 0;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = ATA_DEV_LBA;
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[11] = 0;
    f[12] = (uint8_t)count;
    f[13] = (uint8_t)(count >> 8);
    f[14] = 0;
    f[15] = 0;
    struct ahci_cmd_header *h = &p->cl[slot];
    h->flags = CMDH_CFL(5) | (write ? CMDH_W : 0) | CMDH_PRDTL(nprd);
    h->prdbc = 0;
    h->ctba = p->tables_dma + (dma_addr_t)slot * sizeof(struct ahci_cmd_table);
}

static void prd_set(struct ahci_port *p, unsigned slot, unsigned i, dma_addr_t dma, uint32_t len)
{
    struct ahci_prd *e = &p->tables[slot].prdt[i];
    e->dba = dma;
    e->rsvd = 0;
    e->dbc = (len - 1) & 0x3fffffu;
}

/* Unmap a slot's segments. p->lock held or the slot otherwise quiet. */
static void slot_unmap(struct ahci_port *p, unsigned slot)
{
    struct ahci_slot *s = &p->slots[slot];
    if (s->nomap)
        return;
    for (unsigned i = 0; i < s->nsegs; i++)
        dma_unmap(&p->hba->pdev->dev, s->seg_dma[i], s->seg_len[i], s->dir);
    s->nsegs = 0;
}

/*
 * Complete every active slot with `status` (the victim, if any, with
 * `victim_status`) and free them. Thread context or handler; the caller
 * has made sure the HBA is not executing them (port stopped) -- or they
 * are done. Completions run after the lock is dropped.
 */
static void slots_fail(struct ahci_port *p, struct bio *victim, int victim_status, int status)
{
    for (;;) {
        arch_irq_state_t f = spin_lock_irqsave(&p->lock);
        int slot = -1;
        for (unsigned i = 0; i < AHCI_MAX_SLOTS; i++) {
            if (p->active & (1u << i)) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            spin_unlock_irqrestore(&p->lock, f);
            return;
        }
        struct ahci_slot s = p->slots[slot];
        slot_unmap(p, (unsigned)slot);
        p->active &= ~(1u << slot);
        p->completed++;
        spin_unlock_irqrestore(&p->lock, f);
        int st = s.bio == victim && victim != NULL ? victim_status : status;
        if (s.bio)
            bio_complete(s.bio, st);
        else if (s.sync) {
            s.sync->status = st;
            complete(&s.sync->done);
        }
    }
}

/* Complete the slots in `done` with `status`; they are the caller's to
 * complete (their PxCI bits cleared, or the port is stopped). p->lock held
 * on entry and exit; dropped around each completion. */
static void slots_complete_locked(struct ahci_port *p, uint32_t done, int status, arch_irq_state_t *f)
{
    while (done) {
        unsigned slot = (unsigned)__builtin_ctz(done);
        done &= ~(1u << slot);
        struct ahci_slot s = p->slots[slot];
        slot_unmap(p, slot);
        p->active &= ~(1u << slot);
        p->completed++;
        spin_unlock_irqrestore(&p->lock, *f);
        if (s.bio)
            bio_complete(s.bio, status);
        else if (s.sync) {
            s.sync->status = status;
            complete(&s.sync->done);
        }
        *f = spin_lock_irqsave(&p->lock);
    }
}

/* Slots whose PxCI bit has cleared are done (§5.5.1). Handler context. */
static void port_complete(struct ahci_port *p, int status)
{
    arch_irq_state_t f = spin_lock_irqsave(&p->lock);
    uint32_t ci = prd(p, PX_CI);
    uint32_t done = p->active & ~ci;
    while (done) {
        unsigned slot = (unsigned)__builtin_ctz(done);
        done &= ~(1u << slot);
        struct ahci_slot s = p->slots[slot];
        slot_unmap(p, slot);
        p->active &= ~(1u << slot);
        p->completed++;
        spin_unlock_irqrestore(&p->lock, f);
        if (s.bio)
            bio_complete(s.bio, status);
        else if (s.sync) {
            s.sync->status = status;
            complete(&s.sync->done);
        }
        f = spin_lock_irqsave(&p->lock);
    }
    spin_unlock_irqrestore(&p->lock, f);
}

/*
 * Stop the port, fail everything it holds (the victim with
 * `victim_status`, the rest with `status`), clear the error state, reset
 * the link if the device is stuck, start again. `recovering` is set for
 * the whole of it so submit and cmd_sync refuse meanwhile: a command
 * accepted while the port is stopped would be failed with the rest or
 * sit unissued until another timeout (review, PR #53, three rounds of
 * finding paths that restarted without saying so). Thread context.
 */
static void port_restart(struct ahci_port *p, struct bio *victim, int victim_status, int status)
{
    arch_irq_state_t f = spin_lock_irqsave(&p->lock);
    p->recovering = true;
    spin_unlock_irqrestore(&p->lock, f);
    (void)port_stop_cmd(p);
    slots_fail(p, victim, victim_status, status);
    pwr(p, PX_SERR, 0xffffffffu);
    if (PXTFD_STS(prd(p, PX_TFD)) & (ATA_STS_BSY | ATA_STS_DRQ))
        (void)port_comreset(p);
    port_start(p);
    f = spin_lock_irqsave(&p->lock);
    p->recovering = false;
    p->errors++;
    spin_unlock_irqrestore(&p->lock, f);
}

/*
 * A synchronous command: one PRDT entry over `buf` (or `raw_dma`, a bus
 * address the caller chose, for debug_dma), waited for with a bound. On
 * the bound the port is restarted and the command fails -ETIMEDOUT.
 * Thread context.
 */
static int cmd_sync(struct ahci_port *p, uint8_t cmd, uint64_t lba, uint32_t count, void *buf, uint32_t len,
                    bool write, dma_addr_t raw_dma)
{
    struct ahci_sync w;
    completion_init(&w.done, "ahci-sync");
    w.status = -EINPROGRESS;
    /* Not while the port is being restarted: a slot taken now would be
     * absent from the recovery's PxCI snapshot and sorted wrongly (review,
     * PR #53). Wait for the restart, bounded like the command itself. */
    uint64_t until = clock_now_ns() + AHCI_SYNC_NS;
    while (__atomic_load_n(&p->recovering, __ATOMIC_ACQUIRE) && clock_now_ns() < until)
        thread_sleep_ms(1);
    dma_addr_t dma = raw_dma;
    if (len > 0 && raw_dma == 0) {
        dma = dma_map(&p->hba->pdev->dev, buf, len, write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
        if (dma == 0)
            return -EINVAL;
    }
    arch_irq_state_t f = spin_lock_irqsave(&p->lock);
    int slot = p->recovering ? -1 : slot_alloc(p);
    if (slot < 0) {
        bool busy = p->recovering;
        spin_unlock_irqrestore(&p->lock, f);
        if (len > 0 && raw_dma == 0)
            dma_unmap(&p->hba->pdev->dev, dma, len, write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
        return busy ? -EBUSY : -EAGAIN;
    }
    struct ahci_slot *s = &p->slots[slot];
    s->sync = &w;
    s->dir = write ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    s->nomap = raw_dma != 0;
    if (len > 0) {
        s->seg_dma[0] = dma;
        s->seg_len[0] = len;
        s->nsegs = 1;
        prd_set(p, (unsigned)slot, 0, dma, len);
    }
    cmd_fill(p, (unsigned)slot, cmd, lba, count, write, len > 0 ? 1 : 0);
    wmb();
    pwr(p, PX_CI, 1u << slot);
    p->issued++;
    spin_unlock_irqrestore(&p->lock, f);

    uint64_t deadline = clock_now_ns() + AHCI_SYNC_NS;
    while (!completion_done(&w.done) && clock_now_ns() < deadline)
        thread_sleep_ns(100000);
    if (!completion_done(&w.done)) {
        /* Take the slot back: the same restart the block layer's timeout runs. */
        kwarn("ahci%u: port %u: command 0x%02x did not complete in %llu ms; restarting the port", p->hba->index,
              p->index, cmd, (unsigned long long)(AHCI_SYNC_NS / 1000000));
        port_restart(p, NULL, 0, -ETIMEDOUT);
    }
    wait_for_completion(&w.done);
    return w.status;
}

/* --- the disk ---------------------------------------------------------------------------- */

static struct ahci_disk *disk_of(struct blkdev *bd) { return bd->priv; }

static void ata_string(char *out, const uint16_t *words, unsigned nwords)
{
    for (unsigned i = 0; i < nwords; i++) {
        out[2 * i] = (char)(words[i] >> 8);
        out[2 * i + 1] = (char)(words[i] & 0xff);
    }
    out[2 * nwords] = '\0';
    for (char *e = out + 2 * nwords - 1; e >= out && *e == ' '; e--)
        *e = '\0';
    char *s = out;
    while (*s == ' ')
        s++;
    if (s != out)
        memmove(out, s, strlen(s) + 1);
}

static int disk_identify(struct ahci_port *p, struct ahci_disk *d, uint64_t *capacity, uint32_t *sector)
{
    uint16_t *id = kmalloc(512, KMEM_ZERO);
    if (id == NULL)
        return -ENOMEM;
    int rc = cmd_sync(p, ATA_CMD_IDENTIFY, 0, 0, id, 512, false, 0);
    if (rc) {
        kfree(id);
        return rc;
    }
    ata_string(d->model, id + ID_MODEL, 20);
    ata_string(d->serial, id + ID_SERIAL, 10);
    d->lba48 = (id[ID_CMD_SET_2] & (1u << 10)) != 0;
    uint64_t sectors;
    if (d->lba48) {
        sectors = (uint64_t)id[ID_LBA48_COUNT] | ((uint64_t)id[ID_LBA48_COUNT + 1] << 16) |
                  ((uint64_t)id[ID_LBA48_COUNT + 2] << 32) | ((uint64_t)id[ID_LBA48_COUNT + 3] << 48);
    } else {
        sectors = (uint64_t)id[ID_LBA28_COUNT] | ((uint64_t)id[ID_LBA28_COUNT + 1] << 16);
    }
    uint32_t ss = 512;
    uint16_t info = id[ID_SECTOR_INFO];
    if ((info & 0xc000u) == 0x4000u && (info & (1u << 12))) {
        uint32_t words = (uint32_t)id[ID_SECTOR_SIZE] | ((uint32_t)id[ID_SECTOR_SIZE + 1] << 16);
        ss = words * 2;
    }
    d->wcache = (id[ID_CMD_SET_EN_2] & (1u << 5)) != 0;
    d->ncq = (id[ID_SATA_CAP] & (1u << 8)) != 0;
    d->qdepth = (id[ID_QUEUE_DEPTH] & 0x1fu) + 1;
    kfree(id);
    if (sectors == 0 || ss < 512 || ss > 4096 || (ss & (ss - 1)) != 0) {
        kwarn("ahci%u: port %u: geometry refused (%llu sectors of %u bytes)", p->hba->index, p->index,
              (unsigned long long)sectors, ss);
        return -ENOTSUP;
    }
    *capacity = sectors;
    *sector = ss;
    return 0;
}

/* The last holder let go. Nothing here may touch the port or the
 * controller: a holder's reference can outlive ahci_remove, which has
 * freed both by then (review, PR #53). The disk's own memory is all
 * that is still ours. */
static void disk_release(struct blkdev *bd)
{
    kfree(disk_of(bd));
}

static const struct blkdev_ops ahci_blk_ops;

/* A SATA disk on the port: identify it and register its blkdev. p->disk
 * must be NULL. Thread context. */
static int disk_attach(struct ahci_port *p)
{
    struct ahci_disk *d = kzalloc(sizeof(*d));
    if (d == NULL)
        return -ENOMEM;
    d->port = p;
    uint64_t capacity = 0;
    uint32_t sector = 512;
    int rc = disk_identify(p, d, &capacity, &sector);
    if (rc) {
        kerror("ahci%u: port %u: IDENTIFY DEVICE failed (%d)", p->hba->index, p->index, rc);
        kfree(d);
        return rc;
    }
    d->bd.dev = &p->hba->pdev->dev;   /* the controller does the DMA; a port has no identity of its own */
    d->bd.ops = &ahci_blk_ops;
    d->bd.sector_size = sector;
    d->bd.capacity = capacity;
    d->bd.max_sectors = AHCI_MAX_SECTORS;
    d->bd.max_segments = AHCI_PRDT_MAX;
    d->bd.timeout_ns = AHCI_TIMEOUT_NS;
    d->bd.priv = d;
    d->bd.nr_queues = 1;
    char name[BLKDEV_NAME_MAX];
    ksnprintf(name, sizeof(name), "ahci%up%u", p->hba->index, p->index);
    rc = blk_register_named(&d->bd, name);
    if (rc) {
        kerror("ahci%u: port %u: blk_register_named(%s): %d", p->hba->index, p->index, name, rc);
        kfree(d);
        return rc;
    }
    arch_irq_state_t f = spin_lock_irqsave(&p->lock);
    p->disk = d;
    spin_unlock_irqrestore(&p->lock, f);
    kinfo("ahci%u: port %u: %s (%s) is %s: %llu sectors of %u bytes%s%s, NCQ %s depth %u", p->hba->index, p->index,
          d->model, d->serial, d->bd.name, (unsigned long long)capacity, sector, d->lba48 ? ", LBA48" : ", LBA28",
          d->wcache ? ", write cache" : "", d->ncq ? "yes" : "no", d->qdepth);
    return 0;
}

/* The disk is gone (the port says so, or a test says so): stop the port,
 * fail everything it holds, unregister, drop the creator's reference. The
 * port is started again so a returning disk is seen. Thread context. */
static void disk_detach(struct ahci_port *p, int status)
{
    arch_irq_state_t f = spin_lock_irqsave(&p->lock);
    struct ahci_disk *d = p->disk;
    p->disk = NULL;
    spin_unlock_irqrestore(&p->lock, f);
    if (d == NULL)
        return;
    (void)port_stop_cmd(p);
    slots_fail(p, NULL, 0, status);
    pwr(p, PX_SERR, 0xffffffffu);
    port_start(p);
    blk_unregister(&d->bd);   /* refuses new bios; waits for submits inside the driver */
    kinfo("ahci%u: port %u: %s removed (%llu issued, %llu completed, %llu errors, %llu resets)", p->hba->index,
          p->index, d->bd.name, (unsigned long long)p->issued, (unsigned long long)p->completed,
          (unsigned long long)p->errors, (unsigned long long)p->resets);
    blkdev_put(&d->bd);       /* the creator's; disk_release frees d when the holders are gone */
}

/* What is on the port now: a signature, or 0 for nothing usable. */
static uint32_t port_signature(struct ahci_port *p)
{
    uint32_t ssts = prd(p, PX_SSTS);
    if (PXSSTS_DET(ssts) != DET_PRESENT || PXSSTS_IPM(ssts) != IPM_ACTIVE)
        return 0;
    return prd(p, PX_SIG);
}

/* Look at the port and make the disk match: attach one that appeared,
 * detach one that left. Thread context. */
static void port_probe_locked(struct ahci_port *p)
{
    uint32_t sig = port_signature(p);
    bool have = p->disk != NULL;
    if (sig == SIG_SATA && !have) {
        (void)wait_bits(p->regs + PX_TFD, ATA_STS_BSY | ATA_STS_DRQ, 0, AHCI_RESET_MS);
        (void)disk_attach(p);
    } else if (sig != SIG_SATA && have) {
        disk_detach(p, -ENODEV);
    } else if (sig != 0 && sig != SIG_SATA && !have) {
        kinfo("ahci%u: port %u: %s (signature 0x%08x) is not driven", p->hba->index, p->index,
              sig == SIG_ATAPI ? "an ATAPI device" : sig == SIG_PMP ? "a port multiplier" : "an unknown device", sig);
    }
}

static void port_probe(struct ahci_port *p)
{
    mutex_lock(&p->hotplug);
    port_probe_locked(p);
    mutex_unlock(&p->hotplug);
}

/* --- the block device ---------------------------------------------------------------------- */

static int ahci_submit(struct blkdev *bd, struct bio *bio)
{
    struct ahci_disk *d = disk_of(bd);
    struct ahci_port *p = d->port;
    if (bio->dir == BIO_FLUSH && !d->wcache) {
        bio_complete(bio, 0);   /* nothing to flush */
        return 0;
    }
    unsigned nsegs = bio->dir == BIO_FLUSH ? 0 : bio_segments(bio);
    if (nsegs > AHCI_PRDT_MAX || (bio->dir != BIO_FLUSH && (bio->nsectors == 0 || bio->nsectors > 65536)))
        return -EINVAL;
    if (bio->dir != BIO_FLUSH && !d->lba48 && bio->sector + bio->nsectors > (1ull << 28))
        return -EINVAL;
    /* Debug builds, on request: the slot is filled and never issued -- a
     * command that never starts, for the timeout path. Decided here, in
     * thread context, where the injector answers. */
    bool hang = faultinject_should_fail(FI_AHCI_CI);

    /* Map outside the lock; a segment that cannot be mapped means nothing
     * was written to the port. */
    enum dma_dir dir = bio->dir == BIO_WRITE ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    dma_addr_t seg_dma[AHCI_PRDT_MAX];
    uint32_t seg_len[AHCI_PRDT_MAX];
    for (unsigned i = 0; i < nsegs; i++) {
        struct bio_vec v;
        bio_segment(bio, i, &v);
        if (v.len == 0 || (v.len & 1) || v.len > PRD_MAX_BYTES) {
            for (unsigned k = 0; k < i; k++)
                dma_unmap(bd->dev, seg_dma[k], seg_len[k], dir);
            return -EINVAL;
        }
        seg_dma[i] = dma_map(bd->dev, v.buf, v.len, dir);
        seg_len[i] = v.len;
        if (seg_dma[i] == 0) {
            for (unsigned k = 0; k < i; k++)
                dma_unmap(bd->dev, seg_dma[k], seg_len[k], dir);
            return -EINVAL;
        }
    }

    arch_irq_state_t f = spin_lock_irqsave(&p->lock);
    if (p->disk != d) {
        spin_unlock_irqrestore(&p->lock, f);
        for (unsigned k = 0; k < nsegs; k++)
            dma_unmap(bd->dev, seg_dma[k], seg_len[k], dir);
        return -ENODEV;
    }
    if (p->recovering) {
        /* The port is being stopped, failed and restarted: a command
         * accepted now would be failed with the rest (the racing readers
         * in ahci-timeout found it on aarch64). Refuse; the block layer
         * queues it and resubmits when the victim completes. */
        spin_unlock_irqrestore(&p->lock, f);
        for (unsigned k = 0; k < nsegs; k++)
            dma_unmap(bd->dev, seg_dma[k], seg_len[k], dir);
        return -EAGAIN;
    }
    int slot = slot_alloc(p);
    if (slot < 0) {
        spin_unlock_irqrestore(&p->lock, f);
        for (unsigned k = 0; k < nsegs; k++)
            dma_unmap(bd->dev, seg_dma[k], seg_len[k], dir);
        return -EAGAIN;   /* every slot taken: the block layer queues it */
    }
    struct ahci_slot *s = &p->slots[slot];
    s->bio = bio;
    s->dir = dir;
    s->nsegs = nsegs;
    for (unsigned i = 0; i < nsegs; i++) {
        s->seg_dma[i] = seg_dma[i];
        s->seg_len[i] = seg_len[i];
        prd_set(p, (unsigned)slot, i, seg_dma[i], seg_len[i]);
    }
    uint8_t cmd = bio->dir == BIO_FLUSH ? ATA_CMD_FLUSH_CACHE_EXT
                  : bio->dir == BIO_WRITE ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    cmd_fill(p, (unsigned)slot, cmd, bio->dir == BIO_FLUSH ? 0 : bio->sector,
             bio->dir == BIO_FLUSH ? 0 : (bio->nsectors == 65536 ? 0 : bio->nsectors), bio->dir == BIO_WRITE, nsegs);
    wmb();
    if (!hang)
        pwr(p, PX_CI, 1u << slot);
    p->issued++;
    spin_unlock_irqrestore(&p->lock, f);
    return 0;
}

/* The block layer's timeout thread: the port has stopped answering. Stop
 * it, fail what it holds (the victim -ETIMEDOUT, the rest -EIO: a queue
 * is not replayed on the guess that one command was the problem), and
 * start it again. */
static void ahci_timeout(struct blkdev *bd, struct bio *victim)
{
    struct ahci_disk *d = disk_of(bd);
    struct ahci_port *p = d->port;
    arch_irq_state_t f = spin_lock_irqsave(&p->lock);
    bool mine = false;
    for (unsigned i = 0; i < AHCI_MAX_SLOTS; i++)
        if ((p->active & (1u << i)) && p->slots[i].bio == victim)
            mine = true;
    spin_unlock_irqrestore(&p->lock, f);
    if (!mine)
        return;   /* completed between the layer's decision and now */
    kwarn("ahci%u: port %u: command timed out (PxCI 0x%08x, PxTFD 0x%08x); restarting the port", p->hba->index,
          p->index, prd(p, PX_CI), prd(p, PX_TFD));
    port_restart(p, victim, -ETIMEDOUT, -EIO);
}

/* Tests only: a READ DMA EXT of one sector into an address the caller
 * chose (typically one no IOMMU mapping covers). */
static int ahci_debug_dma(struct blkdev *bd, uint64_t addr)
{
    struct ahci_disk *d = disk_of(bd);
    return cmd_sync(d->port, ATA_CMD_READ_DMA_EXT, 0, 1, NULL, bd->sector_size, false, addr);
}

/* Tests only (blk.h): the hotplug paths on demand. */
static int ahci_debug_presence(struct blkdev *bd, bool present)
{
    struct ahci_disk *d = disk_of(bd);
    struct ahci_port *p = d->port;
    int rc = 0;
    mutex_lock(&p->hotplug);   /* not while the worker probes or remove detaches */
    if (!present) {
        if (p->disk != d) {
            rc = -ENODEV;
            goto out;
        }
        disk_detach(p, -ENODEV);
        goto out;
    }
    if (p->disk == NULL) {
        /* `bd` is unregistered and only names the port: probe it and
         * register a new disk. */
        port_probe_locked(p);
        rc = p->disk != NULL ? 0 : -ENODEV;
        goto out;
    }
    if (p->disk != d) {
        rc = -EBUSY;
        goto out;
    }
    /* A live disk: reset the link with whatever is in flight, re-identify,
     * keep this blkdev if the same disk answers (the recovery an error
     * that needs a COMRESET goes through). */
    arch_irq_state_t f = spin_lock_irqsave(&p->lock);
    p->recovering = true;
    spin_unlock_irqrestore(&p->lock, f);
    (void)port_stop_cmd(p);
    slots_fail(p, NULL, 0, -EIO);
    bool up = port_comreset(p);
    port_start(p);
    f = spin_lock_irqsave(&p->lock);
    p->recovering = false;
    spin_unlock_irqrestore(&p->lock, f);
    if (!up || port_signature(p) != SIG_SATA) {
        disk_detach(p, -ENODEV);
        rc = -ENODEV;
        goto out;
    }
    struct ahci_disk probe;
    memset(&probe, 0, sizeof(probe));
    probe.port = p;
    uint64_t capacity = 0;
    uint32_t sector = 0;
    rc = disk_identify(p, &probe, &capacity, &sector);
    if (rc) {
        disk_detach(p, -EIO);
        goto out;
    }
    if (strcmp(probe.serial, d->serial) != 0 || capacity != d->bd.capacity || sector != d->bd.sector_size) {
        kinfo("ahci%u: port %u: a different disk answered after the reset (%s)", p->hba->index, p->index,
              probe.serial);
        disk_detach(p, -ENODEV);
        port_probe_locked(p);
        rc = -ENODEV;
    }
out:
    mutex_unlock(&p->hotplug);
    return rc;
}

static const struct blkdev_ops ahci_blk_ops = {
    .submit = ahci_submit,
    .release = disk_release,
    .timeout = ahci_timeout,
    .debug_dma = ahci_debug_dma,
    .debug_presence = ahci_debug_presence,
};

/* --- interrupts and the worker -------------------------------------------------------------- */

static void ahci_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct ahci *h = arg;
    uint32_t is = rd32(h->abar + AHCI_IS);
    if (is == 0)
        return;
    /* With a message-signalled interrupt IS is an edge: it is cleared
     * *first* (§10.7.2.1), so a port event that lands while the ports below
     * are being served sets its bit again and raises a new message. Cleared
     * last, that event would be acknowledged unseen and wait for the next
     * one (the lost-wakeup shape CI found in the xHCI handler). */
    wr32(h->abar + AHCI_IS, is);
    h->irqs++;
    bool wake = false;
    for (unsigned i = 0; i < h->nports; i++) {
        if (!(is & (1u << i)) || !h->ports[i].implemented)
            continue;
        struct ahci_port *p = &h->ports[i];
        uint32_t pis = prd(p, PX_IS);
        pwr(p, PX_IS, pis);   /* write-one-to-clear, before the completions are read */
        if (pis & (PXIS_PCS | PXIS_PRCS)) {
            p->change = true;
            wake = true;
        }
        if (pis & PXIS_ERRORS) {
            /* The port has stopped processing (§6.2.2.1); the recovery waits
             * on registers, so the worker does it. Note which slot was
             * executing while the port still says. */
            arch_irq_state_t f = spin_lock_irqsave(&p->lock);
            if (!p->error) {
                p->error = true;
                p->err_slot = PXCMD_CCS(prd(p, PX_CMD));
                p->err_ci = prd(p, PX_CI);   /* the HBA has halted: what it still holds, and so what completed */
            }
            spin_unlock_irqrestore(&p->lock, f);
            p->errors++;
            wake = true;
        } else {
            port_complete(p, 0);
        }
    }
    if (wake) {
        __atomic_store_n(&h->wake, true, __ATOMIC_RELEASE);
        waitqueue_wake_all(&h->wq);
    }
}

/* Error recovery (§6.2.2.1), thread context: stop, fail the command that
 * was executing, clear the error, reset the link if the device is stuck,
 * start, and reissue the commands the HBA had not got to. */
static void port_recover(struct ahci_port *p)
{
    uint32_t tfd = prd(p, PX_TFD), serr = prd(p, PX_SERR);
    arch_irq_state_t f = spin_lock_irqsave(&p->lock);
    p->recovering = true;
    spin_unlock_irqrestore(&p->lock, f);
    (void)port_stop_cmd(p);
    f = spin_lock_irqsave(&p->lock);
    p->error = false;
    unsigned slot = p->err_slot;
    uint32_t ci = p->err_ci;
    /* Three kinds of slot at the error (§6.2.2.1): the one that was
     * executing (PxCMD.CCS) failed; those whose PxCI bit had already
     * cleared completed before it and are done; those still set in PxCI
     * the HBA never issued and are reissued once the port runs again.
     * PxCI itself is gone once ST is cleared, hence the handler's
     * snapshot. Reissuing every active slot would have run the completed
     * ones a second time and left their bios waiting (Greptile, PR #53). */
    uint32_t done = p->active & ~ci & ~(1u << slot);
    slots_complete_locked(p, done, 0, &f);
    struct ahci_slot s = p->slots[slot];
    bool failed = (p->active & (1u << slot)) != 0;
    if (failed) {
        slot_unmap(p, slot);
        p->active &= ~(1u << slot);
        p->completed++;
    }
    uint32_t reissue = p->active & ci;
    spin_unlock_irqrestore(&p->lock, f);
    kwarn("ahci%u: port %u: task file error (PxTFD 0x%08x, PxSERR 0x%08x) in slot %u; %u completed, %u reissued",
          p->hba->index, p->index, tfd, serr, slot, (unsigned)__builtin_popcount(done),
          (unsigned)__builtin_popcount(reissue));
    if (failed) {
        if (s.bio)
            bio_complete(s.bio, -EIO);
        else if (s.sync) {
            s.sync->status = -EIO;
            complete(&s.sync->done);
        }
    }
    pwr(p, PX_SERR, 0xffffffffu);
    if (PXTFD_STS(prd(p, PX_TFD)) & (ATA_STS_BSY | ATA_STS_DRQ))
        (void)port_comreset(p);
    port_start(p);
    f = spin_lock_irqsave(&p->lock);
    reissue &= p->active;   /* still ours */
    if (reissue) {
        wmb();
        pwr(p, PX_CI, reissue);
    }
    p->recovering = false;
    spin_unlock_irqrestore(&p->lock, f);
}

static void ahci_worker(void *arg)
{
    struct ahci *h = arg;
    for (;;) {
        wait_event(&h->wq, __atomic_load_n(&h->wake, __ATOMIC_ACQUIRE) || __atomic_load_n(&h->stop, __ATOMIC_ACQUIRE));
        if (__atomic_load_n(&h->stop, __ATOMIC_ACQUIRE))
            break;
        __atomic_store_n(&h->wake, false, __ATOMIC_RELEASE);
        for (unsigned i = 0; i < h->nports; i++) {
            struct ahci_port *p = &h->ports[i];
            if (!p->implemented)
                continue;
            if (p->error)
                port_recover(p);
            if (p->change) {
                p->change = false;
                thread_sleep_ms(AHCI_DEBOUNCE_MS);   /* a cable settles; a Phy renegotiates */
                port_probe(p);
            }
        }
    }
    thread_exit(0);
}

/* --- ports: memory and init ------------------------------------------------------------------ */

static int port_alloc(struct ahci_port *p)
{
    struct device *dev = &p->hba->pdev->dev;
    void *page = dma_alloc(dev, PAGE_SIZE, &p->cl_dma, DMA_ZERO);   /* 1 KiB command list + 256 B FIS area */
    if (page == NULL)
        return -ENOMEM;
    p->cl = page;
    p->fis = (uint8_t *)page + 1024;
    p->fis_dma = p->cl_dma + 1024;
    p->tables = dma_alloc(dev, AHCI_TABLES_BYTES, &p->tables_dma, DMA_ZERO);
    if (p->tables == NULL) {
        dma_free(dev, PAGE_SIZE, page, p->cl_dma);
        p->cl = NULL;
        return -ENOMEM;
    }
    return 0;
}

static void port_free(struct ahci_port *p)
{
    struct device *dev = &p->hba->pdev->dev;
    if (p->tables)
        dma_free(dev, AHCI_TABLES_BYTES, p->tables, p->tables_dma);
    if (p->cl)
        dma_free(dev, PAGE_SIZE, p->cl, p->cl_dma);
    p->tables = NULL;
    p->cl = NULL;
}

static int port_init(struct ahci_port *p)
{
    if (!port_stop(p)) {
        kerror("ahci%u: port %u: did not stop", p->hba->index, p->index);
        return -EIO;
    }
    int rc = port_alloc(p);
    if (rc)
        return rc;
    wr32(p->regs + PX_CLB, (uint32_t)p->cl_dma);
    wr32(p->regs + PX_CLBU, (uint32_t)(p->cl_dma >> 32));
    wr32(p->regs + PX_FB, (uint32_t)p->fis_dma);
    wr32(p->regs + PX_FBU, (uint32_t)(p->fis_dma >> 32));
    pwr(p, PX_SERR, 0xffffffffu);
    pwr(p, PX_IS, 0xffffffffu);
    pwr(p, PX_CMD, prd(p, PX_CMD) | PXCMD_SUD | PXCMD_POD | PXCMD_ICC_ACTIVE);
    port_start(p);
    pwr(p, PX_IE, PXIE_WANTED);
    return 0;
}

/* --- probe and remove -------------------------------------------------------------------------- */

static int ahci_probe(struct pci_device *pdev, const struct pci_id *id)
{
    (void)id;
    if (pdev->prog_if != 0x01) {
        kinfo("ahci: %s: SATA controller with programming interface 0x%02x is not AHCI; not driven", pdev->dev.name,
              pdev->prog_if);
        return -ENODEV;
    }
    struct ahci *h = kzalloc(sizeof(*h));
    if (h == NULL)
        return -ENOMEM;
    h->pdev = pdev;
    h->vector = -1;
    h->index = __atomic_fetch_add(&g_next_index, 1u, __ATOMIC_RELAXED);
    waitqueue_init(&h->wq, "ahci-worker");
    for (unsigned i = 0; i < AHCI_MAX_PORTS; i++) {
        h->ports[i].hba = h;
        h->ports[i].index = i;
        spinlock_init(&h->ports[i].lock, "ahci-port");
        mutex_init(&h->ports[i].hotplug, "ahci-hotplug");
    }

    pci_enable_device(pdev, true);
    int rc = -EIO;
    h->abar = pci_map_bar(pdev, 5);
    if (h->abar == 0) {
        kerror("ahci%u: %s: cannot map ABAR", h->index, pdev->dev.name);
        goto fail_free;
    }

    /* BIOS/OS handoff, where the controller has it (§10.6.1). */
    if (rd32(h->abar + AHCI_CAP2) & CAP2_BOH) {
        wr32(h->abar + AHCI_BOHC, rd32(h->abar + AHCI_BOHC) | BOHC_OOS);
        if (!wait_bits(h->abar + AHCI_BOHC, BOHC_BOS, 0, 2000)) {
            kerror("ahci%u: firmware did not release the controller (BOHC 0x%08x)", h->index,
                   rd32(h->abar + AHCI_BOHC));
            goto fail_unmap;
        }
    }
    /* AHCI mode, an HBA reset, AHCI mode again (reset clears it). */
    wr32(h->abar + AHCI_GHC, rd32(h->abar + AHCI_GHC) | GHC_AE);
    wr32(h->abar + AHCI_GHC, rd32(h->abar + AHCI_GHC) | GHC_HR);
    if (!wait_bits(h->abar + AHCI_GHC, GHC_HR, 0, AHCI_RESET_MS)) {
        kerror("ahci%u: did not come out of reset (GHC 0x%08x)", h->index, rd32(h->abar + AHCI_GHC));
        goto fail_unmap;
    }
    wr32(h->abar + AHCI_GHC, rd32(h->abar + AHCI_GHC) | GHC_AE);

    uint32_t cap = rd32(h->abar + AHCI_CAP);
    h->nports = CAP_NP(cap) + 1;
    h->nslots = CAP_NCS(cap) + 1;
    h->s64 = (cap & CAP_S64A) != 0;
    h->sncq = (cap & CAP_SNCQ) != 0;
    h->pi = rd32(h->abar + AHCI_PI);
    uint32_t vs = rd32(h->abar + AHCI_VS);
    h->version = (uint16_t)(((vs >> 16) & 0xff) << 8 | ((vs >> 8) & 0xff));
    dma_set_mask(&pdev->dev, h->s64 ? 64 : 32);

    int granted = pci_msix_enable(pdev, 1);
    if (granted >= 1) {
        h->vector = pci_msix_request(pdev, 0, ahci_irq, h, "ahci", 0);
        if (h->vector < 0) {
            pci_msix_disable(pdev);
            rc = h->vector;
            goto fail_unmap;
        }
        h->msix = true;
    } else {
        h->vector = pci_msi_enable(pdev, ahci_irq, h, "ahci", 0);
        if (h->vector < 0) {
            rc = h->vector;
            kerror("ahci%u: neither MSI-X nor MSI (%d); INTx is not driven", h->index, rc);
            goto fail_unmap;
        }
    }

    unsigned implemented = 0;
    for (unsigned i = 0; i < h->nports; i++) {
        if (!(h->pi & (1u << i)))
            continue;
        struct ahci_port *p = &h->ports[i];
        p->regs = h->abar + AHCI_PORT_BASE + AHCI_PORT_SIZE * i;
        rc = port_init(p);
        if (rc) {
            kerror("ahci%u: port %u: init failed (%d)", h->index, i, rc);
            port_free(p);
            continue;
        }
        p->implemented = true;
        implemented++;
    }
    kinfo("ahci%u: %s: AHCI %x.%x, %u port(s) implemented of %u, %u slots, %s%s", h->index, pdev->dev.name,
          h->version >> 8, h->version & 0xff, implemented, h->nports, h->nslots, h->s64 ? "64-bit DMA" : "32-bit DMA",
          h->sncq ? ", NCQ capable" : "");

    char tname[16];
    ksnprintf(tname, sizeof(tname), "ahci/%u", h->index);
    h->worker = thread_create(ahci_worker, h, tname, SCHED_PRIO_DEFAULT);
    if (h->worker == NULL) {
        rc = -ENOMEM;
        goto fail_ports;
    }
    pdev->dev.drvdata = h;
    wr32(h->abar + AHCI_IS, 0xffffffffu);
    wr32(h->abar + AHCI_GHC, rd32(h->abar + AHCI_GHC) | GHC_IE);

    /* The disks present at boot: registering a blkdev does not need the
     * device model's lock, so this runs here and the disks exist when
     * probe returns. */
    unsigned disks = 0;
    for (unsigned i = 0; i < h->nports; i++) {
        struct ahci_port *p = &h->ports[i];
        if (!p->implemented)
            continue;
        port_probe(p);
        if (p->disk)
            disks++;
    }
    if (disks == 0)
        kinfo("ahci%u: no disk on any port", h->index);
    return 0;

fail_ports:
    for (unsigned i = 0; i < h->nports; i++) {
        if (h->ports[i].implemented) {
            (void)port_stop(&h->ports[i]);
            port_free(&h->ports[i]);
        }
    }
    if (h->msix)
        pci_msix_disable(pdev);
    else
        pci_msi_disable(pdev);
    synchronize_irq((unsigned)h->vector);
fail_unmap:
    device_unmap_mmio(h->abar);
fail_free:
    kfree(h);
    return rc;
}

static void ahci_remove(struct pci_device *pdev)
{
    struct ahci *h = pdev->dev.drvdata;
    if (h == NULL)
        return;
    /* The worker first, so no probe can attach a disk behind the detach
     * pass below (Greptile, PR #53); then the disks (their commands
     * -ENODEV), then the controller, then the memory. */
    __atomic_store_n(&h->stop, true, __ATOMIC_RELEASE);
    waitqueue_wake_all(&h->wq);
    thread_join(h->worker);
    for (unsigned i = 0; i < h->nports; i++) {
        if (h->ports[i].implemented) {
            mutex_lock(&h->ports[i].hotplug);
            disk_detach(&h->ports[i], -ENODEV);
            mutex_unlock(&h->ports[i].hotplug);
        }
    }
    wr32(h->abar + AHCI_GHC, rd32(h->abar + AHCI_GHC) & ~GHC_IE);
    for (unsigned i = 0; i < h->nports; i++) {
        if (h->ports[i].implemented) {
            pwr(&h->ports[i], PX_IE, 0);
            (void)port_stop(&h->ports[i]);
        }
    }
    int vector = h->vector;
    if (h->msix)
        pci_msix_disable(pdev);
    else
        pci_msi_disable(pdev);
    if (vector >= 0)
        synchronize_irq((unsigned)vector);
    for (unsigned i = 0; i < h->nports; i++)
        if (h->ports[i].implemented)
            port_free(&h->ports[i]);
    device_unmap_mmio(h->abar);
    pdev->dev.drvdata = NULL;
    kinfo("ahci%u: removed (%llu interrupts)", h->index, (unsigned long long)h->irqs);
    kfree(h);
}

static const struct pci_id ahci_ids[] = {
    { PCI_ANY, PCI_ANY, 0x01, 0x06, PCI_ID_CLASS },   /* mass storage, SATA; prog_if 0x01 (AHCI) checked in probe */
    PCI_ID_END,
};

static struct pci_driver ahci_driver = {
    .drv = { .name = "ahci" },
    .ids = ahci_ids,
    .probe = ahci_probe,
    .remove = ahci_remove,
};

static int ahci_module_init(void)
{
    return pci_register_driver(&ahci_driver);
}

static void ahci_module_shutdown(void)
{
    pci_unregister_driver(&ahci_driver);
}

COSMO_MODULE("ahci", "1.0", ahci_module_init, ahci_module_shutdown, "", MODULE_CAP_DRIVER);
