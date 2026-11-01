/*
 * virtq_used.c - The split virtqueue's decoder of what the device returns
 * (roadmap 2.0 SA5, the virtq/USB/ACPI family): a used-ring element judged
 * against the driver's own records, and the completed chain's return to
 * the free list by those records alone. Moved unchanged from
 * virtqueue.c's pop_locked and reclaim_chain: the C step, to be replaced
 * in CeeSafe once tests/host/test_virtq.c's recorded digest agrees. The
 * ring access, its barriers, the lock and the log stay in virtqueue.c.
 */

#include <drivers/virtio.h>

/*
 * A used element (id, len) as read once from the ring. "Bad" is anything
 * the driver's records do not confirm: an index outside the table, a head
 * that is not in flight (never posted, completed already, or completed
 * twice). Otherwise *written is what the chain holds.
 */
int virtq_used_check(const struct virtqueue *vq, uint32_t id, uint32_t len, bool legacy, uint32_t *written)
{
    if (id >= vq->size || vq->chain_len[id] == 0 || vq->cookies[id] == NULL)
        return VIRTQ_USED_BAD_ID;
    uint16_t head = (uint16_t)id;
    *written = len;
    if (len > vq->in_bytes[head] && vq->in_bytes[head] == 0 && legacy) {
        /* A legacy device may report the bytes it read from a chain it
         * may only read (Parallels Desktop's network adapter does, for
         * every transmit); the spec tells legacy drivers to ignore the
         * length where they can (2.7.8.2). Nothing was written. */
        *written = 0;
        return VIRTQ_USED_OK;
    }
    if (len > vq->in_bytes[head]) {
        /* The device claims more than the chain could hold: report what
         * the buffers can actually contain. */
        *written = vq->in_bytes[head];
        return VIRTQ_USED_CLAMPED;
    }
    return VIRTQ_USED_OK;
}

/* Lock held. Return a completed chain to the free list using only the
 * driver's records: chain_len bounds the walk, shadow_next supplies the
 * links, so nothing the device wrote can steer it. */
void virtq_reclaim(struct virtqueue *vq, uint16_t head)
{
    uint16_t len = vq->chain_len[head];
    uint16_t last = head;
    for (uint16_t k = 1; k < len; k++)
        last = vq->shadow_next[last];
    vq->shadow_next[last] = vq->free_head;
    vq->free_head = head;
    vq->num_free = (uint16_t)(vq->num_free + len);
    vq->chain_len[head] = 0;
    vq->in_bytes[head] = 0;
    vq->cookies[head] = NULL;
}
