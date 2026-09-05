/*
 * aio.h - The asynchronous I/O ring (docs/kernel/io/design.md): a kobject
 * holding parked requests and a completion queue. Entries execute in the
 * submitting process's own threads, at submission when the object is ready
 * and otherwise from aio_wait when its readiness changes.
 */
#ifndef KERNEL_AIO_H
#define KERNEL_AIO_H

#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/process.h>
#include <kernel/object.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

struct aio_ring {
    struct kobject obj;            /* io type "aio": ready = completions waiting; no read/write */
    struct mutex lock;             /* every field below; never held while sleeping for readiness */
    unsigned entries;              /* capacity: parked + completed <= entries */
    struct list_node parked;       /* struct aio_req waiting for readiness, oldest first */
    unsigned nr_parked;
    struct cosmo_cqe *cq;          /* ring buffer of `entries` */
    unsigned cq_head, cq_len;
    struct waitqueue wait;         /* woken on every completion */
    pid_t owner;                   /* the creating process: others get -EPERM */
    uint64_t submitted, completed, parked_total, executed_at_submit;
};

/* A ring with `entries` (1..COSMO_AIO_MAX_ENTRIES) and one reference; flags must be 0. */
int aio_ring_create(unsigned entries, unsigned flags, struct aio_ring **out);
struct aio_ring *aio_ring_from_kobject(struct kobject *obj);

/* The system calls' bodies: `usqes`/`ucqes` are user addresses. Thread
 * context of the owning process. */
int64_t aio_submit(struct aio_ring *r, uint64_t usqes, unsigned n);
int64_t aio_wait(struct aio_ring *r, uint64_t ucqes, unsigned n, unsigned min, uint64_t timeout_ns);

#endif /* KERNEL_AIO_H */
