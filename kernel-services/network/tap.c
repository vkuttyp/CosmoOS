/*
 * tap.c - A tap interface (tap.h): a netif whose far end is userland.
 *
 * transmit enqueues the frame for the far end to read; tap_inject hands a
 * frame from the far end to netif_rx. The stack does everything else --
 * ARP, IP, the transport -- over an interface whose driver is a reader and
 * a writer rather than a device.
 */
#include <kernel/net/tap.h>

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/netif.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#define TAP_TXQ_MAX 64u   /* frames the stack has queued for the reader; drops when full */

struct tap {
    struct netif nif;
    struct mbufq txq;      /* stack -> far end: frames transmitted out the tap */
};

/* Stack -> tap: the stack is sending a frame out the interface. Queue it for
 * the far end; drop (freeing) when the queue is full, as a NIC drops when it
 * has nowhere to put a frame. Runs in a read-side section: no sleeping. */
static int tap_transmit(struct netif *nif, struct mbuf *m)
{
    struct tap *t = container_of(nif, struct tap, nif);
    if (!mbufq_enqueue(&t->txq, m))
        m_freem(m);        /* "sent" and dropped */
    return 0;
}

static void tap_release(struct netif *nif)
{
    struct tap *t = container_of(nif, struct tap, nif);
    mbufq_drain(&t->txq);
    kfree(t);
}

static const struct netif_ops tap_ops = { .transmit = tap_transmit, .release = tap_release };

struct tap *tap_create(const char *name, uint32_t ip, uint32_t mask, const uint8_t mac[6])
{
    struct tap *t = kmalloc(sizeof(*t), KMEM_ZERO);
    if (t == NULL)
        return NULL;
    strlcpy(t->nif.name, name, sizeof(t->nif.name));
    memcpy(t->nif.mac, mac, 6);
    t->nif.mtu = 1500;
    /* A tap is a point-to-point link to one owner, never the machine's route
     * to the world: keep it out of default-interface selection, so a tap left
     * up (the persistent tap0 has no close hook to bring it down) can never
     * swallow the host's outbound traffic, whatever the registration order. */
    t->nif.flags = NETIF_NODEFAULT;
    t->nif.ops = &tap_ops;
    mbufq_init(&t->txq, TAP_TXQ_MAX, "tap-tx");
    if (netif_register(&t->nif) != 0) {
        kfree(t);
        return NULL;
    }
    netif_set_ipv4(&t->nif, ip, mask, 0);
    netif_set_up(&t->nif, true);
    return t;
}

void tap_destroy(struct tap *t)
{
    if (t == NULL)
        return;
    netif_unregister(&t->nif);   /* down and gone; drops the registry's reference */
    netif_put(&t->nif);          /* drop the creator's reference -> release frees t */
}

int tap_inject(struct tap *t, const void *frame, uint32_t len)
{
    if (len < ETH_HLEN || len > t->nif.mtu + ETH_HLEN)
        return -EMSGSIZE;
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return -ENOMEM;
    if (len > m_trailingspace(m)) {   /* a cluster holds a full frame; this cannot happen */
        m_freem(m);
        return -EMSGSIZE;
    }
    memcpy(m->data, frame, len);
    m->len = m->pkt.len = len;
    netif_rx(&t->nif, m);            /* into the stack, as a driver's completion would */
    return 0;
}

struct mbuf *tap_recv(struct tap *t)
{
    return mbufq_dequeue(&t->txq);
}

struct netif *tap_netif(struct tap *t)
{
    return &t->nif;
}

/* --- /dev/net/tap: the owner's frame channel ---------------------------- */

/* One tap for the one guest an owner runs. Created down at boot so it never
 * competes as the default interface; brought up when the owner first uses
 * the channel. (A per-open lifecycle would need chrdev open/close hooks the
 * ramfs does not have; one persistent tap is enough for one guest.) */
static struct tap *g_devtap;
static struct vnode *g_tapnode;

/* Read one frame the stack transmitted out the tap, or 0 when none waits (a
 * frame is never zero-length, so 0 is unambiguously "nothing now"; the owner
 * polls in its run loop as it drains the console). Never blocks. */
/* A VM has attached to the channel: bring the tap up and, once, turn on
 * forwarding and masquerade so the guest reaches beyond the host (through
 * the host's real interface, with its replies NAT'd back --
 * docs/audit/next-subsystem-nat.md). The flags live on the tap, the guest's
 * ingress, never on the NIC, so the host does not route for its real link. */
static void tap_dev_activate(void)
{
    struct netif *nif = tap_netif(g_devtap);
    if (nif->flags & NETIF_FORWARD) {
        netif_set_up(nif, true);
        return;
    }
    netif_set_up(nif, true);
    netif_set_forward(nif, true);
    netif_set_masquerade(nif, true);
}

static int64_t tap_chr_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    (void)vn; (void)off;
    if (g_devtap == NULL)
        return 0;
    tap_dev_activate();
    struct mbuf *m = tap_recv(g_devtap);
    if (m == NULL)
        return 0;
    uint32_t fl = m_length(m);
    if (fl > len) {          /* the owner must offer a frame-sized buffer */
        m_freem(m);
        return -EMSGSIZE;
    }
    m_copydata(m, 0, fl, buf);
    m_freem(m);
    return (int64_t)fl;
}

/* Inject one frame from the guest into the stack. */
static int64_t tap_chr_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    (void)vn; (void)off;
    if (g_devtap == NULL)
        return -ENODEV;
    tap_dev_activate();
    int rc = tap_inject(g_devtap, buf, (uint32_t)len);
    return rc ? rc : (int64_t)len;
}

static const struct chrdev_ops tap_chr_ops = { .read = tap_chr_read, .write = tap_chr_write };

void tap_dev_init(void)
{
    static const uint8_t host_mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };
    /* 10.0.3.0/24, a subnet of its own: the interfaces the host autoconfigures
     * (a NIC) default to 10.0.2.0/24, and two interfaces on one subnet route
     * ambiguously. The guest gets 10.0.3.15, the host end is 10.0.3.1. */
    g_devtap = tap_create("tap0", IPV4_ADDR(10, 0, 3, 1), htonl(0xffffff00u), host_mac);
    if (g_devtap == NULL) {
        kwarn("tap: cannot create tap0");
        return;
    }
    netif_set_up(tap_netif(g_devtap), false);   /* down until the owner uses the channel */
    vfs_mkdir(NULL, "/dev/net", 0755);           /* -EEXIST is fine */
    int rc = ramfs_mkchr("/dev/net/tap", 0600, &tap_chr_ops, NULL, &g_tapnode);
    if (rc)
        kwarn("tap: cannot create /dev/net/tap (%d)", rc);
}
