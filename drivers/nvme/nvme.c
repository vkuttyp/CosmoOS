/*
 * nvme.c - NVM Express driver (docs/drivers/nvme/design.md). Module `nvme`,
 * bound through the `pci` bus to class 01:08 programming interface 02.
 *
 * One admin queue pair and one I/O queue pair per CPU (as many as the
 * controller grants), each I/O pair with its own MSI-X vector routed to
 * its CPU. A bio's segments become PRP entries without bouncing. Every
 * namespace is a struct blkdev named nvme<ctrl>n<nsid>. The block layer's
 * timeout thread drives Abort and, failing that, a controller reset that
 * fails everything in flight and leaves the controller dead.
 */

#include <kernel/blk.h>
#include <kernel/completion.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/mutex.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <arch/cpu.h>

#include <drivers/pci.h>

#include "nvme.h"

#define NVME_ADMIN_DEPTH   32u
#define NVME_IO_DEPTH      32u
#define NVME_MAX_IOQ       CONFIG_MAX_CPUS
#define NVME_MAX_PAGES     128u                       /* per request: 512 KiB */
#define NVME_ADMIN_TIMEOUT_MS 5000u
#define NVME_PRP_PER_PAGE  (PAGE_SIZE / sizeof(uint64_t))

struct nvme_ctrl;

/* A synchronous (admin) command's waiter. */
struct nvme_cmd_wait {
    struct completion done;
    uint32_t result;
    uint16_t status;
};

/* One command slot: a bio or a waiter, and the slot's PRP list page. */
struct nvme_cmd {
    struct bio *bio;
    struct nvme_cmd_wait *wait;
    uint64_t *prp_list;
    dma_addr_t prp_dma;
    dma_addr_t seg_dma[NVME_MAX_PAGES];   /* the bio's mapped segments, undone at completion */
    uint32_t seg_len[NVME_MAX_PAGES];
    unsigned nr_segs;
    enum dma_dir dir;
    uint16_t next_free;                    /* free list through the slots */
    /* A command id names a slot to the controller too, so a slot the
     * controller may still answer is never handed out again:
     * `orphan` after a software timeout of an admin command (freed when
     * the late completion arrives), `aborting` while an Abort naming it
     * is in flight (freed by the timeout path once the abort is over). */
    bool orphan, aborting;
};

struct nvme_queue {
    struct nvme_ctrl *ctrl;
    uint16_t qid, depth;
    struct nvme_sqe *sq;
    struct nvme_cqe *cq;
    dma_addr_t sq_dma, cq_dma;
    uint16_t sq_tail, cq_head, phase;
    volatile uint32_t *sq_db, *cq_db;
    struct nvme_cmd *cmds;
    uint64_t *prp_pages;                   /* depth pages, one per slot */
    dma_addr_t prp_pages_dma;
    uint16_t free_head;                    /* 0xffff: none */
    unsigned inflight;
    spinlock_t lock;
    int vector;
    unsigned cpu;
    uint64_t completions;
};

struct nvme_ns {
    struct blkdev bd;
    struct nvme_ctrl *ctrl;
    uint32_t nsid;
    unsigned lba_shift;
    struct list_node link;
    bool registered;
};

struct nvme_ctrl {
    struct pci_device *pdev;
    vaddr_t bar;
    uint64_t cap;
    unsigned dstrd;
    unsigned index;                        /* nvme<index> */
    struct nvme_queue admin;
    struct nvme_queue *ioq[NVME_MAX_IOQ];
    unsigned nr_ioq;
    unsigned max_pages;                    /* per request, from MDTS */
    struct list_node namespaces;
    char model[41], serial[21];
    bool dead;
    struct mutex admin_lock;               /* one admin command at a time */
};

static unsigned g_next_index;

/* --- registers ------------------------------------------------------------- */

static uint32_t rd32(struct nvme_ctrl *c, unsigned off) { return *(volatile uint32_t *)(c->bar + off); }
static void wr32(struct nvme_ctrl *c, unsigned off, uint32_t v) { *(volatile uint32_t *)(c->bar + off) = v; }
static uint64_t rd64(struct nvme_ctrl *c, unsigned off)
{
    return (uint64_t)rd32(c, off) | ((uint64_t)rd32(c, off + 4) << 32);
}
static void wr64(struct nvme_ctrl *c, unsigned off, uint64_t v)
{
    wr32(c, off, (uint32_t)v);
    wr32(c, off + 4, (uint32_t)(v >> 32));
}

/* Wait for CSTS.RDY to become `ready`, within CAP.TO. */
static int wait_ready(struct nvme_ctrl *c, bool ready)
{
    unsigned to_ms = (NVME_CAP_TO(c->cap) + 1) * 500;
    for (unsigned waited = 0; waited <= to_ms; waited += 5) {
        uint32_t csts = rd32(c, NVME_REG_CSTS);
        if (csts & NVME_CSTS_CFS)
            return -EIO;
        if (((csts & NVME_CSTS_RDY) != 0) == ready)
            return 0;
        thread_sleep_ms(5);
    }
    return -ETIMEDOUT;
}

/* --- queues ------------------------------------------------------------------ */

static int queue_alloc(struct nvme_ctrl *c, struct nvme_queue *q, uint16_t qid, uint16_t depth)
{
    memset(q, 0, sizeof(*q));
    q->ctrl = c;
    q->qid = qid;
    q->depth = depth;
    q->phase = 1;
    q->vector = -1;
    spinlock_init(&q->lock, "nvme-queue");
    q->sq = dma_alloc(&c->pdev->dev, (size_t)depth * sizeof(struct nvme_sqe), &q->sq_dma, DMA_ZERO);
    q->cq = dma_alloc(&c->pdev->dev, (size_t)depth * sizeof(struct nvme_cqe), &q->cq_dma, DMA_ZERO);
    q->cmds = kzalloc((size_t)depth * sizeof(*q->cmds));
    q->prp_pages = dma_alloc(&c->pdev->dev, (size_t)depth * PAGE_SIZE, &q->prp_pages_dma, DMA_ZERO);
    if (q->sq == NULL || q->cq == NULL || q->cmds == NULL || q->prp_pages == NULL)
        return -ENOMEM;
    for (uint16_t i = 0; i < depth; i++) {
        q->cmds[i].prp_list = (uint64_t *)((uint8_t *)q->prp_pages + (size_t)i * PAGE_SIZE);
        q->cmds[i].prp_dma = q->prp_pages_dma + (dma_addr_t)i * PAGE_SIZE;
        q->cmds[i].next_free = (uint16_t)(i + 1 < depth ? i + 1 : 0xffff);
    }
    q->free_head = 0;
    unsigned stride = 4u << c->dstrd;
    q->sq_db = (volatile uint32_t *)(c->bar + NVME_REG_DBS + (2u * qid) * stride);
    q->cq_db = (volatile uint32_t *)(c->bar + NVME_REG_DBS + (2u * qid + 1) * stride);
    return 0;
}

static void queue_free(struct nvme_ctrl *c, struct nvme_queue *q)
{
    if (q->sq)
        dma_free(&c->pdev->dev, (size_t)q->depth * sizeof(struct nvme_sqe), q->sq, q->sq_dma);
    if (q->cq)
        dma_free(&c->pdev->dev, (size_t)q->depth * sizeof(struct nvme_cqe), q->cq, q->cq_dma);
    if (q->prp_pages)
        dma_free(&c->pdev->dev, (size_t)q->depth * PAGE_SIZE, q->prp_pages, q->prp_pages_dma);
    kfree(q->cmds);
    q->sq = NULL;
    q->cq = NULL;
    q->prp_pages = NULL;
    q->cmds = NULL;
}

/* Queue lock held. A free slot, or 0xffff. */
static uint16_t slot_get(struct nvme_queue *q)
{
    uint16_t id = q->free_head;
    if (id != 0xffff) {
        q->free_head = q->cmds[id].next_free;
        q->cmds[id].next_free = 0xffff;
        q->inflight++;
    }
    return id;
}

/* Queue lock held. */
static void slot_put(struct nvme_queue *q, uint16_t id)
{
    struct nvme_cmd *cmd = &q->cmds[id];
    cmd->bio = NULL;
    cmd->wait = NULL;
    cmd->nr_segs = 0;
    cmd->orphan = false;
    cmd->aborting = false;
    cmd->next_free = q->free_head;
    q->free_head = id;
    q->inflight--;
}

/* Queue lock held. Copy the command into the ring and ring the doorbell. */
static void submit_locked(struct nvme_queue *q, const struct nvme_sqe *sqe)
{
    q->sq[q->sq_tail] = *sqe;
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);
    dma_sync_for_device(&q->ctrl->pdev->dev, q->sq_dma, (size_t)q->depth * sizeof(*sqe), DMA_TO_DEVICE);
    *q->sq_db = q->sq_tail;
}

static void unmap_cmd(struct nvme_ctrl *c, struct nvme_cmd *cmd)
{
    for (unsigned i = 0; i < cmd->nr_segs; i++)
        dma_unmap(&c->pdev->dev, cmd->seg_dma[i], cmd->seg_len[i], cmd->dir);
    cmd->nr_segs = 0;
}

static int status_to_errno(uint16_t status)
{
    if (status == 0)
        return 0;
    unsigned sct = (status >> 8) & 7, sc = status & 0xff;
    if (sct == 0 && sc == NVME_SC_ABORT_REQUESTED)
        return -ETIMEDOUT;
    return -EIO;
}

/* Reap completions. Interrupt or thread context. */
static void queue_process(struct nvme_queue *q)
{
    struct nvme_ctrl *c = q->ctrl;
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&q->lock);
        dma_sync_for_cpu(&c->pdev->dev, q->cq_dma, (size_t)q->depth * sizeof(struct nvme_cqe), DMA_FROM_DEVICE);
        struct nvme_cqe cqe = q->cq[q->cq_head];
        if (NVME_CQE_PHASE(cqe.dw3) != q->phase) {
            spin_unlock_irqrestore(&q->lock, s);
            return;
        }
        q->cq_head = (uint16_t)(q->cq_head + 1);
        if (q->cq_head == q->depth) {
            q->cq_head = 0;
            q->phase ^= 1;
        }
        *q->cq_db = q->cq_head;
        q->completions++;
        uint16_t cid = NVME_CQE_CID(cqe.dw3);
        struct bio *bio = NULL;
        struct nvme_cmd_wait *wait = NULL;
        int status = -EIO;
        bool orphan = false;
        if (cid < q->depth) {
            struct nvme_cmd *cmd = &q->cmds[cid];
            bio = cmd->bio;
            wait = cmd->wait;
            orphan = cmd->orphan;
            status = status_to_errno(NVME_CQE_STATUS(cqe.dw3));
            if (wait) {
                wait->result = cqe.dw0;
                wait->status = NVME_CQE_STATUS(cqe.dw3);
            }
            if (orphan) {
                slot_put(q, cid);   /* the late answer to a timed-out admin command: nobody waits */
            } else if (bio || wait) {
                unmap_cmd(c, cmd);
                if (cmd->aborting) {
                    cmd->bio = NULL;   /* the Abort naming this id is in flight: the slot stays reserved */
                    cmd->wait = NULL;
                } else {
                    slot_put(q, cid);
                }
            }
        }
        spin_unlock_irqrestore(&q->lock, s);
        if (bio)
            bio_complete(bio, status);
        else if (wait)
            complete(&wait->done);
        else if (!orphan)
            kwarn("nvme%u: completion for an unknown command %u on queue %u", c->index, cid, q->qid);
    }
}

static void nvme_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    queue_process(arg);
}

/* --- admin commands ------------------------------------------------------------ */

/* Issue one admin command and wait for it (bounded). Returns the errno of
 * its status; `result` gets DW0. */
static int admin_cmd(struct nvme_ctrl *c, struct nvme_sqe *sqe, uint32_t *result)
{
    if (c->dead)
        return -EIO;
    struct nvme_cmd_wait w;
    completion_init(&w.done, "nvme-admin");
    w.result = 0;
    w.status = 0;
    mutex_lock(&c->admin_lock);
    struct nvme_queue *q = &c->admin;
    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    uint16_t cid = slot_get(q);
    if (cid == 0xffff) {
        spin_unlock_irqrestore(&q->lock, s);
        mutex_unlock(&c->admin_lock);
        return -EBUSY;
    }
    q->cmds[cid].wait = &w;
    sqe->cdw0 = (sqe->cdw0 & 0xffff) | ((uint32_t)cid << 16);
    submit_locked(q, sqe);
    spin_unlock_irqrestore(&q->lock, s);
    for (unsigned waited = 0; waited < NVME_ADMIN_TIMEOUT_MS && !completion_done(&w.done); waited++) {
        thread_sleep_ms(1);
        if (c->admin.vector < 0)
            queue_process(q);   /* polled before the vector exists */
    }
    int rc;
    bool timed_out = false;
    if (!completion_done(&w.done)) {
        /* Decide under the lock. If the slot still names our waiter, the
         * controller has not answered: keep the slot out of the free list
         * until it does (queue_process frees an orphan), so the late
         * completion can never be delivered to a new command. Otherwise
         * the interrupt path has already detached the waiter and is about
         * to signal it: wait for that, the command did complete. */
        s = spin_lock_irqsave(&q->lock);
        if (q->cmds[cid].wait == &w) {
            q->cmds[cid].wait = NULL;
            q->cmds[cid].orphan = true;
            timed_out = true;
        }
        spin_unlock_irqrestore(&q->lock, s);
        if (!timed_out)
            wait_for_completion(&w.done);   /* the stack frame stays valid until it is signalled */
    }
    if (timed_out) {
        rc = -ETIMEDOUT;
    } else {
        rc = status_to_errno(w.status);
        if (result)
            *result = w.result;
    }
    mutex_unlock(&c->admin_lock);
    return rc;
}

static int identify(struct nvme_ctrl *c, uint32_t cns, uint32_t nsid, void *buf, dma_addr_t buf_dma)
{
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_ADMIN_IDENTIFY;
    sqe.nsid = nsid;
    sqe.prp1 = buf_dma;
    sqe.cdw10 = cns;
    (void)buf;
    return admin_cmd(c, &sqe, NULL);
}

static int create_ioq(struct nvme_ctrl *c, struct nvme_queue *q)
{
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_ADMIN_CREATE_CQ;
    sqe.prp1 = q->cq_dma;
    sqe.cdw10 = ((uint32_t)(q->depth - 1) << 16) | q->qid;
    sqe.cdw11 = ((uint32_t)q->qid << 16) | (1u << 1) | 1u;   /* vector = qid, interrupts on, contiguous */
    int rc = admin_cmd(c, &sqe, NULL);
    if (rc)
        return rc;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_ADMIN_CREATE_SQ;
    sqe.prp1 = q->sq_dma;
    sqe.cdw10 = ((uint32_t)(q->depth - 1) << 16) | q->qid;
    sqe.cdw11 = ((uint32_t)q->qid << 16) | 1u;              /* bound to the CQ of the same id, contiguous */
    return admin_cmd(c, &sqe, NULL);
}

/* --- I/O ------------------------------------------------------------------------ */

static struct nvme_queue *queue_for_this_cpu(struct nvme_ctrl *c)
{
    return c->ioq[arch_cpu_id() % c->nr_ioq];
}

static int nvme_submit(struct blkdev *bd, struct bio *bio)
{
    struct nvme_ns *ns = bd->priv;
    struct nvme_ctrl *c = ns->ctrl;
    if (__atomic_load_n(&c->dead, __ATOMIC_ACQUIRE))
        return -EIO;
    struct nvme_queue *q = queue_for_this_cpu(c);

    /* Map the segments and lay out the PRP entries before taking the
     * queue lock: no allocation, no sleeping, but no need to hold it. */
    dma_addr_t seg_dma[NVME_MAX_PAGES];
    uint32_t seg_len[NVME_MAX_PAGES];
    uint64_t prps[NVME_MAX_PAGES + 1];
    unsigned nsegs = 0, nprps = 0;
    enum dma_dir dir = bio->dir == BIO_WRITE ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    if (bio->dir != BIO_FLUSH) {
        unsigned segs = bio_segments(bio);
        if (segs > NVME_MAX_PAGES)
            return -EINVAL;
        for (unsigned i = 0; i < segs; i++) {
            struct bio_vec v;
            bio_segment(bio, i, &v);
            dma_addr_t d = dma_map(bd->dev, v.buf, v.len, dir);
            if (d == 0) {
                for (unsigned k = 0; k < nsegs; k++)
                    dma_unmap(bd->dev, seg_dma[k], seg_len[k], dir);
                return -EINVAL;
            }
            seg_dma[nsegs] = d;
            seg_len[nsegs] = v.len;
            nsegs++;
            /* One PRP per page the segment touches; only the first may start mid-page. */
            dma_addr_t p = d;
            uint32_t left = v.len;
            while (left) {
                uint32_t in_page = (uint32_t)(PAGE_SIZE - (p & (PAGE_SIZE - 1)));
                if (in_page > left)
                    in_page = left;
                if (nprps == ARRAY_SIZE(prps)) {
                    for (unsigned k = 0; k < nsegs; k++)
                        dma_unmap(bd->dev, seg_dma[k], seg_len[k], dir);
                    return -EINVAL;
                }
                prps[nprps++] = p;
                p += in_page;
                left -= in_page;
            }
        }
    }

    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    uint16_t cid = slot_get(q);
    if (cid == 0xffff) {
        spin_unlock_irqrestore(&q->lock, s);
        for (unsigned k = 0; k < nsegs; k++)
            dma_unmap(bd->dev, seg_dma[k], seg_len[k], dir);
        return -EAGAIN;   /* the block layer parks it and retries on the next completion */
    }
    struct nvme_cmd *cmd = &q->cmds[cid];
    cmd->bio = bio;
    cmd->dir = dir;
    cmd->nr_segs = nsegs;
    memcpy(cmd->seg_dma, seg_dma, nsegs * sizeof(seg_dma[0]));
    memcpy(cmd->seg_len, seg_len, nsegs * sizeof(seg_len[0]));

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.nsid = ns->nsid;
    sqe.cdw0 = (uint32_t)cid << 16;
    if (bio->dir == BIO_FLUSH) {
        sqe.cdw0 |= NVME_CMD_FLUSH;
    } else {
        sqe.cdw0 |= bio->dir == BIO_WRITE ? NVME_CMD_WRITE : NVME_CMD_READ;
        uint64_t slba = bio->sector;   /* blkdev sectors are namespace blocks */
        sqe.cdw10 = (uint32_t)slba;
        sqe.cdw11 = (uint32_t)(slba >> 32);
        sqe.cdw12 = bio->nsectors - 1;
        sqe.prp1 = prps[0];
        if (nprps == 2) {
            sqe.prp2 = prps[1];
        } else if (nprps > 2) {
            for (unsigned i = 1; i < nprps; i++)
                cmd->prp_list[i - 1] = prps[i];
            dma_sync_for_device(bd->dev, cmd->prp_dma, PAGE_SIZE, DMA_TO_DEVICE);
            sqe.prp2 = cmd->prp_dma;
        }
    }
    bio->drvpriv = (void *)(uintptr_t)((q->qid << 16) | cid | 0x80000000u);
    submit_locked(q, &sqe);
    spin_unlock_irqrestore(&q->lock, s);
    return 0;
}

/* Disable the controller and fail everything in flight; it stays dead. */
static void controller_die(struct nvme_ctrl *c, const char *why)
{
    if (__atomic_exchange_n(&c->dead, true, __ATOMIC_ACQ_REL))
        return;
    kerror("nvme%u: %s; disabling the controller, every request fails from here", c->index, why);
    wr32(c, NVME_REG_CC, rd32(c, NVME_REG_CC) & ~NVME_CC_EN);
    wait_ready(c, false);
    for (unsigned qi = 0; qi <= c->nr_ioq; qi++) {
        struct nvme_queue *q = qi == 0 ? &c->admin : c->ioq[qi - 1];
        for (uint16_t cid = 0; cid < q->depth; cid++) {
            arch_irq_state_t s = spin_lock_irqsave(&q->lock);
            struct nvme_cmd *cmd = &q->cmds[cid];
            struct bio *bio = cmd->bio;
            struct nvme_cmd_wait *w = cmd->wait;
            if (bio || w || cmd->orphan || cmd->aborting) {
                if (w) {
                    w->status = 0x4000;   /* an invented "controller dead" status: -EIO */
                    w->result = 0;
                }
                unmap_cmd(c, cmd);
                slot_put(q, cid);   /* disabled: no late answer can come for a reserved slot */
            }
            spin_unlock_irqrestore(&q->lock, s);
            if (bio)
                bio_complete(bio, -ETIMEDOUT);
            else if (w)
                complete(&w->done);
        }
    }
}

/* The block layer's timeout thread: abort the command, and reset when the
 * abort gets nowhere. `victim` is only compared, never dereferenced, until
 * it is found in a slot under the queue lock; the slot is then marked
 * `aborting` so its id cannot be reused while an Abort names it (a
 * completion in the meantime leaves the slot reserved for us to free). */
static void nvme_timeout(struct blkdev *bd, struct bio *victim)
{
    struct nvme_ns *ns = bd->priv;
    struct nvme_ctrl *c = ns->ctrl;
    struct nvme_queue *q = NULL;
    uint16_t cid = 0xffff;
    for (unsigned i = 0; i < c->nr_ioq && q == NULL; i++) {
        struct nvme_queue *cand = c->ioq[i];
        arch_irq_state_t s = spin_lock_irqsave(&cand->lock);
        for (uint16_t k = 0; k < cand->depth; k++) {
            if (cand->cmds[k].bio == victim && !cand->cmds[k].aborting) {
                cand->cmds[k].aborting = true;
                q = cand;
                cid = k;
                break;
            }
        }
        spin_unlock_irqrestore(&cand->lock, s);
    }
    if (q == NULL)
        return;   /* completed meanwhile, or already being aborted */
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_ADMIN_ABORT;
    sqe.cdw10 = ((uint32_t)cid << 16) | q->qid;
    uint32_t result = 1;
    int rc = admin_cmd(c, &sqe, &result);
    if (rc == -ETIMEDOUT) {
        /* The Abort itself got no answer: it may still be executed later
         * against this id, so the slot cannot be released. A controller
         * that does not answer an Abort within 5 s is reset. */
        controller_die(c, "an Abort command did not complete");
        return;
    }
    if (__atomic_load_n(&c->dead, __ATOMIC_ACQUIRE))
        return;   /* reset meanwhile: controller_die released every slot */
    /* From here the Abort has completed (accepted, refused with an error
     * status, or never issued: -EBUSY); whatever it says, nothing names the
     * id any more, and a refused Abort of a request that finished on its
     * own is not a failure of the controller. */
    bool done = false;
    if (rc == 0 && (result & 1) == 0) {
        /* The controller says it aborted: its completion for the command
         * arrives with Command Abort Requested. Give it a moment. */
        for (unsigned waited = 0; waited < 1000 && !done; waited++) {
            arch_irq_state_t s = spin_lock_irqsave(&q->lock);
            done = q->cmds[cid].bio == NULL;
            spin_unlock_irqrestore(&q->lock, s);
            if (!done)
                thread_sleep_ms(1);
        }
    } else {
        arch_irq_state_t s = spin_lock_irqsave(&q->lock);
        done = q->cmds[cid].bio == NULL;   /* it may have completed on its own while we asked */
        spin_unlock_irqrestore(&q->lock, s);
    }
    if (done) {
        /* The slot was held for the Abort's sake; release it now. */
        arch_irq_state_t s = spin_lock_irqsave(&q->lock);
        if (q->cmds[cid].aborting && q->cmds[cid].bio == NULL)
            slot_put(q, cid);
        spin_unlock_irqrestore(&q->lock, s);
        return;
    }
    controller_die(c, "a request did not complete and could not be aborted");
}

static void nvme_release(struct blkdev *bd)
{
    struct nvme_ns *ns = bd->priv;
    kfree(ns);
}

/* Test hook: Identify Controller into `addr`. With `addr` unmapped in the
 * device's IOMMU domain the unit refuses the write; QEMU's controller does
 * not notice and completes the command anyway. Either way it stays usable. */
static int nvme_debug_dma(struct blkdev *bd, uint64_t addr)
{
    struct nvme_ns *ns = bd->priv;
    return identify(ns->ctrl, 1, 0, NULL, addr);
}

static const struct blkdev_ops nvme_ops = {
    .submit = nvme_submit,
    .release = nvme_release,
    .timeout = nvme_timeout,
    .debug_dma = nvme_debug_dma,
};

/* --- probe ---------------------------------------------------------------------- */

static int add_namespace(struct nvme_ctrl *c, uint32_t nsid, uint8_t *id, dma_addr_t id_dma)
{
    int rc = identify(c, NVME_CNS_NAMESPACE, nsid, id, id_dma);
    if (rc)
        return rc;
    uint64_t nsze;
    memcpy(&nsze, id + NVME_ID_NS_NSZE, sizeof(nsze));
    unsigned flbas = id[NVME_ID_NS_FLBAS] & 0xf;
    uint16_t ms;
    memcpy(&ms, id + NVME_ID_NS_LBAF + 4 * flbas, sizeof(ms));
    unsigned lbads = id[NVME_ID_NS_LBAF + 4 * flbas + 2];
    if (nsze == 0 || ms != 0 || lbads < 9 || lbads > 12) {
        kwarn("nvme%u: namespace %u refused (blocks %llu, metadata %u, block size 2^%u)", c->index, nsid,
              (unsigned long long)nsze, ms, lbads);
        return -ENOTSUP;
    }
    struct nvme_ns *ns = kzalloc(sizeof(*ns));
    if (ns == NULL)
        return -ENOMEM;
    ns->ctrl = c;
    ns->nsid = nsid;
    ns->lba_shift = lbads;
    ns->bd.dev = &c->pdev->dev;
    ns->bd.ops = &nvme_ops;
    ns->bd.sector_size = 1u << lbads;
    ns->bd.capacity = nsze;
    ns->bd.max_sectors = (c->max_pages * (unsigned)PAGE_SIZE) >> lbads;
    ns->bd.max_segments = c->max_pages;
    ns->bd.nr_queues = c->nr_ioq;
    ns->bd.priv = ns;
    char name[BLKDEV_NAME_MAX];
    ksnprintf(name, sizeof(name), "nvme%un%u", c->index, nsid);
    rc = blk_register_named(&ns->bd, name);
    if (rc) {
        kfree(ns);
        return rc;
    }
    ns->registered = true;
    list_push_back(&c->namespaces, &ns->link);
    return 0;
}

static void free_queues(struct nvme_ctrl *c)
{
    for (unsigned i = 0; i < c->nr_ioq; i++) {
        if (c->ioq[i]) {
            queue_free(c, c->ioq[i]);
            kfree(c->ioq[i]);
            c->ioq[i] = NULL;
        }
    }
    c->nr_ioq = 0;
    queue_free(c, &c->admin);
}

static int nvme_probe(struct pci_device *pdev, const struct pci_id *id)
{
    (void)id;
    if (pdev->prog_if != 0x02)
        return -ENODEV;   /* class 01:08 without the NVMe programming interface */
    struct nvme_ctrl *c = kzalloc(sizeof(*c));
    if (c == NULL)
        return -ENOMEM;
    c->pdev = pdev;
    c->index = g_next_index;
    list_init(&c->namespaces);
    mutex_init(&c->admin_lock, "nvme-admin");
    pci_enable_device(pdev, true);
    dma_set_mask(&pdev->dev, 64);
    c->bar = pci_map_bar(pdev, 0);
    int rc = -EIO;
    uint8_t *idbuf = NULL;
    dma_addr_t id_dma = 0;
    if (c->bar == 0) {
        kerror("nvme: %s: cannot map BAR0", pdev->dev.name);
        goto fail;
    }
    c->cap = rd64(c, NVME_REG_CAP);
    c->dstrd = NVME_CAP_DSTRD(c->cap);
    if (!NVME_CAP_CSS_NVM(c->cap) || NVME_CAP_MPSMIN(c->cap) != 0) {
        kerror("nvme: %s: unsupported controller (CAP 0x%llx)", pdev->dev.name, (unsigned long long)c->cap);
        goto fail;
    }
    unsigned mqes = NVME_CAP_MQES(c->cap) + 1;

    /* Disable, program the admin queues, enable. */
    if (rd32(c, NVME_REG_CSTS) & NVME_CSTS_RDY) {
        wr32(c, NVME_REG_CC, rd32(c, NVME_REG_CC) & ~NVME_CC_EN);
        if (wait_ready(c, false)) {
            kerror("nvme: %s: controller will not disable", pdev->dev.name);
            goto fail;
        }
    }
    rc = queue_alloc(c, &c->admin, 0, (uint16_t)(NVME_ADMIN_DEPTH < mqes ? NVME_ADMIN_DEPTH : mqes));
    if (rc)
        goto fail;
    wr32(c, NVME_REG_AQA, ((uint32_t)(c->admin.depth - 1) << 16) | (uint32_t)(c->admin.depth - 1));
    wr64(c, NVME_REG_ASQ, c->admin.sq_dma);
    wr64(c, NVME_REG_ACQ, c->admin.cq_dma);
    wr32(c, NVME_REG_CC, NVME_CC_IOSQES | NVME_CC_IOCQES | NVME_CC_CSS_NVM | NVME_CC_MPS_4K | NVME_CC_AMS_RR | NVME_CC_EN);
    rc = wait_ready(c, true);
    if (rc) {
        kerror("nvme: %s: controller did not become ready (%d)", pdev->dev.name, rc);
        goto fail;
    }

    /* Vectors: 0 for the admin queue, one per I/O queue. */
    unsigned want_ioq = cpu_count();
    if (want_ioq > NVME_MAX_IOQ)
        want_ioq = NVME_MAX_IOQ;
    int granted = pci_msix_enable(pdev, 1 + want_ioq);
    if (granted < 2) {
        kerror("nvme: %s: MSI-X unavailable (%d)", pdev->dev.name, granted);
        rc = granted < 0 ? granted : -ENODEV;
        goto fail_disable;
    }
    c->admin.vector = pci_msix_request(pdev, 0, nvme_irq, &c->admin, "nvme-admin", 0);
    if (c->admin.vector < 0) {
        rc = c->admin.vector;
        goto fail_msix;
    }
    c->admin.cpu = 0;

    idbuf = dma_alloc(&pdev->dev, PAGE_SIZE, &id_dma, DMA_ZERO);
    if (idbuf == NULL) {
        rc = -ENOMEM;
        goto fail_msix;
    }
    rc = identify(c, NVME_CNS_CONTROLLER, 0, idbuf, id_dma);
    if (rc) {
        kerror("nvme: %s: Identify Controller failed (%d)", pdev->dev.name, rc);
        goto fail_msix;
    }
    memcpy(c->serial, idbuf + NVME_ID_CTRL_SN, 20);
    memcpy(c->model, idbuf + NVME_ID_CTRL_MN, 40);
    for (int i = 19; i >= 0 && c->serial[i] == ' '; i--)
        c->serial[i] = '\0';
    for (int i = 39; i >= 0 && c->model[i] == ' '; i--)
        c->model[i] = '\0';
    unsigned mdts = idbuf[NVME_ID_CTRL_MDTS];
    c->max_pages = mdts == 0 || mdts >= 7 ? NVME_MAX_PAGES : (1u << mdts);
    if (c->max_pages > NVME_MAX_PAGES)
        c->max_pages = NVME_MAX_PAGES;
    uint32_t nn;
    memcpy(&nn, idbuf + NVME_ID_CTRL_NN, sizeof(nn));

    /* I/O queues: ask, take what is granted, create one per CPU. */
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_ADMIN_SET_FEATURES;
    sqe.cdw10 = NVME_FEAT_NUM_QUEUES;
    sqe.cdw11 = ((uint32_t)(want_ioq - 1) << 16) | (want_ioq - 1);
    uint32_t nq = 0;
    rc = admin_cmd(c, &sqe, &nq);
    if (rc) {
        kerror("nvme: %s: Set Features (queues) failed (%d)", pdev->dev.name, rc);
        goto fail_msix;
    }
    unsigned sq_ok = (nq & 0xffff) + 1, cq_ok = (nq >> 16) + 1;
    unsigned nr = want_ioq;
    if (nr > sq_ok)
        nr = sq_ok;
    if (nr > cq_ok)
        nr = cq_ok;
    if (nr > (unsigned)granted - 1)
        nr = (unsigned)granted - 1;
    uint16_t depth = (uint16_t)(NVME_IO_DEPTH < mqes ? NVME_IO_DEPTH : mqes);
    for (unsigned i = 0; i < nr; i++) {
        struct nvme_queue *q = kzalloc(sizeof(*q));
        if (q == NULL) {
            rc = -ENOMEM;
            goto fail_queues;
        }
        c->ioq[i] = q;
        c->nr_ioq = i + 1;
        rc = queue_alloc(c, q, (uint16_t)(i + 1), depth);
        if (rc)
            goto fail_queues;
        q->cpu = i % cpu_count();
        q->vector = pci_msix_request(pdev, i + 1, nvme_irq, q, "nvme-ioq", q->cpu);
        if (q->vector < 0) {
            rc = q->vector;
            goto fail_queues;
        }
        rc = create_ioq(c, q);
        if (rc) {
            kerror("nvme: %s: cannot create I/O queue %u (%d)", pdev->dev.name, i + 1, rc);
            goto fail_queues;
        }
    }

    /* Namespaces. */
    rc = identify(c, NVME_CNS_ACTIVE_LIST, 0, idbuf, id_dma);
    if (rc)
        goto fail_queues;
    uint32_t list[1024];
    memcpy(list, idbuf, sizeof(list));
    unsigned added = 0;
    for (unsigned i = 0; i < 1024 && list[i] != 0; i++) {
        if (add_namespace(c, list[i], idbuf, id_dma) == 0)
            added++;
    }
    dma_free(&pdev->dev, PAGE_SIZE, idbuf, id_dma);
    idbuf = NULL;
    pdev->dev.drvdata = c;
    g_next_index++;
    kinfo("nvme%u: %s: %s (%s), %u namespace(s) of %u, %u I/O queue(s) of depth %u, %u KiB per request",
          c->index, pdev->dev.name, c->model, c->serial, added, nn, c->nr_ioq, depth,
          (unsigned)(c->max_pages * PAGE_SIZE / 1024));
    return 0;

fail_queues:
    for (unsigned i = 0; i < c->nr_ioq; i++)
        if (c->ioq[i] && c->ioq[i]->vector >= 0)
            pci_msix_release(pdev, i + 1);
fail_msix:
    pci_msix_disable(pdev);
fail_disable:
    wr32(c, NVME_REG_CC, rd32(c, NVME_REG_CC) & ~NVME_CC_EN);
    wait_ready(c, false);
fail:
    if (idbuf)
        dma_free(&pdev->dev, PAGE_SIZE, idbuf, id_dma);
    free_queues(c);
    if (c->bar)
        device_unmap_mmio(c->bar);
    kfree(c);
    return rc;
}

static void nvme_remove(struct pci_device *pdev)
{
    struct nvme_ctrl *c = pdev->dev.drvdata;
    if (c == NULL)
        return;
    struct nvme_ns *ns, *tmp;
    list_for_each_entry_safe(ns, tmp, &c->namespaces, link)
        blk_unregister(&ns->bd);   /* no submit is inside the driver after this */
    controller_die(c, "device removed");
    /* The vectors: released by pci_msix_disable; a handler still running
     * elsewhere finishes before the queues go (synchronize_irq). */
    int vectors[NVME_MAX_IOQ + 1];
    unsigned nv = 0;
    vectors[nv++] = c->admin.vector;
    for (unsigned i = 0; i < c->nr_ioq; i++)
        vectors[nv++] = c->ioq[i]->vector;
    pci_msix_disable(pdev);
    for (unsigned i = 0; i < nv; i++)
        if (vectors[i] >= 0)
            synchronize_irq((unsigned)vectors[i]);
    free_queues(c);
    list_for_each_entry_safe(ns, tmp, &c->namespaces, link) {
        list_remove(&ns->link);
        blkdev_put(&ns->bd);   /* the creator's reference; nvme_release frees when the holders are gone */
    }
    device_unmap_mmio(c->bar);
    pdev->dev.drvdata = NULL;
    kfree(c);
}

static const struct pci_id nvme_ids[] = {
    { PCI_ANY, PCI_ANY, 0x01, 0x08, PCI_ID_CLASS },
    PCI_ID_END,
};

static struct pci_driver nvme_driver = {
    .drv = { .name = "nvme" },
    .ids = nvme_ids,
    .probe = nvme_probe,
    .remove = nvme_remove,
};

static int nvme_module_init(void)
{
    return pci_register_driver(&nvme_driver);
}

static void nvme_module_shutdown(void)
{
    pci_unregister_driver(&nvme_driver);
}

COSMO_MODULE("nvme", "1.0", nvme_module_init, nvme_module_shutdown, "", MODULE_CAP_DRIVER);
