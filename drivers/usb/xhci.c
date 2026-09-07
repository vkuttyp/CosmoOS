/*
 * xhci.c - eXtensible Host Controller Interface driver
 * (docs/drivers/usb/design.md, "xHCI"; invariants U1-U7).
 *
 * The first bus here whose devices arrive after boot, have a parent
 * that is a device, and leave with I/O in flight. This file owns the
 * controller: rings, contexts, commands, the event ring, the root-hub
 * ports. The USB core (usb.c) owns what a device *is*. Written to xHCI
 * 1.2; the paths only QEMU has exercised are listed in testing.md.
 *
 * One interrupter, one MSI-X vector on CPU 0, one command in flight,
 * one worker thread per controller for the ports. Every DMA goes through
 * the controller's PCI device (U1).
 */

#include <kernel/device.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/mutex.h>
#include <kernel/page.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <drivers/pci.h>
#include <drivers/usb.h>

#include "xhci.h"

#define XHCI_RING_TRBS    256u                     /* one page; the last is the link */
#define XHCI_RING_LAST    (XHCI_RING_TRBS - 1)     /* index of the link TRB */
#define XHCI_TD_MAX       64u                      /* TRBs per TD: what a 64 KiB command needs, and then some */
#define XHCI_TRB_MAX_LEN  65536u                   /* a TRB's buffer never crosses a 64 KiB boundary (§4.11.7) */
#define XHCI_CMD_TIMEOUT_NS  (1000ull * 1000 * 1000)
#define XHCI_RESET_TIMEOUT_MS 1000u
#define XHCI_PORT_RESET_MS    500u
#define XHCI_MAX_SLOTS    255u
#define XHCI_MAX_DCI      31u

struct xhci_td {
    unsigned first, last, count;   /* TRB indices (link excluded) and TRBs used */
    enum dma_dir dir;
    uint32_t total;                /* data bytes the TD asks for */
    uint32_t moved;                /* what a short-packet or error event said was moved (valid with `cut`) */
    bool cut;                      /* the TD ended early: `moved` is the answer, not `total` */
    bool nomap;                    /* debug_dma: the address was never mapped, so it is never unmapped */
};

struct xhci_ring {
    struct xhci_trb *trbs;
    dma_addr_t dma;
    unsigned enq, deq;             /* 0 .. XHCI_RING_LAST - 1 */
    bool cycle;                    /* producer cycle state */
    unsigned used;                 /* TRBs in flight */
    struct usb_request *req[XHCI_RING_TRBS];   /* the request owning each TRB */
    struct xhci_td td[XHCI_RING_TRBS];         /* by the TD's first TRB */
};

struct xhci_ep {
    struct xhci_ring *ring;        /* NULL: not configured */
    unsigned mps;
    bool halted;
};

/* udev->hcd_priv */
struct xhci_dev {
    void *ctx;                     /* the device context: 32 entries */
    dma_addr_t ctx_dma;
    void *input;                   /* the input context: 33 entries */
    dma_addr_t input_dma;
    struct xhci_ep ep[XHCI_MAX_DCI + 1];   /* by DCI */
};

struct xhci_erst_entry {
    uint64_t base;
    uint32_t size;
    uint32_t rsvd;
} __packed;

struct xhci {
    struct pci_device *pdev;
    vaddr_t bar, op, rt, db;
    unsigned csz;                  /* context size: 32 or 64 */
    unsigned max_slots, nr_ports, hw_page;
    bool ac64;

    uint64_t *dcbaa;
    dma_addr_t dcbaa_dma;
    uint64_t *scratch_arr;
    dma_addr_t scratch_arr_dma;
    void **scratch_va;
    dma_addr_t *scratch_dma;
    unsigned nr_scratch;

    struct xhci_ring *cmd;
    struct mutex cmd_lock;         /* one command in flight */
    struct {
        struct completion done;
        dma_addr_t trb;            /* the command TRB awaited, 0 for none */
        uint32_t status, control;  /* the completion event's */
    } cmdw;

    struct xhci_trb *evt;
    dma_addr_t evt_dma;
    unsigned evt_deq;
    bool evt_cycle;
    struct xhci_erst_entry *erst;
    dma_addr_t erst_dma;

    struct usb_device *slot_dev[XHCI_MAX_SLOTS + 1];
    spinlock_t lock;               /* rings, req/td tables, slot_dev, cmdw */

    int vector;
    bool msix;
    bool dead;
    uint64_t events, transfers, commands, errors;

    struct thread *worker;
    struct waitqueue wq;
    bool port_change, stop;
    bool port_failed[USB_MAX_PORTS + 1];   /* enumeration failed; wait for a connect change */
    struct completion first_scan;          /* the worker's first pass over the ports is done */
    struct list_node link;                 /* g_controllers */

    struct usb_hcd hcd;
};

/* Every controller probed, so the module's init can wait for each one's
 * first port scan: the boot's devices exist before the next module
 * loads and before the self-tests run. */
static LIST_HEAD(g_controllers);
static struct mutex g_controllers_lock;

static uint32_t rd32(vaddr_t addr) { return *(volatile uint32_t *)addr; }
static void wr32(vaddr_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }
static void wr64(vaddr_t addr, uint64_t v)
{
    /* Two dwords, low first: every 64-bit register here accepts it
     * (§5.1), and it is what a 32-bit build would have to do. */
    wr32(addr, (uint32_t)v);
    wr32(addr + 4, (uint32_t)(v >> 32));
}
static inline void wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
static inline void rmb(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }

static struct xhci *hcd_to_xhci(struct usb_hcd *hcd) { return hcd->priv; }

/* --- rings ------------------------------------------------------------------------ */

static struct xhci_ring *ring_alloc(struct xhci *x)
{
    struct xhci_ring *r = kzalloc(sizeof(*r));
    if (r == NULL)
        return NULL;
    r->trbs = dma_alloc(&x->pdev->dev, PAGE_SIZE, &r->dma, DMA_ZERO);
    if (r->trbs == NULL) {
        kfree(r);
        return NULL;
    }
    /* The link back to the start, with Toggle Cycle: the ring's cycle
     * state flips every lap. Its cycle bit is set as the producer passes. */
    r->trbs[XHCI_RING_LAST].ptr = r->dma;
    r->trbs[XHCI_RING_LAST].status = 0;
    r->trbs[XHCI_RING_LAST].control = TRB_TYPE(TRB_LINK) | TRB_TC;
    r->cycle = true;
    return r;
}

static void ring_free(struct xhci *x, struct xhci_ring *r)
{
    if (r == NULL)
        return;
    dma_free(&x->pdev->dev, PAGE_SIZE, r->trbs, r->dma);
    kfree(r);
}

static inline unsigned ring_next(unsigned i) { return i + 1 == XHCI_RING_LAST ? 0 : i + 1; }
static inline unsigned ring_space(const struct xhci_ring *r) { return XHCI_RING_LAST - 1 - r->used; }
static inline dma_addr_t ring_trb_dma(const struct xhci_ring *r, unsigned i) { return r->dma + (dma_addr_t)i * sizeof(struct xhci_trb); }

/*
 * Write the next TRB. The first TRB of a TD is written with its cycle
 * bit inverted so the controller, which may be running the ring, cannot
 * start the TD before it is whole; ring_commit flips it (§4.9.2). x->lock
 * held. Returns the index.
 */
static unsigned ring_put(struct xhci_ring *r, uint64_t ptr, uint32_t status, uint32_t control, bool first,
                         bool *first_cycle)
{
    if (r->enq == XHCI_RING_LAST) {
        /* Hand the link to the controller with the current cycle, then
         * flip the producer's for the next lap. */
        volatile struct xhci_trb *l = &r->trbs[XHCI_RING_LAST];
        uint32_t c = (l->control & ~TRB_CYCLE) | (r->cycle ? TRB_CYCLE : 0);
        wmb();
        l->control = c;
        r->cycle = !r->cycle;
        r->enq = 0;
    }
    unsigned i = r->enq;
    volatile struct xhci_trb *t = &r->trbs[i];
    bool cycle = r->cycle;
    if (first) {
        *first_cycle = cycle;
        cycle = !cycle;
    }
    t->ptr = ptr;
    t->status = status;
    wmb();
    t->control = (control & ~TRB_CYCLE) | (cycle ? TRB_CYCLE : 0);
    r->enq = i + 1;   /* == XHCI_RING_LAST: the link is taken by the next put */
    r->used++;
    return i;
}

static void ring_commit(struct xhci_ring *r, unsigned first, bool first_cycle)
{
    volatile struct xhci_trb *t = &r->trbs[first];
    uint32_t c = (t->control & ~TRB_CYCLE) | (first_cycle ? TRB_CYCLE : 0);
    wmb();
    t->control = c;
    wmb();
}

/* The address the controller should resume from: the enqueue point,
 * with the cycle it will find there (for Set TR Dequeue Pointer). */
static uint64_t ring_enqueue_ptr(const struct xhci_ring *r)
{
    unsigned i = r->enq;
    bool cycle = r->cycle;
    if (i == XHCI_RING_LAST) {
        i = 0;
        cycle = !cycle;
    }
    return ring_trb_dma(r, i) | (cycle ? EP_DCS : 0);
}

/* --- the controller's tables ----------------------------------------------------- */

static inline void *dev_ctx(struct xhci *x, struct xhci_dev *d, unsigned dci) { return (uint8_t *)d->ctx + (size_t)dci * x->csz; }
static inline void *in_ctrl(struct xhci_dev *d) { return d->input; }
static inline void *in_ctx(struct xhci *x, struct xhci_dev *d, unsigned dci) { return (uint8_t *)d->input + (size_t)(dci + 1) * x->csz; }

/* --- commands ----------------------------------------------------------------------- */

/*
 * One command at a time, waited for on a completion with a bound. A
 * command that never completes makes the controller dead: its state is
 * unknown, and a driver that keeps issuing commands to a controller in
 * an unknown state is guessing (U6). Returns the completion code or a
 * negative errno; *slot_out gets the event's slot id.
 */
static int xhci_cmd(struct xhci *x, uint64_t ptr, uint32_t control, unsigned *slot_out)
{
    if (x->dead)
        return -EIO;
    mutex_lock(&x->cmd_lock);
    completion_init(&x->cmdw.done, "xhci-cmd");
    bool first_cycle;
    arch_irq_state_t s = spin_lock_irqsave(&x->lock);
    unsigned i = ring_put(x->cmd, ptr, 0, control, true, &first_cycle);
    x->cmdw.trb = ring_trb_dma(x->cmd, i);
    x->cmdw.status = 0;
    ring_commit(x->cmd, i, first_cycle);
    x->commands++;
    spin_unlock_irqrestore(&x->lock, s);
    wr32(x->db, 0);

    uint64_t deadline = clock_now_ns() + XHCI_CMD_TIMEOUT_NS;
    while (!completion_done(&x->cmdw.done) && clock_now_ns() < deadline)
        thread_sleep_ns(100000);
    if (completion_done(&x->cmdw.done))
        wait_for_completion(&x->cmdw.done);   /* the handshake: complete() has let go before the next init */
    int rc;
    s = spin_lock_irqsave(&x->lock);
    if (!completion_done(&x->cmdw.done)) {
        x->cmdw.trb = 0;   /* a late event finds nobody waiting */
        x->dead = true;
        x->hcd.dead = true;
        rc = -ETIMEDOUT;
    } else {
        rc = (int)TRB_CC_OF(x->cmdw.status);
        if (slot_out)
            *slot_out = TRB_SLOT_OF(x->cmdw.control);
    }
    /* The command ring is consumed in order; the TRB is the controller's
     * no longer either way. */
    x->cmd->used--;
    x->cmd->deq = ring_next(x->cmd->deq);
    spin_unlock_irqrestore(&x->lock, s);
    mutex_unlock(&x->cmd_lock);
    if (rc == -ETIMEDOUT)
        kerror("xhci%u: command type %u did not complete in %llu ms; the controller is dead", x->hcd.index,
               TRB_TYPE_OF(control), (unsigned long long)(XHCI_CMD_TIMEOUT_NS / 1000000));
    return rc;
}

static int cc_to_errno(unsigned cc)
{
    switch (cc) {
    case CC_SUCCESS:
    case CC_SHORT_PACKET:   return 0;
    case CC_STALL:          return -EPIPE;
    case CC_BABBLE:         return -EOVERFLOW;
    case CC_USB_TRANSACTION:
    case CC_DATA_BUFFER_ERR: return -EIO;
    case CC_NO_SLOTS:
    case CC_RESOURCE:
    case CC_BANDWIDTH:      return -ENOSPC;
    case CC_PARAMETER:
    case CC_TRB_ERROR:
    case CC_CONTEXT_STATE:  return -EINVAL;
    default:                return -EIO;
    }
}

/* A command's completion code as an errno, logging what the controller said. */
static int cmd_result(struct xhci *x, const char *what, int cc)
{
    if (cc < 0)
        return cc;
    if (cc == CC_SUCCESS)
        return 0;
    kerror("xhci%u: %s: completion code %d", x->hcd.index, what, cc);
    return cc_to_errno((unsigned)cc);
}

/* --- transfers -------------------------------------------------------------------- */

struct xhci_seg {
    dma_addr_t dma;
    uint32_t len;
};

/* Map one physically contiguous buffer, cut at 64 KiB boundaries. */
static int map_segment(struct xhci *x, const void *buf, uint32_t len, enum dma_dir dir, struct xhci_seg *segs,
                       unsigned *n)
{
    const uint8_t *p = buf;
    while (len > 0) {
        if (*n >= XHCI_TD_MAX)
            return -EINVAL;
        uint32_t chunk = len;
        uintptr_t off = (uintptr_t)p & (XHCI_TRB_MAX_LEN - 1);
        if (off + chunk > XHCI_TRB_MAX_LEN)
            chunk = XHCI_TRB_MAX_LEN - (uint32_t)off;
        dma_addr_t d = dma_map(&x->pdev->dev, p, chunk, dir);
        if (d == 0)
            return -EINVAL;
        segs[*n].dma = d;
        segs[*n].len = chunk;
        (*n)++;
        p += chunk;
        len -= chunk;
    }
    return 0;
}

static void unmap_segments(struct xhci *x, const struct xhci_seg *segs, unsigned n, enum dma_dir dir)
{
    for (unsigned i = 0; i < n; i++)
        dma_unmap(&x->pdev->dev, segs[i].dma, segs[i].len, dir);
}

/* Unmap a completed TD's data TRBs by reading them back: the TRB holds
 * the address and length the controller was given, so nothing else has
 * to remember them. x->lock held. */
static void td_unmap(struct xhci *x, struct xhci_ring *r, const struct xhci_td *td)
{
    if (td->nomap)
        return;
    unsigned i = td->first;
    for (unsigned n = 0; n < td->count; n++, i = ring_next(i)) {
        const struct xhci_trb *t = &r->trbs[i];
        unsigned type = TRB_TYPE_OF(t->control);
        if (type == TRB_NORMAL || type == TRB_DATA)
            dma_unmap(&x->pdev->dev, t->ptr, t->status & 0x1ffffu, td->dir);
    }
}

/* Data bytes in the TD's TRBs before index `idx`. x->lock held. */
static uint32_t td_bytes_before(const struct xhci_ring *r, const struct xhci_td *td, unsigned idx)
{
    uint32_t sum = 0;
    unsigned i = td->first;
    for (unsigned n = 0; n < td->count && i != idx; n++, i = ring_next(i)) {
        const struct xhci_trb *t = &r->trbs[i];
        unsigned type = TRB_TYPE_OF(t->control);
        if (type == TRB_NORMAL || type == TRB_DATA)
            sum += t->status & 0x1ffffu;
    }
    return sum;
}

/* Retire a TD: forget its TRBs, advance the dequeue point. x->lock held. */
static void td_retire(struct xhci_ring *r, const struct xhci_td *td)
{
    unsigned i = td->first;
    for (unsigned n = 0; n < td->count; n++, i = ring_next(i))
        r->req[i] = NULL;
    r->deq = ring_next(td->last);
    r->used -= td->count;
}

static struct xhci_ep *xhci_ep_of(struct usb_device *udev, uint8_t ep_addr)
{
    struct xhci_dev *d = udev->hcd_priv;
    if (d == NULL)
        return NULL;
    unsigned dci = xhci_dci(ep_addr);
    if (dci > XHCI_MAX_DCI || d->ep[dci].ring == NULL)
        return NULL;
    return &d->ep[dci];
}

/* TD Size (§4.11.2.4): packets left after this TRB, at most 31, 0 on the last. */
static uint32_t td_size(uint32_t remaining_after, unsigned mps, bool last)
{
    if (last || mps == 0)
        return 0;
    uint32_t packets = (remaining_after + mps - 1) / mps;
    return (packets > 31 ? 31 : packets) << 17;
}

static int xhci_submit(struct usb_hcd *hcd, struct usb_request *r)
{
    struct xhci *x = hcd_to_xhci(hcd);
    struct usb_device *udev = r->udev;
    struct xhci_ep *ep = xhci_ep_of(udev, r->ep);
    if (ep == NULL || udev->slot == 0)
        return -EINVAL;
    bool control = r->ep == 0;
    bool in = control ? (r->setup.bmRequestType & USB_DIR_IN) != 0 : (r->ep & USB_EP_DIR_IN) != 0;
    enum dma_dir dir = in ? DMA_FROM_DEVICE : DMA_TO_DEVICE;

    /* Map first, outside the lock: a segment that cannot be mapped means
     * nothing was written to the ring. */
    struct xhci_seg segs[XHCI_TD_MAX];
    unsigned nsegs = 0;
    uint32_t total = 0;
    int rc = 0;
    if (r->debug_dma != 0 && !control) {
        segs[0].dma = r->debug_dma;   /* tests only: the caller's address, as it is */
        segs[0].len = r->len;
        nsegs = 1;
        total = r->len;
    } else if (r->nr_sgs > 0 && !control) {
        for (unsigned i = 0; i < r->nr_sgs && rc == 0; i++) {
            rc = map_segment(x, r->sgs[i].buf, r->sgs[i].len, dir, segs, &nsegs);
            total += r->sgs[i].len;
        }
    } else if (r->len > 0) {
        rc = map_segment(x, r->buf, r->len, dir, segs, &nsegs);
        total = r->len;
    }
    bool nomap = r->debug_dma != 0 && !control;
    if (rc) {
        if (!nomap)
            unmap_segments(x, segs, nsegs, dir);
        return rc;
    }
    unsigned ntrbs = control ? 2 + nsegs : nsegs;
    if (ntrbs == 0) {
        return -EINVAL;
    }

    arch_irq_state_t s = spin_lock_irqsave(&x->lock);
    if (x->dead || __atomic_load_n(&udev->gone, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&x->lock, s);
        if (!nomap)
            unmap_segments(x, segs, nsegs, dir);
        return -ENODEV;
    }
    struct xhci_ring *ring = ep->ring;
    if (ring_space(ring) < ntrbs) {
        spin_unlock_irqrestore(&x->lock, s);
        if (!nomap)
            unmap_segments(x, segs, nsegs, dir);
        return -ENOSPC;
    }
    unsigned first = 0, last = 0;
    bool first_cycle = false;
    unsigned n = 0;
    if (control) {
        /* Setup (immediate data; TRT says which way data goes), Data
         * (ISP: a short answer is reported), Status (IOC; the direction
         * opposite to the data, IN when there is none). */
        uint64_t setup;
        memcpy(&setup, &r->setup, sizeof(setup));
        uint32_t trt = nsegs == 0 ? 0 : (in ? TRB_TRT_IN : TRB_TRT_OUT);
        first = ring_put(ring, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt, true, &first_cycle);
        n++;
        uint32_t remaining = total;
        for (unsigned i = 0; i < nsegs; i++) {
            remaining -= segs[i].len;
            bool last_seg = i + 1 == nsegs;
            uint32_t ctl = TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0) | (last_seg ? 0 : TRB_CH);
            ring_put(ring, segs[i].dma, segs[i].len | td_size(remaining, ep->mps, last_seg), ctl, false, NULL);
            n++;
        }
        bool status_in = nsegs == 0 || !in;
        last = ring_put(ring, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (status_in ? TRB_DIR_IN : 0), false, NULL);
        n++;
    } else {
        uint32_t remaining = total;
        for (unsigned i = 0; i < nsegs; i++) {
            remaining -= segs[i].len;
            bool last_seg = i + 1 == nsegs;
            uint32_t ctl = TRB_TYPE(TRB_NORMAL) | TRB_ISP | (last_seg ? TRB_IOC : TRB_CH);
            unsigned idx = ring_put(ring, segs[i].dma, segs[i].len | td_size(remaining, ep->mps, last_seg), ctl,
                                    i == 0, i == 0 ? &first_cycle : NULL);
            if (i == 0)
                first = idx;
            last = idx;
            n++;
        }
    }
    struct xhci_td *td = &ring->td[first];
    td->first = first;
    td->last = last;
    td->count = n;
    td->dir = dir;
    td->total = total;
    td->moved = 0;
    td->cut = false;
    td->nomap = nomap;
    for (unsigned i = first, k = 0; k < n; k++, i = ring_next(i))
        ring->req[i] = r;
    r->hcd_priv = td;
    ring_commit(ring, first, first_cycle);
    x->transfers++;
    spin_unlock_irqrestore(&x->lock, s);
    if (!r->debug_no_doorbell)
        wr32(x->db + 4 * udev->slot, xhci_dci(r->ep));
    return 0;
}

/*
 * A Transfer Event names a TRB; the ring's table names the request that
 * owns it. Bytes are summed from every event of the TD; the request
 * completes on the event for its last TRB (the controller generates one
 * after a short packet too, §4.10.1.1) or on the first error. Runs from
 * the interrupt handler; the completion callback runs after the lock is
 * dropped, because a callback may submit. Returns the request to
 * complete, or NULL.
 */
static struct usb_request *handle_transfer_event(struct xhci *x, const struct xhci_trb *ev, int *status_out)
{
    unsigned slot = TRB_SLOT_OF(ev->control), dci = TRB_EP_ID_OF(ev->control);
    unsigned cc = TRB_CC_OF(ev->status);
    uint32_t residual = TRB_RESIDUAL_OF(ev->status);
    if (slot == 0 || slot > x->max_slots || dci == 0 || dci > XHCI_MAX_DCI)
        return NULL;
    struct usb_device *udev = x->slot_dev[slot];
    struct xhci_dev *d = udev ? udev->hcd_priv : NULL;
    if (d == NULL || d->ep[dci].ring == NULL)
        return NULL;
    struct xhci_ring *ring = d->ep[dci].ring;
    if (ev->ptr < ring->dma || ev->ptr >= ring->dma + XHCI_RING_LAST * sizeof(struct xhci_trb))
        return NULL;   /* not one of ours, or the link TRB */
    unsigned idx = (unsigned)((ev->ptr - ring->dma) / sizeof(struct xhci_trb));
    struct usb_request *r = ring->req[idx];
    if (r == NULL)
        return NULL;   /* already retired: a cancel or an error took the TD */
    if (cc == CC_STOPPED || cc == CC_STOPPED_LEN_INV || cc == CC_STOPPED_SHORT)
        return NULL;   /* Stop Endpoint's report; the cancel path completes the request */
    struct xhci_td *td = r->hcd_priv;
    const struct xhci_trb *t = &ring->trbs[idx];
    unsigned type = TRB_TYPE_OF(t->control);
    int status = cc_to_errno(cc);
    /* A TRB that moved all its bytes generates no event unless it is
     * the last (IOC); only a short packet or an error says where the
     * transfer stopped. So the count is `total` unless an event cut it. */
    if ((type == TRB_NORMAL || type == TRB_DATA) && (cc == CC_SHORT_PACKET || status != 0) && !td->cut) {
        uint32_t len = t->status & 0x1ffffu;
        td->moved = td_bytes_before(ring, td, idx) + (len > residual ? len - residual : 0);
        td->cut = true;
    }
    if (status == 0 && idx != td->last)
        return NULL;   /* more of the TD to come */
    r->actual = td->cut ? td->moved : td->total;
    if (status != 0) {
        x->errors++;
        if (cc == CC_STALL || cc == CC_USB_TRANSACTION || cc == CC_BABBLE)
            d->ep[dci].halted = true;
        if (cc != CC_STALL && x->errors <= 8)
            kwarn("xhci%u: %s ep 0x%02x: completion code %u", x->hcd.index, udev->dev.name, r->ep, cc);
    }
    td_unmap(x, ring, td);
    td_retire(ring, td);
    r->hcd_priv = NULL;
    *status_out = status;
    return r;
}

/* Every request on a ring, completed with `status` (the first with
 * `first_status`). Thread context, the endpoint stopped; runs the
 * callbacks after the lock is dropped. */
static void ring_flush(struct xhci *x, struct xhci_ring *ring, struct usb_request *victim, int victim_status,
                       int status)
{
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&x->lock);
        struct usb_request *r = NULL;
        unsigned i = ring->deq;
        for (unsigned n = 0; n < XHCI_RING_LAST && r == NULL; n++, i = ring_next(i)) {
            if (ring->req[i] != NULL)
                r = ring->req[i];
        }
        if (r == NULL) {
            spin_unlock_irqrestore(&x->lock, s);
            return;
        }
        struct xhci_td *td = r->hcd_priv;
        td_unmap(x, ring, td);
        td_retire(ring, td);
        r->hcd_priv = NULL;
        spin_unlock_irqrestore(&x->lock, s);
        usb_request_complete(r, r == victim ? victim_status : status, r->actual);
    }
}

/* Stop the endpoint and move its dequeue pointer to the enqueue point:
 * nothing on the ring runs afterwards. Thread context. */
static int ep_stop_and_drain(struct xhci *x, struct usb_device *udev, unsigned dci)
{
    int cc = xhci_cmd(x, 0, TRB_TYPE(TRB_CMD_STOP_EP) | TRB_EP_ID(dci) | TRB_SLOT(udev->slot), NULL);
    /* Context State means the endpoint was not running (halted, or already
     * stopped): the drain below is still right. */
    if (cc < 0 || (cc != CC_SUCCESS && cc != CC_CONTEXT_STATE))
        return cmd_result(x, "stop endpoint", cc);
    struct xhci_dev *d = udev->hcd_priv;
    arch_irq_state_t s = spin_lock_irqsave(&x->lock);
    uint64_t deq = ring_enqueue_ptr(d->ep[dci].ring);
    spin_unlock_irqrestore(&x->lock, s);
    cc = xhci_cmd(x, deq, TRB_TYPE(TRB_CMD_SET_DEQ) | TRB_EP_ID(dci) | TRB_SLOT(udev->slot), NULL);
    return cmd_result(x, "set dequeue pointer", cc);
}

static int xhci_cancel(struct usb_hcd *hcd, struct usb_request *r, int status)
{
    struct xhci *x = hcd_to_xhci(hcd);
    struct usb_device *udev = r->udev;
    struct xhci_ep *ep = xhci_ep_of(udev, r->ep);
    if (ep == NULL)
        return -ENOENT;
    arch_irq_state_t s = spin_lock_irqsave(&x->lock);
    bool mine = r->hcd_priv != NULL && ep->ring->req[((struct xhci_td *)r->hcd_priv)->first] == r;
    spin_unlock_irqrestore(&x->lock, s);
    if (!mine)
        return -ENOENT;
    if (!x->dead) {
        int rc = ep_stop_and_drain(x, udev, xhci_dci(r->ep));
        if (rc && rc != -ETIMEDOUT)
            kwarn("xhci%u: %s: cancel on ep 0x%02x: %d", x->hcd.index, udev->dev.name, r->ep, rc);
    }
    /* The controller is stopped on this ring (or dead, which is the same
     * for the ring's purposes): whatever is left is ours to complete. */
    ring_flush(x, ep->ring, r, status, -ECANCELED);
    return 0;
}

static int xhci_reset_endpoint(struct usb_hcd *hcd, struct usb_device *udev, uint8_t ep_addr)
{
    struct xhci *x = hcd_to_xhci(hcd);
    struct xhci_ep *ep = xhci_ep_of(udev, ep_addr);
    if (ep == NULL)
        return -EINVAL;
    unsigned dci = xhci_dci(ep_addr);
    int cc = xhci_cmd(x, 0, TRB_TYPE(TRB_CMD_RESET_EP) | TRB_EP_ID(dci) | TRB_SLOT(udev->slot), NULL);
    if (cc == CC_CONTEXT_STATE) {
        /* Not halted after all (a recovery run on a healthy endpoint):
         * Set TR Dequeue needs the endpoint stopped, so stop it first --
         * the first version moved the dequeue pointer of a running
         * endpoint and the controller refused (usb-storage-timeout's
         * racing readers found it). */
        cc = xhci_cmd(x, 0, TRB_TYPE(TRB_CMD_STOP_EP) | TRB_EP_ID(dci) | TRB_SLOT(udev->slot), NULL);
        if (cc == CC_CONTEXT_STATE)
            cc = CC_SUCCESS;   /* already stopped */
    }
    if (cc < 0 || cc != CC_SUCCESS)
        return cmd_result(x, "reset endpoint", cc);
    struct xhci_dev *d = udev->hcd_priv;
    arch_irq_state_t s = spin_lock_irqsave(&x->lock);
    uint64_t deq = ring_enqueue_ptr(d->ep[dci].ring);
    spin_unlock_irqrestore(&x->lock, s);
    cc = xhci_cmd(x, deq, TRB_TYPE(TRB_CMD_SET_DEQ) | TRB_EP_ID(dci) | TRB_SLOT(udev->slot), NULL);
    int rc = cmd_result(x, "set dequeue pointer", cc);
    if (rc)
        return rc;
    ring_flush(x, ep->ring, NULL, 0, -ECANCELED);
    ep->halted = false;
    return 0;
}

/* --- slots and contexts ------------------------------------------------------------ */

static void xhci_dev_free(struct xhci *x, struct xhci_dev *d)
{
    for (unsigned dci = 1; dci <= XHCI_MAX_DCI; dci++)
        ring_free(x, d->ep[dci].ring);
    if (d->ctx)
        dma_free(&x->pdev->dev, PAGE_SIZE, d->ctx, d->ctx_dma);
    if (d->input)
        dma_free(&x->pdev->dev, PAGE_SIZE, d->input, d->input_dma);
    kfree(d);
}

static int xhci_enable_device(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct xhci *x = hcd_to_xhci(hcd);
    unsigned slot = 0;
    int cc = xhci_cmd(x, 0, TRB_TYPE(TRB_CMD_ENABLE_SLOT), &slot);
    int rc = cmd_result(x, "enable slot", cc);
    if (rc)
        return rc;
    if (slot == 0 || slot > x->max_slots) {
        kerror("xhci%u: enable slot returned slot %u", x->hcd.index, slot);
        return -EIO;
    }
    struct xhci_dev *d = kzalloc(sizeof(*d));
    if (d == NULL) {
        rc = -ENOMEM;
        goto fail_slot;
    }
    d->ctx = dma_alloc(&x->pdev->dev, PAGE_SIZE, &d->ctx_dma, DMA_ZERO);
    d->input = dma_alloc(&x->pdev->dev, PAGE_SIZE, &d->input_dma, DMA_ZERO);
    d->ep[1].ring = ring_alloc(x);
    d->ep[1].mps = usb_ep0_mps(udev->speed);
    if (d->ctx == NULL || d->input == NULL || d->ep[1].ring == NULL) {
        rc = -ENOMEM;
        goto fail_free;
    }

    /* Input: the slot context (speed, root port, one endpoint) and EP0. */
    struct xhci_input_ctrl_ctx *icc = in_ctrl(d);
    icc->drop = 0;
    icc->add = (1u << 0) | (1u << 1);
    struct xhci_slot_ctx *sc = in_ctx(x, d, 0);
    sc->dw[0] = SLOT_ENTRIES(1) | SLOT_SPEED(udev->speed);
    sc->dw[1] = SLOT_ROOT_PORT(udev->port);
    struct xhci_ep_ctx *ec = in_ctx(x, d, 1);
    ec->dw[1] = EP_TYPE(EP_TYPE_CONTROL) | EP_MPS(d->ep[1].mps) | EP_CERR(3);
    uint64_t deq = d->ep[1].ring->dma | EP_DCS;
    ec->dw[2] = (uint32_t)deq;
    ec->dw[3] = (uint32_t)(deq >> 32);
    ec->dw[4] = EP_AVG_TRB_LEN(8);

    arch_irq_state_t s = spin_lock_irqsave(&x->lock);
    x->dcbaa[slot] = d->ctx_dma;
    x->slot_dev[slot] = udev;
    udev->slot = slot;
    udev->hcd_priv = d;
    spin_unlock_irqrestore(&x->lock, s);
    wmb();

    cc = xhci_cmd(x, d->input_dma, TRB_TYPE(TRB_CMD_ADDRESS_DEV) | TRB_SLOT(slot), NULL);
    rc = cmd_result(x, "address device", cc);
    if (rc) {
        s = spin_lock_irqsave(&x->lock);
        x->dcbaa[slot] = 0;
        x->slot_dev[slot] = NULL;
        udev->slot = 0;
        udev->hcd_priv = NULL;
        spin_unlock_irqrestore(&x->lock, s);
        goto fail_free;
    }
    struct xhci_slot_ctx *out = dev_ctx(x, d, 0);
    kdebug("xhci%u: %s: slot %u, address %u, state %u", x->hcd.index, udev->dev.name, slot, SLOT_ADDR_OF(out->dw[3]),
           SLOT_STATE_OF(out->dw[3]));
    return 0;

fail_free:
    xhci_dev_free(x, d);
fail_slot:
    if (!x->dead)
        (void)xhci_cmd(x, 0, TRB_TYPE(TRB_CMD_DISABLE_SLOT) | TRB_SLOT(slot), NULL);
    return rc;
}

static int xhci_update_ep0(struct usb_hcd *hcd, struct usb_device *udev, unsigned mps)
{
    struct xhci *x = hcd_to_xhci(hcd);
    struct xhci_dev *d = udev->hcd_priv;
    if (d == NULL)
        return -EINVAL;
    /* Evaluate Context reads only the Max Packet Size of an EP0 context
     * (§6.2.3.3); the rest is copied from the device context so the
     * input is a whole, current context either way. */
    struct xhci_input_ctrl_ctx *icc = in_ctrl(d);
    icc->drop = 0;
    icc->add = 1u << 1;
    memcpy(in_ctx(x, d, 1), dev_ctx(x, d, 1), x->csz);
    struct xhci_ep_ctx *ec = in_ctx(x, d, 1);
    ec->dw[1] = (ec->dw[1] & 0xffffu) | EP_MPS(mps);
    wmb();
    int cc = xhci_cmd(x, d->input_dma, TRB_TYPE(TRB_CMD_EVAL_CTX) | TRB_SLOT(udev->slot), NULL);
    int rc = cmd_result(x, "evaluate context (EP0 max packet)", cc);
    if (rc == 0)
        d->ep[1].mps = mps;
    return rc;
}

/* An endpoint context's type field, or 0 for one that gets no context. */
static unsigned ep_type_of(const struct usb_endpoint_descriptor *e)
{
    bool in = (e->bEndpointAddress & USB_EP_DIR_IN) != 0;
    switch (USB_EP_XFER(e->bmAttributes)) {
    case USB_EP_BULK:      return in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT;
    case USB_EP_INTERRUPT: return in ? EP_TYPE_INT_IN : EP_TYPE_INT_OUT;
    default:               return 0;   /* isochronous: recorded in the descriptor, not driven */
    }
}

/* The Interval field (§6.2.3.6): 125 us * 2^interval. */
static uint32_t ep_interval_of(enum usb_speed speed, const struct usb_endpoint_descriptor *e)
{
    if (USB_EP_XFER(e->bmAttributes) != USB_EP_INTERRUPT)
        return 0;
    unsigned b = e->bInterval ? e->bInterval : 1;
    if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER) {
        unsigned i = b - 1;
        return i > 15 ? 15 : i;   /* bInterval is already the exponent + 1 */
    }
    /* Full/low speed: bInterval in frames (1 ms); the smallest 2^n frames >= it. */
    unsigned i = 3;   /* 2^3 * 125 us = 1 ms */
    while (i < 10 && (1u << (i - 3)) < b)
        i++;
    return i;
}

static int xhci_configure(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct xhci *x = hcd_to_xhci(hcd);
    struct xhci_dev *d = udev->hcd_priv;
    if (d == NULL)
        return -EINVAL;
    struct xhci_input_ctrl_ctx *icc = in_ctrl(d);
    icc->drop = 0;
    icc->add = 1u << 0;
    unsigned max_dci = 1;
    int rc = 0;
    for (unsigned i = 0; i < udev->nr_intf && rc == 0; i++) {
        const struct usb_interface *intf = &udev->intf[i];
        for (unsigned k = 0; k < intf->nr_ep; k++) {
            const struct usb_endpoint_descriptor *e = &intf->ep[k].desc;
            unsigned type = ep_type_of(e);
            unsigned dci = xhci_dci(e->bEndpointAddress);
            if (type == 0 || dci > XHCI_MAX_DCI || d->ep[dci].ring != NULL)
                continue;
            d->ep[dci].ring = ring_alloc(x);
            if (d->ep[dci].ring == NULL) {
                rc = -ENOMEM;
                break;
            }
            d->ep[dci].mps = e->wMaxPacketSize & 0x7ffu;
            struct xhci_ep_ctx *ec = in_ctx(x, d, dci);
            memset(ec, 0, x->csz);
            ec->dw[0] = EP_INTERVAL(ep_interval_of(udev->speed, e));
            ec->dw[1] = EP_TYPE(type) | EP_MPS(d->ep[dci].mps) | EP_CERR(3);
            uint64_t deq = d->ep[dci].ring->dma | EP_DCS;
            ec->dw[2] = (uint32_t)deq;
            ec->dw[3] = (uint32_t)(deq >> 32);
            ec->dw[4] = EP_AVG_TRB_LEN(type == EP_TYPE_INT_IN || type == EP_TYPE_INT_OUT ? 1024 : 3072);
            icc->add |= 1u << dci;
            if (dci > max_dci)
                max_dci = dci;
        }
    }
    if (rc)
        return rc;
    /* The slot context: as the controller has it, with the entry count
     * raised to the highest endpoint configured. */
    memcpy(in_ctx(x, d, 0), dev_ctx(x, d, 0), x->csz);
    struct xhci_slot_ctx *sc = in_ctx(x, d, 0);
    sc->dw[0] = (sc->dw[0] & ~SLOT_ENTRIES(31)) | SLOT_ENTRIES(max_dci);
    wmb();
    int cc = xhci_cmd(x, d->input_dma, TRB_TYPE(TRB_CMD_CONFIG_EP) | TRB_SLOT(udev->slot), NULL);
    return cmd_result(x, "configure endpoint", cc);
}

static void xhci_disable_device(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct xhci *x = hcd_to_xhci(hcd);
    struct xhci_dev *d = udev->hcd_priv;
    if (d == NULL)
        return;
    unsigned slot = udev->slot;
    /* Quiet every endpoint, then the slot: no event names this slot after
     * Disable Slot completes, so the rings are ours to empty. */
    if (!x->dead && slot != 0) {
        for (unsigned dci = 1; dci <= XHCI_MAX_DCI; dci++) {
            if (d->ep[dci].ring != NULL && d->ep[dci].ring->used > 0)
                (void)xhci_cmd(x, 0, TRB_TYPE(TRB_CMD_STOP_EP) | TRB_EP_ID(dci) | TRB_SLOT(slot), NULL);
        }
        int cc = xhci_cmd(x, 0, TRB_TYPE(TRB_CMD_DISABLE_SLOT) | TRB_SLOT(slot), NULL);
        (void)cmd_result(x, "disable slot", cc);
    }
    arch_irq_state_t s = spin_lock_irqsave(&x->lock);
    if (slot != 0) {
        x->dcbaa[slot] = 0;
        x->slot_dev[slot] = NULL;
    }
    spin_unlock_irqrestore(&x->lock, s);
    /* Whatever was still in flight completes now, with -ENODEV: a bio
     * behind it gets an error rather than a wait that never ends (U4). */
    for (unsigned dci = 1; dci <= XHCI_MAX_DCI; dci++) {
        if (d->ep[dci].ring != NULL)
            ring_flush(x, d->ep[dci].ring, NULL, 0, -ENODEV);
    }
    udev->slot = 0;
    udev->hcd_priv = NULL;
    xhci_dev_free(x, d);
}

static int xhci_debug_port(struct usb_hcd *hcd, unsigned port, bool connected);   /* with the ports, below */

static const struct usb_hcd_ops xhci_hcd_ops = {
    .enable_device = xhci_enable_device,
    .update_ep0 = xhci_update_ep0,
    .configure = xhci_configure,
    .disable_device = xhci_disable_device,
    .submit = xhci_submit,
    .cancel = xhci_cancel,
    .reset_endpoint = xhci_reset_endpoint,
    .debug_port = xhci_debug_port,
};

/* --- the event ring and the interrupt ---------------------------------------------- */

static void xhci_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct xhci *x = arg;
    uint32_t sts = rd32(x->op + XHCI_USBSTS);
    if (sts & (USBSTS_EINT | USBSTS_HSE | USBSTS_HCE))
        wr32(x->op + XHCI_USBSTS, sts & (USBSTS_EINT | USBSTS_HSE | USBSTS_HCE));   /* write-one-to-clear */
    vaddr_t ir = x->rt + XHCI_IR0;
    uint32_t iman = rd32(ir + XHCI_IMAN);
    if (iman & IMAN_IP)
        wr32(ir + XHCI_IMAN, iman);   /* IP is W1C; IE is preserved as read */
    if ((sts & (USBSTS_HSE | USBSTS_HCE)) && !x->dead) {
        x->dead = true;
        x->hcd.dead = true;
        kerror("xhci%u: host %s error (USBSTS 0x%08x); the controller is dead", x->hcd.index,
               (sts & USBSTS_HSE) ? "system" : "controller", sts);
    }

    bool wake_ports = false;
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&x->lock);
        volatile struct xhci_trb *ev = &x->evt[x->evt_deq];
        uint32_t control = ev->control;
        if (((control & TRB_CYCLE) != 0) != x->evt_cycle) {
            /* Caught up: tell the controller where, and that the handler is
             * done (EHB cleared). Then look once more: an event that landed
             * between the check above and this write raised no interrupt,
             * because EHB was still set, and would sit there until the next
             * one -- which for a serial device waiting on this very event
             * never comes. CI's aarch64 run found it under the concurrent
             * benchmark: the ring filled (completion code 21) with nobody
             * reading (docs/drivers/usb/testing.md). */
            wr64(ir + XHCI_ERDP, (x->evt_dma + (dma_addr_t)x->evt_deq * sizeof(struct xhci_trb)) | ERDP_EHB);
            rmb();
            control = ev->control;
            if (((control & TRB_CYCLE) != 0) != x->evt_cycle) {
                spin_unlock_irqrestore(&x->lock, s);
                break;
            }
            /* One arrived in the window: EHB is clear now, so the controller
             * will interrupt for the next, and this one is handled here. */
        }
        rmb();
        struct xhci_trb e = { .ptr = ev->ptr, .status = ev->status, .control = control };
        x->evt_deq++;
        if (x->evt_deq == XHCI_RING_TRBS) {
            x->evt_deq = 0;
            x->evt_cycle = !x->evt_cycle;
        }
        x->events++;
        struct usb_request *done = NULL;
        int status = 0;
        switch (TRB_TYPE_OF(e.control)) {
        case TRB_EV_CMD_COMPLETE:
            if (x->cmdw.trb != 0 && e.ptr == x->cmdw.trb) {
                x->cmdw.status = e.status;
                x->cmdw.control = e.control;
                x->cmdw.trb = 0;
                complete(&x->cmdw.done);
            }
            break;
        case TRB_EV_TRANSFER:
            done = handle_transfer_event(x, &e, &status);
            break;
        case TRB_EV_PORT_STATUS:
            __atomic_store_n(&x->port_change, true, __ATOMIC_RELEASE);
            wake_ports = true;
            break;
        case TRB_EV_HOST_CTRL:
            if (!x->dead) {
                x->dead = true;
                x->hcd.dead = true;
                kerror("xhci%u: host controller event, completion code %u; the controller is dead", x->hcd.index,
                       TRB_CC_OF(e.status));
            }
            break;
        default:
            break;
        }
        spin_unlock_irqrestore(&x->lock, s);
        if (done != NULL)
            usb_request_complete(done, status, done->actual);   /* interrupt context; may submit */
    }
    if (wake_ports)
        waitqueue_wake_all(&x->wq);
}

/* --- root-hub ports and the worker ------------------------------------------------ */

static uint32_t portsc_rd(struct xhci *x, unsigned port) { return rd32(x->op + XHCI_PORTSC(port)); }

/* Write PORTSC preserving what must be preserved (PP above all: a 0
 * powers the port off) and acting only through `bits`. */
static void portsc_wr(struct xhci *x, unsigned port, uint32_t portsc, uint32_t bits)
{
    wr32(x->op + XHCI_PORTSC(port), (portsc & PORTSC_RW_MASK) | bits);
}

/* A device appeared on `port`: enable the port (a reset, unless the
 * controller enabled it itself, as it does for USB3, or `reset` asks
 * for one regardless), learn the speed, hand it to the core. */
static void port_connect(struct xhci *x, unsigned port, bool reset)
{
    uint32_t sc = portsc_rd(x, port);
    if (reset || !(sc & PORTSC_PED)) {
        portsc_wr(x, port, sc, PORTSC_PR);
        unsigned waited = 0;
        for (; waited < XHCI_PORT_RESET_MS; waited += 10) {
            thread_sleep_ms(10);
            sc = portsc_rd(x, port);
            if (sc & PORTSC_PRC)
                break;
        }
        if (sc & PORTSC_PRC)
            portsc_wr(x, port, sc, PORTSC_PRC);
        sc = portsc_rd(x, port);
        if (!(sc & PORTSC_CCS))
            return;   /* gone during the reset; the change bit will say so */
        if (!(sc & PORTSC_PED)) {
            kerror("xhci%u: port %u: not enabled after reset (PORTSC 0x%08x)", x->hcd.index, port, sc);
            x->port_failed[port] = true;
            return;
        }
        thread_sleep_ms(10);   /* reset recovery (USB 2.0 §7.1.7.5) */
    }
    enum usb_speed speed = (enum usb_speed)PORTSC_SPEED(sc);
    if (speed < USB_SPEED_FULL || speed > USB_SPEED_SUPER) {
        kerror("xhci%u: port %u: unknown speed %u (PORTSC 0x%08x)", x->hcd.index, port, speed, sc);
        x->port_failed[port] = true;
        return;
    }
    int rc = usb_port_connected(&x->hcd, port, speed);
    if (rc)
        x->port_failed[port] = true;   /* the core has said which step; wait for the next connect change */
}

static void port_scan(struct xhci *x)
{
    for (unsigned p = 1; p <= x->nr_ports; p++) {
        uint32_t sc = portsc_rd(x, p);
        uint32_t changes = sc & PORTSC_CHANGE_BITS;
        if (changes)
            portsc_wr(x, p, sc, changes);   /* acknowledged before acting, so a change during the action is kept */
        if (changes & PORTSC_CSC)
            x->port_failed[p] = false;
        bool present = x->hcd.port_dev[p] != NULL;
        if ((sc & PORTSC_CCS) && !present && !x->port_failed[p])
            port_connect(x, p, !(sc & PORTSC_PED));
        else if (!(sc & PORTSC_CCS) && present)
            usb_port_disconnected(&x->hcd, p);
    }
}

/* Tests only: the worker's own connect and disconnect paths, on demand. */
static int xhci_debug_port(struct usb_hcd *hcd, unsigned port, bool connected)
{
    struct xhci *x = hcd_to_xhci(hcd);
    if (port == 0 || port > x->nr_ports || x->dead)
        return -EINVAL;
    if (!connected) {
        /* The device is taken down as on a disconnect; the port keeps its
         * device attached, so the worker is told to leave it alone until
         * a real connect change or the connect half of this call. */
        x->port_failed[port] = true;
        usb_port_disconnected(hcd, port);
        return 0;
    }
    x->port_failed[port] = false;
    if (hcd->port_dev[port] != NULL)
        return -EBUSY;
    port_connect(x, port, true);   /* a reset, as a re-plugged device gets */
    return hcd->port_dev[port] != NULL ? 0 : -EIO;
}

static void xhci_worker(void *arg)
{
    struct xhci *x = arg;
    /* The first pass: devices present at boot. The controller reports
     * them as it starts; give the ports a moment to settle so the first
     * scan finds them, then say the scan is done -- the module's init is
     * waiting for exactly that. */
    for (unsigned waited = 0; waited < 200 && !x->dead; waited += 5) {
        bool any = false;
        for (unsigned p = 1; p <= x->nr_ports; p++)
            if (portsc_rd(x, p) & PORTSC_CSC)
                any = true;
        if (any)
            break;
        thread_sleep_ms(5);
    }
    if (!x->dead)
        port_scan(x);
    complete(&x->first_scan);
    for (;;) {
        wait_event(&x->wq, __atomic_load_n(&x->port_change, __ATOMIC_ACQUIRE) || __atomic_load_n(&x->stop, __ATOMIC_ACQUIRE));
        if (__atomic_load_n(&x->stop, __ATOMIC_ACQUIRE))
            break;
        __atomic_store_n(&x->port_change, false, __ATOMIC_RELEASE);
        if (!x->dead)
            port_scan(x);
    }
    thread_exit(0);
}

/* --- bring-up and teardown ----------------------------------------------------------- */

static bool wait_bits(vaddr_t reg, uint32_t mask, uint32_t want, unsigned ms)
{
    for (unsigned waited = 0; waited <= ms; waited++) {
        if ((rd32(reg) & mask) == want)
            return true;
        thread_sleep_ms(1);
    }
    return (rd32(reg) & mask) == want;
}

static int xhci_halt_and_reset(struct xhci *x)
{
    uint32_t cmd = rd32(x->op + XHCI_USBCMD);
    if (cmd & USBCMD_RS) {
        wr32(x->op + XHCI_USBCMD, cmd & ~USBCMD_RS);
        if (!wait_bits(x->op + XHCI_USBSTS, USBSTS_HCH, USBSTS_HCH, 100)) {
            kerror("xhci%u: did not halt", x->hcd.index);
            return -EIO;
        }
    }
    wr32(x->op + XHCI_USBCMD, USBCMD_HCRST);
    if (!wait_bits(x->op + XHCI_USBCMD, USBCMD_HCRST, 0, XHCI_RESET_TIMEOUT_MS) ||
        !wait_bits(x->op + XHCI_USBSTS, USBSTS_CNR, 0, XHCI_RESET_TIMEOUT_MS)) {
        kerror("xhci%u: did not come out of reset (USBCMD 0x%08x, USBSTS 0x%08x)", x->hcd.index,
               rd32(x->op + XHCI_USBCMD), rd32(x->op + XHCI_USBSTS));
        return -EIO;
    }
    return 0;
}

static void xhci_free_tables(struct xhci *x)
{
    struct device *dev = &x->pdev->dev;
    ring_free(x, x->cmd);
    x->cmd = NULL;
    if (x->evt)
        dma_free(dev, PAGE_SIZE, x->evt, x->evt_dma);
    if (x->erst)
        dma_free(dev, PAGE_SIZE, x->erst, x->erst_dma);
    if (x->scratch_va) {
        for (unsigned i = 0; i < x->nr_scratch; i++)
            if (x->scratch_va[i])
                dma_free(dev, x->hw_page, x->scratch_va[i], x->scratch_dma[i]);
        kfree(x->scratch_va);
        kfree(x->scratch_dma);
    }
    if (x->scratch_arr)
        dma_free(dev, PAGE_SIZE, x->scratch_arr, x->scratch_arr_dma);
    if (x->dcbaa)
        dma_free(dev, PAGE_SIZE, x->dcbaa, x->dcbaa_dma);
    x->evt = NULL;
    x->erst = NULL;
    x->scratch_va = NULL;
    x->scratch_dma = NULL;
    x->scratch_arr = NULL;
    x->dcbaa = NULL;
}

static int xhci_alloc_tables(struct xhci *x)
{
    struct device *dev = &x->pdev->dev;
    x->dcbaa = dma_alloc(dev, PAGE_SIZE, &x->dcbaa_dma, DMA_ZERO);
    x->cmd = ring_alloc(x);
    x->evt = dma_alloc(dev, PAGE_SIZE, &x->evt_dma, DMA_ZERO);
    x->erst = dma_alloc(dev, PAGE_SIZE, &x->erst_dma, DMA_ZERO);
    if (x->dcbaa == NULL || x->cmd == NULL || x->evt == NULL || x->erst == NULL)
        return -ENOMEM;
    x->erst[0].base = x->evt_dma;
    x->erst[0].size = XHCI_RING_TRBS;
    x->evt_cycle = true;
    x->evt_deq = 0;
    if (x->nr_scratch > 0) {
        /* Scratchpad buffers: the controller's private memory, one buffer
         * of its page size per entry, the array at DCBAA[0] (§4.20). */
        x->scratch_arr = dma_alloc(dev, PAGE_SIZE, &x->scratch_arr_dma, DMA_ZERO);
        x->scratch_va = kzalloc(x->nr_scratch * sizeof(void *));
        x->scratch_dma = kzalloc(x->nr_scratch * sizeof(dma_addr_t));
        if (x->scratch_arr == NULL || x->scratch_va == NULL || x->scratch_dma == NULL)
            return -ENOMEM;
        for (unsigned i = 0; i < x->nr_scratch; i++) {
            x->scratch_va[i] = dma_alloc(dev, x->hw_page, &x->scratch_dma[i], DMA_ZERO);
            if (x->scratch_va[i] == NULL)
                return -ENOMEM;
            x->scratch_arr[i] = x->scratch_dma[i];
        }
        x->dcbaa[0] = x->scratch_arr_dma;
    }
    return 0;
}

static int xhci_probe(struct pci_device *pdev, const struct pci_id *id)
{
    (void)id;
    if (pdev->prog_if != 0x30) {
        /* UHCI (00), OHCI (10), EHCI (20): no driver, by §60's rule; the
         * model records DEV_FAILED and this line says why. */
        kinfo("xhci: %s: USB controller with programming interface 0x%02x is not xHCI; not driven",
              pdev->dev.name, pdev->prog_if);
        return -ENODEV;
    }
    struct xhci *x = kzalloc(sizeof(*x));
    if (x == NULL)
        return -ENOMEM;
    x->pdev = pdev;
    x->vector = -1;
    spinlock_init(&x->lock, "xhci");
    mutex_init(&x->cmd_lock, "xhci-cmd");
    waitqueue_init(&x->wq, "xhci-ports");
    completion_init(&x->cmdw.done, "xhci-cmd");
    completion_init(&x->first_scan, "xhci-first-scan");
    list_init(&x->link);
    x->hcd.dev = &pdev->dev;
    x->hcd.ops = &xhci_hcd_ops;
    x->hcd.priv = x;

    pci_enable_device(pdev, true);
    int rc = -EIO;
    x->bar = pci_map_bar(pdev, 0);
    if (x->bar == 0) {
        kerror("xhci: %s: cannot map BAR0", pdev->dev.name);
        goto fail_free;
    }
    uint32_t caplen = rd32(x->bar + XHCI_CAPLENGTH);
    x->op = x->bar + (caplen & 0xffu);
    x->rt = x->bar + (rd32(x->bar + XHCI_RTSOFF) & ~0x1fu);
    x->db = x->bar + (rd32(x->bar + XHCI_DBOFF) & ~0x3u);
    uint32_t hcs1 = rd32(x->bar + XHCI_HCSPARAMS1), hcs2 = rd32(x->bar + XHCI_HCSPARAMS2);
    uint32_t hcc1 = rd32(x->bar + XHCI_HCCPARAMS1);
    unsigned version = (caplen >> 16) & 0xffffu;
    x->max_slots = HCS1_MAX_SLOTS(hcs1);
    x->nr_ports = HCS1_MAX_PORTS(hcs1);
    x->nr_scratch = HCS2_SCRATCHPADS(hcs2);
    x->csz = (hcc1 & HCC1_CSZ) ? 64 : 32;
    x->ac64 = (hcc1 & HCC1_AC64) != 0;
    if (x->max_slots == 0 || x->max_slots > XHCI_MAX_SLOTS || x->nr_ports == 0 || x->nr_ports > USB_MAX_PORTS) {
        kerror("xhci: %s: HCSPARAMS1 0x%08x: %u slots, %u ports; refused", pdev->dev.name, hcs1, x->max_slots,
               x->nr_ports);
        goto fail_unmap;
    }
    dma_set_mask(&pdev->dev, x->ac64 ? 64 : 32);

    /* Register the controller with the core first: its index names the
     * log lines from here on. */
    x->hcd.nr_ports = x->nr_ports;
    rc = usb_hcd_register(&x->hcd);
    if (rc)
        goto fail_unmap;

    rc = xhci_halt_and_reset(x);
    if (rc)
        goto fail_unmap;
    uint32_t pgsz = rd32(x->op + XHCI_PAGESIZE);
    x->hw_page = PAGE_SIZE;
    for (unsigned b = 0; b < 16; b++) {
        if (pgsz & (1u << b)) {
            x->hw_page = 1u << (b + 12);
            break;
        }
    }
    if (x->hw_page < PAGE_SIZE)
        x->hw_page = PAGE_SIZE;

    rc = xhci_alloc_tables(x);
    if (rc) {
        kerror("xhci%u: cannot allocate the controller's tables (%d)", x->hcd.index, rc);
        goto fail_tables;
    }

    /* Program: slots, the context array, the command ring, interrupter 0. */
    wr32(x->op + XHCI_CONFIG, x->max_slots);
    wr64(x->op + XHCI_DCBAAP, x->dcbaa_dma);
    wr64(x->op + XHCI_CRCR, x->cmd->dma | CRCR_RCS);
    vaddr_t ir = x->rt + XHCI_IR0;
    wr32(ir + XHCI_ERSTSZ, 1);
    wr64(ir + XHCI_ERDP, x->evt_dma);
    wr64(ir + XHCI_ERSTBA, x->erst_dma);
    wr32(ir + XHCI_IMOD, 0);   /* no moderation: latency is what the benchmark measures first */
    wr32(ir + XHCI_IMAN, IMAN_IE | IMAN_IP);

    int granted = pci_msix_enable(pdev, 1);
    if (granted >= 1) {
        x->vector = pci_msix_request(pdev, 0, xhci_irq, x, "xhci", 0);
        if (x->vector < 0) {
            pci_msix_disable(pdev);
            rc = x->vector;
            kerror("xhci%u: MSI-X vector (%d)", x->hcd.index, rc);
            goto fail_tables;
        }
        x->msix = true;
    } else {
        x->vector = pci_msi_enable(pdev, xhci_irq, x, "xhci", 0);
        if (x->vector < 0) {
            rc = x->vector;
            kerror("xhci%u: neither MSI-X nor MSI (%d)", x->hcd.index, rc);
            goto fail_tables;
        }
    }

    wmb();
    wr32(x->op + XHCI_USBCMD, USBCMD_RS | USBCMD_INTE | USBCMD_HSEE);
    pdev->dev.drvdata = x;
    mutex_lock(&g_controllers_lock);
    list_push_back(&g_controllers, &x->link);
    mutex_unlock(&g_controllers_lock);

    /* The worker: the first scan now (after the model's lock is released
     * -- enumeration registers devices, which probe runs under), then
     * every port change. */
    char tname[16];
    ksnprintf(tname, sizeof(tname), "xhci/%u", x->hcd.index);
    x->worker = thread_create(xhci_worker, x, tname, SCHED_PRIO_DEFAULT);
    if (x->worker == NULL) {
        rc = -ENOMEM;
        mutex_lock(&g_controllers_lock);
        list_remove(&x->link);
        mutex_unlock(&g_controllers_lock);
        pdev->dev.drvdata = NULL;
        wr32(x->op + XHCI_USBCMD, 0);
        goto fail_irq;
    }
    kinfo("xhci%u: %s: xHCI %x.%02x, %u ports, %u slots, %u-byte contexts, %u scratchpad(s), %s", x->hcd.index,
          pdev->dev.name, version >> 8, version & 0xff, x->nr_ports, x->max_slots, x->csz, x->nr_scratch,
          x->ac64 ? "64-bit DMA" : "32-bit DMA");
    waitqueue_wake_all(&x->wq);
    return 0;

fail_irq:
    if (x->msix)
        pci_msix_disable(pdev);
    else
        pci_msi_disable(pdev);
    synchronize_irq((unsigned)x->vector);
fail_tables:
    xhci_free_tables(x);
fail_unmap:
    device_unmap_mmio(x->bar);
fail_free:
    kfree(x);
    return rc;
}

static void xhci_remove(struct pci_device *pdev)
{
    struct xhci *x = pdev->dev.drvdata;
    if (x == NULL)
        return;
    /* Children first (the core disconnects every port), then the worker,
     * then the controller, then the memory. */
    mutex_lock(&g_controllers_lock);
    list_remove(&x->link);
    mutex_unlock(&g_controllers_lock);
    __atomic_store_n(&x->stop, true, __ATOMIC_RELEASE);
    waitqueue_wake_all(&x->wq);
    thread_join(x->worker);   /* no scan runs while the ports are taken down */
    x->worker = NULL;
    usb_hcd_unregister(&x->hcd);
    if (!x->dead) {
        wr32(x->op + XHCI_USBCMD, rd32(x->op + XHCI_USBCMD) & ~(USBCMD_RS | USBCMD_INTE));
        (void)wait_bits(x->op + XHCI_USBSTS, USBSTS_HCH, USBSTS_HCH, 100);
    }
    int vector = x->vector;
    if (x->msix)
        pci_msix_disable(pdev);
    else
        pci_msi_disable(pdev);
    if (vector >= 0)
        synchronize_irq((unsigned)vector);
    xhci_free_tables(x);
    device_unmap_mmio(x->bar);
    pdev->dev.drvdata = NULL;
    kinfo("xhci%u: removed (%llu events, %llu transfers, %llu commands, %llu errors)", x->hcd.index,
          (unsigned long long)x->events, (unsigned long long)x->transfers, (unsigned long long)x->commands,
          (unsigned long long)x->errors);
    kfree(x);
}

static const struct pci_id xhci_ids[] = {
    { PCI_ANY, PCI_ANY, 0x0c, 0x03, PCI_ID_CLASS },   /* serial bus, USB; prog_if 0x30 checked in probe */
    PCI_ID_END,
};

static struct pci_driver xhci_driver = {
    .drv = { .name = "xhci" },
    .ids = xhci_ids,
    .probe = xhci_probe,
    .remove = xhci_remove,
};

static int xhci_module_init(void)
{
    usb_core_init();
    mutex_init(&g_controllers_lock, "xhci-controllers");
    int rc = pci_register_driver(&xhci_driver);
    if (rc)
        return rc;
    /* Every controller's first scan, bounded: the module is loaded when
     * the boot's devices are on the bus, so the class drivers that load
     * next bind at their own init and the self-tests see a settled bus. */
    mutex_lock(&g_controllers_lock);
    struct xhci *x;
    list_for_each_entry(x, &g_controllers, link) {
        uint64_t deadline = clock_now_ns() + 3000ull * 1000000ull;
        while (!completion_done(&x->first_scan) && clock_now_ns() < deadline)
            thread_sleep_ms(1);
        if (!completion_done(&x->first_scan))
            kwarn("xhci%u: the first port scan has not finished after 3 s; continuing", x->hcd.index);
    }
    mutex_unlock(&g_controllers_lock);
    return 0;
}

static void xhci_module_shutdown(void)
{
    pci_unregister_driver(&xhci_driver);
}

COSMO_MODULE("xhci", "1.0", xhci_module_init, xhci_module_shutdown, "", MODULE_CAP_DRIVER);
