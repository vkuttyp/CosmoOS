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
#define VIRTIO_BLK_S_OK    0u
#define VIRTIO_BLK_S_IOERR 1u
#define VIRTIO_BLK_S_UNSUPP 2u
#define VBLK_SECTOR        512u

struct vblk_io {
    /* Move `len` bytes between guest memory at `gpa` and `buf`; 0 ok, <0 a
     * fault (an address outside the guest's regions -- the backstop against
     * a descriptor that points anywhere). */
    int (*read_guest)(void *ctx, uint64_t gpa, void *buf, uint32_t len);
    int (*write_guest)(void *ctx, uint64_t gpa, const void *buf, uint32_t len);
    /* Serve a disk read: `len` bytes from byte offset `off`; 0 ok, <0 error. */
    int (*disk_read)(void *ctx, uint64_t off, void *buf, uint32_t len);
    void *ctx;
    uint64_t capacity_sectors;   /* the disk's size, in 512-byte sectors */
};

struct vblk_queue {
    uint64_t desc_gpa, avail_gpa, used_gpa;   /* where the guest placed the ring */
    uint16_t size;                            /* power of two, <= VBLK_QUEUE_MAX */
    uint16_t last_avail;                      /* the next available index to serve */
    uint16_t used_idx;                        /* the device's view of used->idx */
    int ready;
};

#define VBLK_QUEUE_MAX 256u

/*
 * Serve every request the guest has made available since the last call.
 * Returns the number served (>= 0), each appended to the used ring; the
 * caller raises the device's interrupt when this is > 0. Returns -1 and
 * stops on a hostile ring -- a malformed descriptor chain, an index or
 * length out of range, a guest-memory fault -- having served whatever was
 * well-formed before it.
 */
int vblk_process(struct vblk_io *io, struct vblk_queue *q);

#endif /* COSMO_VBLK_H */
