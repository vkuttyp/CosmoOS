/*
 * signalfd.c - a signalfd as an I/O object (signalfd(2)).
 *
 * A kobject carrying a signal mask, on the eventfd/timerfd readiness template.
 * It becomes readable when a signal in the mask is pending for the reading
 * process or thread, and read() drains those pending signals as
 * signalfd_siginfo records. It holds no process reference: ready/read read the
 * current process (signal_pending_set / signal_consume_mask) and it polls on
 * the process's signalfd_wqh, which the signal path wakes (signal_after_route).
 * The signal core keeps a blocked ignored signal pending so a signalfd can
 * read it. See docs/audit/next-subsystem-signalfd.md.
 *
 * Lifetime: a plain kobject with no process pointer, so a descriptor passed to
 * or inherited by another process simply reports that process's signals.
 */

#include <kernel/compiler.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/signalfd.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

#include "../../compat/linux/linux_abi.h"

#include <stdint.h>

struct signalfd_obj {
    struct kobject obj;
    spinlock_t lock;     /* mask, nonblock */
    uint64_t mask;
    bool nonblock;
};

static struct signalfd_obj *signalfd_of(struct kobject *obj)
{
    return container_of(obj, struct signalfd_obj, obj);
}

/* Build one signalfd_siginfo record from a consumed signal_info, mirroring the
 * lx_siginfo mapping in compat/linux/signal.c. */
static void fill_ssi(struct lx_signalfd_siginfo *ssi, const struct signal_info *info)
{
    memset(ssi, 0, sizeof(*ssi));
    ssi->ssi_signo = (uint32_t)info->sig;
    switch (info->source) {
    case SIGSRC_USER:
        ssi->ssi_code = LX_SI_USER;
        ssi->ssi_pid = info->sender_pid;
        ssi->ssi_uid = info->sender_uid;
        break;
    case SIGSRC_TKILL:
        ssi->ssi_code = LX_SI_TKILL;
        ssi->ssi_pid = info->sender_pid;
        ssi->ssi_uid = info->sender_uid;
        break;
    case SIGSRC_FAULT:
        ssi->ssi_code = info->code ? (int32_t)info->code : 1;
        ssi->ssi_addr = info->fault_addr;
        break;
    case SIGSRC_CHILD:
        /* SIGCHLD: the CLD_* cause, the child's pid/uid, and its exit code or
         * the signal that killed/stopped it, so a parent can tell which child
         * did what from the record alone. */
        ssi->ssi_code = (int32_t)info->code;
        ssi->ssi_pid = info->sender_pid;
        ssi->ssi_uid = info->sender_uid;
        ssi->ssi_status = info->status;
        break;
    case SIGSRC_KERNEL:
    default:
        ssi->ssi_code = LX_SI_KERNEL;
        break;
    }
}

/* A matching signal is pending right now, re-reading the mask so a concurrent
 * mask update takes effect in a blocked read's wait. */
static bool signalfd_has_pending(struct signalfd_obj *sf)
{
    arch_irq_state_t s = spin_lock_irqsave(&sf->lock);
    uint64_t mask = sf->mask;
    spin_unlock_irqrestore(&sf->lock, s);
    return (signal_pending_set() & mask) != 0;
}

static unsigned signalfd_ready(struct kobject *obj)
{
    struct signalfd_obj *sf = signalfd_of(obj);
    arch_irq_state_t s = spin_lock_irqsave(&sf->lock);
    uint64_t mask = sf->mask;
    spin_unlock_irqrestore(&sf->lock, s);
    return (signal_pending_set() & mask) ? COSMO_IO_READABLE : 0;
}

static struct waitqueue *signalfd_poll_wq(struct kobject *obj, unsigned events)
{
    (void)obj;
    (void)events;
    return &process_current()->signalfd_wqh;
}

static int64_t signalfd_read(struct kobject *obj, void *buf, size_t len)
{
    struct signalfd_obj *sf = signalfd_of(obj);
    if (len < sizeof(struct lx_signalfd_siginfo))
        return -EINVAL;
    size_t cap = len / sizeof(struct lx_signalfd_siginfo);

    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&sf->lock);
        uint64_t mask = sf->mask;
        bool nb = io_nonblocking(sf->nonblock);
        spin_unlock_irqrestore(&sf->lock, s);

        struct lx_signalfd_siginfo *recs = buf;
        struct signal_info info;
        bool from_shared;
        struct thread *th = thread_current();
        /* Record what this read consumes, by its set, so a copy fault can put
         * it back (signalfd_read_undo). Reset each attempt, including after a
         * blocking wait, so it names only this read's drained signals. */
        th->sigfd_undo_thread = 0;
        th->sigfd_undo_shared = 0;
        size_t n = 0;
        while (n < cap && signal_consume_mask(mask, &info, &from_shared)) {
            if (from_shared)
                th->sigfd_undo_shared |= SIGMASK(info.sig);
            else
                th->sigfd_undo_thread |= SIGMASK(info.sig);
            fill_ssi(&recs[n++], &info);
        }
        if (n > 0)
            return (int64_t)(n * sizeof(struct lx_signalfd_siginfo));
        if (nb)
            return -EAGAIN;
        /* The predicate re-reads the mask, so a mask update (which wakes this
         * queue) is honoured rather than slept through. */
        int rc = wait_event_killable(&process_current()->signalfd_wqh, signalfd_has_pending(sf));
        if (rc)
            return rc;
    }
}

/* The copy of a read's records to user space faulted: put the signals it
 * drained back into the pending set, so the failed read loses none of them. */
static void signalfd_read_undo(struct kobject *obj)
{
    (void)obj;
    struct thread *t = thread_current();
    signal_reinject_sets(t->sigfd_undo_thread, t->sigfd_undo_shared);
    t->sigfd_undo_thread = 0;
    t->sigfd_undo_shared = 0;
    /* A poller that found nothing and slept while the copy faulted (the
     * signals briefly gone from the pending set) must re-check now. */
    waitqueue_wake_all(&t->proc->signalfd_wqh);
}

static int signalfd_set_nonblock(struct kobject *obj, int on)
{
    struct signalfd_obj *sf = signalfd_of(obj);
    arch_irq_state_t s = spin_lock_irqsave(&sf->lock);
    int prev = sf->nonblock ? 1 : 0;
    if (on == 0 || on == 1)
        sf->nonblock = on;
    spin_unlock_irqrestore(&sf->lock, s);
    return prev;
}

static void signalfd_release(struct kobject *obj)
{
    kfree(signalfd_of(obj));
}

static const struct kobject_io_type signalfd_type = {
    .base = { .name = "signalfd", .release = signalfd_release, .flags = KOBJECT_TYPE_IO },
    .read = signalfd_read,
    .read_undo = signalfd_read_undo,
    .ready = signalfd_ready,
    .set_nonblock = signalfd_set_nonblock,
    .poll_wq = signalfd_poll_wq,
};

int signalfd_obj_create(uint64_t mask, bool nonblock, struct kobject **out)
{
    struct signalfd_obj *sf = kzalloc(sizeof(*sf));
    if (sf == NULL)
        return -ENOMEM;
    kobject_init(&sf->obj, &signalfd_type.base);
    spinlock_init(&sf->lock, "signalfd");
    sf->mask = mask;
    sf->nonblock = nonblock;
    *out = &sf->obj;
    return 0;
}

int signalfd_obj_set_mask(struct kobject *obj, uint64_t mask)
{
    if (kobject_io_of(obj) != &signalfd_type)
        return -EINVAL;
    struct signalfd_obj *sf = signalfd_of(obj);
    arch_irq_state_t s = spin_lock_irqsave(&sf->lock);
    sf->mask = mask;
    spin_unlock_irqrestore(&sf->lock, s);
    /* Wake this process's signalfd waiters: a signal already pending that the
     * new mask now includes must make a blocked read/poll return. */
    waitqueue_wake_all(&process_current()->signalfd_wqh);
    return 0;
}
