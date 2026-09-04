/*
 * virtio.c - The virtio bus, device initialisation, driver binding.
 *
 * Part of the `virtio` module together with virtqueue.c and
 * virtio_pci.c. Everything here is transport-independent.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/printf.h>
#include <kernel/string.h>

#include <drivers/virtio.h>

static bool virtio_match(struct device *dev, struct device_driver *drv)
{
    const struct virtio_device *vdev = to_virtio_device(dev);
    const uint32_t *ids = drv->match_data;
    for (; *ids; ids++) {
        if (*ids == vdev->device_id)
            return true;
    }
    return false;
}

struct bus_type virtio_bus = {
    .name = "virtio",
    .match = virtio_match,
};

/* The model names the driver in dev->driver before calling probe. */
static int virtio_probe_thunk(struct device *dev)
{
    struct virtio_driver *vdrv = container_of(dev->driver, struct virtio_driver, drv);
    return vdrv->probe(to_virtio_device(dev));
}

static void virtio_remove_thunk(struct device *dev)
{
    struct virtio_driver *vdrv = container_of(dev->driver, struct virtio_driver, drv);
    if (vdrv->remove)
        vdrv->remove(to_virtio_device(dev));
}

int virtio_register_driver(struct virtio_driver *vdrv)
{
    if (vdrv->ids == NULL || vdrv->probe == NULL)
        return -EINVAL;
    vdrv->drv.bus = &virtio_bus;
    vdrv->drv.match_data = vdrv->ids;
    vdrv->drv.probe = virtio_probe_thunk;
    vdrv->drv.remove = virtio_remove_thunk;
    return driver_register(&vdrv->drv);
}

void virtio_unregister_driver(struct virtio_driver *vdrv)
{
    driver_unregister(&vdrv->drv);
}

static unsigned g_next_index;

int virtio_device_register(struct virtio_device *vdev)
{
    char name[DEVICE_NAME_MAX];
    ksnprintf(name, sizeof(name), "virtio%u", g_next_index++);
    device_setup(&vdev->dev, &virtio_bus, vdev->hw, name);
    vdev->dev.dma_mask = vdev->hw ? vdev->hw->dma_mask : 0xFFFFFFFFULL;
    return device_register(&vdev->dev);
}

void virtio_device_unregister(struct virtio_device *vdev)
{
    device_unregister(&vdev->dev);
}

int virtio_device_init(struct virtio_device *vdev, uint64_t wanted)
{
    const struct virtio_transport *tr = vdev->tr;

    tr->set_status(vdev, 0);   /* reset */
    for (unsigned spin = 0; tr->get_status(vdev) != 0 && spin < 1000000; spin++)
        ;
    if (tr->get_status(vdev) != 0) {
        kerror("virtio: %s: device did not reset", vdev->dev.name);
        return -EIO;
    }
    tr->set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE);
    tr->set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    vdev->device_features = tr->get_features(vdev);
    if ((vdev->device_features & VIRTIO_F_VERSION_1) == 0) {
        kerror("virtio: %s: legacy device (no VERSION_1); refused", vdev->dev.name);
        tr->set_status(vdev, VIRTIO_STATUS_FAILED);
        return -ENOTSUP;
    }
    uint64_t features = (vdev->device_features & wanted) | VIRTIO_F_VERSION_1;
    tr->set_features(vdev, features);
    tr->set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    if ((tr->get_status(vdev) & VIRTIO_STATUS_FEATURES_OK) == 0) {
        kerror("virtio: %s: device rejected features 0x%llx", vdev->dev.name, (unsigned long long)features);
        tr->set_status(vdev, VIRTIO_STATUS_FAILED);
        return -ENOTSUP;
    }
    vdev->features = features;
    kdebug("virtio: %s: id %u, features offered 0x%llx, negotiated 0x%llx", vdev->dev.name, vdev->device_id,
           (unsigned long long)vdev->device_features, (unsigned long long)features);
    return 0;
}

void virtio_device_ready(struct virtio_device *vdev)
{
    uint8_t st = vdev->tr->get_status(vdev);
    vdev->tr->set_status(vdev, st | VIRTIO_STATUS_DRIVER_OK);
}

void virtio_device_reset(struct virtio_device *vdev)
{
    vdev->tr->set_status(vdev, 0);
    for (unsigned spin = 0; vdev->tr->get_status(vdev) != 0 && spin < 1000000; spin++)
        ;
}

void virtio_read_config(struct virtio_device *vdev, unsigned off, void *buf, size_t len)
{
    vdev->tr->read_config(vdev, off, buf, len);
}

uint32_t virtio_read_config32(struct virtio_device *vdev, unsigned off)
{
    uint32_t v;
    vdev->tr->read_config(vdev, off, &v, sizeof(v));
    return v;
}

uint64_t virtio_read_config64(struct virtio_device *vdev, unsigned off)
{
    uint64_t v;
    vdev->tr->read_config(vdev, off, &v, sizeof(v));
    return v;
}

EXPORT_SYMBOL(virtio_bus);
EXPORT_SYMBOL(virtio_register_driver);
EXPORT_SYMBOL(virtio_unregister_driver);
EXPORT_SYMBOL(virtio_device_init);
EXPORT_SYMBOL(virtio_device_ready);
EXPORT_SYMBOL(virtio_device_reset);
EXPORT_SYMBOL(virtio_read_config);
EXPORT_SYMBOL(virtio_read_config32);
EXPORT_SYMBOL(virtio_read_config64);
