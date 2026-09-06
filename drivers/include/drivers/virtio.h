/*
 * virtio.h - VirtIO device abstraction, split virtqueues, transports.
 *
 * Implemented by the `virtio` kernel module (drivers/virtio/), used by
 * the virtio device driver modules. A virtio_device sits on the "virtio"
 * bus; its transport (virtio-pci modern in this phase) implements the
 * operations in struct virtio_transport. Virtqueues are generic and
 * shared by every device type (constitution section 27). Spec: VirtIO
 * 1.1, sections 2.6 (split virtqueues), 3.1 (device initialisation),
 * 4.1 (PCI transport).
 */

#ifndef DRIVERS_VIRTIO_H
#define DRIVERS_VIRTIO_H

#include <kernel/device.h>
#include <kernel/dma.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

#define VIRTIO_ID_NET     1u
#define VIRTIO_ID_BLOCK   2u
#define VIRTIO_ID_CONSOLE 3u
#define VIRTIO_ID_RNG     4u

/* Device status (spec 2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER      2u
#define VIRTIO_STATUS_DRIVER_OK   4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_NEEDS_RESET 64u
#define VIRTIO_STATUS_FAILED      128u

/* Reserved feature bits (spec 6). */
#define VIRTIO_F_RING_INDIRECT_DESC (1ULL << 28)
#define VIRTIO_F_RING_EVENT_IDX     (1ULL << 29)
#define VIRTIO_F_VERSION_1          (1ULL << 32)

#define VIRTIO_MAX_QUEUES 8u
#define VIRTQ_MAX_SIZE    256u
#define VIRTIO_MSI_NO_VECTOR 0xffffu

struct virtio_device;
struct virtqueue;

struct virtio_transport {
    const char *name;
    uint64_t (*get_features)(struct virtio_device *vdev);
    void     (*set_features)(struct virtio_device *vdev, uint64_t features);
    uint8_t  (*get_status)(struct virtio_device *vdev);
    void     (*set_status)(struct virtio_device *vdev, uint8_t status);
    void     (*read_config)(struct virtio_device *vdev, unsigned off, void *buf, size_t len);
    /* Query the device's maximum size for queue `index`; 0 if absent. */
    unsigned (*queue_max_size)(struct virtio_device *vdev, unsigned index);
    /* Program addresses and the interrupt for an allocated queue and
     * enable it. If vq->callback is NULL no vector is assigned. */
    int      (*setup_queue)(struct virtio_device *vdev, struct virtqueue *vq);
    void     (*teardown_queue)(struct virtio_device *vdev, struct virtqueue *vq);
    void     (*notify)(struct virtio_device *vdev, struct virtqueue *vq);
    /* Mandatory: the last reference to the virtio device dropped, after
     * virtio_device_unregister; free the transport's memory. */
    void     (*release)(struct virtio_device *vdev);
};

struct virtio_device {
    struct device dev;                          /* "virtioN" on the virtio bus */
    uint32_t device_id;
    uint64_t device_features;                   /* offered */
    uint64_t features;                          /* negotiated */
    const struct virtio_transport *tr;
    void *tr_priv;
    struct virtqueue *vq[VIRTIO_MAX_QUEUES];
    unsigned nr_vq;
    struct device *hw;                          /* the bus device behind the transport */
    void *priv;                                 /* device driver private */
};

static inline struct virtio_device *to_virtio_device(struct device *dev)
{
    return container_of(dev, struct virtio_device, dev);
}

/* A segment of a request: bus address and length. */
struct virtq_sg {
    dma_addr_t addr;
    uint32_t len;
};

/* Split ring layout (spec 2.6), little endian. */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __packed;
#define VIRTQ_DESC_F_NEXT  1u
#define VIRTQ_DESC_F_WRITE 2u

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __packed;

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __packed;

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __packed;
#define VIRTQ_USED_F_NO_NOTIFY 1u

struct virtqueue {
    struct virtio_device *vdev;
    unsigned index;
    unsigned size;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    dma_addr_t desc_dma, avail_dma, used_dma;
    void *ring_mem;                             /* one dma_alloc */
    dma_addr_t ring_dma;
    size_t ring_bytes;
    /* Driver-private view of the descriptor table. The table itself is in
     * memory the device may write, so the free list and every chain link
     * live here and are never read back from desc[]; the device-visible
     * `next` fields are written from this copy and treated as write-only. */
    uint16_t *shadow_next;                      /* free-list and chain links, per descriptor */
    uint16_t *chain_len;                        /* per head: descriptors in flight in its chain, 0 = free */
    uint32_t *in_bytes;                         /* per head: device-writable bytes; bounds used->len */
    uint16_t free_head;
    uint16_t num_free;
    uint16_t last_used;                         /* next used->ring slot to consume */
    void **cookies;                             /* per head descriptor */
    void (*callback)(struct virtqueue *vq);     /* interrupt context; NULL = polled */
    spinlock_t lock;
    int vector;                                 /* MSI-X vector or -1 */
    unsigned msix_index;                        /* transport use */
    unsigned cpu;                               /* the CPU the vector is routed to (virtq_alloc_on) */
    uint64_t kicks, interrupts, bad_used;      /* bad_used: invalid device completions */
};

/* --- device drivers ------------------------------------------------- */

struct virtio_driver {
    struct device_driver drv;
    const uint32_t *ids;                        /* device ids, 0-terminated */
    uint64_t features;                          /* wanted device features */
    int (*probe)(struct virtio_device *vdev);
    void (*remove)(struct virtio_device *vdev);
};

extern struct bus_type virtio_bus;

int virtio_register_driver(struct virtio_driver *vdrv);
void virtio_unregister_driver(struct virtio_driver *vdrv);

/* Steps 1-6 of spec 3.1.1: reset, ACKNOWLEDGE, DRIVER, negotiate
 * (`wanted` & offered, plus VERSION_1 which is mandatory), FEATURES_OK.
 * -ENOTSUP if the device is legacy or rejects the features. Sleeps. */
int virtio_device_init(struct virtio_device *vdev, uint64_t wanted);
/* Step 8: DRIVER_OK. */
void virtio_device_ready(struct virtio_device *vdev);
/* Reset: every in-flight buffer is dropped by the device. */
void virtio_device_reset(struct virtio_device *vdev);
static inline bool virtio_has_feature(const struct virtio_device *vdev, uint64_t bit)
{
    return (vdev->features & bit) != 0;
}
void virtio_read_config(struct virtio_device *vdev, unsigned off, void *buf, size_t len);
uint32_t virtio_read_config32(struct virtio_device *vdev, unsigned off);
uint64_t virtio_read_config64(struct virtio_device *vdev, unsigned off);

/* --- virtqueues --------------------------------------------------------- */

/* Allocate and enable queue `index` with at most `max` entries (0 = the
 * device's maximum, capped at VIRTQ_MAX_SIZE). Sleeps. */
int virtq_alloc(struct virtio_device *vdev, unsigned index, unsigned max, void (*callback)(struct virtqueue *),
                struct virtqueue **out);
/* The same with the queue's interrupt routed to `cpu` (virtq_alloc: CPU 0);
 * a multi-queue driver binds each queue to the CPU that consumes it. */
int virtq_alloc_on(struct virtio_device *vdev, unsigned index, unsigned max, void (*callback)(struct virtqueue *),
                   unsigned cpu, struct virtqueue **out);
void virtq_free(struct virtqueue *vq);

/* Add a chain: `out` device-readable segments then `in` device-writable
 * ones. -ENOSPC when the ring is full. Any context (IRQ-safe lock). */
int virtq_add(struct virtqueue *vq, const struct virtq_sg *sg, unsigned out, unsigned in, void *cookie);
/* Publish added chains and notify the device if it wants that. */
void virtq_kick(struct virtqueue *vq);
/* Next completed chain's cookie, with the bytes the device wrote, or
 * NULL when nothing has completed. Any context. */
void *virtq_pop(struct virtqueue *vq, uint32_t *len);
unsigned virtq_free_count(struct virtqueue *vq);

/* Transport side: register a discovered device (fills dev.name). */
int virtio_device_register(struct virtio_device *vdev);
void virtio_device_unregister(struct virtio_device *vdev);
/* Transport side: the interrupt for a queue arrived. */
void virtq_interrupt(struct virtqueue *vq);

#endif /* DRIVERS_VIRTIO_H */
