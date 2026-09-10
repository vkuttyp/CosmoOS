/*
 * vblk.h - The device side of a virtio-blk virtqueue.
 *
 * The block `serve` over the shared virtqueue walk (vq.h): given a head, it
 * reads a virtio-blk request (a readable header, data buffers, a writable
 * status byte) and moves the sectors between the disk and guest memory. The
 * walk, the ring and the hostile-input disciplines are vq.h's, shared with
 * every other device; this file is only what a block request means.
 *
 * It touches no system call: guest memory and the disk are reached through
 * callbacks, so the same code is compiled into vmctl (where the callbacks
 * are cosmo_vm_mem_read/write and a pread of the disk file) and into the
 * host test (where they are an in-memory guest and an in-memory disk).
 */
#ifndef COSMO_VBLK_H
#define COSMO_VBLK_H

#include <stddef.h>
#include <stdint.h>

#include "vq.h"

/* virtio-blk request types and status, and the sector size the protocol
 * fixes at 512 regardless of the backing file's block size. */
#define VIRTIO_BLK_T_IN    0u
#define VIRTIO_BLK_T_OUT   1u
#define VIRTIO_BLK_T_FLUSH 4u
#define VIRTIO_BLK_S_OK    0u
#define VIRTIO_BLK_S_IOERR 1u
#define VIRTIO_BLK_S_UNSUPP 2u
#define VBLK_SECTOR        512u

/* Device feature bits the transport negotiates (bit numbers, not masks). */
#define VIRTIO_BLK_F_RO      5u   /* the disk is read-only */
#define VIRTIO_BLK_F_FLUSH   9u   /* writes may be buffered; T_FLUSH makes them durable */
#define VIRTIO_F_VERSION_1  32u   /* a modern (non-legacy) device */

#define VBLK_QUEUE_MAX     VQ_MAX   /* the transport's QueueNumMax */

struct vblk_io {
    /* Guest memory and the per-notification ceiling: vq_process's needs, kept
     * here so a vblk_io builds a vq_io view of these four fields. */
    int (*read_guest)(void *ctx, uint64_t gpa, void *buf, uint32_t len);
    int (*write_guest)(void *ctx, uint64_t gpa, const void *buf, uint32_t len);
    /* Serve a disk read: `len` bytes from byte offset `off`; 0 ok, <0 error. */
    int (*disk_read)(void *ctx, uint64_t off, void *buf, uint32_t len);
    /* The write side of a read-write disk. Both NULL on a read-only device,
     * where T_OUT and T_FLUSH complete as UNSUPP and neither is ever called.
     * disk_write moves `len` bytes at byte offset `off` to the disk; the
     * caller bounds `off + len` to the capacity, so it never grows the file.
     * disk_flush makes prior writes durable. Each returns 0 ok, <0 error. */
    int (*disk_write)(void *ctx, uint64_t off, const void *buf, uint32_t len);
    int (*disk_flush)(void *ctx);
    void *ctx;
    uint64_t capacity_sectors;   /* the disk's size, in 512-byte sectors */
    uint64_t max_bytes_per_call; /* vq_io's ceiling; 0 unbounded. vmctl sets VBLK_MAX_BYTES_PER_CALL */
};

/* Guest-driven work is done synchronously in the owner's thread, so it must
 * be bounded on guest-controlled input. A single request may name at most
 * VBLK_REQ_MAX_BYTES of data (a real driver's requests are far smaller; one
 * larger is a driver error and is refused), and one QueueNotify serves at
 * most VBLK_MAX_BYTES_PER_CALL across all requests before the rest wait for
 * the next -- so no descriptor length and no ring backlog can turn one
 * notification into unbounded reads and copies. */
#define VBLK_REQ_MAX_BYTES       (4u << 20)   /* 4 MiB per request */
#define VBLK_MAX_BYTES_PER_CALL  (32u << 20)  /* 32 MiB per notification */
/* A flush moves no data but is a synchronous fsync, so it must count against
 * the per-notification ceiling too -- otherwise a queue full of flushes runs
 * a queue's worth of fsyncs in one batch. Charge it like a max-size request,
 * so a flood of flushes is broken across owner turns like a flood of writes. */
#define VBLK_FLUSH_COST          VBLK_REQ_MAX_BYTES

/*
 * Serve the block requests the guest has made available since the last call
 * (a thin wrapper over vq_process with the block `serve`). Returns the number
 * served this call (>= 0), each appended to the used ring; the caller raises
 * the interrupt when the used ring advanced and calls again while > 0.
 * Returns -1 and stops on a hostile ring or a fault -- never a positive count,
 * so a draining caller does not replay the faulting request.
 */
int vblk_process(struct vblk_io *io, struct vq_queue *q);

#endif /* COSMO_VBLK_H */
