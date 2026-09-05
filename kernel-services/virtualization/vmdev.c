/*
 * vmdev.c - Device backends and the built-in debug console
 * (docs/kernel-services/virtualization/design.md, "Device backends").
 *
 * The device table is frozen once a vCPU has run, so the run loop walks
 * it without the VM lock. The console ring has its own spinlock: the
 * producer is the run loop, the consumer the owner's read.
 */

#include <kernel/errno.h>
#include <kernel/string.h>

#include "hv_internal.h"

#define DEBUG_PORT 0xE9u

void vm_console_put(struct vm *vm, const uint8_t *bytes, size_t n)
{
    arch_irq_state_t s = spin_lock_irqsave(&vm->console.lock);
    for (size_t i = 0; i < n; i++) {
        unsigned next = (vm->console.head + 1) % HV_CONSOLE_SIZE;
        if (next == vm->console.tail) {          /* full: drop the oldest byte */
            vm->console.tail = (vm->console.tail + 1) % HV_CONSOLE_SIZE;
            vm->console.dropped++;
        }
        vm->console.buf[vm->console.head] = bytes[i];
        vm->console.head = next;
    }
    spin_unlock_irqrestore(&vm->console.lock, s);
}

size_t vm_console_read(struct vm *vm, void *buf, size_t len)
{
    uint8_t *out = buf;
    size_t n = 0;
    arch_irq_state_t s = spin_lock_irqsave(&vm->console.lock);
    while (n < len && vm->console.tail != vm->console.head) {
        out[n++] = vm->console.buf[vm->console.tail];
        vm->console.tail = (vm->console.tail + 1) % HV_CONSOLE_SIZE;
    }
    spin_unlock_irqrestore(&vm->console.lock, s);
    return n;
}

size_t vm_console_pending(struct vm *vm)
{
    arch_irq_state_t s = spin_lock_irqsave(&vm->console.lock);
    size_t n = (vm->console.head + HV_CONSOLE_SIZE - vm->console.tail) % HV_CONSOLE_SIZE;
    spin_unlock_irqrestore(&vm->console.lock, s);
    return n;
}

static int debug_console_pio(struct vm_device *d, uint16_t port, bool write, unsigned size, uint32_t *value)
{
    struct vm *vm = d->priv;
    (void)port;
    if (write) {
        uint8_t bytes[4];
        for (unsigned i = 0; i < size && i < 4; i++)
            bytes[i] = (uint8_t)(*value >> (8 * i));
        vm_console_put(vm, bytes, size < 4 ? size : 4);
    } else {
        *value = DEBUG_PORT;   /* the Bochs convention: reading the port returns its number */
    }
    return 0;
}

void vmdev_init(struct vm *vm)
{
    struct vm_device *d = &vm->debug_console;
    memset(d, 0, sizeof(*d));
    list_init(&d->link);
    d->name = "debug-console";
    d->pio_base = DEBUG_PORT;
    d->pio_count = 1;
    d->pio = debug_console_pio;
    d->priv = vm;
    list_push_back(&vm->devices, &d->link);
}

int vm_device_register(struct vm *vm, struct vm_device *dev)
{
    if ((dev->pio_count == 0 && dev->mmio_len == 0) || (dev->pio_count && dev->pio == NULL))
        return -EINVAL;
    mutex_lock(&vm->lock);
    if (vm->started) {
        mutex_unlock(&vm->lock);
        return -EBUSY;
    }
    struct list_node *n;
    for (n = vm->devices.next; n != &vm->devices; n = n->next) {
        struct vm_device *o = container_of(n, struct vm_device, link);
        if (dev->pio_count && o->pio_count && dev->pio_base < (unsigned)o->pio_base + o->pio_count &&
            o->pio_base < (unsigned)dev->pio_base + dev->pio_count) {
            mutex_unlock(&vm->lock);
            return -EEXIST;
        }
        if (dev->mmio_len && o->mmio_len && dev->mmio_base < o->mmio_base + o->mmio_len &&
            o->mmio_base < dev->mmio_base + dev->mmio_len) {
            mutex_unlock(&vm->lock);
            return -EEXIST;
        }
    }
    list_init(&dev->link);
    list_push_back(&vm->devices, &dev->link);
    mutex_unlock(&vm->lock);
    return 0;
}

int vmdev_pio(struct vm *vm, uint16_t port, bool write, unsigned size, uint32_t *value)
{
    struct list_node *n;
    for (n = vm->devices.next; n != &vm->devices; n = n->next) {
        struct vm_device *d = container_of(n, struct vm_device, link);
        if (d->pio_count && port >= d->pio_base && port < (unsigned)d->pio_base + d->pio_count)
            return d->pio(d, port, write, size, value);
    }
    return -ENODEV;
}

void vmdev_mmio(struct vm *vm, uint64_t gpa, bool write)
{
    struct list_node *n;
    for (n = vm->devices.next; n != &vm->devices; n = n->next) {
        struct vm_device *d = container_of(n, struct vm_device, link);
        if (d->mmio_len && gpa >= d->mmio_base && gpa < d->mmio_base + d->mmio_len && d->mmio) {
            d->mmio(d, gpa, write);
            return;
        }
    }
}
