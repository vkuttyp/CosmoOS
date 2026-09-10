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
#include <kernel/net/ether.h>
#include <kernel/netif.h>
#include <kernel/string.h>

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
