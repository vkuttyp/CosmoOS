/*
 * virtio_pci.c - The virtio-pci transports (VirtIO 1.1 section 4.1) and
 * the `virtio` module's entry points.
 *
 * Modern: vendor-specific PCI capabilities locate the common, notify, ISR
 * and device configuration structures in the device's BARs. Legacy (a
 * device with no common capability, such as Parallels Desktop's network
 * adapter; section 4.1.4.8): one register block at the start of BAR0, a
 * memory BAR or, where the architecture reaches PCI I/O space (x86-64),
 * an I/O BAR. Either way every queue gets its own MSI-X vector (entry 0
 * is the configuration vector).
 */

#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/string.h>

#include <drivers/pci.h>
#include <drivers/virtio.h>

#define VIRTIO_PCI_VENDOR       0x1af4
#define VIRTIO_PCI_CAP_COMMON   1
#define VIRTIO_PCI_CAP_NOTIFY   2
#define VIRTIO_PCI_CAP_ISR      3
#define VIRTIO_PCI_CAP_DEVICE   4

/* struct virtio_pci_common_cfg offsets (spec 4.1.4.3). */
#define COMMON_DEVICE_FEATURE_SELECT 0x00
#define COMMON_DEVICE_FEATURE        0x04
#define COMMON_DRIVER_FEATURE_SELECT 0x08
#define COMMON_DRIVER_FEATURE        0x0c
#define COMMON_MSIX_CONFIG           0x10
#define COMMON_NUM_QUEUES            0x12
#define COMMON_DEVICE_STATUS         0x14
#define COMMON_CONFIG_GENERATION     0x15
#define COMMON_QUEUE_SELECT          0x16
#define COMMON_QUEUE_SIZE            0x18
#define COMMON_QUEUE_MSIX_VECTOR     0x1a
#define COMMON_QUEUE_ENABLE          0x1c
#define COMMON_QUEUE_NOTIFY_OFF      0x1e
#define COMMON_QUEUE_DESC            0x20
#define COMMON_QUEUE_DRIVER          0x28
#define COMMON_QUEUE_DEVICE          0x30

struct vpci {
    struct pci_device *pdev;
    struct virtio_device vdev;
    vaddr_t bar_va[PCI_MAX_BARS];
    volatile uint8_t *common;
    volatile uint8_t *notify;
    uint32_t notify_multiplier;
    size_t notify_len;
    volatile uint8_t *isr;
    volatile uint8_t *devcfg;
    size_t devcfg_len;
    unsigned msix_vectors;      /* granted */
    spinlock_t lock;            /* queue_select and feature_select are shared registers */
    /* Legacy only: the register block, in a mapped memory BAR0 (leg_mmio)
     * or I/O BAR0 (leg_mmio NULL, through pci_io_*), and the size of the
     * device configuration that follows it. */
    bool legacy;
    volatile uint8_t *leg_mmio;
    size_t leg_cfg_len;
};

/* Legacy register block (spec 4.1.4.8), with MSI-X enabled: the device
 * configuration starts at 0x18. */
#define LEG_HOST_FEATURES   0x00
#define LEG_GUEST_FEATURES  0x04
#define LEG_QUEUE_PFN       0x08
#define LEG_QUEUE_SIZE      0x0c
#define LEG_QUEUE_SELECT    0x0e
#define LEG_QUEUE_NOTIFY    0x10
#define LEG_STATUS          0x12
#define LEG_MSIX_CONFIG     0x14
#define LEG_MSIX_QUEUE      0x16
#define LEG_CONFIG          0x18
#define LEG_PFN_SHIFT       12

static inline uint8_t rd8(volatile uint8_t *base, unsigned off)
{
    return *(volatile uint8_t *)(base + off);
}
static inline uint16_t rd16(volatile uint8_t *base, unsigned off)
{
    return *(volatile uint16_t *)(base + off);
}
static inline uint32_t rd32(volatile uint8_t *base, unsigned off)
{
    return *(volatile uint32_t *)(base + off);
}
static inline void wr8(volatile uint8_t *base, unsigned off, uint8_t v)
{
    *(volatile uint8_t *)(base + off) = v;
}
static inline void wr16(volatile uint8_t *base, unsigned off, uint16_t v)
{
    *(volatile uint16_t *)(base + off) = v;
}
static inline void wr32(volatile uint8_t *base, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}
static inline void wr64(volatile uint8_t *base, unsigned off, uint64_t v)
{
    wr32(base, off, (uint32_t)v);
    wr32(base, off + 4, (uint32_t)(v >> 32));
}

static struct vpci *to_vpci(struct virtio_device *vdev)
{
    return vdev->tr_priv;
}

/* --- transport operations ------------------------------------------------ */

static uint64_t vpci_get_features(struct virtio_device *vdev)
{
    struct vpci *v = to_vpci(vdev);
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    wr32(v->common, COMMON_DEVICE_FEATURE_SELECT, 0);
    uint64_t lo = rd32(v->common, COMMON_DEVICE_FEATURE);
    wr32(v->common, COMMON_DEVICE_FEATURE_SELECT, 1);
    uint64_t hi = rd32(v->common, COMMON_DEVICE_FEATURE);
    spin_unlock_irqrestore(&v->lock, s);
    return lo | (hi << 32);
}

static void vpci_set_features(struct virtio_device *vdev, uint64_t f)
{
    struct vpci *v = to_vpci(vdev);
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    wr32(v->common, COMMON_DRIVER_FEATURE_SELECT, 0);
    wr32(v->common, COMMON_DRIVER_FEATURE, (uint32_t)f);
    wr32(v->common, COMMON_DRIVER_FEATURE_SELECT, 1);
    wr32(v->common, COMMON_DRIVER_FEATURE, (uint32_t)(f >> 32));
    spin_unlock_irqrestore(&v->lock, s);
}

static uint8_t vpci_get_status(struct virtio_device *vdev)
{
    return rd8(to_vpci(vdev)->common, COMMON_DEVICE_STATUS);
}

static void vpci_set_status(struct virtio_device *vdev, uint8_t st)
{
    wr8(to_vpci(vdev)->common, COMMON_DEVICE_STATUS, st);
}

static void vpci_read_config(struct virtio_device *vdev, unsigned off, void *buf, size_t len)
{
    struct vpci *v = to_vpci(vdev);
    uint8_t *out = buf;
    if (v->devcfg == NULL || off + len > v->devcfg_len) {
        memset(buf, 0, len);
        return;
    }
    /* Re-read until the generation is stable (spec 2.4.1). */
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        uint8_t gen = rd8(v->common, COMMON_CONFIG_GENERATION);
        for (size_t i = 0; i < len; i++)
            out[i] = rd8(v->devcfg, off + (unsigned)i);
        if (rd8(v->common, COMMON_CONFIG_GENERATION) == gen)
            return;
    }
}

static void vpci_write_config32(struct virtio_device *vdev, unsigned off, uint32_t val)
{
    struct vpci *v = to_vpci(vdev);
    if (v->devcfg == NULL || (off & 3) != 0 || off + 4 > v->devcfg_len)
        return;
    wr32(v->devcfg, off, val);
}

static unsigned vpci_queue_max_size(struct virtio_device *vdev, unsigned index)
{
    struct vpci *v = to_vpci(vdev);
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    unsigned n = 0;
    if (index < rd16(v->common, COMMON_NUM_QUEUES)) {
        wr16(v->common, COMMON_QUEUE_SELECT, (uint16_t)index);
        n = rd16(v->common, COMMON_QUEUE_SIZE);
    }
    spin_unlock_irqrestore(&v->lock, s);
    return n;
}

static void vpci_queue_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    virtq_interrupt(arg);
}

static int vpci_setup_queue(struct virtio_device *vdev, struct virtqueue *vq)
{
    struct vpci *v = to_vpci(vdev);
    int rc = 0;

    if (vq->callback) {
        unsigned entry = vq->index + 1;   /* entry 0 is the config vector */
        if (entry >= v->msix_vectors)
            return -ENOSPC;
        vq->msix_index = entry;
        int vector = pci_msix_request(v->pdev, entry, vpci_queue_irq, vq, "virtio-vq", vq->cpu);
        if (vector < 0)
            return vector;
        vq->vector = vector;
    }

    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    wr16(v->common, COMMON_QUEUE_SELECT, (uint16_t)vq->index);
    wr16(v->common, COMMON_QUEUE_SIZE, (uint16_t)vq->size);
    wr64(v->common, COMMON_QUEUE_DESC, vq->desc_dma);
    wr64(v->common, COMMON_QUEUE_DRIVER, vq->avail_dma);
    wr64(v->common, COMMON_QUEUE_DEVICE, vq->used_dma);
    uint16_t want = vq->callback ? (uint16_t)vq->msix_index : (uint16_t)VIRTIO_MSI_NO_VECTOR;
    wr16(v->common, COMMON_QUEUE_MSIX_VECTOR, want);
    if (rd16(v->common, COMMON_QUEUE_MSIX_VECTOR) != want)
        rc = -EIO;
    else
        wr16(v->common, COMMON_QUEUE_ENABLE, 1);
    spin_unlock_irqrestore(&v->lock, s);

    if (rc && vq->vector >= 0) {
        pci_msix_release(v->pdev, vq->msix_index);
        vq->vector = -1;
        kerror("virtio-pci: %s: device refused MSI-X vector for queue %u", vdev->dev.name, vq->index);
    }
    return rc;
}

static void vpci_teardown_queue(struct virtio_device *vdev, struct virtqueue *vq)
{
    struct vpci *v = to_vpci(vdev);
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    wr16(v->common, COMMON_QUEUE_SELECT, (uint16_t)vq->index);
    wr16(v->common, COMMON_QUEUE_ENABLE, 0);
    wr16(v->common, COMMON_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
    spin_unlock_irqrestore(&v->lock, s);
    if (vq->vector >= 0) {
        pci_msix_release(v->pdev, vq->msix_index);
        vq->vector = -1;
    }
}

static void vpci_notify(struct virtio_device *vdev, struct virtqueue *vq)
{
    struct vpci *v = to_vpci(vdev);
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    wr16(v->common, COMMON_QUEUE_SELECT, (uint16_t)vq->index);
    uint32_t off = (uint32_t)rd16(v->common, COMMON_QUEUE_NOTIFY_OFF) * v->notify_multiplier;
    spin_unlock_irqrestore(&v->lock, s);
    if ((size_t)off + 2 <= v->notify_len)
        wr16(v->notify, off, (uint16_t)vq->index);
}

static void vpci_release(struct virtio_device *vdev);

static const struct virtio_transport vpci_transport = {
    .name = "virtio-pci",
    .get_features = vpci_get_features,
    .set_features = vpci_set_features,
    .get_status = vpci_get_status,
    .set_status = vpci_set_status,
    .read_config = vpci_read_config,
    .write_config32 = vpci_write_config32,
    .queue_max_size = vpci_queue_max_size,
    .setup_queue = vpci_setup_queue,
    .teardown_queue = vpci_teardown_queue,
    .notify = vpci_notify,
    .release = vpci_release,
};

/* --- legacy transport operations ------------------------------------------ */

static uint32_t lrd(struct vpci *v, unsigned off, unsigned width)
{
    if (v->leg_mmio == NULL)
        return pci_io_read(v->pdev, 0, off, width);
    return width == 1 ? rd8(v->leg_mmio, off) : width == 2 ? rd16(v->leg_mmio, off) : rd32(v->leg_mmio, off);
}

static void lwr(struct vpci *v, unsigned off, unsigned width, uint32_t val)
{
    if (v->leg_mmio == NULL) {
        pci_io_write(v->pdev, 0, off, width, val);
        return;
    }
    if (width == 1)
        wr8(v->leg_mmio, off, (uint8_t)val);
    else if (width == 2)
        wr16(v->leg_mmio, off, (uint16_t)val);
    else
        wr32(v->leg_mmio, off, val);
}

static uint64_t vleg_get_features(struct virtio_device *vdev)
{
    return lrd(to_vpci(vdev), LEG_HOST_FEATURES, 4);   /* 32 feature bits only */
}

static void vleg_set_features(struct virtio_device *vdev, uint64_t f)
{
    lwr(to_vpci(vdev), LEG_GUEST_FEATURES, 4, (uint32_t)f);
}

static uint8_t vleg_get_status(struct virtio_device *vdev)
{
    return (uint8_t)lrd(to_vpci(vdev), LEG_STATUS, 1);
}

static void vleg_set_status(struct virtio_device *vdev, uint8_t st)
{
    lwr(to_vpci(vdev), LEG_STATUS, 1, st);
}

static void vleg_write_config32(struct virtio_device *vdev, unsigned off, uint32_t val)
{
    struct vpci *v = to_vpci(vdev);
    if ((off & 3) != 0 || off + 4 > v->leg_cfg_len)
        return;
    lwr(v, LEG_CONFIG + off, 4, val);
}

static void vleg_read_config(struct virtio_device *vdev, unsigned off, void *buf, size_t len)
{
    struct vpci *v = to_vpci(vdev);
    uint8_t *out = buf;
    if (off + len > v->leg_cfg_len) {
        memset(buf, 0, len);
        return;
    }
    /* No generation counter in legacy: read until two reads agree. */
    uint8_t again[64];
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        for (size_t i = 0; i < len; i++)
            out[i] = (uint8_t)lrd(v, LEG_CONFIG + off + (unsigned)i, 1);
        if (len > sizeof(again))
            return;
        for (size_t i = 0; i < len; i++)
            again[i] = (uint8_t)lrd(v, LEG_CONFIG + off + (unsigned)i, 1);
        if (memcmp(again, out, len) == 0)
            return;
    }
}

static unsigned vleg_queue_max_size(struct virtio_device *vdev, unsigned index)
{
    struct vpci *v = to_vpci(vdev);
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    lwr(v, LEG_QUEUE_SELECT, 2, index);
    unsigned n = lrd(v, LEG_QUEUE_SIZE, 2);
    spin_unlock_irqrestore(&v->lock, s);
    return n;
}

static int vleg_setup_queue(struct virtio_device *vdev, struct virtqueue *vq)
{
    struct vpci *v = to_vpci(vdev);
    /* The legacy layout is virtqueue.c's: descriptors, the available
     * ring, the used ring at the next 4 KiB boundary, in one allocation
     * whose address the device takes as a 32-bit page number. Its size is
     * the device's, which the driver cannot change. */
    if ((vq->ring_dma & ((1ull << LEG_PFN_SHIFT) - 1)) != 0 || (vq->ring_dma >> LEG_PFN_SHIFT) > 0xffffffffull ||
        vq->used_dma != vq->ring_dma + ((vq->avail_dma - vq->ring_dma + 6 + 2 * (uint64_t)vq->size + 4095) & ~4095ull))
        return -EINVAL;
    if (vleg_queue_max_size(vdev, vq->index) != vq->size) {
        kerror("virtio-pci: %s: legacy queue %u is %u entries, not %u", vdev->dev.name, vq->index,
               vleg_queue_max_size(vdev, vq->index), vq->size);
        return -EINVAL;
    }
    int rc = 0;
    if (vq->callback) {
        unsigned entry = vq->index + 1;   /* entry 0 is the config vector */
        if (entry >= v->msix_vectors)
            return -ENOSPC;
        vq->msix_index = entry;
        int vector = pci_msix_request(v->pdev, entry, vpci_queue_irq, vq, "virtio-vq", vq->cpu);
        if (vector < 0)
            return vector;
        vq->vector = vector;
    }
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    lwr(v, LEG_QUEUE_SELECT, 2, vq->index);
    uint16_t want = vq->callback ? (uint16_t)vq->msix_index : (uint16_t)VIRTIO_MSI_NO_VECTOR;
    lwr(v, LEG_MSIX_QUEUE, 2, want);
    if (lrd(v, LEG_MSIX_QUEUE, 2) != want)
        rc = -EIO;
    else
        lwr(v, LEG_QUEUE_PFN, 4, (uint32_t)(vq->ring_dma >> LEG_PFN_SHIFT));   /* the queue is live from here */
    spin_unlock_irqrestore(&v->lock, s);
    if (rc && vq->vector >= 0) {
        pci_msix_release(v->pdev, vq->msix_index);
        vq->vector = -1;
        kerror("virtio-pci: %s: device refused MSI-X vector for queue %u", vdev->dev.name, vq->index);
    }
    return rc;
}

static void vleg_teardown_queue(struct virtio_device *vdev, struct virtqueue *vq)
{
    struct vpci *v = to_vpci(vdev);
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    lwr(v, LEG_QUEUE_SELECT, 2, vq->index);
    lwr(v, LEG_QUEUE_PFN, 4, 0);
    lwr(v, LEG_MSIX_QUEUE, 2, VIRTIO_MSI_NO_VECTOR);
    spin_unlock_irqrestore(&v->lock, s);
    if (vq->vector >= 0) {
        pci_msix_release(v->pdev, vq->msix_index);
        vq->vector = -1;
    }
}

static void vleg_notify(struct virtio_device *vdev, struct virtqueue *vq)
{
    lwr(to_vpci(vdev), LEG_QUEUE_NOTIFY, 2, vq->index);
}

static const struct virtio_transport vleg_transport = {
    .name = "virtio-pci-legacy",
    .legacy = true,
    .get_features = vleg_get_features,
    .set_features = vleg_set_features,
    .get_status = vleg_get_status,
    .set_status = vleg_set_status,
    .read_config = vleg_read_config,
    .write_config32 = vleg_write_config32,
    .queue_max_size = vleg_queue_max_size,
    .setup_queue = vleg_setup_queue,
    .teardown_queue = vleg_teardown_queue,
    .notify = vleg_notify,
    .release = vpci_release,
};

/* --- PCI driver ------------------------------------------------------------ */

static void vpci_config_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct vpci *v = arg;
    kdebug("virtio-pci: %s: configuration change interrupt", v->vdev.dev.name);
}

/* Map the BAR a capability points into (once per BAR) and return the
 * virtual address of the capability's window, or NULL. */
static volatile uint8_t *cap_window(struct vpci *v, uint8_t cap, size_t *len_out)
{
    struct pci_device *p = v->pdev;
    uint8_t bar = pci_cfg_read8(p, (uint16_t)(cap + 4));
    uint32_t off = pci_cfg_read32(p, (uint16_t)(cap + 8));
    uint32_t len = pci_cfg_read32(p, (uint16_t)(cap + 12));
    if (bar >= PCI_MAX_BARS || p->bar[bar].size == 0 || p->bar[bar].io)
        return NULL;
    if ((uint64_t)off + len > p->bar[bar].size)
        return NULL;
    if (v->bar_va[bar] == 0) {
        v->bar_va[bar] = pci_map_bar(p, bar);
        if (v->bar_va[bar] == 0)
            return NULL;
    }
    if (len_out)
        *len_out = len;
    return (volatile uint8_t *)(v->bar_va[bar] + off);
}

static void vpci_unmap_all(struct vpci *v)
{
    for (unsigned i = 0; i < PCI_MAX_BARS; i++) {
        if (v->bar_va[i]) {
            device_unmap_mmio(v->bar_va[i]);
            v->bar_va[i] = 0;
        }
    }
}

static int vpci_probe(struct pci_device *pdev, const struct pci_id *id)
{
    (void)id;
    struct vpci *v = kzalloc(sizeof(*v));
    if (v == NULL)
        return -ENOMEM;
    v->pdev = pdev;
    spinlock_init(&v->lock, "virtio-pci");

    /* Walk the vendor capabilities. */
    bool modern_caps = false;   /* any COMMON or NOTIFY capability, mapped or not */
    for (uint8_t cap = pci_find_capability(pdev, PCI_CAP_ID_VENDOR, 0); cap;
         cap = pci_find_capability(pdev, PCI_CAP_ID_VENDOR, cap)) {
        uint8_t type = pci_cfg_read8(pdev, (uint16_t)(cap + 3));
        if (type == VIRTIO_PCI_CAP_COMMON || type == VIRTIO_PCI_CAP_NOTIFY)
            modern_caps = true;
        switch (type) {
        case VIRTIO_PCI_CAP_COMMON:
            if (v->common == NULL)
                v->common = cap_window(v, cap, NULL);
            break;
        case VIRTIO_PCI_CAP_NOTIFY:
            if (v->notify == NULL) {
                v->notify = cap_window(v, cap, &v->notify_len);
                v->notify_multiplier = pci_cfg_read32(pdev, (uint16_t)(cap + 16));
            }
            break;
        case VIRTIO_PCI_CAP_ISR:
            if (v->isr == NULL)
                v->isr = cap_window(v, cap, NULL);
            break;
        case VIRTIO_PCI_CAP_DEVICE:
            if (v->devcfg == NULL)
                v->devcfg = cap_window(v, cap, &v->devcfg_len);
            break;
        default:
            break;
        }
    }
    if ((v->common == NULL || v->notify == NULL) && (modern_caps || pdev->device < 0x1000 || pdev->device > 0x103f)) {
        /* A modern device whose windows could not be mapped, or a 0x1af4
         * function that is not a transitional virtio device: not legacy,
         * and not driven at all. */
        kerror("virtio-pci: %s: %s; refused", pdev->dev.name,
               modern_caps ? "a modern capability window could not be mapped" : "not a virtio device id");
        vpci_unmap_all(v);
        kfree(v);
        return -ENODEV;
    }
    if (v->common == NULL || v->notify == NULL) {
        /* Legacy-only (no modern capability at all; a transitional id,
         * 0x1000-0x103f): the register block at the start of BAR0. */
        vpci_unmap_all(v);
        v->common = v->notify = v->isr = v->devcfg = NULL;
        v->legacy = true;
        struct pci_bar *b0 = &pdev->bar[0];
        if (b0->size < LEG_CONFIG) {
            kerror("virtio-pci: %s: legacy device with no register BAR; refused", pdev->dev.name);
            kfree(v);
            return -ENODEV;
        }
        if (!b0->io) {
            v->bar_va[0] = pci_map_bar(pdev, 0);
            v->leg_mmio = (volatile uint8_t *)v->bar_va[0];
            if (v->leg_mmio == NULL) {
                kerror("virtio-pci: %s: cannot map legacy BAR0", pdev->dev.name);
                kfree(v);
                return -ENOMEM;
            }
        } else if (!pci_io_bar_usable(pdev, 0)) {
            kerror("virtio-pci: %s: legacy registers in I/O space, which this architecture cannot reach; refused",
                   pdev->dev.name);
            kfree(v);
            return -ENODEV;
        }
        v->leg_cfg_len = (size_t)(b0->size - LEG_CONFIG);
    }

    pci_enable_device(pdev, true);
    /* Modern addresses 64 bits; legacy takes a queue's address as a
     * 32-bit page number, 44 bits. The virtio device inherits it. */
    dma_set_mask(&pdev->dev, v->legacy ? 32 + LEG_PFN_SHIFT : 64);

    /* Queues + the configuration vector. */
    int granted = pci_msix_enable(pdev, VIRTIO_MAX_QUEUES + 1);
    if (granted < 2) {
        kerror("virtio-pci: %s: MSI-X unavailable (%d); refused", pdev->dev.name, granted);
        vpci_unmap_all(v);
        kfree(v);
        return granted < 0 ? granted : -ENODEV;
    }
    v->msix_vectors = (unsigned)granted;
    if (pci_msix_request(pdev, 0, vpci_config_irq, v, "virtio-cfg", IRQ_CPU_ANY) < 0) {
        pci_msix_disable(pdev);
        vpci_unmap_all(v);
        kfree(v);
        return -ENOSPC;
    }
    if (v->legacy) {
        /* With MSI-X enabled the configuration starts at 0x18; a device
         * that cannot take the vector answers NO_VECTOR. */
        lwr(v, LEG_MSIX_CONFIG, 2, 0);
        if (lrd(v, LEG_MSIX_CONFIG, 2) != 0) {
            kerror("virtio-pci: %s: legacy device refused the configuration vector; refused", pdev->dev.name);
            pci_msix_disable(pdev);
            vpci_unmap_all(v);
            kfree(v);
            return -EIO;
        }
    } else {
        wr16(v->common, COMMON_MSIX_CONFIG, 0);
    }

    /* Transitional devices carry the virtio id in the subsystem id;
     * modern ids are 0x1040 + id. */
    v->vdev.device_id = pdev->device >= 0x1040 ? pdev->device - 0x1040 : pdev->subsys_id;
    v->vdev.tr = v->legacy ? &vleg_transport : &vpci_transport;
    v->vdev.tr_priv = v;
    v->vdev.hw = &pdev->dev;
    pdev->dev.drvdata = v;

    int rc = virtio_device_register(&v->vdev);
    if (rc) {
        pci_msix_disable(pdev);
        vpci_unmap_all(v);
        kfree(v);
        return rc;
    }
    kinfo("virtio-pci: %s: virtio device %u as %s (%u MSI-X vectors%s)", pdev->dev.name, v->vdev.device_id,
          v->vdev.dev.name, v->msix_vectors,
          !v->legacy ? "" : v->leg_mmio ? "; legacy, registers in memory" : "; legacy, registers in I/O space");
    return 0;
}

/* Last reference to the virtio device: a holder from device_find may
 * outlive vpci_remove, so the memory goes here and nowhere else. */
static void vpci_release(struct virtio_device *vdev)
{
    kfree(vdev->tr_priv);
}

static void vpci_remove(struct pci_device *pdev)
{
    struct vpci *v = pdev->dev.drvdata;
    virtio_device_unregister(&v->vdev);   /* runs the virtio driver's remove; drops the bus's reference */
    v->vdev.tr->set_status(&v->vdev, 0);
    pci_msix_disable(pdev);
    vpci_unmap_all(v);
    device_put(&v->vdev.dev);             /* the creator's reference; release frees v when the last holder is gone */
}

static const struct pci_id vpci_ids[] = {
    { VIRTIO_PCI_VENDOR, PCI_ANY, 0, 0, 0 },
    PCI_ID_END,
};

static struct pci_driver vpci_driver = {
    .drv = { .name = "virtio-pci" },
    .ids = vpci_ids,
    .probe = vpci_probe,
    .remove = vpci_remove,
};

/* --- module -------------------------------------------------------------- */

static int virtio_module_init(void)
{
    bus_register(&virtio_bus);
    int rc = pci_register_driver(&vpci_driver);
    if (rc)
        return rc;
    kinfo("virtio: core ready, %u device(s) on the virtio bus", device_count(&virtio_bus));
    return 0;
}

static void virtio_module_shutdown(void)
{
    pci_unregister_driver(&vpci_driver);
}

COSMO_MODULE("virtio", "1.0", virtio_module_init, virtio_module_shutdown, "", MODULE_CAP_DRIVER);
