/*
 * vnet.h - The device side of a virtio-net device: two `serve`s over the
 * shared virtqueue walk (vq.h).
 *
 * A net device has two queues. Transmit (queue 1) is drain-all: the guest
 * posts frames as device-readable chains and the device hands each to the
 * wire. Receive (queue 0) is a pull queue: the guest posts empty
 * device-writable buffers as a pool, and the device fills one only when a
 * frame arrives from the wire. Both are the same walk as block; what
 * differs is that a served buffer is a frame, prefixed by a (zeroed)
 * virtio_net_hdr, and its direction is fixed by the queue, not read from a
 * request header.
 *
 * It touches no system call: guest memory and the wire are reached through
 * callbacks, so the same code compiles into vmctl and the host test.
 */
#ifndef COSMO_VNET_H
#define COSMO_VNET_H

#include <stddef.h>
#include <stdint.h>

#include "vq.h"

#define VIRTIO_NET_F_MAC     5u    /* config space carries a MAC */
#define VIRTIO_NET_F_VERSION_1 32u /* a modern (non-legacy) device */

#define VNET_HDR_LEN         12u   /* struct virtio_net_hdr, zeroed (no offloads) */
#define VNET_FRAME_MAX       1514u /* a full Ethernet frame, no jumbo */
#define VNET_BUF_MAX         (VNET_HDR_LEN + VNET_FRAME_MAX)
/* The per-notification work ceiling, as block has: the most frame data one
 * QueueNotify serves before the rest waits for the next. */
#define VNET_MAX_BYTES_PER_CALL  (1u << 20)   /* 1 MiB of frames per notification */

struct vnet_io {
    /* Guest memory and the per-notification ceiling, as block's -- a vnet_io
     * builds a vq_io view of these four fields. */
    int (*read_guest)(void *ctx, uint64_t gpa, void *buf, uint32_t len);
    int (*write_guest)(void *ctx, uint64_t gpa, const void *buf, uint32_t len);
    /* The wire. wire_tx takes a transmitted frame (`len` bytes); 0 ok, <0
     * error. wire_rx fills `buf` (up to `max`) with the next frame from the
     * wire and returns its length, 0 if none is waiting, <0 on error. */
    int (*wire_tx)(void *ctx, const void *frame, uint32_t len);
    int (*wire_rx)(void *ctx, void *buf, uint32_t max);
    void *ctx;
    uint64_t max_bytes_per_call;
};

/* Serve the transmit queue (queue 1): every frame the guest has posted goes
 * to the wire. Serve the receive queue (queue 0): frames waiting on the wire
 * fill posted buffers, stopping when the wire is empty or the pool is. Each
 * returns the number of buffers completed this call (>= 0), or -1 on a
 * hostile ring; the caller raises the interrupt when the used ring advanced
 * and, for > 0, calls again. */
int vnet_process_tx(struct vnet_io *io, struct vq_queue *q);
int vnet_process_rx(struct vnet_io *io, struct vq_queue *q);

#endif /* COSMO_VNET_H */
