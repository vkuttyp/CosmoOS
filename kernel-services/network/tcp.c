/*
 * tcp.c - TCP: RFC 793 state machine, RFC 6298 retransmission timer,
 * RFC 5681 slow start / congestion avoidance / fast retransmit, delayed
 * ACK, TIME_WAIT.
 *
 * One spinlock (g_lock) covers the pcb table and every pcb. Segments are
 * built under the lock into a batch and transmitted after it is dropped;
 * timers run in interrupt context and only queue work for the network
 * worker thread, which takes the lock again.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/tcp.h>
#include <kernel/random.h>
#include <kernel/socket.h>
#include <kernel/string.h>

static LIST_HEAD(g_pcbs);
static spinlock_t g_lock = SPINLOCK_INIT("tcp");
static struct tcp_stats g_stats;
static uint16_t g_next_ephemeral = NET_EPHEMERAL_LO;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)
#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

#define WORK_REXMIT   (1u << 0)
#define WORK_DELACK   (1u << 1)
#define WORK_TIMEWAIT (1u << 2)
#define WORK_FREE     (1u << 3)

/* Segments built under the lock, sent after it. */
struct tcp_batch {
    struct {
        struct mbuf *m;
        struct netaddr src, dst;
    } seg[16];
    unsigned n;
};

/* --- byte rings ---------------------------------------------------------- */

static int netbuf_init(struct netbuf *nb, uint32_t size)
{
    nb->data = kmalloc(size, 0);
    if (nb->data == NULL)
        return -ENOMEM;
    nb->size = size;
    nb->head = nb->len = 0;
    return 0;
}

static void netbuf_free(struct netbuf *nb)
{
    kfree(nb->data);
    nb->data = NULL;
}

static uint32_t netbuf_space(const struct netbuf *nb)
{
    return nb->size - nb->len;
}

static uint32_t netbuf_put(struct netbuf *nb, const void *src, uint32_t len)
{
    const uint8_t *in = src;
    uint32_t n = len < netbuf_space(nb) ? len : netbuf_space(nb);
    for (uint32_t done = 0; done < n;) {
        uint32_t tail = (nb->head + nb->len) % nb->size;
        uint32_t chunk = nb->size - tail;
        if (chunk > n - done)
            chunk = n - done;
        memcpy(nb->data + tail, in + done, chunk);
        nb->len += chunk;
        done += chunk;
    }
    return n;
}

static uint32_t netbuf_put_mbuf(struct netbuf *nb, const struct mbuf *m, uint32_t off, uint32_t len)
{
    uint32_t n = len < netbuf_space(nb) ? len : netbuf_space(nb);
    for (uint32_t done = 0; done < n;) {
        uint32_t tail = (nb->head + nb->len) % nb->size;
        uint32_t chunk = nb->size - tail;
        if (chunk > n - done)
            chunk = n - done;
        m_copydata(m, off + done, chunk, nb->data + tail);
        nb->len += chunk;
        done += chunk;
    }
    return n;
}

static void netbuf_peek(const struct netbuf *nb, uint32_t off, void *dst, uint32_t len)
{
    uint8_t *out = dst;
    for (uint32_t done = 0; done < len;) {
        uint32_t pos = (nb->head + off + done) % nb->size;
        uint32_t chunk = nb->size - pos;
        if (chunk > len - done)
            chunk = len - done;
        memcpy(out + done, nb->data + pos, chunk);
        done += chunk;
    }
}

static void netbuf_drop(struct netbuf *nb, uint32_t n)
{
    if (n > nb->len)
        n = nb->len;
    nb->head = (nb->head + n) % nb->size;
    nb->len -= n;
}

/* --- pcbs -------------------------------------------------------------- */

static void pcb_work(void *arg);

static void rexmit_timer(struct timer *t, void *arg)
{
    (void)t;
    struct tcp_pcb *pcb = arg;
    __atomic_fetch_or(&pcb->work_flags, WORK_REXMIT, __ATOMIC_RELAXED);
    net_work_queue(&pcb->work);
}

static void delack_timer(struct timer *t, void *arg)
{
    (void)t;
    struct tcp_pcb *pcb = arg;
    __atomic_fetch_or(&pcb->work_flags, WORK_DELACK, __ATOMIC_RELAXED);
    net_work_queue(&pcb->work);
}

static void timewait_timer(struct timer *t, void *arg)
{
    (void)t;
    struct tcp_pcb *pcb = arg;
    __atomic_fetch_or(&pcb->work_flags, WORK_TIMEWAIT, __ATOMIC_RELAXED);
    net_work_queue(&pcb->work);
}

struct tcp_pcb *tcp_pcb_new(uint16_t family)
{
    struct tcp_pcb *pcb = kzalloc(sizeof(*pcb));
    if (pcb == NULL)
        return NULL;
    if (netbuf_init(&pcb->sndbuf, TCP_SNDBUF) || netbuf_init(&pcb->rcvbuf, TCP_RCVBUF)) {
        netbuf_free(&pcb->sndbuf);
        netbuf_free(&pcb->rcvbuf);
        kfree(pcb);
        return NULL;
    }
    pcb->state = TCP_CLOSED;
    pcb->local.family = pcb->remote.family = family;
    pcb->mss = family == COSMO_AF_INET ? TCP_MSS_V4 : TCP_MSS_V6;
    pcb->rcv_wnd = TCP_RCVBUF;
    pcb->rto_ns = TCP_RTO_INIT_NS;
    pcb->cwnd = 2 * pcb->mss;
    pcb->ssthresh = 0x7fffffff;
    timer_setup(&pcb->rexmit, rexmit_timer, pcb);
    timer_setup(&pcb->delack, delack_timer, pcb);
    timer_setup(&pcb->timewait, timewait_timer, pcb);
    list_init(&pcb->link);
    list_init(&pcb->accept_link);
    list_init(&pcb->accept_queue);
    net_work_init(&pcb->work, pcb_work, pcb);
    return pcb;
}

/* Lock held. Frees now, or after a queued work item has run. */
static void pcb_free_locked(struct tcp_pcb *pcb)
{
    timer_cancel(&pcb->rexmit);
    timer_cancel(&pcb->delack);
    timer_cancel(&pcb->timewait);
    if (!list_empty(&pcb->link)) {
        list_remove(&pcb->link);
        list_init(&pcb->link);
    }
    if (pcb->listener && !list_empty(&pcb->accept_link)) {
        list_remove(&pcb->accept_link);
        list_init(&pcb->accept_link);
        pcb->listener->nr_queued--;
    }
    pcb->state = TCP_CLOSED;
    pcb->sock = NULL;
    if (pcb->work.queued) {
        __atomic_fetch_or(&pcb->work_flags, WORK_FREE, __ATOMIC_RELAXED);
        return;
    }
    netbuf_free(&pcb->sndbuf);
    netbuf_free(&pcb->rcvbuf);
    kfree(pcb);
}

/*
 * Lock held. The connection is over but a socket still holds the pcb: it
 * becomes CLOSED and leaves the table, so it neither reserves its port nor
 * matches a segment; tcp_close frees it. A later connect re-inserts it.
 */
static void pcb_retire_locked(struct tcp_pcb *pcb)
{
    pcb->state = TCP_CLOSED;
    timer_cancel(&pcb->rexmit);
    timer_cancel(&pcb->delack);
    timer_cancel(&pcb->timewait);
    if (!list_empty(&pcb->link)) {
        list_remove(&pcb->link);
        list_init(&pcb->link);
    }
}

/* Lock held. The connection has ended: free the pcb, or retire it under a live socket. */
static void pcb_end_locked(struct tcp_pcb *pcb)
{
    if (pcb->sock == NULL)
        pcb_free_locked(pcb);
    else
        pcb_retire_locked(pcb);
}

static bool port_in_use(uint16_t family, uint16_t port, const struct netaddr *addr, const struct tcp_pcb *self)
{
    for (struct list_node *node = g_pcbs.next; node != &g_pcbs; node = node->next) {
        KASSERT(node != NULL);
        struct tcp_pcb *p = container_of(node, struct tcp_pcb, link);
        if (p == self || p->local.family != family || p->local.port != port)
            continue;
        if (p->state == TCP_TIME_WAIT)
            continue;   /* reuse is allowed once the socket is gone */
        if (netaddr_is_unspecified(&p->local) || netaddr_is_unspecified(addr) || netaddr_addr_equal(&p->local, addr))
            return true;
    }
    return false;
}

static uint16_t pick_ephemeral(uint16_t family, const struct netaddr *addr)
{
    for (unsigned n = 0; n < NET_EPHEMERAL_HI - NET_EPHEMERAL_LO; n++) {
        uint16_t port = g_next_ephemeral;
        g_next_ephemeral = g_next_ephemeral >= NET_EPHEMERAL_HI ? (uint16_t)NET_EPHEMERAL_LO : (uint16_t)(g_next_ephemeral + 1);
        if (!port_in_use(family, port, addr, NULL))
            return port;
    }
    return 0;
}

/* --- segment construction ---------------------------------------------------- */

static uint16_t seg_mss(const struct tcp_pcb *pcb)
{
    bool lo = (pcb->local.family == COSMO_AF_INET) ? ((ntohl(pcb->remote.v4) >> 24) == 127 || netif_owns_ipv4(pcb->remote.v4))
                                                    : (in6_is_loopback(&pcb->remote.v6) || netif_owns_ipv6(&pcb->remote.v6));
    return lo ? TCP_MSS_LO : (pcb->local.family == COSMO_AF_INET ? TCP_MSS_V4 : TCP_MSS_V6);
}

/* Lock held. Build one segment into the batch; data comes from sndbuf at
 * (seq - snd_una). */
static int build_segment(struct tcp_pcb *pcb, struct tcp_batch *b, uint8_t flags, uint32_t seq, uint32_t datalen,
                         bool mss_option)
{
    if (b->n == ARRAY_SIZE(b->seg))
        return -ENOSPC;
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return -ENOMEM;
    unsigned hlen = sizeof(struct tcp_hdr) + (mss_option ? 4 : 0);
    struct tcp_hdr *th = (struct tcp_hdr *)m->data;
    memset(th, 0, hlen);
    th->sport = htons(pcb->local.port);
    th->dport = htons(pcb->remote.port);
    th->seq = htonl(seq);
    th->ack = htonl(pcb->rcv_nxt);
    th->doff = (uint8_t)((hlen / 4) << 4);
    th->flags = flags;
    uint32_t wnd = pcb->rcv_wnd > 65535 ? 65535 : pcb->rcv_wnd;
    th->win = htons((uint16_t)wnd);
    if (mss_option) {
        uint8_t *opt = m->data + sizeof(*th);
        uint16_t mss = seg_mss(pcb);
        opt[0] = 2;
        opt[1] = 4;
        opt[2] = (uint8_t)(mss >> 8);
        opt[3] = (uint8_t)mss;
    }
    m->len = m->pkt.len = hlen;
    if (datalen) {
        uint8_t tmp[1500];
        uint32_t off = seq - pcb->snd_una;
        for (uint32_t done = 0; done < datalen;) {
            uint32_t chunk = datalen - done < sizeof(tmp) ? datalen - done : (uint32_t)sizeof(tmp);
            netbuf_peek(&pcb->sndbuf, off + done, tmp, chunk);
            if (m_append(m, tmp, chunk)) {
                m_freem(m);
                return -ENOMEM;
            }
            done += chunk;
        }
    }
    uint32_t sum;
    if (pcb->local.family == COSMO_AF_INET)
        sum = cksum_pseudo4(pcb->local.v4, pcb->remote.v4, IPPROTO_TCP, (uint16_t)m->pkt.len);
    else
        sum = cksum_pseudo6(&pcb->local.v6, &pcb->remote.v6, IPPROTO_TCP, m->pkt.len);
    th->cksum = cksum_fold(m_cksum_partial(m, 0, m->pkt.len, sum));
    b->seg[b->n].m = m;
    b->seg[b->n].src = pcb->local;
    b->seg[b->n].dst = pcb->remote;
    b->n++;
    pcb->segs_out++;
    STAT(segs_out);
    return 0;
}

/* Reset in reply to a segment for which there is no connection. */
static void build_rst(struct tcp_batch *b, const struct netaddr *src, const struct netaddr *dst, const struct tcp_hdr *th,
                      uint32_t seglen)
{
    if (b->n == ARRAY_SIZE(b->seg) || (th->flags & TH_RST))
        return;
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return;
    struct tcp_hdr *r = (struct tcp_hdr *)m->data;
    memset(r, 0, sizeof(*r));
    r->sport = th->dport;
    r->dport = th->sport;
    r->doff = (uint8_t)((sizeof(*r) / 4) << 4);
    if (th->flags & TH_ACK) {
        r->seq = th->ack;
        r->flags = TH_RST;
    } else {
        r->seq = 0;
        r->ack = htonl(ntohl(th->seq) + seglen + ((th->flags & TH_SYN) ? 1 : 0) + ((th->flags & TH_FIN) ? 1 : 0));
        r->flags = TH_RST | TH_ACK;
    }
    m->len = m->pkt.len = sizeof(*r);
    uint32_t sum = src->family == COSMO_AF_INET ? cksum_pseudo4(src->v4, dst->v4, IPPROTO_TCP, (uint16_t)m->pkt.len)
                                                 : cksum_pseudo6(&src->v6, &dst->v6, IPPROTO_TCP, m->pkt.len);
    r->cksum = cksum_fold(m_cksum_partial(m, 0, m->pkt.len, sum));
    b->seg[b->n].m = m;
    b->seg[b->n].src = *src;
    b->seg[b->n].dst = *dst;
    b->n++;
    STAT(rsts_out);
    STAT(segs_out);
}

/* No lock. */
static void batch_send(struct tcp_batch *b)
{
    for (unsigned i = 0; i < b->n; i++) {
        struct mbuf *m = b->seg[i].m;
        if (b->seg[i].src.family == COSMO_AF_INET)
            ipv4_output(m, b->seg[i].src.v4, b->seg[i].dst.v4, IPPROTO_TCP, IP_DEFAULT_TTL);
        else
            ipv6_output(m, &b->seg[i].src.v6, &b->seg[i].dst.v6, IPPROTO_TCP, IP_DEFAULT_TTL);
    }
    b->n = 0;
}

static void arm_rexmit(struct tcp_pcb *pcb)
{
    timer_cancel(&pcb->rexmit);   /* timer_start on an armed timer panics */
    timer_start(&pcb->rexmit, pcb->rto_ns);
}

static void disarm_rexmit(struct tcp_pcb *pcb)
{
    timer_cancel(&pcb->rexmit);
}

/* Lock held. Send what the windows allow, a queued FIN, or a pending ACK. */
static void tcp_output_locked(struct tcp_pcb *pcb, struct tcp_batch *b)
{
    bool sent = false;
    if (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_CLOSE_WAIT || pcb->state == TCP_FIN_WAIT_1 ||
        pcb->state == TCP_CLOSING || pcb->state == TCP_LAST_ACK) {
        for (;;) {
            uint32_t inflight = pcb->snd_nxt - pcb->snd_una;
            uint32_t off = inflight;
            if (off > pcb->sndbuf.len)
                break;
            uint32_t avail = pcb->sndbuf.len - off;
            uint32_t wnd = pcb->snd_wnd < pcb->cwnd ? pcb->snd_wnd : pcb->cwnd;
            uint32_t room = wnd > inflight ? wnd - inflight : 0;
            uint32_t seglen = avail;
            if (seglen > pcb->mss)
                seglen = pcb->mss;
            if (seglen > room)
                seglen = room;
            if (seglen == 0) {
                /* Zero window with data waiting: one-byte probe on the timer. */
                if (avail && pcb->snd_wnd == 0 && inflight == 0 && pcb->work_flags & WORK_REXMIT)
                    seglen = 1;
                else
                    break;
            }
            uint8_t flags = TH_ACK | (seglen == avail ? TH_PSH : 0);
            if (build_segment(pcb, b, flags, pcb->snd_nxt, seglen, false))
                break;
            if (!pcb->rtt_pending) {
                pcb->rtt_pending = true;
                pcb->rtt_seq = pcb->snd_nxt + seglen;
                pcb->rtt_start_ns = clock_now_ns();
            }
            pcb->snd_nxt += seglen;
            if (SEQ_GT(pcb->snd_nxt, pcb->snd_max))
                pcb->snd_max = pcb->snd_nxt;
            sent = true;
        }
        if (pcb->fin_queued && !pcb->fin_sent && pcb->snd_nxt - pcb->snd_una == pcb->sndbuf.len) {
            if (build_segment(pcb, b, TH_FIN | TH_ACK, pcb->snd_nxt, 0, false) == 0) {
                pcb->fin_sent = true;
                pcb->snd_nxt++;
                if (SEQ_GT(pcb->snd_nxt, pcb->snd_max))
                    pcb->snd_max = pcb->snd_nxt;
                sent = true;
            }
        }
    }
    if (!sent && pcb->delack_pending && pcb->state != TCP_CLOSED && pcb->state != TCP_LISTEN &&
        pcb->state != TCP_SYN_SENT) {
        build_segment(pcb, b, TH_ACK, pcb->snd_nxt, 0, false);
        sent = true;
    }
    if (sent) {
        pcb->delack_pending = false;
        timer_cancel(&pcb->delack);
    }
    if (SEQ_GT(pcb->snd_nxt, pcb->snd_una) || (pcb->sndbuf.len && pcb->snd_wnd == 0))
        arm_rexmit(pcb);
    else
        disarm_rexmit(pcb);
}

static void sock_wake_after(struct socket *s)
{
    if (s) {
        sock_wake(s);
        ksock_put(s);
    }
}

/* Lock held: take a reference to wake after unlocking. */
static struct socket *sock_ref(struct tcp_pcb *pcb)
{
    if (pcb->sock)
        ksock_get(pcb->sock);
    return pcb->sock;
}

/* --- worker-side timer handling ----------------------------------------------------- */

static void pcb_work(void *arg)
{
    struct tcp_pcb *pcb = arg;
    struct tcp_batch b = { .n = 0 };
    struct socket *wake = NULL;

    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    unsigned flags = __atomic_exchange_n(&pcb->work_flags, 0, __ATOMIC_ACQ_REL);
    if (flags & WORK_FREE) {
        netbuf_free(&pcb->sndbuf);
        netbuf_free(&pcb->rcvbuf);
        kfree(pcb);
        spin_unlock_irqrestore(&g_lock, s);
        return;
    }
    if ((flags & WORK_TIMEWAIT) && pcb->state == TCP_TIME_WAIT) {
        /* Under a socket that was shut down but not closed the pcb is
         * retired, not freed; it lives until close. */
        wake = sock_ref(pcb);
        pcb_end_locked(pcb);
        spin_unlock_irqrestore(&g_lock, s);
        sock_wake_after(wake);
        return;
    }
    if (flags & WORK_DELACK)
        tcp_output_locked(pcb, &b);
    if ((flags & WORK_REXMIT) && pcb->state != TCP_CLOSED && pcb->state != TCP_LISTEN && pcb->state != TCP_TIME_WAIT) {
        if (SEQ_GT(pcb->snd_max, pcb->snd_una) || pcb->state == TCP_SYN_SENT || pcb->state == TCP_SYN_RCVD ||
            (pcb->sndbuf.len && pcb->snd_wnd == 0)) {
            pcb->rexmit_count++;
            pcb->retransmits++;
            STAT(retransmits);
            if (pcb->rexmit_count > TCP_MAX_REXMIT) {
                STAT(timeouts);
                pcb->error = -ETIMEDOUT;
                wake = sock_ref(pcb);
                pcb_end_locked(pcb);
                spin_unlock_irqrestore(&g_lock, s);
                sock_wake_after(wake);
                return;
            }
            pcb->rto_ns *= 2;
            if (pcb->rto_ns > TCP_RTO_MAX_NS)
                pcb->rto_ns = TCP_RTO_MAX_NS;
            uint32_t inflight = pcb->snd_max - pcb->snd_una;
            pcb->ssthresh = inflight / 2 > 2u * pcb->mss ? inflight / 2 : 2u * pcb->mss;
            pcb->cwnd = pcb->mss;
            pcb->rtt_pending = false;
            if (pcb->state == TCP_SYN_SENT) {
                build_segment(pcb, &b, TH_SYN, pcb->iss, 0, true);
                arm_rexmit(pcb);
            } else if (pcb->state == TCP_SYN_RCVD) {
                build_segment(pcb, &b, TH_SYN | TH_ACK, pcb->iss, 0, true);
                arm_rexmit(pcb);
            } else {
                /* Go back to snd_una; a sent FIN is sent again too. */
                pcb->snd_nxt = pcb->snd_una;
                if (pcb->fin_sent)
                    pcb->fin_sent = false;
                __atomic_fetch_or(&pcb->work_flags, WORK_REXMIT, __ATOMIC_RELAXED);   /* allow the probe */
                tcp_output_locked(pcb, &b);
                __atomic_fetch_and(&pcb->work_flags, ~WORK_REXMIT, __ATOMIC_RELAXED);
            }
        }
    }
    spin_unlock_irqrestore(&g_lock, s);
    batch_send(&b);
}

/* --- API used by the socket layer ------------------------------------------------- */

int tcp_bind(struct tcp_pcb *pcb, const struct netaddr *local)
{
    if (local->family != pcb->local.family)
        return -EAFNOSUPPORT;
    if (!netaddr_is_unspecified(local)) {
        bool ours = local->family == COSMO_AF_INET ? netif_owns_ipv4(local->v4) : netif_owns_ipv6(&local->v6);
        if (!ours)
            return -EADDRNOTAVAIL;
    }
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    if (pcb->local.port != 0 || pcb->state != TCP_CLOSED) {
        spin_unlock_irqrestore(&g_lock, s);
        return -EINVAL;
    }
    uint16_t port = local->port;
    if (port == 0)
        port = pick_ephemeral(local->family, local);
    if (port == 0 || port_in_use(local->family, port, local, pcb)) {
        spin_unlock_irqrestore(&g_lock, s);
        return -EADDRINUSE;
    }
    pcb->local = *local;
    pcb->local.port = port;
    if (list_empty(&pcb->link))
        list_push_back(&g_pcbs, &pcb->link);
    spin_unlock_irqrestore(&g_lock, s);
    return 0;
}

int tcp_listen(struct tcp_pcb *pcb, unsigned backlog)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    if (pcb->local.port == 0 || pcb->state != TCP_CLOSED) {
        spin_unlock_irqrestore(&g_lock, s);
        return -EINVAL;
    }
    pcb->state = TCP_LISTEN;
    pcb->backlog = backlog == 0 ? 1 : (backlog > TCP_MAX_BACKLOG ? TCP_MAX_BACKLOG : backlog);
    spin_unlock_irqrestore(&g_lock, s);
    return 0;
}

struct tcp_pcb *tcp_accept(struct tcp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    struct tcp_pcb *child = NULL, *c;
    list_for_each_entry(c, &pcb->accept_queue, accept_link) {
        if (c->state != TCP_SYN_RCVD) {
            child = c;
            break;
        }
    }
    if (child) {
        list_remove(&child->accept_link);
        list_init(&child->accept_link);
        pcb->nr_queued--;
        child->listener = NULL;
    }
    spin_unlock_irqrestore(&g_lock, s);
    return child;
}

int tcp_connect(struct tcp_pcb *pcb, const struct netaddr *remote)
{
    if (remote->family != pcb->local.family)
        return -EAFNOSUPPORT;
    if (remote->port == 0 || netaddr_is_unspecified(remote))
        return -EINVAL;
    struct netaddr local = pcb->local;
    if (netaddr_is_unspecified(&local)) {
        if (local.family == COSMO_AF_INET) {
            local.v4 = ipv4_source_for(remote->v4);
            if (local.v4 == 0)
                return -ENETUNREACH;
        } else {
            ipv6_source_for(&remote->v6, &local.v6);
            if (in6_is_unspecified(&local.v6))
                return -ENETUNREACH;
        }
    }
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    if (pcb->state != TCP_CLOSED) {
        spin_unlock_irqrestore(&g_lock, s);
        return pcb->state == TCP_SYN_SENT ? -EALREADY : -EISCONN;
    }
    if (pcb->local.port == 0) {
        uint16_t port = pick_ephemeral(local.family, &local);
        if (port == 0) {
            spin_unlock_irqrestore(&g_lock, s);
            return -EADDRINUSE;
        }
        local.port = port;
    }
    pcb->local = local;
    pcb->remote = *remote;
    if (list_empty(&pcb->link))
        list_push_back(&g_pcbs, &pcb->link);
    pcb->iss = (uint32_t)random_u64();
    pcb->snd_una = pcb->iss;
    pcb->snd_nxt = pcb->snd_max = pcb->iss + 1;
    pcb->snd_wnd = pcb->mss;
    pcb->mss = seg_mss(pcb);
    pcb->cwnd = 2 * pcb->mss;
    pcb->state = TCP_SYN_SENT;
    STAT(conns_active);
    build_segment(pcb, &b, TH_SYN, pcb->iss, 0, true);
    arm_rexmit(pcb);
    spin_unlock_irqrestore(&g_lock, s);
    batch_send(&b);
    return 0;
}

int64_t tcp_send(struct tcp_pcb *pcb, const void *data, size_t len)
{
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    if (pcb->error) {
        int e = pcb->error;
        spin_unlock_irqrestore(&g_lock, s);
        return e;
    }
    if (pcb->state != TCP_ESTABLISHED && pcb->state != TCP_CLOSE_WAIT) {
        spin_unlock_irqrestore(&g_lock, s);
        return pcb->state == TCP_SYN_SENT || pcb->state == TCP_SYN_RCVD ? -EAGAIN : -EPIPE;
    }
    if (pcb->fin_queued) {
        spin_unlock_irqrestore(&g_lock, s);
        return -EPIPE;
    }
    uint32_t n = netbuf_put(&pcb->sndbuf, data, (uint32_t)(len > 0xffffffffu ? 0xffffffffu : len));
    if (n)
        tcp_output_locked(pcb, &b);
    spin_unlock_irqrestore(&g_lock, s);
    batch_send(&b);
    return (int64_t)n;
}

int64_t tcp_recv(struct tcp_pcb *pcb, void *data, size_t len, bool *peer_closed)
{
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    uint32_t n = pcb->rcvbuf.len < len ? pcb->rcvbuf.len : (uint32_t)len;
    if (n) {
        netbuf_peek(&pcb->rcvbuf, 0, data, n);
        netbuf_drop(&pcb->rcvbuf, n);
        uint32_t old = pcb->rcv_wnd;
        pcb->rcv_wnd = netbuf_space(&pcb->rcvbuf);
        /* Window update when the peer may be stalled on a small window. */
        if (old < pcb->mss && pcb->rcv_wnd >= pcb->mss && pcb->state == TCP_ESTABLISHED) {
            pcb->delack_pending = true;
            tcp_output_locked(pcb, &b);
        }
    }
    *peer_closed = pcb->fin_rcvd && pcb->rcvbuf.len == 0;
    int err = pcb->error;
    spin_unlock_irqrestore(&g_lock, s);
    batch_send(&b);
    if (n == 0 && err && !*peer_closed)
        return err;
    return (int64_t)n;
}

int tcp_shutdown_write(struct tcp_pcb *pcb)
{
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    switch (pcb->state) {
    case TCP_ESTABLISHED:
        pcb->state = TCP_FIN_WAIT_1;
        break;
    case TCP_CLOSE_WAIT:
        pcb->state = TCP_LAST_ACK;
        break;
    case TCP_SYN_RCVD:
        pcb->state = TCP_FIN_WAIT_1;
        break;
    default:
        spin_unlock_irqrestore(&g_lock, s);
        return pcb->fin_queued ? 0 : -ENOTCONN;
    }
    pcb->fin_queued = true;
    tcp_output_locked(pcb, &b);
    spin_unlock_irqrestore(&g_lock, s);
    batch_send(&b);
    return 0;
}

uint32_t tcp_send_space(struct tcp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    uint32_t n = netbuf_space(&pcb->sndbuf);
    spin_unlock_irqrestore(&g_lock, s);
    return n;
}

uint32_t tcp_recv_avail(struct tcp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    uint32_t n = pcb->rcvbuf.len;
    spin_unlock_irqrestore(&g_lock, s);
    return n;
}

void tcp_close(struct tcp_pcb *pcb)
{
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    pcb->sock = NULL;
    switch (pcb->state) {
    case TCP_CLOSED:
    case TCP_SYN_SENT:
        pcb_free_locked(pcb);
        break;
    case TCP_LISTEN: {
        struct tcp_pcb *c, *tmp;
        list_for_each_entry_safe(c, tmp, &pcb->accept_queue, accept_link) {
            list_remove(&c->accept_link);
            list_init(&c->accept_link);
            c->listener = NULL;
            /* A reset carrying the child's own sequence number: build_rst
             * takes the peer's view, whose ack is our snd_nxt. */
            struct tcp_hdr peer = { .flags = TH_ACK, .sport = htons(c->remote.port), .dport = htons(c->local.port),
                                    .ack = htonl(c->snd_nxt) };
            build_rst(&b, &c->local, &c->remote, &peer, 0);
            pcb_free_locked(c);
        }
        pcb->nr_queued = 0;
        pcb_free_locked(pcb);
        break;
    }
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        if (pcb->rcvbuf.len > 0) {
            /* Unread data: the peer learns through a reset (RFC 2525 2.17). */
            build_segment(pcb, &b, TH_RST | TH_ACK, pcb->snd_nxt, 0, false);
            STAT(rsts_out);
            pcb_free_locked(pcb);
            break;
        }
        pcb->state = pcb->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
        pcb->fin_queued = true;
        tcp_output_locked(pcb, &b);
        break;
    default:
        /* Already closing; it frees itself when done. */
        break;
    }
    spin_unlock_irqrestore(&g_lock, s);
    batch_send(&b);
}

/* --- input -------------------------------------------------------------------- */

static struct tcp_pcb *lookup(const struct netaddr *local, const struct netaddr *remote)
{
    struct tcp_pcb *p, *listener = NULL;
    list_for_each_entry(p, &g_pcbs, link) {
        if (p->local.family != local->family || p->local.port != local->port)
            continue;
        if (p->state == TCP_LISTEN) {
            if (netaddr_is_unspecified(&p->local) || netaddr_addr_equal(&p->local, local))
                listener = p;
            continue;
        }
        if (p->remote.port == remote->port && netaddr_addr_equal(&p->remote, remote) &&
            netaddr_addr_equal(&p->local, local))
            return p;
    }
    return listener;
}

static uint16_t parse_mss(const struct tcp_hdr *th, const uint8_t *opts, unsigned optlen, uint16_t dflt)
{
    (void)th;
    unsigned i = 0;
    while (i < optlen) {
        uint8_t kind = opts[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= optlen)
            break;
        uint8_t len = opts[i + 1];
        if (len < 2 || i + len > optlen)
            break;
        if (kind == 2 && len == 4) {
            uint16_t mss = (uint16_t)((opts[i + 2] << 8) | opts[i + 3]);
            return mss < 536 ? 536 : mss;
        }
        i += len;
    }
    return dflt;
}

static void rtt_sample(struct tcp_pcb *pcb, uint64_t now)
{
    uint64_t r = now - pcb->rtt_start_ns;
    if (pcb->srtt_ns == 0) {
        pcb->srtt_ns = r;
        pcb->rttvar_ns = r / 2;
    } else {
        uint64_t diff = pcb->srtt_ns > r ? pcb->srtt_ns - r : r - pcb->srtt_ns;
        pcb->rttvar_ns = (3 * pcb->rttvar_ns + diff) / 4;
        pcb->srtt_ns = (7 * pcb->srtt_ns + r) / 8;
    }
    uint64_t rto = pcb->srtt_ns + (4 * pcb->rttvar_ns > 10000000ull ? 4 * pcb->rttvar_ns : 10000000ull);
    if (rto < TCP_RTO_MIN_NS)
        rto = TCP_RTO_MIN_NS;
    if (rto > TCP_RTO_MAX_NS)
        rto = TCP_RTO_MAX_NS;
    pcb->rto_ns = rto;
    pcb->rtt_pending = false;
}

static void enter_time_wait(struct tcp_pcb *pcb)
{
    pcb->state = TCP_TIME_WAIT;
    disarm_rexmit(pcb);
    timer_cancel(&pcb->delack);
    timer_cancel(&pcb->timewait);
    timer_start(&pcb->timewait, TCP_TIMEWAIT_NS);
}

void tcp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6)
{
    (void)nif;
    STAT(segs_in);
    uint32_t len = m->pkt.len;
    if (len < sizeof(struct tcp_hdr)) {
        m_freem(m);
        return;
    }
    m = m_pullup(m, sizeof(struct tcp_hdr));
    if (m == NULL)
        return;
    const struct tcp_hdr *th = (const struct tcp_hdr *)m->data;
    unsigned hlen = TCP_HDR_LEN(th);
    if (hlen < sizeof(*th) || hlen > len) {
        m_freem(m);
        return;
    }
    m = m_pullup(m, hlen);
    if (m == NULL)
        return;
    th = (const struct tcp_hdr *)m->data;

    struct netaddr src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    uint32_t sum;
    if (ip4) {
        src.family = dst.family = COSMO_AF_INET;
        src.v4 = ip4->src;
        dst.v4 = ip4->dst;
        sum = cksum_pseudo4(ip4->src, ip4->dst, IPPROTO_TCP, (uint16_t)len);
    } else {
        src.family = dst.family = COSMO_AF_INET6;
        src.v6 = ip6->src;
        dst.v6 = ip6->dst;
        sum = cksum_pseudo6(&ip6->src, &ip6->dst, IPPROTO_TCP, len);
    }
    if (cksum_fold(m_cksum_partial(m, 0, len, sum)) != 0) {
        STAT(bad_cksum);
        m_freem(m);
        return;
    }
    if (m->flags & (M_BCAST | M_MCAST)) {
        m_freem(m);
        return;
    }
    src.port = ntohs(th->sport);
    dst.port = ntohs(th->dport);
    struct tcp_hdr hdr;
    memcpy(&hdr, th, sizeof(hdr));
    uint8_t opts[40];
    unsigned optlen = hlen - sizeof(hdr);
    memcpy(opts, m->data + sizeof(hdr), optlen);
    m_adj(m, (int)hlen);
    uint32_t seglen = m->pkt.len;
    uint32_t seq = ntohl(hdr.seq), ack = ntohl(hdr.ack);
    uint8_t flags = hdr.flags;
    uint16_t win = ntohs(hdr.win);

    struct tcp_batch b = { .n = 0 };
    struct socket *wake = NULL, *wake_listener = NULL;

    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    struct tcp_pcb *pcb = lookup(&dst, &src);
    if (pcb == NULL) {
        STAT(dropped_no_pcb);
        build_rst(&b, &dst, &src, &hdr, seglen);
        goto out;
    }
    pcb->segs_in++;

    if (pcb->state == TCP_LISTEN) {
        if (flags & TH_RST)
            goto out;
        if (flags & TH_ACK) {
            build_rst(&b, &dst, &src, &hdr, seglen);
            goto out;
        }
        if (!(flags & TH_SYN))
            goto out;
        if (pcb->nr_queued >= pcb->backlog)
            goto out;   /* backlog full: the client retransmits */
        struct tcp_pcb *c = tcp_pcb_new(dst.family);
        if (c == NULL)
            goto out;
        c->local = dst;
        c->remote = src;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->iss = (uint32_t)random_u64();
        c->snd_una = c->iss;
        c->snd_nxt = c->snd_max = c->iss + 1;
        c->snd_wnd = win;
        c->snd_wl1 = seq;
        c->snd_wl2 = ack;
        c->mss = parse_mss(&hdr, opts, optlen, c->mss);
        if (c->mss > seg_mss(c))
            c->mss = seg_mss(c);
        c->cwnd = 2 * c->mss;
        c->state = TCP_SYN_RCVD;
        c->listener = pcb;
        list_push_back(&pcb->accept_queue, &c->accept_link);
        pcb->nr_queued++;
        list_push_back(&g_pcbs, &c->link);
        STAT(conns_passive);
        build_segment(c, &b, TH_SYN | TH_ACK, c->iss, 0, true);
        arm_rexmit(c);
        goto out;
    }

    if (pcb->state == TCP_SYN_SENT) {
        if (flags & TH_ACK) {
            if (SEQ_LEQ(ack, pcb->iss) || SEQ_GT(ack, pcb->snd_nxt)) {
                if (!(flags & TH_RST))
                    build_rst(&b, &dst, &src, &hdr, seglen);
                goto out;
            }
        }
        if (flags & TH_RST) {
            if (flags & TH_ACK) {
                STAT(rsts_in);
                pcb->error = -ECONNREFUSED;
                wake = sock_ref(pcb);
                pcb_end_locked(pcb);
            }
            goto out;
        }
        if (!(flags & TH_SYN))
            goto out;
        pcb->irs = seq;
        pcb->rcv_nxt = seq + 1;
        pcb->mss = parse_mss(&hdr, opts, optlen, pcb->mss);
        if (pcb->mss > seg_mss(pcb))
            pcb->mss = seg_mss(pcb);
        pcb->cwnd = 2 * pcb->mss;
        pcb->snd_wnd = win;
        pcb->snd_wl1 = seq;
        pcb->snd_wl2 = ack;
        if (flags & TH_ACK) {
            pcb->snd_una = ack;
            pcb->state = TCP_ESTABLISHED;
            pcb->rexmit_count = 0;
            STAT(conns_established);
            disarm_rexmit(pcb);
            pcb->delack_pending = true;
            tcp_output_locked(pcb, &b);
            wake = sock_ref(pcb);
        } else {
            pcb->state = TCP_SYN_RCVD;
            build_segment(pcb, &b, TH_SYN | TH_ACK, pcb->iss, 0, true);
            arm_rexmit(pcb);
        }
        goto out;
    }

    /* --- synchronized states: sequence check first --- */
    if (pcb->state == TCP_TIME_WAIT) {
        if (flags & TH_RST)
            goto out;
        /* Re-ACK a retransmitted FIN and restart the timer. */
        pcb->delack_pending = true;
        build_segment(pcb, &b, TH_ACK, pcb->snd_nxt, 0, false);
        pcb->delack_pending = false;
        timer_cancel(&pcb->timewait);
        timer_start(&pcb->timewait, TCP_TIMEWAIT_NS);
        goto out;
    }

    bool acceptable;
    uint32_t wnd_end = pcb->rcv_nxt + pcb->rcv_wnd;
    if (seglen == 0)
        acceptable = pcb->rcv_wnd == 0 ? seq == pcb->rcv_nxt : (SEQ_GEQ(seq, pcb->rcv_nxt) && SEQ_LT(seq, wnd_end));
    else
        acceptable = pcb->rcv_wnd != 0 &&
                     ((SEQ_GEQ(seq, pcb->rcv_nxt) && SEQ_LT(seq, wnd_end)) ||
                      (SEQ_GEQ(seq + seglen - 1, pcb->rcv_nxt) && SEQ_LT(seq + seglen - 1, wnd_end)));
    if (!acceptable) {
        if (!(flags & TH_RST)) {
            pcb->delack_pending = true;
            build_segment(pcb, &b, TH_ACK, pcb->snd_nxt, 0, false);
            pcb->delack_pending = false;
        }
        goto out;
    }
    /* Trim data before rcv_nxt (retransmitted overlap). */
    if (SEQ_LT(seq, pcb->rcv_nxt)) {
        uint32_t skip = pcb->rcv_nxt - seq;
        if (skip > seglen)
            skip = seglen;
        m_adj(m, (int)skip);
        seq += skip;
        seglen -= skip;
        flags &= (uint8_t)~TH_SYN;
    }

    if (flags & TH_RST) {
        STAT(rsts_in);
        if (pcb->state == TCP_SYN_RCVD && pcb->listener) {
            pcb_free_locked(pcb);   /* back to nothing; the listener keeps listening */
            goto out;
        }
        pcb->error = pcb->state == TCP_SYN_RCVD ? -ECONNREFUSED : -ECONNRESET;
        wake = sock_ref(pcb);
        pcb_end_locked(pcb);
        goto out;
    }
    if (flags & TH_SYN) {
        /* SYN in the window: error and reset. */
        build_rst(&b, &dst, &src, &hdr, seglen);
        pcb->error = -ECONNRESET;
        wake = sock_ref(pcb);
        pcb_end_locked(pcb);
        goto out;
    }
    if (!(flags & TH_ACK))
        goto out;

    if (pcb->state == TCP_SYN_RCVD) {
        if (SEQ_LT(ack, pcb->snd_una) || SEQ_GT(ack, pcb->snd_nxt)) {
            build_rst(&b, &dst, &src, &hdr, seglen);
            goto out;
        }
        pcb->state = TCP_ESTABLISHED;
        pcb->snd_una = ack;
        pcb->snd_wnd = win;
        pcb->snd_wl1 = seq;
        pcb->snd_wl2 = ack;
        pcb->rexmit_count = 0;
        disarm_rexmit(pcb);
        STAT(conns_established);
        if (pcb->listener)
            wake_listener = sock_ref(pcb->listener);
        else
            wake = sock_ref(pcb);   /* simultaneous open */
        /* fall through to data processing */
    }

    /* ACK processing. */
    if (SEQ_GT(ack, pcb->snd_una) && SEQ_LEQ(ack, pcb->snd_max)) {
        uint32_t acked = ack - pcb->snd_una;
        uint32_t data_acked = acked;
        bool fin_acked = false;
        if (pcb->fin_sent && ack == pcb->snd_max) {
            fin_acked = true;
            data_acked--;
        }
        netbuf_drop(&pcb->sndbuf, data_acked);
        pcb->snd_una = ack;
        if (SEQ_LT(pcb->snd_nxt, pcb->snd_una))
            pcb->snd_nxt = pcb->snd_una;
        if (pcb->rtt_pending && SEQ_GEQ(ack, pcb->rtt_seq))
            rtt_sample(pcb, clock_now_ns());
        pcb->rexmit_count = 0;
        pcb->dupacks = 0;
        if (pcb->cwnd < pcb->ssthresh)
            pcb->cwnd += pcb->mss;                                /* slow start */
        else
            pcb->cwnd += (pcb->mss * pcb->mss) / (pcb->cwnd ? pcb->cwnd : 1);   /* congestion avoidance */
        if (pcb->cwnd > TCP_SNDBUF)
            pcb->cwnd = TCP_SNDBUF;
        wake = wake ? wake : sock_ref(pcb);   /* send space */
        if (fin_acked) {
            if (pcb->state == TCP_FIN_WAIT_1)
                pcb->state = TCP_FIN_WAIT_2;
            else if (pcb->state == TCP_CLOSING)
                enter_time_wait(pcb);
            else if (pcb->state == TCP_LAST_ACK) {
                pcb_end_locked(pcb);
                goto out;
            }
        }
    } else if (ack == pcb->snd_una && seglen == 0 && SEQ_GT(pcb->snd_max, pcb->snd_una) && win == pcb->snd_wnd) {
        if (++pcb->dupacks == 3) {
            /* Fast retransmit. */
            uint32_t inflight = pcb->snd_max - pcb->snd_una;
            pcb->ssthresh = inflight / 2 > 2u * pcb->mss ? inflight / 2 : 2u * pcb->mss;
            pcb->cwnd = pcb->ssthresh;
            pcb->retransmits++;
            STAT(retransmits);
            uint32_t seglen1 = pcb->sndbuf.len < pcb->mss ? pcb->sndbuf.len : pcb->mss;
            if (seglen1)
                build_segment(pcb, &b, TH_ACK, pcb->snd_una, seglen1, false);
        }
    }
    if (SEQ_LT(pcb->snd_wl1, seq) || (pcb->snd_wl1 == seq && SEQ_LEQ(pcb->snd_wl2, ack))) {
        pcb->snd_wnd = win;
        pcb->snd_wl1 = seq;
        pcb->snd_wl2 = ack;
    }

    /* Data. */
    if (seglen && (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_FIN_WAIT_1 || pcb->state == TCP_FIN_WAIT_2)) {
        if (seq == pcb->rcv_nxt) {
            uint32_t n = netbuf_put_mbuf(&pcb->rcvbuf, m, 0, seglen);
            pcb->rcv_nxt += n;
            pcb->rcv_wnd = netbuf_space(&pcb->rcvbuf);
            if (n < seglen)
                flags &= (uint8_t)~TH_FIN;   /* the FIN is beyond what we took */
            wake = wake ? wake : sock_ref(pcb);
            if (pcb->delack_pending) {
                tcp_output_locked(pcb, &b);   /* every second segment: ACK now */
            } else {
                pcb->delack_pending = true;
                timer_cancel(&pcb->delack);
                timer_start(&pcb->delack, TCP_DELACK_NS);
            }
        } else {
            STAT(out_of_order);
            pcb->delack_pending = true;
            build_segment(pcb, &b, TH_ACK, pcb->snd_nxt, 0, false);
            pcb->delack_pending = false;
            flags &= (uint8_t)~TH_FIN;
        }
    }

    /* FIN. */
    if ((flags & TH_FIN) && seq + seglen == pcb->rcv_nxt && !pcb->fin_rcvd) {
        pcb->fin_rcvd = true;
        pcb->rcv_nxt++;
        wake = wake ? wake : sock_ref(pcb);
        switch (pcb->state) {
        case TCP_ESTABLISHED: pcb->state = TCP_CLOSE_WAIT; break;
        case TCP_FIN_WAIT_1:
            if (pcb->fin_sent && pcb->snd_una == pcb->snd_max)
                enter_time_wait(pcb);
            else
                pcb->state = TCP_CLOSING;
            break;
        case TCP_FIN_WAIT_2: enter_time_wait(pcb); break;
        default: break;
        }
        pcb->delack_pending = true;
        build_segment(pcb, &b, TH_ACK, pcb->snd_nxt, 0, false);
        pcb->delack_pending = false;
        timer_cancel(&pcb->delack);
    } else if (pcb->state != TCP_CLOSED && pcb->state != TCP_TIME_WAIT) {
        tcp_output_locked(pcb, &b);   /* new window or ack may allow more data */
    }

out:
    spin_unlock_irqrestore(&g_lock, s);
    m_freem(m);
    batch_send(&b);
    sock_wake_after(wake);
    sock_wake_after(wake_listener);
}

bool tcp_accept_ready(struct tcp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    bool ready = false;
    struct tcp_pcb *c;
    list_for_each_entry(c, &pcb->accept_queue, accept_link) {
        if (c->state != TCP_SYN_RCVD) {
            ready = true;
            break;
        }
    }
    spin_unlock_irqrestore(&g_lock, s);
    return ready;
}

enum tcp_state tcp_state_of(struct tcp_pcb *pcb)
{
    return __atomic_load_n(&pcb->state, __ATOMIC_ACQUIRE);
}

void tcp_attach_socket(struct tcp_pcb *pcb, struct socket *sock)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    pcb->sock = sock;
    spin_unlock_irqrestore(&g_lock, s);
}

void tcp_init(void)
{
    g_next_ephemeral = (uint16_t)(NET_EPHEMERAL_LO + (random_u64() % (NET_EPHEMERAL_HI - NET_EPHEMERAL_LO)));
}

void tcp_get_stats(struct tcp_stats *out)
{
    *out = g_stats;
}

const char *tcp_state_name(enum tcp_state s)
{
    static const char *const names[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                         "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };
    return (unsigned)s < ARRAY_SIZE(names) ? names[s] : "?";
}
