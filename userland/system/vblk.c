/*
 * vblk.c - The block `serve` over the shared virtqueue walk (vblk.h, vq.h).
 */
#include "vblk.h"
#include <string.h>

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/* One request: its descriptor chain is a readable header, one or more data
 * buffers (writable for a read, readable for a write), and a writable
 * one-byte status. Guest memory is io->read_guest/write_guest; the disk is
 * the vblk_io in sctx. Returns 0 with the used length and work set, or -1 on
 * a hostile or faulting chain. */
static int blk_serve(void *sctx, const struct vq_io *io, const struct vq_queue *q,
                     uint16_t head, uint32_t *used_len, uint64_t *work)
{
    struct vblk_io *bio = sctx;
    struct vq_desc d;
    if (vq_read_desc(io, q, head, &d) != 0)
        return -1;
    if ((d.flags & VQ_DESC_F_WRITE) || d.len < sizeof(struct virtio_blk_req_hdr))
        return -1;                               /* the header is device-readable */
    struct virtio_blk_req_hdr hdr;
    if (io->read_guest(io->ctx, d.addr, &hdr, sizeof(hdr)) != 0)
        return -1;

    uint8_t status = VIRTIO_BLK_S_OK;
    uint32_t written = 0;                          /* bytes returned to the guest (reads only) */
    uint64_t req_bytes = 0;                        /* data this request names, so far */
    uint64_t off = hdr.sector * (uint64_t)VBLK_SECTOR;

    /* Which request, and whether this device can serve it. A read-only disk
     * has no disk_write/disk_flush, so a write or flush is UNSUPP. */
    int is_read  = hdr.type == VIRTIO_BLK_T_IN;
    int is_write = hdr.type == VIRTIO_BLK_T_OUT && bio->disk_write != NULL;
    int is_flush = hdr.type == VIRTIO_BLK_T_FLUSH && bio->disk_flush != NULL;
    int unsupported = !is_read && !is_write && !is_flush;

    /* The starting sector must lie within the disk, checked before the
     * multiply above is trusted: hdr.sector * 512 can wrap a huge sector down
     * into a low, in-range offset that the per-chunk capacity check would
     * then accept, landing the transfer on the wrong part of the disk. */
    if ((is_read || is_write) && hdr.sector >= bio->capacity_sectors)
        status = VIRTIO_BLK_S_IOERR;

    /* Walk the data descriptors to the status byte, bounding the chain at
     * the queue size so a `next` that loops cannot spin us forever. */
    unsigned steps = 0;
    uint8_t buf[VBLK_SECTOR];
    while (d.flags & VQ_DESC_F_NEXT) {
        if (++steps > q->size)
            return -1;                           /* a chain longer than the ring: a loop */
        if (vq_read_desc(io, q, d.next, &d) != 0)
            return -1;
        if (!(d.flags & VQ_DESC_F_NEXT)) {
            /* the last descriptor is the writable status byte */
            if (!(d.flags & VQ_DESC_F_WRITE) || d.len < 1)
                return -1;
            break;
        }
        /* A data descriptor. Its direction must match the request: a read
         * fills it, so it must be device-writable; a write drains it, so it
         * must be device-readable. A flush carries none, and any that a
         * hostile ring attaches are walked but never moved. */
        if (is_read && !(d.flags & VQ_DESC_F_WRITE))
            return -1;
        if (is_write && (d.flags & VQ_DESC_F_WRITE))
            return -1;
        /* A request cannot name more data than the device serves; a length
         * that would drive an unbounded chunk loop is a driver error, and is
         * refused before a byte is read or copied. */
        req_bytes += d.len;
        if (req_bytes > VBLK_REQ_MAX_BYTES)
            return -1;
        if (!is_read && !is_write)
            continue;                            /* flush/unsupported move no data */
        /* serve this data buffer, a sector at a time, between disk and guest */
        uint32_t remaining = d.len;
        uint64_t gpa = d.addr;
        while (remaining > 0 && status == VIRTIO_BLK_S_OK) {
            uint32_t chunk = remaining < VBLK_SECTOR ? remaining : VBLK_SECTOR;
            if (off + chunk > bio->capacity_sectors * (uint64_t)VBLK_SECTOR) {
                status = VIRTIO_BLK_S_IOERR;     /* past the end of the disk */
                break;
            }
            if (is_read) {
                if (bio->disk_read(io->ctx, off, buf, chunk) != 0) {
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }
                if (io->write_guest(io->ctx, gpa, buf, chunk) != 0)
                    return -1;                   /* a data buffer that points out of the guest */
                written += chunk;
            } else {                             /* is_write */
                if (io->read_guest(io->ctx, gpa, buf, chunk) != 0)
                    return -1;                   /* a data buffer that points out of the guest */
                if (bio->disk_write(io->ctx, off, buf, chunk) != 0) {
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }
            }
            gpa += chunk;
            off += chunk;
            remaining -= chunk;
        }
    }
    if (is_flush && status == VIRTIO_BLK_S_OK && bio->disk_flush(io->ctx) != 0)
        status = VIRTIO_BLK_S_IOERR;
    if (unsupported)
        status = VIRTIO_BLK_S_UNSUPP;
    /* `d` is now the status descriptor. */
    if (io->write_guest(io->ctx, d.addr, &status, 1) != 0)
        return -1;
    *used_len = written + 1u;                    /* data returned to the guest plus the status byte */
    /* Work for the ceiling: the data moved, plus a fixed charge for a flush,
     * which moves nothing but does a synchronous fsync. */
    *work = req_bytes + (is_flush ? VBLK_FLUSH_COST : 0u);
    return 1;                                    /* block always serves an available head */
}

int vblk_process(struct vblk_io *io, struct vq_queue *q)
{
    struct vq_io vio = { io->read_guest, io->write_guest, io->ctx, io->max_bytes_per_call };
    return vq_process(&vio, q, blk_serve, io);
}
