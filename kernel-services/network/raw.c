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

/* Raw sockets are taken this many at a time: their references under the
 * list lock, the copies after it, so nothing allocates under a spinlock
 * with interrupts off. */
#define RAW_DELIVER_MAX 16
static uint64_t g_next_id = 1;   /* under g_lock: the list stays in id order */

struct raw_pcb *raw_pcb_new(struct socket *sock, uint8_t proto)
{
    struct raw_pcb *pcb = kzalloc(sizeof(*pcb));
    if (pcb == NULL)
        return NULL;
    pcb->proto = proto;
    pcb->sock = sock;
    mbufq_init(&pcb->rxq, RAW_RXQ_MAX, "raw-rxq");
    list_init(&pcb->link);
    sock->raw = pcb;   /* set before anything can find it on the list */
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    pcb->id = g_next_id++;
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
    uint32_t hlen = IPV4_HDR_LEN(iph);
    uint64_t after = 0;   /* the last pcb id visited: the list is in id order */
    for (;;) {
        /* A batch of references under the lock, the copies after it; then
         * the next batch from where this one stopped, so every raw socket
         * gets its copy however many there are. The pcb travels with its
         * socket's reference: the socket's release frees it, so it lives as
         * long as that reference, and delivery never reads s->raw. */
        struct socket *socks[RAW_DELIVER_MAX];
        struct raw_pcb *pcbs[RAW_DELIVER_MAX];
        unsigned n = 0;
        bool more = false;
        arch_irq_state_t s = spin_lock_irqsave(&g_lock);
        struct raw_pcb *pcb;
        list_for_each_entry(pcb, &g_pcbs, link) {
            if (pcb->id <= after || pcb->proto != IPPROTO_ICMP)
                continue;
            if (n == RAW_DELIVER_MAX) {
                more = true;
                break;
            }
            after = pcb->id;
            /* A tryget: a release clears pcb->sock under this lock but
             * starts at count zero, as UDP's does (design.md, "UDP"). */
            struct socket *sock = sock_wake_ref(pcb->sock);
            if (sock) {
                socks[n] = sock;
                pcbs[n] = pcb;
                n++;
            }
        }
        spin_unlock_irqrestore(&g_lock, s);

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
                if (mbufq_enqueue(&pcbs[i]->rxq, c)) {
                    STAT(rx_delivered);
                    sock_wake(socks[i]);
                } else {
                    /* mbufq_enqueue freed it (mbuf.h): freeing it here too
                     * was a double free (net-raw-icmp, one boot in eight). */
                    __atomic_fetch_add(&pcbs[i]->rx_dropped, 1, __ATOMIC_RELAXED);
                    STAT(rx_queue_full);
                }
            }
            sock_wake_unref(socks[i]);
        }
        if (!more)
            break;
    }
}

void raw_get_stats(struct raw_stats *out)
{
    out->rx_delivered = __atomic_load_n(&g_stats.rx_delivered, __ATOMIC_RELAXED);
    out->rx_queue_full = __atomic_load_n(&g_stats.rx_queue_full, __ATOMIC_RELAXED);
    out->rx_nomem = __atomic_load_n(&g_stats.rx_nomem, __ATOMIC_RELAXED);
    out->tx = __atomic_load_n(&g_stats.tx, __ATOMIC_RELAXED);
}
