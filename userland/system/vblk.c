/*
 * vblk.c - The device side of a virtio-blk virtqueue (vblk.h).
 */
#include "vblk.h"
#include <string.h>

/* The split-virtqueue layout, device's view. Defined here rather than
 * pulled from the kernel driver header so the file is self-contained for
 * both vmctl and the host test. Little-endian, which every platform this
 * builds for is. */
struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VQ_DESC_F_NEXT  1u
#define VQ_DESC_F_WRITE 2u

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/* avail: flags(2) idx(2) ring[size](2 each). used: flags(2) idx(2)
 * ring[size]({id(4) len(4)}). Reached field by field through the
 * callbacks, because the whole ring is in guest memory. */
static int read_u16(struct vblk_io *io, uint64_t gpa, uint16_t *out)
{
    return io->read_guest(io->ctx, gpa, out, 2);
}

static int read_desc(struct vblk_io *io, const struct vblk_queue *q, uint16_t i, struct vq_desc *d)
{
    if (i >= q->size)
        return -1;
    return io->read_guest(io->ctx, q->desc_gpa + (uint64_t)i * 16u, d, sizeof(*d));
}

/* One request: its descriptor chain is a readable header, one or more data
 * buffers (writable for a read), and a writable one-byte status. Returns
 * the number of bytes written into the used entry, or -1 on a hostile or
 * faulting chain. */
static int serve_one(struct vblk_io *io, const struct vblk_queue *q, uint16_t head, uint32_t *used_len)
{
    struct vq_desc d;
    if (read_desc(io, q, head, &d) != 0)
        return -1;
    if ((d.flags & VQ_DESC_F_WRITE) || d.len < sizeof(struct virtio_blk_req_hdr))
        return -1;                               /* the header is device-readable */
    struct virtio_blk_req_hdr hdr;
    if (io->read_guest(io->ctx, d.addr, &hdr, sizeof(hdr)) != 0)
        return -1;

    uint8_t status = VIRTIO_BLK_S_OK;
    uint32_t written = 0;
    uint64_t req_bytes = 0;                       /* data this request names, so far */
    uint64_t off = hdr.sector * (uint64_t)VBLK_SECTOR;
    int unsupported = hdr.type != VIRTIO_BLK_T_IN;   /* read-only device: only IN is served */

    /* Walk the data descriptors to the status byte, bounding the chain at
     * the queue size so a `next` that loops cannot spin us forever. */
    unsigned steps = 0;
    uint8_t buf[VBLK_SECTOR];
    while (d.flags & VQ_DESC_F_NEXT) {
        if (++steps > q->size)
            return -1;                           /* a chain longer than the ring: a loop */
        if (read_desc(io, q, d.next, &d) != 0)
            return -1;
        if (!(d.flags & VQ_DESC_F_NEXT)) {
            /* the last descriptor is the writable status byte */
            if (!(d.flags & VQ_DESC_F_WRITE) || d.len < 1)
                return -1;
            break;
        }
        if (!(d.flags & VQ_DESC_F_WRITE))
            return -1;                           /* a data buffer for a read must be writable */
        /* A request cannot name more data than the device serves; a length
         * that would drive an unbounded chunk loop is a driver error, and is
         * refused before a byte is read or copied. */
        req_bytes += d.len;
        if (req_bytes > VBLK_REQ_MAX_BYTES)
            return -1;
        /* serve this data buffer, a sector at a time, from the disk */
        uint32_t remaining = d.len;
        uint64_t gpa = d.addr;
        while (remaining > 0 && !unsupported) {
            uint32_t chunk = remaining < VBLK_SECTOR ? remaining : VBLK_SECTOR;
            if (off + chunk > io->capacity_sectors * (uint64_t)VBLK_SECTOR ||
                io->disk_read(io->ctx, off, buf, chunk) != 0) {
                status = VIRTIO_BLK_S_IOERR;
                break;
            }
            if (io->write_guest(io->ctx, gpa, buf, chunk) != 0)
                return -1;                       /* a data buffer that points out of the guest */
            gpa += chunk;
            off += chunk;
            remaining -= chunk;
            written += chunk;
        }
    }
    if (unsupported)
        status = VIRTIO_BLK_S_UNSUPP;
    /* `d` is now the status descriptor. */
    if (io->write_guest(io->ctx, d.addr, &status, 1) != 0)
        return -1;
    *used_len = written + 1u;                    /* data written plus the status byte */
    return 0;
}

int vblk_process(struct vblk_io *io, struct vblk_queue *q)
{
    if (!q->ready || q->size == 0 || q->size > VBLK_QUEUE_MAX)
        return -1;
    uint16_t avail_idx;
    if (read_u16(io, q->avail_gpa + 2u, &avail_idx) != 0)   /* avail->idx */
        return -1;
    /* A driver can expose at most q->size buffers before the device consumes
     * one -- there are only that many descriptors. A larger gap between
     * avail->idx and what we last saw is a fatal driver error, not work to
     * do: refuse it, so one notification cannot drive up to 65535 disk reads
     * and guest writes and monopolize the owner. */
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
        if (serve_one(io, q, head, &used_len) != 0)
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
        call_bytes += used_len;
    }
    return served;
}
