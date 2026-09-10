/*
 * vq.c - The device-side split-virtqueue walk (vq.h), shared by every
 * virtio device the owner models.
 */
#include "vq.h"

static int read_u16(const struct vq_io *io, uint64_t gpa, uint16_t *out)
{
    return io->read_guest(io->ctx, gpa, out, 2);
}

int vq_read_desc(const struct vq_io *io, const struct vq_queue *q, uint16_t i, struct vq_desc *d)
{
    if (i >= q->size)
        return -1;
    return io->read_guest(io->ctx, q->desc_gpa + (uint64_t)i * 16u, d, sizeof(*d));
}

int vq_process(const struct vq_io *io, struct vq_queue *q, vq_serve_fn serve, void *sctx)
{
    if (!q->ready || q->size == 0 || q->size > VQ_MAX)
        return -1;
    uint16_t avail_idx;
    if (read_u16(io, q->avail_gpa + 2u, &avail_idx) != 0)   /* avail->idx */
        return -1;
    /* A driver can expose at most q->size buffers before the device consumes
     * one -- there are only that many descriptors. A larger gap between
     * avail->idx and what we last saw is a fatal driver error, not work to
     * do: refuse it, so one notification cannot drive up to 65535 chains and
     * monopolize the owner. */
    if ((uint16_t)(avail_idx - q->last_avail) > q->size)
        return -1;
    int served = 0;
    uint64_t call_bytes = 0;                     /* work done this notification */
    while (q->last_avail != avail_idx) {
        /* Stop before starting a request that would take this notification
         * past its work ceiling; the rest of the backlog is served on the
         * next notify, so one kick cannot monopolize the owner's thread even
         * with a ring full of maximal (or overlapping) requests. */
        if (io->max_bytes_per_call && call_bytes >= io->max_bytes_per_call)
            break;
        uint16_t slot = q->last_avail % q->size;
        uint16_t head;
        /* A fault anywhere in the walk stops it and is reported as failure
         * (-1), never as the count served so far: a caller that drains on a
         * positive return would otherwise replay the faulting request and
         * re-raise its interrupt forever, since last_avail has not advanced
         * past it. Requests completed before the fault already hold their
         * used-ring entries; they are simply not counted on this call. */
        if (read_u16(io, q->avail_gpa + 4u + (uint64_t)slot * 2u, &head) != 0)   /* avail->ring[slot] */
            return -1;
        uint32_t used_len = 0;
        uint64_t work = 0;
        if (serve(sctx, io, q, head, &used_len, &work) != 0)
            return -1;
        /* used->ring[used_idx % size] = { head, used_len } at used_gpa + 4 + 8*slot,
         * then used->idx. The device's own used_idx and last_avail advance
         * only once both writes land: if the index write faults, this request
         * is not counted as published (no phantom interrupt, no advance) and
         * the next call re-publishes it to the same slot -- idempotent, not a
         * second entry with the ring left inconsistent. */
        uint16_t uslot = q->used_idx % q->size;
        uint32_t elem[2] = { head, used_len };
        if (io->write_guest(io->ctx, q->used_gpa + 4u + (uint64_t)uslot * 8u, elem, 8) != 0)
            return -1;
        uint16_t next_used = (uint16_t)(q->used_idx + 1);
        if (io->write_guest(io->ctx, q->used_gpa + 2u, &next_used, 2) != 0)   /* used->idx */
            return -1;
        q->used_idx = next_used;
        q->last_avail++;
        served++;
        call_bytes += work;
    }
    return served;
}
