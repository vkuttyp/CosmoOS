/*
 * e1000e.c - Intel 82574L gigabit Ethernet
 * (docs/drivers/e1000e/design.md, invariants E1-E6).
 *
 * The first network device here that is not virtio, and so the first
 * test of the stack's claim not to depend on one: this driver adds
 * nothing to the kernel's interface -- netif_ops, netif_rx, the PCI, DMA
 * and timer APIs were enough (E6). Where a descriptor-ring device
 * differs from a virtqueue is recorded in the design document as facts
 * about this device.
 *
 * One receive ring, one transmit ring, one MSI-X vector, no offloads
 * claimed until a benchmark says they pay (docs/audit/next-subsystem.md).
 */

#include <kernel/device.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/module.h>
#include <kernel/net/ether.h>
#include <kernel/netif.h>
#include <kernel/page.h>
#include <kernel/printf.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>

#include <drivers/pci.h>

#include "e1000e.h"

#define E1000E_RING       256u   /* descriptors per ring: 16 bytes each, one page */
#define E1000E_WATCHDOG_NS (1000ull * 1000 * 1000)
#define E1000E_HUNG_TICKS  5u    /* seconds of no TDH movement with work outstanding */
#define E1000E_LINK_WAIT_MS 2000u

struct e1000e {
    struct pci_device *pdev;
    vaddr_t bar;
    struct netif nif;
    spinlock_t lock;   /* both rings, their bookkeeping, the counters */

    struct e1000_rx_desc *rxd;
    dma_addr_t rxd_dma;
    struct mbuf *rx_bufs[E1000E_RING];
    unsigned rx_head;   /* the next descriptor hardware will complete */

    struct e1000_tx_desc *txd;
    dma_addr_t txd_dma;
    struct mbuf *tx_bufs[E1000E_RING];   /* a chain, stored at its last (EOP) descriptor */
    unsigned tx_head, tx_tail, tx_used;

    int vector;
    bool msix;
    bool link;

    struct timer watchdog;
    uint32_t last_tdh;
    unsigned stuck_ticks;
    uint64_t resets;
};

static uint32_t rd32(struct e1000e *e, unsigned off) { return *(volatile uint32_t *)(e->bar + off); }
static void wr32(struct e1000e *e, unsigned off, uint32_t v) { *(volatile uint32_t *)(e->bar + off) = v; }

/* The descriptor rings are coherent memory, but the compiler and the CPU
 * still need telling that a descriptor is complete before the tail is
 * written, and that a status byte is read before the buffer it covers. */
static inline void wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
static inline void rmb(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }

/* --- receive ---------------------------------------------------------------- */

/* Give descriptor `i` a fresh cluster. The replacement is allocated
 * before the received frame is handed up, so a shortage drops the frame
 * and recycles its buffer rather than leaving the device a descriptor
 * with nowhere to write (design.md, "Rings"). */
static bool rx_arm(struct e1000e *e, unsigned i, struct mbuf *m)
{
    m->data = m->buf;
    dma_addr_t dma = dma_map(&e->pdev->dev, m->data, MCLBYTES, DMA_FROM_DEVICE);
    if (dma == 0)
        return false;
    m->pkt.dma = dma;
    e->rx_bufs[i] = m;
    e->rxd[i].addr = dma;
    e->rxd[i].length = 0;
    e->rxd[i].csum = 0;
    e->rxd[i].status = 0;
    e->rxd[i].errors = 0;
    e->rxd[i].special = 0;
    return true;
}

/* e->lock held. */
static void rx_process(struct e1000e *e)
{
    bool any = false;
    for (;;) {
        unsigned i = e->rx_head;
        volatile struct e1000_rx_desc *d = &e->rxd[i];
        if (!(d->status & E1000_RXD_STAT_DD))
            break;
        rmb();
        struct mbuf *m = e->rx_bufs[i];
        uint16_t len = d->length;
        uint8_t status = d->status, errors = d->errors;

        struct mbuf *fresh = m_getcl();
        if (fresh == NULL || !rx_arm(e, i, fresh)) {
            /*
             * No replacement: the frame is lost and its buffer serves
             * again -- *as it is*. It stays mapped and the descriptor
             * keeps its address, so there is no unmap-and-remap that
             * could fail and leave the device a descriptor pointing at
             * freed memory. Only the status has to be cleared. (rx_arm
             * touches the descriptor only after its map succeeded, so a
             * failed one has left it untouched.)
             */
            if (fresh)
                m_freem(fresh);
            __atomic_fetch_add(&e->nif.stats.rx_dropped, 1, __ATOMIC_RELAXED);
            d->length = 0;
            d->status = 0;
            d->errors = 0;
        } else {
            dma_unmap(&e->pdev->dev, m->pkt.dma, MCLBYTES, DMA_FROM_DEVICE);
            m->pkt.dma = 0;
            if (errors != 0 || !(status & E1000_RXD_STAT_EOP) || len < ETH_HLEN || len > MCLBYTES) {
                /* A frame the hardware already reported as damaged, or
                 * one split across descriptors (never, at 2 KiB buffers
                 * and a 1500 MTU): not handed up (E1). */
                if (errors)
                    __atomic_fetch_add(&e->nif.stats.rx_errors, 1, __ATOMIC_RELAXED);
                else
                    __atomic_fetch_add(&e->nif.stats.rx_dropped, 1, __ATOMIC_RELAXED);
                m_freem(m);
            } else {
                /* netif_rx counts the packet and its bytes; the driver
                 * counts only what the layer cannot see -- the drops and
                 * errors above. The first version counted both here and
                 * there, and the NIC-path benchmark, with virtio-net's
                 * figures beside it, showed every number doubled. */
                m->len = m->pkt.len = len;
                netif_rx(&e->nif, m);
            }
        }
        e->rx_head = (i + 1) % E1000E_RING;
        any = true;
    }
    if (any) {
        /* The tail is the last descriptor software has filled; hardware
         * stops one short of it, so the ring is never fully owned by
         * the device. */
        wmb();
        wr32(e, E1000_RDT0, (e->rx_head + E1000E_RING - 1) % E1000E_RING);
    }
}

/* --- transmit --------------------------------------------------------------- */

/* Reclaim completed descriptors, oldest first. e->lock held. */
static void tx_reclaim(struct e1000e *e)
{
    while (e->tx_used > 0) {
        unsigned i = e->tx_head;
        volatile struct e1000_tx_desc *d = &e->txd[i];
        if (!(d->status & E1000_TXD_STAT_DD))
            break;
        rmb();
        dma_unmap(&e->pdev->dev, d->addr, d->length, DMA_TO_DEVICE);
        if (e->tx_bufs[i] != NULL) {
            m_freem(e->tx_bufs[i]);   /* the whole chain, once its last segment is done */
            e->tx_bufs[i] = NULL;
        }
        d->status = 0;
        e->tx_head = (i + 1) % E1000E_RING;
        e->tx_used--;
    }
}

/* Drop everything outstanding and start the ring over. e->lock held,
 * TCTL.EN clear. The watchdog's recovery and remove's teardown. */
static void tx_drain(struct e1000e *e)
{
    while (e->tx_used > 0) {
        unsigned i = e->tx_head;
        dma_unmap(&e->pdev->dev, e->txd[i].addr, e->txd[i].length, DMA_TO_DEVICE);
        if (e->tx_bufs[i] != NULL) {
            m_freem(e->tx_bufs[i]);
            e->tx_bufs[i] = NULL;
            __atomic_fetch_add(&e->nif.stats.tx_dropped, 1, __ATOMIC_RELAXED);   /* counted as sent by netif, then thrown away here */
        }
        e->tx_head = (i + 1) % E1000E_RING;
        e->tx_used--;
    }
    memset(e->txd, 0, E1000E_RING * sizeof(*e->txd));
    e->tx_head = e->tx_tail = 0;
}

static void tx_ring_program(struct e1000e *e)
{
    wr32(e, E1000_TDBAL0, (uint32_t)e->txd_dma);
    wr32(e, E1000_TDBAH0, (uint32_t)(e->txd_dma >> 32));
    wr32(e, E1000_TDLEN0, E1000E_RING * sizeof(struct e1000_tx_desc));
    wr32(e, E1000_TDH0, 0);
    wr32(e, E1000_TDT0, 0);
    wr32(e, E1000_TIPG, E1000_TIPG_DEFAULT);
    wr32(e, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD);
}

/*
 * Takes the packet (consumed on every path, as netif_ops says). Runs
 * inside a read-side section and must not sleep; the spinlock is the
 * only wait. A chain that needs more descriptors than are free is
 * refused rather than queued (E2).
 */
static int e1000e_transmit(struct netif *nif, struct mbuf *m)
{
    struct e1000e *e = nif->priv;
    unsigned nsegs = 0;
    for (struct mbuf *b = m; b; b = b->next)
        if (b->len > 0)
            nsegs++;
    if (nsegs == 0) {
        m_freem(m);
        return -EINVAL;
    }

    arch_irq_state_t s = spin_lock_irqsave(&e->lock);
    tx_reclaim(e);   /* free what has completed before deciding there is no room */
    if (e->tx_used + nsegs > E1000E_RING - 1) {
        spin_unlock_irqrestore(&e->lock, s);
        m_freem(m);
        return -ENOBUFS;   /* netif_transmit counts an error return as tx_errors */
    }

    unsigned first = e->tx_tail, i = first, mapped = 0;
    struct mbuf *b = m;
    for (; b; b = b->next) {
        if (b->len == 0)
            continue;
        dma_addr_t dma = dma_map(&e->pdev->dev, b->data, b->len, DMA_TO_DEVICE);
        if (dma == 0)
            break;
        e->txd[i].addr = dma;
        e->txd[i].length = (uint16_t)b->len;
        e->txd[i].cso = 0;
        e->txd[i].cmd = E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
        e->txd[i].status = 0;
        e->txd[i].css = 0;
        e->txd[i].special = 0;
        e->tx_bufs[i] = NULL;
        mapped++;
        i = (i + 1) % E1000E_RING;
    }
    if (mapped != nsegs) {
        /* A segment that cannot be mapped: undo what was, refuse. */
        for (unsigned k = first, n = 0; n < mapped; n++, k = (k + 1) % E1000E_RING)
            dma_unmap(&e->pdev->dev, e->txd[k].addr, e->txd[k].length, DMA_TO_DEVICE);
        spin_unlock_irqrestore(&e->lock, s);
        m_freem(m);
        return -EINVAL;
    }
    unsigned last = (i + E1000E_RING - 1) % E1000E_RING;
    e->txd[last].cmd |= E1000_TXD_CMD_EOP;
    e->tx_bufs[last] = m;   /* freed when its last descriptor completes */
    e->tx_tail = i;
    e->tx_used += nsegs;
    wmb();
    wr32(e, E1000_TDT0, e->tx_tail);
    spin_unlock_irqrestore(&e->lock, s);
    return 0;
}

/* --- interrupts and the watchdog ------------------------------------------- */

static void e1000e_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct e1000e *e = arg;
    uint32_t icr = rd32(e, E1000_ICR);   /* reading clears it */

    arch_irq_state_t s = spin_lock_irqsave(&e->lock);
    /* Both rings, whatever the cause bits say (E5): the legacy and the
     * queue-mapped causes differ and a model raises one or the other. */
    rx_process(e);
    tx_reclaim(e);
    bool link = (rd32(e, E1000_STATUS) & E1000_STATUS_LU) != 0;
    bool changed = (icr & E1000_ICR_LSC) && link != e->link;
    if (changed)
        e->link = link;
    spin_unlock_irqrestore(&e->lock, s);

    if (changed)
        kinfo("e1000e: %s: link %s", e->nif.name, link ? "up" : "down");
}

/*
 * Once a second. Transmit descriptors outstanding while TDH has not
 * moved for E1000E_HUNG_TICKS is a hung transmitter: say so, drop what
 * is outstanding, start the ring again (E4). Bounded and repeatable --
 * a device that keeps hanging keeps being reset and keeps saying so.
 */
static void e1000e_watchdog(struct timer *t, void *arg)
{
    struct e1000e *e = arg;
    unsigned outstanding = 0;
    uint32_t tdh = 0;
    bool reset = false;

    arch_irq_state_t s = spin_lock_irqsave(&e->lock);
    tx_reclaim(e);
    if (e->tx_used > 0) {
        tdh = rd32(e, E1000_TDH0);
        if (tdh == e->last_tdh)
            e->stuck_ticks++;
        else
            e->stuck_ticks = 0;
        e->last_tdh = tdh;
        if (e->stuck_ticks >= E1000E_HUNG_TICKS) {
            outstanding = e->tx_used;
            wr32(e, E1000_TCTL, rd32(e, E1000_TCTL) & ~E1000_TCTL_EN);
            tx_drain(e);
            tx_ring_program(e);
            e->stuck_ticks = 0;
            e->resets++;
            reset = true;
        }
    } else {
        e->stuck_ticks = 0;
    }
    spin_unlock_irqrestore(&e->lock, s);

    if (reset)
        kwarn("e1000e: %s: transmit hung (%u outstanding, TDH %u for %u s); resetting the ring", e->nif.name,
              outstanding, tdh, E1000E_HUNG_TICKS);
    timer_start(t, E1000E_WATCHDOG_NS);
}

/* --- bring-up ----------------------------------------------------------------- */

static int rings_alloc(struct e1000e *e)
{
    e->rxd = dma_alloc(&e->pdev->dev, PAGE_SIZE, &e->rxd_dma, DMA_ZERO);
    e->txd = dma_alloc(&e->pdev->dev, PAGE_SIZE, &e->txd_dma, DMA_ZERO);
    if (e->rxd == NULL || e->txd == NULL)
        return -ENOMEM;
    for (unsigned i = 0; i < E1000E_RING; i++) {
        struct mbuf *m = m_getcl();
        if (m == NULL || !rx_arm(e, i, m)) {
            if (m)
                m_freem(m);
            return -ENOMEM;
        }
    }
    return 0;
}

static void rings_free(struct e1000e *e)
{
    for (unsigned i = 0; i < E1000E_RING; i++) {
        struct mbuf *m = e->rx_bufs[i];
        if (m != NULL) {
            if (m->pkt.dma)
                dma_unmap(&e->pdev->dev, m->pkt.dma, MCLBYTES, DMA_FROM_DEVICE);
            m_freem(m);
            e->rx_bufs[i] = NULL;
        }
    }
    if (e->txd)
        tx_drain(e);
    if (e->rxd)
        dma_free(&e->pdev->dev, PAGE_SIZE, e->rxd, e->rxd_dma);
    if (e->txd)
        dma_free(&e->pdev->dev, PAGE_SIZE, e->txd, e->txd_dma);
    e->rxd = NULL;
    e->txd = NULL;
}

/* The first ethN nobody has. netif_register refuses a duplicate, and
 * this driver never asks for one. */
static void pick_name(struct netif *nif)
{
    for (unsigned i = 0; i < 16; i++) {
        ksnprintf(nif->name, sizeof(nif->name), "eth%u", i);
        struct netif *taken = netif_find(nif->name);
        if (taken == NULL)
            return;
        netif_put(taken);
    }
}

static void e1000e_release(struct netif *nif)
{
    kfree(nif->priv);   /* the last holder: a queued packet or a lookup may outlive remove */
}

static const struct netif_ops e1000e_ops = { .transmit = e1000e_transmit, .release = e1000e_release };

static void hw_quiesce(struct e1000e *e)
{
    wr32(e, E1000_IMC, 0xffffffffu);
    (void)rd32(e, E1000_ICR);
    wr32(e, E1000_RCTL, 0);
    wr32(e, E1000_TCTL, 0);
}

static int e1000e_probe(struct pci_device *pdev, const struct pci_id *id)
{
    (void)id;
    struct e1000e *e = kzalloc(sizeof(*e));
    if (e == NULL)
        return -ENOMEM;
    e->pdev = pdev;
    e->vector = -1;
    spinlock_init(&e->lock, "e1000e");
    timer_setup(&e->watchdog, e1000e_watchdog, e);

    pci_enable_device(pdev, true);
    dma_set_mask(&pdev->dev, 64);
    e->bar = pci_map_bar(pdev, 0);
    int rc = -EIO;
    if (e->bar == 0) {
        kerror("e1000e: %s: cannot map BAR0", pdev->dev.name);
        goto fail_free;
    }

    /* Quiet, reset, quiet again: reset clears IMS, but a cause raised
     * during setup would reach a handler with no rings (design.md). */
    hw_quiesce(e);
    wr32(e, E1000_CTRL, rd32(e, E1000_CTRL) | E1000_CTRL_RST);
    thread_sleep_ms(10);
    hw_quiesce(e);

    uint32_t rah = rd32(e, E1000_RAH0);
    if (!(rah & E1000_RAH_AV)) {
        kerror("e1000e: %s: no station address (RAH0 0x%08x)", pdev->dev.name, rah);
        goto fail_unmap;
    }
    uint32_t ral = rd32(e, E1000_RAL0);
    for (unsigned i = 0; i < 4; i++)
        e->nif.mac[i] = (uint8_t)(ral >> (8 * i));
    e->nif.mac[4] = (uint8_t)rah;
    e->nif.mac[5] = (uint8_t)(rah >> 8);

    for (unsigned i = 0; i < 128; i++)
        wr32(e, E1000_MTA + 4 * i, 0);

    rc = rings_alloc(e);
    if (rc) {
        kerror("e1000e: %s: cannot allocate the rings (%d)", pdev->dev.name, rc);
        goto fail_rings;
    }

    /* Interrupts: MSI-X entry 0 for every cause, on CPU 0; MSI if the
     * function has no MSI-X. */
    int granted = pci_msix_enable(pdev, 1);
    if (granted >= 1) {
        e->vector = pci_msix_request(pdev, 0, e1000e_irq, e, "e1000e", 0);
        if (e->vector < 0) {
            pci_msix_disable(pdev);
            rc = e->vector;
            kerror("e1000e: %s: MSI-X vector (%d)", pdev->dev.name, rc);
            goto fail_rings;
        }
        e->msix = true;
        wr32(e, E1000_IVAR, E1000_IVAR_RXQ0(0) | E1000_IVAR_TXQ0(0) | E1000_IVAR_OTHER(0));
    } else {
        e->vector = pci_msi_enable(pdev, e1000e_irq, e, "e1000e", 0);
        if (e->vector < 0) {
            rc = e->vector;
            kerror("e1000e: %s: neither MSI-X nor MSI (%d)", pdev->dev.name, rc);
            goto fail_rings;
        }
    }

    /* Receive ring, then transmit ring, then the link. */
    wr32(e, E1000_RDBAL0, (uint32_t)e->rxd_dma);
    wr32(e, E1000_RDBAH0, (uint32_t)(e->rxd_dma >> 32));
    wr32(e, E1000_RDLEN0, E1000E_RING * sizeof(struct e1000_rx_desc));
    wr32(e, E1000_RDH0, 0);
    wr32(e, E1000_RDT0, E1000E_RING - 1);
    wr32(e, E1000_RXCSUM, 0);   /* no offload claimed: see design.md, "Offloads" */
    wr32(e, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);
    tx_ring_program(e);

    wr32(e, E1000_CTRL, rd32(e, E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);
    for (unsigned waited = 0; waited < E1000E_LINK_WAIT_MS; waited += 10) {
        if (rd32(e, E1000_STATUS) & E1000_STATUS_LU)
            break;
        thread_sleep_ms(10);
    }
    e->link = (rd32(e, E1000_STATUS) & E1000_STATUS_LU) != 0;

    pick_name(&e->nif);
    e->nif.mtu = 1500;
    e->nif.ops = &e1000e_ops;
    e->nif.priv = e;
    e->nif.flags = 0;
    e->nif.caps = 0;   /* offloads are measured before they are claimed */
    rc = netif_register(&e->nif);
    if (rc) {
        kerror("e1000e: %s: netif_register (%d)", pdev->dev.name, rc);
        goto fail_irq;
    }
    pdev->dev.drvdata = e;
    wr32(e, E1000_IMS, E1000_IMS_ALL);
    netif_set_up(&e->nif, true);
    timer_start(&e->watchdog, E1000E_WATCHDOG_NS);
    kinfo("e1000e: %s is %s (%02x:%02x:%02x:%02x:%02x:%02x, link %s)", pdev->dev.name, e->nif.name, e->nif.mac[0],
          e->nif.mac[1], e->nif.mac[2], e->nif.mac[3], e->nif.mac[4], e->nif.mac[5], e->link ? "up" : "down");
    return 0;

fail_irq:
    hw_quiesce(e);
    if (e->msix)
        pci_msix_disable(pdev);
    else
        pci_msi_disable(pdev);
    if (e->vector >= 0)
        synchronize_irq((unsigned)e->vector);
fail_rings:
    hw_quiesce(e);
    rings_free(e);
fail_unmap:
    device_unmap_mmio(e->bar);
fail_free:
    kfree(e);
    return rc;
}

static void e1000e_remove(struct pci_device *pdev)
{
    struct e1000e *e = pdev->dev.drvdata;
    if (e == NULL)
        return;
    netif_unregister(&e->nif);   /* no transmit or receive reaches the rings after this */
    timer_cancel_sync(&e->watchdog);
    hw_quiesce(e);
    int vector = e->vector;
    if (e->msix)
        pci_msix_disable(pdev);
    else
        pci_msi_disable(pdev);
    if (vector >= 0)
        synchronize_irq((unsigned)vector);
    rings_free(e);
    device_unmap_mmio(e->bar);
    pdev->dev.drvdata = NULL;
    netif_put(&e->nif);   /* the creator's reference; e1000e_release frees e when the holders are gone */
}

static const struct pci_id e1000e_ids[] = {
    { E1000E_VENDOR, E1000E_82574L, 0, 0, 0 },
    PCI_ID_END,
};

static struct pci_driver e1000e_driver = {
    .drv = { .name = "e1000e" },
    .ids = e1000e_ids,
    .probe = e1000e_probe,
    .remove = e1000e_remove,
};

static int e1000e_module_init(void)
{
    return pci_register_driver(&e1000e_driver);
}

static void e1000e_module_shutdown(void)
{
    pci_unregister_driver(&e1000e_driver);
}

COSMO_MODULE("e1000e", "1.0", e1000e_module_init, e1000e_module_shutdown, "", MODULE_CAP_DRIVER);
