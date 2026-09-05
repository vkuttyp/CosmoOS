/*
 * aio.c - The asynchronous I/O ring (docs/kernel/io/design.md).
 *
 * Submission executes what would not block and parks the rest; aio_wait
 * runs parked entries as their objects become ready, sleeping on the
 * ring's queue and on every parked object's poll_wq at once (the
 * wait_event protocol over many queues). Every copy to or from user memory
 * happens in the submitting process's own thread, inside these two calls.
 */

#include <kernel/aio.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

struct aio_req {
    struct cosmo_sqe sqe;
    struct kobject *obj;           /* referenced; NULL for NOP */
    struct wait_entry we;          /* on `wq` while the waiter sleeps */
    struct waitqueue *wq;          /* the object's poll_wq for `want`, or NULL */
    unsigned want;                 /* COSMO_IO_READABLE or WRITABLE */
    bool prepared;                 /* `we` is queued on `wq` right now */
    struct list_node link;
};

static void aio_release(struct kobject *obj);
static unsigned aio_ready(struct kobject *obj);
static struct waitqueue *aio_poll_wq(struct kobject *obj, unsigned events);

static const struct kobject_io_type aio_type = {
    .base = { .name = "aio", .release = aio_release, .flags = KOBJECT_TYPE_IO },
    .ready = aio_ready,
    .poll_wq = aio_poll_wq,
};

struct aio_ring *aio_ring_from_kobject(struct kobject *obj)
{
    return obj != NULL && obj->type == &aio_type.base ? container_of(obj, struct aio_ring, obj) : NULL;
}

int aio_ring_create(unsigned entries, unsigned flags, struct aio_ring **out)
{
    if (entries == 0 || entries > COSMO_AIO_MAX_ENTRIES || flags != 0)
        return -EINVAL;
    struct aio_ring *r = kzalloc(sizeof(*r));
    if (r == NULL)
        return -ENOMEM;
    r->cq = kzalloc((size_t)entries * sizeof(*r->cq));
    if (r->cq == NULL) {
        kfree(r);
        return -ENOMEM;
    }
    kobject_init(&r->obj, &aio_type.base);
    mutex_init(&r->lock, "aio");
    list_init(&r->parked);
    waitqueue_init(&r->wait, "aio");
    r->entries = entries;
    r->owner = process_current()->pid;
    *out = r;
    return 0;
}

static void req_free(struct aio_req *q)
{
    if (q->obj)
        kobject_put(q->obj);
    kfree(q);
}

static void aio_release(struct kobject *obj)
{
    struct aio_ring *r = container_of(obj, struct aio_ring, obj);
    struct aio_req *q, *tmp;
    list_for_each_entry_safe(q, tmp, &r->parked, link) {
        list_remove(&q->link);
        req_free(q);   /* parked entries are dropped, never executed */
    }
    kfree(r->cq);
    kfree(r);
}

/* Lock held. */
static void cq_push(struct aio_ring *r, uint64_t user_data, int64_t result)
{
    KASSERT(r->cq_len < r->entries);
    r->cq[(r->cq_head + r->cq_len) % r->entries] = (struct cosmo_cqe){ .user_data = user_data, .result = result };
    r->cq_len++;
    r->completed++;
    waitqueue_wake_all(&r->wait);
}

static unsigned aio_ready(struct kobject *obj)
{
    struct aio_ring *r = container_of(obj, struct aio_ring, obj);
    return __atomic_load_n(&r->cq_len, __ATOMIC_ACQUIRE) ? COSMO_IO_READABLE : 0;
}

static struct waitqueue *aio_poll_wq(struct kobject *obj, unsigned events)
{
    (void)events;
    return &container_of(obj, struct aio_ring, obj)->wait;
}

/* --- entries ------------------------------------------------------------------- */

static bool op_needs_buffer(uint8_t op)
{
    return op == COSMO_AIO_READ || op == COSMO_AIO_WRITE || op == COSMO_AIO_PREAD || op == COSMO_AIO_PWRITE;
}

static unsigned op_rights(uint8_t op)
{
    switch (op) {
    case COSMO_AIO_READ:
    case COSMO_AIO_PREAD:
    case COSMO_AIO_POLL:
        return HANDLE_RIGHT_READ;
    case COSMO_AIO_WRITE:
    case COSMO_AIO_PWRITE:
    case COSMO_AIO_FSYNC:
        return HANDLE_RIGHT_WRITE;
    default:
        return 0;
    }
}

/* Would the entry complete without waiting for readiness? */
static bool runnable(const struct aio_req *q)
{
    if (q->obj == NULL || q->sqe.op == COSMO_AIO_FSYNC)
        return true;
    const struct kobject_io_type *io = kobject_io_of(q->obj);
    if (io == NULL || io->ready == NULL)
        return true;   /* a file: always */
    unsigned bits = io->ready(q->obj);
    if (q->sqe.op == COSMO_AIO_POLL)
        return (bits & q->sqe.events) != 0;
    return (bits & (q->want | COSMO_IO_HANGUP | COSMO_IO_ERROR)) != 0;
}

/* Execute in the caller's thread with object waits turned into -EAGAIN. */
static int64_t run(struct aio_req *q)
{
    const struct cosmo_sqe *e = &q->sqe;
    struct thread *t = thread_current();
    int64_t rc;
    t->io_nonblock = true;
    switch (e->op) {
    case COSMO_AIO_NOP:
        rc = 0;
        break;
    case COSMO_AIO_READ:
        rc = syscall_obj_read(q->obj, e->addr, (size_t)e->len);
        break;
    case COSMO_AIO_WRITE:
        rc = syscall_obj_write(q->obj, e->addr, (size_t)e->len);
        break;
    case COSMO_AIO_PREAD:
    case COSMO_AIO_PWRITE: {
        struct file *f = file_from_kobject(q->obj);
        if (f == NULL) {
            rc = -ESPIPE;
            break;
        }
        char tmp[1024];
        size_t done = 0, len = (size_t)e->len;
        rc = 0;
        while (done < len) {
            size_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
            if (e->op == COSMO_AIO_PWRITE) {
                if (copy_from_user(tmp, e->addr + done, n)) {
                    rc = -EFAULT;
                    break;
                }
                rc = file_pwrite(f, tmp, n, e->offset + done);
            } else {
                rc = file_pread(f, tmp, n, e->offset + done);
                if (rc > 0 && copy_to_user(e->addr + done, tmp, (size_t)rc))
                    rc = -EFAULT;
            }
            if (rc <= 0)
                break;
            done += (size_t)rc;
            if ((size_t)rc < n)
                break;
        }
        if (done > 0)
            rc = (int64_t)done;
        break;
    }
    case COSMO_AIO_FSYNC: {
        struct file *f = file_from_kobject(q->obj);
        rc = f ? file_sync(f) : -EINVAL;
        break;
    }
    case COSMO_AIO_POLL:
        rc = (int64_t)(kobject_ready(q->obj) & e->events);
        break;
    default:
        rc = -EINVAL;
        break;
    }
    t->io_nonblock = false;
    return rc;
}

/* Lock held. Run or park a validated entry. */
static void dispatch(struct aio_ring *r, struct aio_req *q, bool at_submit)
{
    if (runnable(q)) {
        int64_t rc = run(q);
        if (rc == -EAGAIN && !(q->sqe.flags & COSMO_AIO_F_NOWAIT) && q->obj) {
            /* Readiness vanished between the check and the call: park. */
        } else {
            cq_push(r, q->sqe.user_data, rc);
            if (at_submit)
                r->executed_at_submit++;
            req_free(q);
            return;
        }
    } else if (q->sqe.flags & COSMO_AIO_F_NOWAIT) {
        cq_push(r, q->sqe.user_data, -EAGAIN);
        req_free(q);
        return;
    }
    q->wq = kobject_poll_wq(q->obj, q->want);
    list_push_back(&r->parked, &q->link);
    r->nr_parked++;
    r->parked_total++;
}

int64_t aio_submit(struct aio_ring *r, uint64_t usqes, unsigned n)
{
    if (r->owner != process_current()->pid)
        return -EPERM;
    if (n == 0)
        return 0;
    if (!user_range_ok(usqes, (size_t)n * sizeof(struct cosmo_sqe)))
        return -EFAULT;
    struct handle_table *ht = &process_current()->handles;
    unsigned accepted = 0;
    mutex_lock(&r->lock);
    for (; accepted < n; accepted++) {
        if (r->nr_parked + r->cq_len >= r->entries)
            break;   /* full: the caller collects completions and retries */
        struct aio_req *q = kzalloc(sizeof(*q));
        if (q == NULL)
            break;
        if (copy_from_user(&q->sqe, usqes + (uint64_t)accepted * sizeof(q->sqe), sizeof(q->sqe))) {
            kfree(q);
            if (accepted == 0) {
                mutex_unlock(&r->lock);
                return -EFAULT;
            }
            break;
        }
        list_init(&q->link);
        wait_entry_init(&q->we);
        r->submitted++;
        const struct cosmo_sqe *e = &q->sqe;
        int64_t err = 0;
        if (e->op > COSMO_AIO_POLL || (e->flags & ~COSMO_AIO_F_NOWAIT) || (e->op == COSMO_AIO_POLL && e->events == 0))
            err = -EINVAL;
        else if (op_needs_buffer(e->op) && !user_range_ok(e->addr, (size_t)e->len))
            err = -EFAULT;
        else if (e->op != COSMO_AIO_NOP) {
            q->obj = handle_lookup(ht, e->handle, op_rights(e->op));
            if (q->obj == NULL)
                err = -EBADF;
        }
        if (err) {
            cq_push(r, e->user_data, err);
            req_free(q);
            continue;
        }
        q->want = (e->op == COSMO_AIO_WRITE || e->op == COSMO_AIO_PWRITE) ? COSMO_IO_WRITABLE
                  : e->op == COSMO_AIO_POLL                                 ? (unsigned)e->events
                                                                            : COSMO_IO_READABLE;
        dispatch(r, q, true);
    }
    /* A waiter asleep in aio_wait (another thread) re-arms on every
     * wake, so a newly parked entry joins the queues it sleeps on. */
    if (r->nr_parked)
        waitqueue_wake_all(&r->wait);
    mutex_unlock(&r->lock);
    if (accepted == 0)
        return -EBUSY;
    return (int64_t)accepted;
}

/* Lock held. Run every parked entry whose object is ready now. */
static void run_parked(struct aio_ring *r)
{
    struct aio_req *q, *tmp;
    list_for_each_entry_safe(q, tmp, &r->parked, link) {
        if (q->prepared || !runnable(q))
            continue;
        list_remove(&q->link);
        list_init(&q->link);
        r->nr_parked--;
        dispatch(r, q, false);   /* may park it again on -EAGAIN */
    }
}

/* Lock held. */
static bool any_runnable(struct aio_ring *r)
{
    struct aio_req *q;
    list_for_each_entry(q, &r->parked, link)
        if (runnable(q))
            return true;
    return false;
}

struct aio_alarm {
    struct thread *thread;
    volatile bool fired;
};

static void alarm_fired(struct timer *t, void *arg)
{
    (void)t;
    struct aio_alarm *al = arg;
    __atomic_store_n(&al->fired, true, __ATOMIC_RELEASE);
    sched_wake(al->thread);
}

int64_t aio_wait(struct aio_ring *r, uint64_t ucqes, unsigned n, unsigned min, uint64_t timeout_ns)
{
    if (r->owner != process_current()->pid)
        return -EPERM;
    if (min > n || n > COSMO_AIO_MAX_ENTRIES)
        return -EINVAL;
    if (n && !user_range_ok(ucqes, (size_t)n * sizeof(struct cosmo_cqe)))
        return -EFAULT;

    struct aio_alarm alarm = { .thread = thread_current(), .fired = false };
    struct timer timer;
    bool armed = false;
    if (min > 0 && timeout_ns != COSMO_AIO_WAIT_FOREVER) {
        if (timeout_ns == 0)
            alarm.fired = true;   /* poll */
        else {
            timer_setup(&timer, alarm_fired, &alarm);
            timer_start(&timer, timeout_ns);
            armed = true;
        }
    }
    struct wait_entry ring_we;
    wait_entry_init(&ring_we);
    int rc = 0;

    mutex_lock(&r->lock);
    for (;;) {
        run_parked(r);
        if (min == 0 || r->cq_len >= min || __atomic_load_n(&alarm.fired, __ATOMIC_ACQUIRE))
            break;
        if (process_kill_pending()) {
            rc = -EINTR;
            break;
        }
        /* Arm every wake source, then decide under the lock. */
        waitqueue_prepare(&r->wait, &ring_we);
        struct aio_req *q;
        list_for_each_entry(q, &r->parked, link) {
            if (q->wq) {
                waitqueue_prepare(q->wq, &q->we);
                q->prepared = true;
            }
        }
        bool sleep = r->cq_len < min && !any_runnable(r) && !__atomic_load_n(&alarm.fired, __ATOMIC_ACQUIRE) &&
                     !process_kill_pending();
        mutex_unlock(&r->lock);
        if (sleep)
            sched_block_current();
        mutex_lock(&r->lock);
        waitqueue_finish(&r->wait, &ring_we);
        list_for_each_entry(q, &r->parked, link) {
            if (q->prepared) {
                waitqueue_finish(q->wq, &q->we);
                q->prepared = false;
            }
        }
    }
    unsigned out = 0;
    if (rc == 0 || r->cq_len > 0) {
        rc = 0;
        while (out < n && r->cq_len > 0) {
            struct cosmo_cqe c = r->cq[r->cq_head];
            if (copy_to_user(ucqes + (uint64_t)out * sizeof(c), &c, sizeof(c))) {
                rc = out ? 0 : -EFAULT;
                break;
            }
            r->cq_head = (r->cq_head + 1) % r->entries;
            r->cq_len--;
            out++;
        }
    }
    mutex_unlock(&r->lock);
    if (armed)
        timer_cancel_sync(&timer);
    return rc ? rc : (int64_t)out;
}
