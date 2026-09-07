/*
 * usb_storage.c - USB mass storage, bulk-only transport, SCSI transparent
 * command set (docs/drivers/usb/design.md, "USB mass storage").
 *
 * A block device over three bulk transfers per command: a Command Block
 * Wrapper out, the data, a Command Status Wrapper in. One exchange in
 * flight per device -- the transport allows no more -- so the block
 * layer's pending list holds the queue and this driver holds a state
 * machine: each transfer's completion submits the next, in interrupt
 * context, and nothing sleeps on the I/O path.
 *
 * Every DMA goes through the host controller (usb_dma_dev; U1): the
 * blkdev's `dev` is the controller, which is what the block layer's
 * buffer checks and the IOMMU's fault attribution need it to be.
 */

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <drivers/usb.h>

#define USBS_CBW_SIG      0x43425355u   /* "USBC" */
#define USBS_CSW_SIG      0x53425355u   /* "USBS" */
#define USBS_CBW_LEN      31u
#define USBS_CSW_LEN      13u
#define USBS_MAX_SECTORS  128u          /* 64 KiB per command at 512-byte sectors; the benchmark's decision */
#define USBS_MAX_SEGS     16u
#define USBS_TIMEOUT_NS   (10ull * 1000 * 1000 * 1000)
#define USBS_SYNC_NS      (3ull * 1000 * 1000 * 1000)
#define USBS_READY_TRIES  10u

/* Class requests (BOT §3) */
#define USBS_REQ_RESET    0xff
#define USBS_REQ_GET_MAX_LUN 0xfe

/* SCSI */
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_MODE_SENSE_6     0x1a
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2a
#define SCSI_SYNC_CACHE_10    0x35

struct usbs_cbw {
    uint32_t sig, tag, data_len;
    uint8_t flags;        /* bit 7: data in */
    uint8_t lun;
    uint8_t cb_len;
    uint8_t cb[16];
} __packed;

struct usbs_csw {
    uint32_t sig, tag, residue;
    uint8_t status;       /* 0 passed, 1 failed, 2 phase error */
} __packed;

enum usbs_phase { USBS_IDLE, USBS_CBW, USBS_DATA, USBS_CSW };

struct usbs {
    struct usb_device *udev;
    uint8_t ep_in, ep_out, ifnum;
    struct blkdev bd;
    bool registered;

    spinlock_t lock;                    /* the exchange state below */
    enum usbs_phase phase;
    struct bio *bio;                    /* the bio of the exchange in flight */
    struct usb_request *cur;            /* the transfer in flight */
    struct usb_request cbw_req, data_req, csw_req;
    struct usbs_cbw *cbw;               /* DMA-able (kmalloc) */
    struct usbs_csw *csw;
    struct usb_sg sgs[USBS_MAX_SEGS];
    uint32_t tag;
    bool broken;                        /* an exchange failed mid-way: recover before the next (thread context) */
    bool drop_csw;                      /* fault injection, decided at submit: this exchange never asks for its CSW */
    uint64_t exchanges, failures, recoveries;
    char vendor[9], product[17], rev[5];
};

static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void cbw_fill(struct usbs *s, const uint8_t *cb, unsigned cb_len, uint32_t data_len, bool in)
{
    struct usbs_cbw *c = s->cbw;
    memset(c, 0, sizeof(*c));
    c->sig = USBS_CBW_SIG;
    c->tag = ++s->tag;
    c->data_len = data_len;
    c->flags = in ? 0x80 : 0;
    c->lun = 0;
    c->cb_len = (uint8_t)cb_len;
    memcpy(c->cb, cb, cb_len);
}

/* --- recovery (BOT §5.3.4) -------------------------------------------------------- */

static int usbs_reset_recovery(struct usbs *s)
{
    s->recoveries++;
    int rc = usb_control_msg(s->udev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USBS_REQ_RESET, 0, s->ifnum,
                             NULL, 0, 0);
    if (rc < 0)
        return rc;
    rc = usb_clear_halt(s->udev, s->ep_in);
    if (rc)
        return rc;
    return usb_clear_halt(s->udev, s->ep_out);
}

/* --- the synchronous path (probe; thread context) ---------------------------------- */

/*
 * One exchange, waited for. Returns the CSW status (0, 1, 2) with the
 * data moved in *moved, or a negative errno. A stalled data phase is the
 * device declining the data: the halt is cleared and the CSW read as
 * the specification says (§6.7); a stalled CSW is retried once after a
 * clear; a phase error runs the reset recovery.
 */
static int usbs_sync(struct usbs *s, const uint8_t *cb, unsigned cb_len, void *buf, uint32_t len, bool in,
                     uint32_t *moved)
{
    cbw_fill(s, cb, cb_len, len, in);
    uint32_t got = 0;
    int rc = usb_bulk_msg(s->udev, s->ep_out, s->cbw, USBS_CBW_LEN, &got, USBS_SYNC_NS);
    if (rc)
        return rc;
    uint32_t data = 0;
    if (len > 0) {
        rc = usb_bulk_msg(s->udev, in ? s->ep_in : s->ep_out, buf, len, &data, USBS_SYNC_NS);
        if (rc == -EPIPE) {
            rc = usb_clear_halt(s->udev, in ? s->ep_in : s->ep_out);
            if (rc)
                return rc;
            data = 0;
        } else if (rc) {
            return rc;
        }
    }
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        rc = usb_bulk_msg(s->udev, s->ep_in, s->csw, USBS_CSW_LEN, &got, USBS_SYNC_NS);
        if (rc == -EPIPE) {
            rc = usb_clear_halt(s->udev, s->ep_in);
            if (rc)
                return rc;
            continue;
        }
        break;
    }
    if (rc)
        return rc;
    if (got != USBS_CSW_LEN || s->csw->sig != USBS_CSW_SIG || s->csw->tag != s->cbw->tag) {
        kwarn("usb-storage: %s: bad CSW (%u bytes, sig %08x, tag %u for %u)", s->udev->dev.name, got, s->csw->sig,
              s->csw->tag, s->cbw->tag);
        (void)usbs_reset_recovery(s);
        return -EIO;
    }
    if (s->csw->status == 2) {
        (void)usbs_reset_recovery(s);
        return -EIO;
    }
    if (moved)
        *moved = data;
    return s->csw->status;
}

static int usbs_request_sense(struct usbs *s, uint8_t *key, uint8_t *asc)
{
    uint8_t cb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t *sense = kzalloc(18);
    if (sense == NULL)
        return -ENOMEM;
    uint32_t got = 0;
    int rc = usbs_sync(s, cb, sizeof(cb), sense, 18, true, &got);
    if (rc == 0 && got >= 14) {
        *key = sense[2] & 0x0f;
        *asc = sense[12];
    } else if (rc >= 0) {
        rc = -EIO;
    }
    kfree(sense);
    return rc;
}

/* --- the asynchronous path (bios) --------------------------------------------------- */

static void usbs_finish(struct usbs *s, int status)
{
    arch_irq_state_t f = spin_lock_irqsave(&s->lock);
    struct bio *bio = s->bio;
    s->bio = NULL;
    s->cur = NULL;
    s->phase = USBS_IDLE;
    if (status)
        s->failures++;
    spin_unlock_irqrestore(&s->lock, f);
    /* The slot is free before the completion runs: the block layer
     * resubmits a pending bio from bio_complete, and it must find room. */
    if (bio)
        bio_complete(bio, status);
}

static void usbs_csw_done(struct usb_request *r);
static void usbs_data_done(struct usb_request *r);

static int usbs_start(struct usbs *s, struct usb_request *r, enum usbs_phase phase)
{
    arch_irq_state_t f = spin_lock_irqsave(&s->lock);
    s->cur = r;
    s->phase = phase;
    spin_unlock_irqrestore(&s->lock, f);
    int rc = usb_submit(r);
    if (rc) {
        f = spin_lock_irqsave(&s->lock);
        s->cur = NULL;
        spin_unlock_irqrestore(&s->lock, f);
    }
    return rc;
}

static void usbs_submit_csw(struct usbs *s)
{
    if (s->drop_csw) {
        /* Debug builds, on request (decided in usbs_submit, which runs in
         * thread context, where the injector answers): the status is never
         * asked for, so the exchange hangs with the bio in flight until the
         * block layer's timeout thread runs usbs_timeout -- the path a
         * device that stops answering takes (usb-storage-timeout,
         * usb-unplug). */
        arch_irq_state_t f = spin_lock_irqsave(&s->lock);
        s->drop_csw = false;
        s->cur = NULL;
        s->phase = USBS_CSW;
        spin_unlock_irqrestore(&s->lock, f);
        return;
    }
    struct usb_request *r = &s->csw_req;
    memset(r, 0, sizeof(*r));
    r->udev = s->udev;
    r->ep = s->ep_in;
    r->buf = s->csw;
    r->len = USBS_CSW_LEN;
    r->done = usbs_csw_done;
    r->arg = s;
    int rc = usbs_start(s, r, USBS_CSW);
    if (rc)
        usbs_finish(s, rc == -ENODEV ? -ENODEV : -EIO);
}

static void usbs_cbw_done(struct usb_request *r)
{
    struct usbs *s = r->arg;
    if (r->status != 0 || r->actual != USBS_CBW_LEN) {
        /* The command never reached the device whole: nothing to recover
         * from in the device, but the endpoint may be halted. */
        if (r->status == -EPIPE || r->status == -EIO)
            s->broken = true;
        usbs_finish(s, r->status == -ENODEV ? -ENODEV : -EIO);
        return;
    }
    if (s->data_req.nr_sgs > 0) {
        int rc = usbs_start(s, &s->data_req, USBS_DATA);
        if (rc)
            usbs_finish(s, rc == -ENODEV ? -ENODEV : -EIO);
        return;
    }
    usbs_submit_csw(s);
}

static void usbs_data_done(struct usb_request *r)
{
    struct usbs *s = r->arg;
    if (r->status == -ENODEV) {
        usbs_finish(s, -ENODEV);
        return;
    }
    if (r->status == -EPIPE) {
        /* The device declined the data (BOT §6.7.2/6.7.3): the CSW says
         * why, but the halt must be cleared first, which needs a thread.
         * The exchange fails here; the next submit recovers. */
        s->broken = true;
        usbs_finish(s, -EIO);
        return;
    }
    if (r->status != 0) {
        s->broken = true;
        usbs_finish(s, -EIO);
        return;
    }
    usbs_submit_csw(s);
}

static void usbs_csw_done(struct usb_request *r)
{
    struct usbs *s = r->arg;
    if (r->status == -ENODEV) {
        usbs_finish(s, -ENODEV);
        return;
    }
    if (r->status != 0 || r->actual != USBS_CSW_LEN || s->csw->sig != USBS_CSW_SIG || s->csw->tag != s->cbw->tag) {
        s->broken = true;   /* a stalled or malformed CSW: reset recovery before the next exchange */
        usbs_finish(s, -EIO);
        return;
    }
    int status = 0;
    switch (s->csw->status) {
    case 0:
        if (s->csw->residue != 0 || (s->data_req.nr_sgs > 0 && s->data_req.actual != s->data_req.len))
            status = -EIO;   /* the device moved less than the command asked */
        break;
    case 1:
        status = -EIO;       /* command failed; REQUEST SENSE would say why, and needs a thread */
        break;
    default:
        s->broken = true;    /* phase error */
        status = -EIO;
        break;
    }
    usbs_finish(s, status);
}

/*
 * Build the exchange for a bio and start it. Thread or interrupt context
 * (the block layer resubmits from bio_complete). -EAGAIN while one is in
 * flight; the block layer queues the bio and resubmits it in order.
 */
static int usbs_submit(struct blkdev *bd, struct bio *bio)
{
    struct usbs *s = bd->priv;
    arch_irq_state_t f = spin_lock_irqsave(&s->lock);
    if (s->phase != USBS_IDLE) {
        spin_unlock_irqrestore(&s->lock, f);
        return -EAGAIN;
    }
    s->phase = USBS_CBW;   /* claimed */
    s->bio = bio;
    spin_unlock_irqrestore(&s->lock, f);

    if (s->broken) {
        /* An earlier exchange ended with a halted endpoint or a phase
         * error. Recovery needs commands and control transfers, so it
         * runs here only when this is a thread; from interrupt context
         * the bio fails and the block layer's timeout thread, or the
         * next thread-context submit, recovers. */
        if (preemptible()) {
            int rc = usbs_reset_recovery(s);
            if (rc == 0)
                s->broken = false;
        }
        if (s->broken) {
            usbs_finish(s, -EIO);
            return 0;
        }
    }

    uint8_t cb[16];
    unsigned cb_len = 10;
    memset(cb, 0, sizeof(cb));
    uint32_t data_len = 0;
    bool in = false;
    unsigned nsegs = 0;
    if (bio->dir == BIO_FLUSH) {
        cb[0] = SCSI_SYNC_CACHE_10;
    } else {
        if (bio->sector > 0xffffffffu || bio->nsectors > 0xffffu) {
            usbs_finish(s, -EINVAL);
            return 0;
        }
        cb[0] = bio->dir == BIO_READ ? SCSI_READ_10 : SCSI_WRITE_10;
        put_be32(cb + 2, (uint32_t)bio->sector);
        put_be16(cb + 7, (uint16_t)bio->nsectors);
        in = bio->dir == BIO_READ;
        data_len = bio->nsectors * bd->sector_size;
        nsegs = bio_segments(bio);
        if (nsegs > USBS_MAX_SEGS) {
            usbs_finish(s, -EINVAL);
            return 0;
        }
        for (unsigned i = 0; i < nsegs; i++) {
            struct bio_vec v;
            bio_segment(bio, i, &v);
            s->sgs[i].buf = v.buf;
            s->sgs[i].len = v.len;
        }
    }
    cbw_fill(s, cb, cb_len, data_len, in);
    s->exchanges++;
    s->drop_csw = faultinject_should_fail(FI_USB_CSW);   /* answers only in thread context; false elsewhere */

    struct usb_request *d = &s->data_req;
    memset(d, 0, sizeof(*d));
    d->udev = s->udev;
    d->ep = in ? s->ep_in : s->ep_out;
    d->sgs = s->sgs;
    d->nr_sgs = nsegs;
    d->len = data_len;
    d->done = usbs_data_done;
    d->arg = s;

    struct usb_request *c = &s->cbw_req;
    memset(c, 0, sizeof(*c));
    c->udev = s->udev;
    c->ep = s->ep_out;
    c->buf = s->cbw;
    c->len = USBS_CBW_LEN;
    c->done = usbs_cbw_done;
    c->arg = s;
    int rc = usbs_start(s, c, USBS_CBW);
    if (rc) {
        usbs_finish(s, rc == -ENODEV ? -ENODEV : -EIO);
        return 0;   /* the bio is completed, not refused */
    }
    return 0;
}

/* The block layer's timeout thread: the exchange in flight is taken
 * back, the device reset, the bio failed -ETIMEDOUT. */
static void usbs_timeout(struct blkdev *bd, struct bio *victim)
{
    struct usbs *s = bd->priv;
    /* Take the bio out of the state machine's reach first: cancelling the
     * transfer in flight runs its callback, which would otherwise start
     * the next phase or finish the bio itself. */
    arch_irq_state_t f = spin_lock_irqsave(&s->lock);
    if (s->bio != victim) {
        spin_unlock_irqrestore(&s->lock, f);
        return;   /* completed between the layer's decision and now */
    }
    struct bio *bio = s->bio;
    struct usb_request *cur = s->cur;
    enum usbs_phase phase = s->phase;
    s->bio = NULL;
    spin_unlock_irqrestore(&s->lock, f);
    kwarn("usb-storage: %s: command timed out in phase %u; resetting", bd->name, phase);
    if (cur != NULL)
        (void)usb_cancel(cur, -ETIMEDOUT);
    (void)usbs_reset_recovery(s);
    f = spin_lock_irqsave(&s->lock);
    s->cur = NULL;
    s->phase = USBS_IDLE;
    s->broken = false;
    s->failures++;
    spin_unlock_irqrestore(&s->lock, f);
    if (bio)
        bio_complete(bio, -ETIMEDOUT);
}

/* Tests only: a READ (10) of one block into an address the caller chose
 * (typically one no IOMMU mapping covers), so the IOMMU can be seen to
 * refuse the controller's DMA. Thread context; the exchange is
 * synchronous and the CSW's status is the device's opinion. */
static int usbs_debug_dma(struct blkdev *bd, uint64_t addr)
{
    struct usbs *s = bd->priv;
    uint8_t cb[10] = { SCSI_READ_10, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
    cbw_fill(s, cb, sizeof(cb), bd->sector_size, true);
    uint32_t got = 0;
    int rc = usb_bulk_msg(s->udev, s->ep_out, s->cbw, USBS_CBW_LEN, &got, USBS_SYNC_NS);
    if (rc)
        return rc;
    struct usb_request r;
    memset(&r, 0, sizeof(r));
    r.udev = s->udev;
    r.ep = s->ep_in;
    r.len = bd->sector_size;
    r.debug_dma = addr;
    r.done = NULL;
    rc = usb_submit(&r);
    if (rc)
        return rc;
    /* No callback: poll the status the HCD writes; the transfer either
     * completes (the device sent the block, the IOMMU dropped the write)
     * or is cancelled at the bound. */
    uint64_t deadline = clock_now_ns() + USBS_SYNC_NS;
    while (__atomic_load_n(&r.status, __ATOMIC_ACQUIRE) == -EINPROGRESS && clock_now_ns() < deadline)
        thread_sleep_ns(250000);
    if (__atomic_load_n(&r.status, __ATOMIC_ACQUIRE) == -EINPROGRESS)
        (void)usb_cancel(&r, -ETIMEDOUT);
    if (r.status == -EPIPE)
        (void)usb_clear_halt(s->udev, s->ep_in);
    int data_rc = r.status;
    rc = usb_bulk_msg(s->udev, s->ep_in, s->csw, USBS_CSW_LEN, &got, USBS_SYNC_NS);
    if (rc == -EPIPE) {
        (void)usb_clear_halt(s->udev, s->ep_in);
        rc = usb_bulk_msg(s->udev, s->ep_in, s->csw, USBS_CSW_LEN, &got, USBS_SYNC_NS);
    }
    if (rc)
        return rc;
    if (data_rc == -ETIMEDOUT || data_rc == -ECANCELED)
        return -ETIMEDOUT;
    return s->csw->status == 0 ? 0 : -EIO;
}

static void usbs_release(struct blkdev *bd)
{
    struct usbs *s = bd->priv;
    kfree(s->cbw);
    kfree(s->csw);
    kfree(s);
}

static const struct blkdev_ops usbs_ops = {
    .submit = usbs_submit,
    .release = usbs_release,
    .timeout = usbs_timeout,
    .debug_dma = usbs_debug_dma,
};

/* --- probe and remove ---------------------------------------------------------------- */

static int usbs_identify(struct usbs *s, uint64_t *capacity, uint32_t *block_size, bool *write_protect)
{
    uint8_t *buf = kzalloc(64);
    if (buf == NULL)
        return -ENOMEM;
    int rc;
    uint32_t got = 0;

    uint8_t inq[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    rc = usbs_sync(s, inq, sizeof(inq), buf, 36, true, &got);
    if (rc < 0)
        goto out;
    if (rc == 0 && got >= 36) {
        memcpy(s->vendor, buf + 8, 8);
        memcpy(s->product, buf + 16, 16);
        memcpy(s->rev, buf + 32, 4);
        for (char *p = s->vendor + 7; p >= s->vendor && *p == ' '; p--) *p = '\0';
        for (char *p = s->product + 15; p >= s->product && *p == ' '; p--) *p = '\0';
        if ((buf[0] & 0x1f) != 0x00 && (buf[0] & 0x1f) != 0x0e) {
            kwarn("usb-storage: %s: peripheral type 0x%02x is not a disk; refused", s->udev->dev.name, buf[0] & 0x1f);
            rc = -ENOTSUP;
            goto out;
        }
    }

    /* A fresh device answers NOT READY or UNIT ATTENTION first; REQUEST
     * SENSE clears the attention, and a few tries cover a spin-up. */
    uint8_t tur[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    unsigned tries;
    for (tries = 0; tries < USBS_READY_TRIES; tries++) {
        rc = usbs_sync(s, tur, sizeof(tur), NULL, 0, false, NULL);
        if (rc < 0)
            goto out;
        if (rc == 0)
            break;
        uint8_t key = 0, asc = 0;
        (void)usbs_request_sense(s, &key, &asc);
        thread_sleep_ms(100);
    }
    if (tries == USBS_READY_TRIES) {
        kwarn("usb-storage: %s: not ready after %u tries", s->udev->dev.name, tries);
        rc = -EIO;
        goto out;
    }

    uint8_t rc10[10] = { SCSI_READ_CAPACITY_10, 0 };
    rc = usbs_sync(s, rc10, sizeof(rc10), buf, 8, true, &got);
    if (rc < 0)
        goto out;
    if (rc != 0 || got < 8) {
        rc = -EIO;
        goto out;
    }
    uint32_t last = be32(buf), bs = be32(buf + 4);
    if (bs < 512 || bs > 4096 || (bs & (bs - 1)) != 0 || last == 0xffffffffu) {
        kwarn("usb-storage: %s: capacity refused (last block %u, block size %u)", s->udev->dev.name, last, bs);
        rc = -ENOTSUP;
        goto out;
    }
    *capacity = (uint64_t)last + 1;
    *block_size = bs;

    /* Write protect: MODE SENSE (6), header byte 2 bit 7. Many devices
     * refuse the command; then the disk is assumed writable. */
    uint8_t ms[6] = { SCSI_MODE_SENSE_6, 0, 0x3f, 0, 4, 0 };
    *write_protect = false;
    rc = usbs_sync(s, ms, sizeof(ms), buf, 4, true, &got);
    if (rc == 0 && got >= 4)
        *write_protect = (buf[2] & 0x80) != 0;
    rc = 0;
out:
    kfree(buf);
    return rc;
}

static int usbs_probe(struct usb_device *udev, struct usb_interface *intf, const struct usb_id *id)
{
    (void)id;
    uint8_t ep_in = 0, ep_out = 0;
    for (unsigned i = 0; i < intf->nr_ep; i++) {
        const struct usb_endpoint_descriptor *e = &intf->ep[i].desc;
        if (USB_EP_XFER(e->bmAttributes) != USB_EP_BULK)
            continue;
        if ((e->bEndpointAddress & USB_EP_DIR_IN) && ep_in == 0)
            ep_in = e->bEndpointAddress;
        else if (!(e->bEndpointAddress & USB_EP_DIR_IN) && ep_out == 0)
            ep_out = e->bEndpointAddress;
    }
    if (ep_in == 0 || ep_out == 0) {
        kerror("usb-storage: %s: no bulk in/out pair", udev->dev.name);
        return -ENODEV;
    }
    struct usbs *s = kzalloc(sizeof(*s));
    if (s == NULL)
        return -ENOMEM;
    s->cbw = kzalloc(sizeof(*s->cbw));
    s->csw = kzalloc(sizeof(*s->csw));
    if (s->cbw == NULL || s->csw == NULL) {
        kfree(s->cbw);
        kfree(s->csw);
        kfree(s);
        return -ENOMEM;
    }
    s->udev = udev;
    s->ep_in = ep_in;
    s->ep_out = ep_out;
    s->ifnum = intf->desc.bInterfaceNumber;
    spinlock_init(&s->lock, "usb-storage");

    uint64_t capacity = 0;
    uint32_t bs = 0;
    bool wp = false;
    int rc = usbs_identify(s, &capacity, &bs, &wp);
    if (rc) {
        kerror("usb-storage: %s: identify failed (%d)", udev->dev.name, rc);
        goto fail;
    }

    s->bd.dev = usb_dma_dev(udev);   /* the controller: what the block layer's DMA checks must see (U1) */
    s->bd.ops = &usbs_ops;
    s->bd.sector_size = bs;
    s->bd.capacity = capacity;
    s->bd.max_sectors = USBS_MAX_SECTORS;
    s->bd.max_segments = USBS_MAX_SEGS;
    s->bd.timeout_ns = USBS_TIMEOUT_NS;
    s->bd.read_only = wp;
    s->bd.priv = s;
    s->bd.nr_queues = 1;
    rc = blk_register(&s->bd, "sd");
    if (rc) {
        kerror("usb-storage: %s: blk_register (%d)", udev->dev.name, rc);
        goto fail;
    }
    s->registered = true;
    udev->drvdata = s;
    kinfo("usb-storage: %s is %s: %s %s %s, %llu blocks of %u bytes%s", udev->dev.name, s->bd.name, s->vendor,
          s->product, s->rev, (unsigned long long)capacity, bs, wp ? ", write-protected" : "");
    return 0;

fail:
    kfree(s->cbw);
    kfree(s->csw);
    kfree(s);
    return rc;
}

static void usbs_remove(struct usb_device *udev)
{
    struct usbs *s = udev->drvdata;
    if (s == NULL)
        return;
    /* No new bios reach submit after this; submits inside it have left. */
    blk_unregister(&s->bd);
    /* The exchange in flight, if any, is taken back here rather than
     * left for the controller's teardown, so nothing refers to `s` once
     * the reference below is dropped (design.md, "Disconnect"). */
    arch_irq_state_t f = spin_lock_irqsave(&s->lock);
    struct usb_request *cur = s->cur;
    struct bio *bio = s->bio;
    s->bio = NULL;
    spin_unlock_irqrestore(&s->lock, f);
    if (cur != NULL)
        (void)usb_cancel(cur, -ENODEV);
    f = spin_lock_irqsave(&s->lock);
    s->cur = NULL;
    s->phase = USBS_IDLE;
    spin_unlock_irqrestore(&s->lock, f);
    if (bio != NULL)
        bio_complete(bio, -ENODEV);
    kinfo("usb-storage: %s: %s removed (%llu exchanges, %llu failures, %llu recoveries)", udev->dev.name, s->bd.name,
          (unsigned long long)s->exchanges, (unsigned long long)s->failures, (unsigned long long)s->recoveries);
    udev->drvdata = NULL;
    blkdev_put(&s->bd);   /* the creator's reference; usbs_release frees s when the holders are gone */
}

static const struct usb_id usbs_ids[] = {
    { 0, 0, USB_CLASS_MASS_STORAGE, 0x06, 0x50, USB_ID_CLASS },   /* SCSI transparent, bulk-only */
    USB_ID_END,
};

static struct usb_driver usbs_driver = {
    .drv = { .name = "usb-storage" },
    .ids = usbs_ids,
    .probe = usbs_probe,
    .remove = usbs_remove,
};

static int usbs_module_init(void)
{
    return usb_register_driver(&usbs_driver);
}

static void usbs_module_shutdown(void)
{
    usb_unregister_driver(&usbs_driver);
}

COSMO_MODULE("usb_storage", "1.0", usbs_module_init, usbs_module_shutdown, "xhci", MODULE_CAP_DRIVER);
