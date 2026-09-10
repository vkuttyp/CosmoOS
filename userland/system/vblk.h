/*
 * vblk.h - The device side of a virtio-blk virtqueue
 * (docs/audit/next-subsystem-vblk.md).
 *
 * The mirror of drivers/virtio/virtqueue.c: that is the driver end, which
 * produces the available ring and consumes the used ring; this is the
 * device end, which consumes available and produces used. It is the part
 * a guest's disk needs and the tree did not have.
 *
 * It touches no system call: guest memory and the disk are reached through
 * callbacks, so the same code is compiled into vmctl (where the callbacks
 * are cosmo_vm_mem_read/write and a pread of the disk file) and into the
 * host test (where they are an in-memory guest and an in-memory disk).
 * Every byte read from the ring is the guest's, and therefore untrusted:
 * a descriptor index, a chain length, a buffer address or a length that is
 * out of range is refused, never followed.
 */
#ifndef COSMO_VBLK_H
#define COSMO_VBLK_H

#include <stddef.h>
#include <stdint.h>

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

struct vblk_io {
    /* Move `len` bytes between guest memory at `gpa` and `buf`; 0 ok, <0 a
     * fault (an address outside the guest's regions -- the backstop against
     * a descriptor that points anywhere). */
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
    /* The most bytes one QueueNotify may serve before the rest wait for the
     * next: the ceiling on synchronous, guest-driven work in the owner's
     * thread. 0 means unbounded. vmctl sets VBLK_MAX_BYTES_PER_CALL. */
    uint64_t max_bytes_per_call;
};

struct vblk_queue {
    uint64_t desc_gpa, avail_gpa, used_gpa;   /* where the guest placed the ring */
    uint16_t size;                            /* power of two, <= VBLK_QUEUE_MAX */
    uint16_t last_avail;                      /* the next available index to serve */
    uint16_t used_idx;                        /* the device's view of used->idx */
    int ready;
};

#define VBLK_QUEUE_MAX 256u

/* Guest-driven work is done synchronously in the owner's thread, so it must
 * be bounded on guest-controlled input. A single request may name at most
 * VBLK_REQ_MAX_BYTES of data (a real driver's requests are far smaller; one
 * larger is a driver error and is refused), and one QueueNotify serves at
 * most VBLK_MAX_BYTES_PER_CALL across all requests before the rest wait for
 * the next -- so no descriptor length and no ring backlog can turn one
 * notification into unbounded reads and copies. */
#define VBLK_REQ_MAX_BYTES       (4u << 20)   /* 4 MiB per request */
#define VBLK_MAX_BYTES_PER_CALL  (32u << 20)  /* 32 MiB per notification */

/*
 * Serve requests the guest has made available since the last call, at most
 * max_bytes_per_call of data before deferring the rest to the next call.
 * Returns the number served this call (>= 0), each appended to the used
 * ring; the caller raises the device's interrupt when this is > 0 and, if it
 * is > 0, calls again (the deferred remainder is served a batch at a time).
 * Returns -1 and stops on a hostile ring -- a malformed descriptor chain, an
 * index or length out of range, a guest-memory fault: a failure is never
 * reported as a positive count, so a draining caller does not replay the
 * faulting request. Requests completed before a mid-walk fault keep their
 * used-ring entries; they are just not counted on the failing call.
 */
int vblk_process(struct vblk_io *io, struct vblk_queue *q);

#endif /* COSMO_VBLK_H */
