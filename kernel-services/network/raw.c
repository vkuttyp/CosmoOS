/*
 * raw.c - Raw ICMP sockets over IPv4 (docs/kernel-services/network/design.md,
 * "Raw ICMP sockets"; invariant N31).
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/net/cksum.h>
#include <kernel/net/raw.h>
#include <kernel/socket.h>
#include <kernel/string.h>

#include <uapi/cosmo/syscall.h>

static LIST_HEAD(g_pcbs);
static spinlock_t g_lock = SPINLOCK_INIT("raw");
static struct raw_stats g_stats;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

/* At most this many raw sockets get one message; any more are counted as
 * full. The references are taken under the list lock and the copies made
 * after it, so nothing allocates under a spinlock with interrupts off. */
#define RAW_DELIVER_MAX 16

struct raw_pcb *raw_pcb_new(struct socket *sock, uint8_t proto)
{
    struct raw_pcb *pcb = kzalloc(sizeof(*pcb));
    if (pcb == NULL)
        return NULL;
    pcb->proto = proto;
    pcb->sock = sock;
    mbufq_init(&pcb->rxq, RAW_RXQ_MAX, "raw-rxq");
    list_init(&pcb->link);
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    list_push_back(&g_pcbs, &pcb->link);
    spin_unlock_irqrestore(&g_lock, s);
    return pcb;
}

void raw_pcb_free(struct raw_pcb *pcb)
{
    if (pcb == NULL)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    list_remove(&pcb->link);
    pcb->sock = NULL;
    spin_unlock_irqrestore(&g_lock, s);
    mbufq_drain(&pcb->rxq);
    kfree(pcb);
}

int raw_sendto(struct raw_pcb *pcb, const void *data, size_t len, const struct netaddr *to)
{
    if (to == NULL)
        return -EDESTADDRREQ;
    if (to->family != COSMO_AF_INET)
        return -EAFNOSUPPORT;
    if (netaddr_is_unspecified(to))
        return -EINVAL;
    if (len > 65535 - 20)
        return -EMSGSIZE;
    uint32_t src = ipv4_source_for(to->v4);
    if (src == 0)
        return -ENETUNREACH;
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return -ENOMEM;
    m->len = m->pkt.len = 0;
    int rc = m_append(m, data, (uint32_t)len);
    if (rc) {
        m_freem(m);
        return rc;
    }
    STAT(tx);
    return ipv4_output(m, src, to->v4, pcb->proto, IP_DEFAULT_TTL);
}

struct mbuf *raw_recv(struct raw_pcb *pcb)
{
    return mbufq_dequeue(&pcb->rxq);
}

void raw_icmp_input(const struct mbuf *m, const struct ipv4_hdr *iph)
{
    struct socket *socks[RAW_DELIVER_MAX];
    unsigned n = 0;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    struct raw_pcb *pcb;
    list_for_each_entry(pcb, &g_pcbs, link) {
        if (pcb->proto != IPPROTO_ICMP)
            continue;
        if (n == RAW_DELIVER_MAX) {
            STAT(rx_queue_full);
            continue;
        }
        /* A tryget: a release clears pcb->sock under this lock but starts
         * at count zero, as UDP's does (design.md, "UDP"). */
        struct socket *sock = sock_wake_ref(pcb->sock);
        if (sock)
            socks[n++] = sock;
    }
    spin_unlock_irqrestore(&g_lock, s);

    uint32_t hlen = IPV4_HDR_LEN(iph);
    for (unsigned i = 0; i < n; i++) {
        struct mbuf *c = m_copypacket(m);
        if (c != NULL)
            c = m_prepend(c, hlen);
        if (c == NULL) {
            STAT(rx_nomem);
        } else {
            memcpy(c->data, iph, hlen);
            memset(&c->pkt.src, 0, sizeof(c->pkt.src));
            c->pkt.src.family = COSMO_AF_INET;
            c->pkt.src.v4 = iph->src;
            if (mbufq_enqueue(&socks[i]->raw->rxq, c)) {
                STAT(rx_delivered);
                sock_wake(socks[i]);
            } else {
                /* mbufq_enqueue freed it (mbuf.h): freeing it here too was a
                 * double free, found by net-raw-icmp failing one boot in eight. */
                socks[i]->raw->rx_dropped++;
                STAT(rx_queue_full);
            }
        }
        sock_wake_unref(socks[i]);
    }
}

void raw_get_stats(struct raw_stats *out)
{
    out->rx_delivered = __atomic_load_n(&g_stats.rx_delivered, __ATOMIC_RELAXED);
    out->rx_queue_full = __atomic_load_n(&g_stats.rx_queue_full, __ATOMIC_RELAXED);
    out->rx_nomem = __atomic_load_n(&g_stats.rx_nomem, __ATOMIC_RELAXED);
    out->tx = __atomic_load_n(&g_stats.tx, __ATOMIC_RELAXED);
}
