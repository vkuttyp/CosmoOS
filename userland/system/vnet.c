/*
 * vnet.c - The virtio-net `serve`s over the shared virtqueue walk
 * (vnet.h, vq.h): transmit drains posted frames to the wire, receive fills
 * posted buffers from the wire.
 */
#include "vnet.h"
#include <string.h>

/* Transmit (queue 1): the chain is one or more device-readable buffers
 * holding a virtio_net_hdr then the frame. Gather it, strip the header, and
 * hand the frame to the wire. There is no status byte -- net queues carry
 * frames, not requests. */
static int tx_serve(void *sctx, const struct vq_io *io, const struct vq_queue *q,
                    uint16_t head, uint32_t *used_len, uint64_t *work)
{
    struct vnet_io *nio = sctx;
    uint8_t stage[VNET_BUF_MAX];
    uint32_t total = 0;
    struct vq_desc d;
    if (vq_read_desc(io, q, head, &d) != 0)
        return -1;
    unsigned steps = 0;
    for (;;) {
        if (d.flags & VQ_DESC_F_WRITE)
            return -1;                               /* a transmit buffer is device-readable */
        if (d.len > VNET_BUF_MAX - total)
            return -1;                               /* header + frame past the max: a driver error */
        if (d.len && io->read_guest(io->ctx, d.addr, stage + total, d.len) != 0)
            return -1;                               /* a buffer that points out of the guest */
        total += d.len;
        if (!(d.flags & VQ_DESC_F_NEXT))
            break;
        if (++steps > q->size)
            return -1;                               /* a chain longer than the ring: a loop */
        if (vq_read_desc(io, q, d.next, &d) != 0)
            return -1;
    }
    if (total < VNET_HDR_LEN)
        return -1;                                   /* no room even for the header */
    if (nio->wire_tx(io->ctx, stage + VNET_HDR_LEN, total - VNET_HDR_LEN) != 0)
        return -1;
    *used_len = 0;                                   /* transmit returns nothing to the guest */
    *work = total;
    return 1;
}

/* Receive (queue 0): a pull queue. The chain is one or more device-writable
 * buffers. Measure their capacity first, take a frame off the wire only if
 * there is room, then write a zeroed virtio_net_hdr and the frame across
 * them. When the wire is empty, leave the buffer for next time (return 0). */
static int rx_serve(void *sctx, const struct vq_io *io, const struct vq_queue *q,
                    uint16_t head, uint32_t *used_len, uint64_t *work)
{
    struct vnet_io *nio = sctx;

    /* pass 1: the chain's device-writable capacity, so a frame is pulled off
     * the wire only when there is somewhere to put it. */
    uint32_t cap = 0;
    struct vq_desc d;
    if (vq_read_desc(io, q, head, &d) != 0)
        return -1;
    unsigned steps = 0;
    for (;;) {
        if (!(d.flags & VQ_DESC_F_WRITE))
            return -1;                               /* a receive buffer is device-writable */
        if (cap < VNET_BUF_MAX)
            cap += d.len < VNET_BUF_MAX - cap ? d.len : VNET_BUF_MAX - cap;
        if (!(d.flags & VQ_DESC_F_NEXT))
            break;
        if (++steps > q->size)
            return -1;
        if (vq_read_desc(io, q, d.next, &d) != 0)
            return -1;
    }
    if (cap < VNET_HDR_LEN)
        return -1;                                   /* cannot hold even a header: a driver error */

    uint8_t stage[VNET_BUF_MAX];
    int framelen = nio->wire_rx(io->ctx, stage + VNET_HDR_LEN, cap - VNET_HDR_LEN);
    if (framelen <= 0)
        return 0;                                    /* nothing on the wire: leave the buffer */
    memset(stage, 0, VNET_HDR_LEN);                  /* the header: no offloads, one buffer */
    uint32_t total = VNET_HDR_LEN + (uint32_t)framelen;

    /* pass 2: scatter the header+frame across the writable buffers. */
    if (vq_read_desc(io, q, head, &d) != 0)
        return -1;
    uint32_t done = 0;
    steps = 0;
    while (done < total) {
        uint32_t chunk = d.len < total - done ? d.len : total - done;
        if (chunk && io->write_guest(io->ctx, d.addr, stage + done, chunk) != 0)
            return -1;
        done += chunk;
        if (done >= total)
            break;
        if (!(d.flags & VQ_DESC_F_NEXT))
            break;                                   /* cannot happen: cap >= total */
        if (++steps > q->size)
            return -1;
        if (vq_read_desc(io, q, d.next, &d) != 0)
            return -1;
    }
    *used_len = total;                               /* header + frame written to the guest */
    *work = (uint32_t)framelen;
    return 1;
}

int vnet_process_tx(struct vnet_io *io, struct vq_queue *q)
{
    struct vq_io vio = { io->read_guest, io->write_guest, io->ctx, io->max_bytes_per_call };
    return vq_process(&vio, q, tx_serve, io);
}

int vnet_process_rx(struct vnet_io *io, struct vq_queue *q)
{
    struct vq_io vio = { io->read_guest, io->write_guest, io->ctx, io->max_bytes_per_call };
    return vq_process(&vio, q, rx_serve, io);
}
