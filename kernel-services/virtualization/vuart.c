/*
 * vuart.c - The guest's console: a PL011 a stock kernel can print to
 * (docs/kernel-services/virtualization/design.md, "The guest's console").
 *
 * One per VM, at the address QEMU's virt puts its UART0 and a stock guest
 * kernel's device tree names, registered on AArch64 the way the debug
 * console is on x86. The register set is the one the host's own
 * pl011.c drives; what a console needs is modelled, the rest is stored
 * and returned or reads as zero. Writes to DR go to the VM's console
 * ring, which the owner reads from the VM's descriptor.
 *
 * In the kernel because a console is written a byte at a time -- an
 * earlycon's line of eighty characters is eighty stores -- and a round
 * trip to userland per byte is the cost the distributor was built to
 * avoid. The seam this sits on (vm_device.mmio) lets an owner-side model
 * exist later; the first device should not pay for it.
 *
 * Locking: `lock` covers the register file and the receive FIFO, taken by
 * a guest's access from its vCPU thread and by the owner's write from
 * its own. It is never held while calling into the distributor: an
 * interrupt is raised or lowered after the lock is dropped, from a state
 * decided under it.
 */
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#include "hv_internal.h"

#define PL011_DR      0x000u
#define PL011_RSR     0x004u
#define PL011_FR      0x018u
#define PL011_IBRD    0x024u
#define PL011_FBRD    0x028u
#define PL011_LCR_H   0x02Cu
#define PL011_CR      0x030u
#define PL011_IFLS    0x034u
#define PL011_IMSC    0x038u
#define PL011_RIS     0x03Cu
#define PL011_MIS     0x040u
#define PL011_ICR     0x044u
#define PL011_DMACR   0x048u
#define PL011_PERIPHID0 0xFE0u   /* 0xFE0..0xFEC, then PCellID 0xFF0..0xFFC */

#define FR_TXFE  (1u << 7)       /* transmit FIFO empty: always, the ring absorbs everything */
#define FR_RXFF  (1u << 6)
#define FR_TXFF  (1u << 5)       /* never */
#define FR_RXFE  (1u << 4)

#define INT_RX   (1u << 4)       /* RXIM / RXRIS / RXMIS */
#define INT_TX   (1u << 5)       /* the transmitter is always ready */
#define INT_CLEARABLE 0x7F0u     /* what a write to ICR may clear; RX and TX are level and are not */

/* The PL011's identification, as the peripheral and PrimeCell ids a
 * driver reads to know what it found: part 0x011, revision 1. */
static const uint8_t pl011_ids[8] = { 0x11, 0x10, 0x14, 0x00, 0x0D, 0xF0, 0x05, 0xB1 };

struct vuart {
    struct vm *vm;
    spinlock_t lock;
    uint32_t ibrd, fbrd, lcr_h, cr, ifls, imsc, dmacr;
    uint64_t tx_bytes;
};

static uint32_t vuart_fr(const struct vuart *u)
{
    (void)u;
    return FR_TXFE | FR_RXFE;
}

/* The raw interrupt state: what is asserted before the mask. */
static uint32_t vuart_ris(const struct vuart *u)
{
    (void)u;
    return INT_TX;
}

static int vuart_mmio(struct vm_device *d, uint64_t gpa, bool write, unsigned size, uint64_t *value)
{
    struct vuart *u = d->priv;
    unsigned off = (unsigned)(gpa - d->mmio_base) & ~3u;
    (void)size;   /* every PL011 register is a word; a narrower access reads or writes its low bits */
    if (write) {
        uint32_t v = (uint32_t)*value;
        switch (off) {
        case PL011_DR: {
            uint8_t b = (uint8_t)v;
            vm_console_put(u->vm, &b, 1);
            arch_irq_state_t s = spin_lock_irqsave(&u->lock);
            u->tx_bytes++;
            spin_unlock_irqrestore(&u->lock, s);
            return 0;
        }
        case PL011_IBRD:
        case PL011_FBRD:
        case PL011_LCR_H:
        case PL011_CR:
        case PL011_IFLS:
        case PL011_IMSC:
        case PL011_DMACR: {
            arch_irq_state_t s = spin_lock_irqsave(&u->lock);
            switch (off) {
            case PL011_IBRD: u->ibrd = v & 0xFFFFu; break;
            case PL011_FBRD: u->fbrd = v & 0x3Fu; break;
            case PL011_LCR_H: u->lcr_h = v & 0xFFu; break;
            case PL011_CR: u->cr = v & 0xFFFFu; break;
            case PL011_IFLS: u->ifls = v & 0x3Fu; break;
            case PL011_IMSC: u->imsc = v & 0x7FFu; break;
            default: u->dmacr = v & 0x7u; break;
            }
            spin_unlock_irqrestore(&u->lock, s);
            return 0;
        }
        case PL011_ICR:
            (void)INT_CLEARABLE;   /* nothing this model asserts is clearable by ICR: RX and TX are level */
            return 0;
        default:
            return 0;   /* read-only, or nothing this model keeps */
        }
    }
    uint32_t r = 0;
    arch_irq_state_t s = spin_lock_irqsave(&u->lock);
    switch (off) {
    case PL011_DR: r = 0; break;               /* nothing to receive yet: FR says RXFE */
    case PL011_RSR: r = 0; break;
    case PL011_FR: r = vuart_fr(u); break;
    case PL011_IBRD: r = u->ibrd; break;
    case PL011_FBRD: r = u->fbrd; break;
    case PL011_LCR_H: r = u->lcr_h; break;
    case PL011_CR: r = u->cr; break;
    case PL011_IFLS: r = u->ifls; break;
    case PL011_IMSC: r = u->imsc; break;
    case PL011_RIS: r = vuart_ris(u); break;
    case PL011_MIS: r = vuart_ris(u) & u->imsc; break;
    case PL011_DMACR: r = u->dmacr; break;
    default:
        if (off >= PL011_PERIPHID0 && off < PL011_PERIPHID0 + 8 * 4)
            r = pl011_ids[(off - PL011_PERIPHID0) / 4];
        break;
    }
    spin_unlock_irqrestore(&u->lock, s);
    *value = r;
    return 0;
}

struct vuart *vuart_create(struct vm *vm, struct vm_device *dev)
{
    struct vuart *u = kzalloc(sizeof(*u));
    if (u == NULL)
        return NULL;
    u->vm = vm;
    spinlock_init(&u->lock, "vuart");
    memset(dev, 0, sizeof(*dev));
    list_init(&dev->link);
    dev->name = "pl011";
    dev->mmio_base = VUART_BASE;
    dev->mmio_len = VUART_SIZE;
    dev->mmio = vuart_mmio;
    dev->priv = u;
    return u;
}

void vuart_destroy(struct vuart *u)
{
    if (u == NULL)
        return;
    if (u->tx_bytes)
        kdebug("vuart: %llu byte(s) printed", (unsigned long long)u->tx_bytes);
    kfree(u);
}
