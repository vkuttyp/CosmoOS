/*
 * vq.h - The device side of a split virtqueue, shared by every virtio
 * device the owner models (vblk, vnet, ...).
 *
 * This is the part that is the same whatever the queue carries: the
 * descriptor layout, the available/used rings, and vq_process -- the walk
 * that consumes available and produces used, with the disciplines the
 * block device earned over review (every index bounded by the queue size,
 * a looping chain refused, the available-ring backlog bounded, a
 * per-notification work ceiling, the atomic publish, and a fault reported
 * as failure and never as partial progress). A device supplies only a
 * `serve` callback that walks one head's chain and says how many used
 * bytes and how much work it was; the walk around it is here, once.
 *
 * It touches no system call: guest memory is reached through callbacks, so
 * the same code compiles into vmctl and into the host tests.
 */
#ifndef COSMO_VQ_H
#define COSMO_VQ_H

#include <stddef.h>
#include <stdint.h>

/* The split-virtqueue layout, device's view. Little-endian, which every
 * platform this builds for is. */
struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VQ_DESC_F_NEXT  1u
#define VQ_DESC_F_WRITE 2u

#define VQ_MAX 256u   /* the largest queue the owner offers (QueueNumMax) */

struct vq_queue {
    uint64_t desc_gpa, avail_gpa, used_gpa;   /* where the guest placed the ring */
    uint16_t size;                            /* power of two, <= VQ_MAX */
    uint16_t last_avail;                      /* the next available index to serve */
    uint16_t used_idx;                        /* the device's view of used->idx */
    int ready;
};

/* What the walk needs: guest memory, and the per-notification work ceiling.
 * Move `len` bytes between guest memory at `gpa` and `buf`; 0 ok, <0 a fault
 * (an address outside the guest's regions -- the backstop against a
 * descriptor that points anywhere). `max_bytes_per_call` is the most work
 * one call may do before the rest waits for the next; 0 means unbounded. */
struct vq_io {
    int (*read_guest)(void *ctx, uint64_t gpa, void *buf, uint32_t len);
    int (*write_guest)(void *ctx, uint64_t gpa, const void *buf, uint32_t len);
    void *ctx;
    uint64_t max_bytes_per_call;
};

/* Read descriptor `i`, refusing an index at or past the queue size before it
 * addresses memory. 0 ok, <0 out of range or a fault. */
int vq_read_desc(const struct vq_io *io, const struct vq_queue *q, uint16_t i, struct vq_desc *d);

/*
 * Serve one head: walk its chain and move its data. Returns 0 with
 * `*used_len` set to the bytes to report on the used ring and `*work` to the
 * bytes moved (what counts against the ceiling), or -1 on a hostile or
 * faulting chain -- which stops the whole walk and is never counted as
 * progress.
 */
typedef int (*vq_serve_fn)(void *sctx, const struct vq_io *io, const struct vq_queue *q,
                           uint16_t head, uint32_t *used_len, uint64_t *work);

/*
 * Serve the requests the guest has made available since the last call, at
 * most `io->max_bytes_per_call` of work before deferring the rest. Returns
 * the number served this call (>= 0), each appended to the used ring; the
 * caller raises the device's interrupt when the used ring advanced, and
 * calls again while the return is > 0. Returns -1 and stops on a hostile
 * ring or a fault; requests completed before a mid-walk fault keep their
 * used-ring entries but are not counted on the failing call.
 */
int vq_process(const struct vq_io *io, struct vq_queue *q, vq_serve_fn serve, void *sctx);

#endif /* COSMO_VQ_H */
