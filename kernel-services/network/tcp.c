/*
 * tcp.c - TCP: RFC 793 state machine, RFC 6298 retransmission timer,
 * RFC 5681 slow start / congestion avoidance / fast retransmit, delayed
 * ACK, TIME_WAIT, RFC 5961 challenge ACKs, a SYN cache with SYN cookies,
 * keepalive, an orphaned FIN_WAIT_2 timeout, out-of-order reassembly and
 * path-MTU notifications.
 *
 * Locking (docs/kernel-services/network/design.md, "Hardening and
 * per-connection locking"): one spinlock per pcb covers that pcb; the
 * table of buckets keyed by local port has its own lock, taken inside a
 * pcb lock and never around one. A lookup takes the table lock, takes a
 * reference, drops the table lock and then locks the pcb, so the only
 * nesting of two pcb locks is listener -> child (subclass 1). Pcbs are
 * reference counted; the last put frees. Segments are built under the
 * pcb lock into a batch and transmitted after it is dropped; timers run
 * in interrupt context and only queue work for the network worker
 * thread, holding a reference across the hand-off.
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

#include <uapi/cosmo/syscall.h>

static spinlock_t g_table_lock = SPINLOCK_INIT("tcp-table");
static struct list_node g_hash[TCP_HASH_SIZE];
static struct tcp_stats g_stats;
static uint64_t g_cookie_secret;

/* Tunables the self-tests shorten. */
static uint64_t g_keep_idle_ns = TCP_KEEPIDLE_NS;
static uint64_t g_keep_intvl_ns = TCP_KEEPINTVL_NS;
static unsigned g_keep_cnt = TCP_KEEPCNT;
static uint64_t g_fin_wait2_ns = TCP_FIN_WAIT2_NS;

/* RFC 5961 challenge-ACK budget: a window of one second. */
static spinlock_t g_chal_lock = SPINLOCK_INIT("tcp-challenge");
static uint64_t g_chal_window_ns;
static unsigned g_chal_count;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)
#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

#define WORK_REXMIT   (1u << 0)
#define WORK_DELACK   (1u << 1)
#define WORK_TIMEWAIT (1u << 2)
#define WORK_KEEP     (1u << 3)

/* Segments built under a lock, sent after it. */
struct tcp_batch {
    struct {
        struct mbuf *m;
        struct netaddr src, dst;
    } seg[16];
    unsigned n;
};

static uint16_t family_mss(uint16_t family)
{
    return family == COSMO_AF_INET ? TCP_MSS_V4 : TCP_MSS_V6;
}

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

/* Append ring bytes [off, off + len) to an mbuf chain straight from the
 * ring (no intermediate buffer). */
static int netbuf_append_mbuf(const struct netbuf *nb, uint32_t off, uint32_t len, struct mbuf *m)
{
    for (uint32_t done = 0; done < len;) {
        uint32_t pos = (nb->head + off + done) % nb->size;
        uint32_t chunk = nb->size - pos;
        if (chunk > len - done)
            chunk = len - done;
        if (m_append(m, nb->data + pos, chunk))
            return -ENOMEM;
        done += chunk;
    }
    return 0;
}

static void netbuf_drop(struct netbuf *nb, uint32_t n)
{
    if (n > nb->len)
        n = nb->len;
    nb->head = (nb->head + n) % nb->size;
    nb->len -= n;
}

static void netbuf_clear(struct netbuf *nb)
{
    nb->head = nb->len = 0;
}

/* --- references and the table ------------------------------------------------- */

static void ooo_flush(struct tcp_pcb *pcb);

static void pcb_get(struct tcp_pcb *pcb)
{
    __atomic_fetch_add(&pcb->refs, 1, __ATOMIC_RELAXED);
}

/* Never called with pcb->lock held unless another reference is known to
 * remain (the table's or the caller's own). */
static void pcb_put(struct tcp_pcb *pcb)
{
    uint32_t old = __atomic_fetch_sub(&pcb->refs, 1, __ATOMIC_ACQ_REL);
    KASSERT(old > 0);
    if (old != 1)
        return;
    KASSERT(pcb->state == TCP_CLOSED && list_empty(&pcb->hash_link));
    ooo_flush(pcb);
    netbuf_free(&pcb->sndbuf);
    netbuf_free(&pcb->rcvbuf);
    kfree(pcb->syncache);
    kfree(pcb);
}

static struct list_node *bucket_of(uint16_t port)
{
    return &g_hash[(port * 40503u >> 4) & (TCP_HASH_SIZE - 1)];
}

static enum tcp_state state_of(const struct tcp_pcb *pcb)
{
    return __atomic_load_n(&pcb->state, __ATOMIC_ACQUIRE);
}

static void set_state(struct tcp_pcb *pcb, enum tcp_state st)
{
    __atomic_store_n(&pcb->state, st, __ATOMIC_RELEASE);
}

/* Table lock held. */
static bool port_in_use_locked(uint16_t family, uint16_t port, const struct netaddr *addr,
                               const struct tcp_pcb *self)
{
    struct tcp_pcb *p;
    list_for_each_entry(p, bucket_of(port), hash_link) {
        if (p == self || p->local.family != family || p->local.port != port)
            continue;
        if (state_of(p) == TCP_TIME_WAIT)
            continue;   /* reuse is allowed once the socket is gone */
        if (netaddr_is_unspecified(&p->local) || netaddr_is_unspecified(addr) || netaddr_addr_equal(&p->local, addr))
            return true;
    }
    return false;
}

/* Table lock held. A random start on every call (audit §9.2). */
static uint16_t pick_ephemeral_locked(uint16_t family, const struct netaddr *addr)
{
    unsigned span = NET_EPHEMERAL_HI - NET_EPHEMERAL_LO + 1;
    unsigned start = (unsigned)(random_u64() % span);
    for (unsigned n = 0; n < span; n++) {
        uint16_t port = (uint16_t)(NET_EPHEMERAL_LO + (start + n) % span);
        if (!port_in_use_locked(family, port, addr, NULL))
            return port;
    }
    return 0;
}

/* pcb lock held. Reserve `local` (port 0 picks one) and enter the table. */
static int table_bind(struct tcp_pcb *pcb, const struct netaddr *local)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_table_lock);
    uint16_t port = local->port;
    if (port == 0)
        port = pick_ephemeral_locked(local->family, local);
    if (port == 0 || port_in_use_locked(local->family, port, local, pcb)) {
        spin_unlock_irqrestore(&g_table_lock, s);
        return -EADDRINUSE;
    }
    pcb->local = *local;
    pcb->local.port = port;
    if (list_empty(&pcb->hash_link)) {
        list_push_back(bucket_of(port), &pcb->hash_link);
        pcb_get(pcb);
    }
    spin_unlock_irqrestore(&g_table_lock, s);
    return 0;
}

/* pcb lock held (or the pcb not yet visible). Enter the table on local.port. */
static void table_insert(struct tcp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_table_lock);
    if (list_empty(&pcb->hash_link)) {
        list_push_back(bucket_of(pcb->local.port), &pcb->hash_link);
        pcb_get(pcb);
    }
    spin_unlock_irqrestore(&g_table_lock, s);
}

/* pcb lock held; the caller holds a reference, so the table's put is never the last. */
static void table_remove(struct tcp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_table_lock);
    bool linked = !list_empty(&pcb->hash_link);
    if (linked) {
        list_remove(&pcb->hash_link);
        list_init(&pcb->hash_link);
    }
    spin_unlock_irqrestore(&g_table_lock, s);
    if (linked)
        pcb_put(pcb);
}

/* The connection for (local, remote), else a listener on local, else NULL.
 * Returns a referenced pcb; the caller locks it and puts it. */
static struct tcp_pcb *lookup(const struct netaddr *local, const struct netaddr *remote)
{
    struct tcp_pcb *p, *found = NULL, *listener = NULL;
    arch_irq_state_t s = spin_lock_irqsave(&g_table_lock);
    list_for_each_entry(p, bucket_of(local->port), hash_link) {
        if (p->local.family != local->family || p->local.port != local->port)
            continue;
        if (state_of(p) == TCP_LISTEN) {
            if (netaddr_is_unspecified(&p->local) || netaddr_addr_equal(&p->local, local))
                listener = p;
            continue;
        }
        if (p->remote.port == remote->port && netaddr_addr_equal(&p->remote, remote) &&
            netaddr_addr_equal(&p->local, local)) {
            found = p;
            break;
        }
    }
    if (found == NULL)
        found = listener;
    if (found)
        pcb_get(found);
    spin_unlock_irqrestore(&g_table_lock, s);
    return found;
}

/* --- pcbs -------------------------------------------------------------- */

static void pcb_work(void *arg);

/* Interrupt context. The pcb outlives the callback (the ending path
 * cancels the timers synchronously before dropping the state machine's
 * reference), and the work item carries a reference of its own. */
static void timer_kick(struct tcp_pcb *pcb, unsigned flag)
{
    __atomic_fetch_or(&pcb->work_flags, flag, __ATOMIC_RELAXED);
    pcb_get(pcb);
    if (!net_work_queue(&pcb->work))
        pcb_put(pcb);   /* already queued: that item's reference covers it */
}

static void rexmit_timer(struct timer *t, void *arg)   { (void)t; timer_kick(arg, WORK_REXMIT); }
static void delack_timer(struct timer *t, void *arg)   { (void)t; timer_kick(arg, WORK_DELACK); }
static void timewait_timer(struct timer *t, void *arg) { (void)t; timer_kick(arg, WORK_TIMEWAIT); }
static void keep_timer(struct timer *t, void *arg)     { (void)t; timer_kick(arg, WORK_KEEP); }

/* A CLOSED pcb with two references: the state machine's and the caller's. */
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
    spinlock_init(&pcb->lock, "tcp-pcb");
    pcb->refs = 2;
    pcb->state = TCP_CLOSED;
    pcb->local.family = pcb->remote.family = family;
    pcb->path_mss = family_mss(family);
    pcb->mss = pcb->path_mss;
    pcb->rcv_wnd = TCP_RCVBUF;
    pcb->rto_ns = TCP_RTO_INIT_NS;
    pcb->cwnd = 2 * pcb->mss;
    pcb->ssthresh = 0x7fffffff;
    timer_setup(&pcb->rexmit, rexmit_timer, pcb);
    timer_setup(&pcb->delack, delack_timer, pcb);
    timer_setup(&pcb->timewait, timewait_timer, pcb);
    timer_setup(&pcb->keep, keep_timer, pcb);
    list_init(&pcb->hash_link);
    list_init(&pcb->accept_link);
    list_init(&pcb->accept_queue);
    net_work_init(&pcb->work, pcb_work, pcb);
    return pcb;
}

/* pcb lock held. Back to a fresh CLOSED connection state (a retired pcb
 * that connects again). */
static void conn_reset_locked(struct tcp_pcb *pcb)
{
    netbuf_clear(&pcb->sndbuf);
    netbuf_clear(&pcb->rcvbuf);
    ooo_flush(pcb);
    pcb->rcv_wnd = TCP_RCVBUF;
    pcb->error = 0;
    pcb->fin_queued = pcb->fin_sent = pcb->fin_rcvd = pcb->delack_pending = false;
    pcb->rexmit_count = pcb->dupacks = pcb->keep_probes = 0;
    pcb->srtt_ns = pcb->rttvar_ns = 0;
    pcb->rtt_pending = false;
    pcb->rto_ns = TCP_RTO_INIT_NS;
    pcb->ssthresh = 0x7fffffff;
}

/*
 * pcb lock held. The connection is over and nothing else will use the
 * pcb: cancel the timers synchronously (their callbacks take only the
 * work lock and atomics on the pcb, never pcb->lock), leave the table,
 * become CLOSED. Returns true: the caller drops the state machine's
 * reference after unlocking (a queued work item still holds its own).
 */
static bool pcb_kill_locked(struct tcp_pcb *pcb)
{
    timer_cancel_sync(&pcb->rexmit);
    timer_cancel_sync(&pcb->delack);
    timer_cancel_sync(&pcb->timewait);
    timer_cancel_sync(&pcb->keep);
    table_remove(pcb);
    ooo_flush(pcb);
    set_state(pcb, TCP_CLOSED);
    pcb->sock = NULL;
    return true;
}

/*
 * pcb lock held. The connection is over but a socket still holds the pcb:
 * it becomes CLOSED and leaves the table, so it neither reserves its port
 * nor matches a segment; tcp_close ends it. A later connect reuses it.
 */
static void pcb_retire_locked(struct tcp_pcb *pcb)
{
    set_state(pcb, TCP_CLOSED);
    timer_cancel(&pcb->rexmit);
    timer_cancel(&pcb->delack);
    timer_cancel(&pcb->timewait);
    timer_cancel(&pcb->keep);
    table_remove(pcb);
    ooo_flush(pcb);
}

/* pcb lock held. The connection has ended: kill the pcb, or retire it under a live socket. */
static bool pcb_end_locked(struct tcp_pcb *pcb)
{
    if (pcb->sock == NULL)
        return pcb_kill_locked(pcb);
    pcb_retire_locked(pcb);
    return false;
}

/* --- out-of-order queue ---------------------------------------------------------- */

static void ooo_flush(struct tcp_pcb *pcb)
{
    for (unsigned i = 0; i < pcb->ooo_n; i++)
        m_freem(pcb->ooo[i].m);
    pcb->ooo_n = 0;
    pcb->ooo_bytes = 0;
}

static void ooo_remove_at(struct tcp_pcb *pcb, unsigned i)
{
    pcb->ooo_bytes -= pcb->ooo[i].len;
    m_freem(pcb->ooo[i].m);
    for (unsigned k = i + 1; k < pcb->ooo_n; k++)
        pcb->ooo[k - 1] = pcb->ooo[k];
    pcb->ooo_n--;
}

/* pcb lock held. Takes `m` (data only). A segment overlapping a queued one
 * is dropped unless it covers it entirely, in which case it replaces it. */
static void ooo_insert(struct tcp_pcb *pcb, struct mbuf *m, uint32_t seq, uint32_t len, bool fin)
{
    uint32_t end = seq + len;
    for (unsigned i = 0; i < pcb->ooo_n;) {
        struct tcp_ooo_seg *e = &pcb->ooo[i];
        uint32_t eend = e->seq + e->len;
        if (SEQ_LEQ(eend, seq) || SEQ_GEQ(e->seq, end)) {
            i++;
            continue;   /* disjoint */
        }
        if (SEQ_LEQ(seq, e->seq) && SEQ_GEQ(end, eend)) {
            fin = fin || e->fin;
            ooo_remove_at(pcb, i);   /* the new one covers it */
            continue;
        }
        STAT(ooo_dropped);   /* partial overlap, or already covered */
        m_freem(m);
        return;
    }
    if (pcb->ooo_n == TCP_OOO_MAX || pcb->ooo_bytes + len > netbuf_space(&pcb->rcvbuf)) {
        STAT(ooo_dropped);
        m_freem(m);
        return;
    }
    unsigned i = 0;
    while (i < pcb->ooo_n && SEQ_LT(pcb->ooo[i].seq, seq))
        i++;
    for (unsigned k = pcb->ooo_n; k > i; k--)
        pcb->ooo[k] = pcb->ooo[k - 1];
    pcb->ooo[i].seq = seq;
    pcb->ooo[i].len = len;
    pcb->ooo[i].m = m;
    pcb->ooo[i].fin = fin;
    pcb->ooo_n++;
    pcb->ooo_bytes += len;
    STAT(ooo_queued);
}

/* pcb lock held. Deliver everything now contiguous with rcv_nxt. */
static void ooo_drain(struct tcp_pcb *pcb, bool *fin_out)
{
    while (pcb->ooo_n && SEQ_LEQ(pcb->ooo[0].seq, pcb->rcv_nxt)) {
        struct tcp_ooo_seg *e = &pcb->ooo[0];
        uint32_t eend = e->seq + e->len;
        if (SEQ_LEQ(eend, pcb->rcv_nxt)) {
            if (e->fin && eend == pcb->rcv_nxt)
                *fin_out = true;
            ooo_remove_at(pcb, 0);
            continue;
        }
        uint32_t skip = pcb->rcv_nxt - e->seq;
        uint32_t want = e->len - skip;
        uint32_t n = netbuf_put_mbuf(&pcb->rcvbuf, e->m, skip, want);
        pcb->rcv_nxt += n;
        if (n < want) {
            /* The ring is full: keep the rest for later. */
            m_adj(e->m, (int)(skip + n));
            pcb->ooo_bytes -= skip + n;
            e->seq += skip + n;
            e->len -= skip + n;
            break;
        }
        if (e->fin)
            *fin_out = true;
        ooo_remove_at(pcb, 0);
    }
}

/* --- segment construction ---------------------------------------------------- */

/* A path MTU cache lookup and a netif registry read: never under a pcb lock (N5). */
uint16_t tcp_path_mss(uint16_t family, const struct netaddr *remote)
{
    if (family == COSMO_AF_INET) {
        bool lo = (ntohl(remote->v4) >> 24) == 127 || netif_owns_ipv4(remote->v4);
        uint32_t mtu = ipv4_path_mtu(remote->v4);
        uint32_t mss = mtu > 40 + 256 ? mtu - 40 : 256;
        uint32_t cap = lo ? TCP_MSS_LO : TCP_MSS_V4;
        return (uint16_t)(mss < cap ? mss : cap);
    }
    bool lo = in6_is_loopback(&remote->v6) || netif_owns_ipv6(&remote->v6);
    return lo ? TCP_MSS_LO : TCP_MSS_V6;
}

static int batch_push(struct tcp_batch *b, struct mbuf *m, const struct netaddr *src, const struct netaddr *dst)
{
    if (b->n == ARRAY_SIZE(b->seg)) {
        m_freem(m);
        return -ENOSPC;
    }
    b->seg[b->n].m = m;
    b->seg[b->n].src = *src;
    b->seg[b->n].dst = *dst;
    b->n++;
    STAT(segs_out);
    return 0;
}

static void seg_checksum(struct mbuf *m, struct tcp_hdr *th, const struct netaddr *src, const struct netaddr *dst)
{
    uint32_t sum = src->family == COSMO_AF_INET ? cksum_pseudo4(src->v4, dst->v4, IPPROTO_TCP, (uint16_t)m->pkt.len)
                                                 : cksum_pseudo6(&src->v6, &dst->v6, IPPROTO_TCP, m->pkt.len);
    th->cksum = 0;
    th->cksum = cksum_fold(m_cksum_partial(m, 0, m->pkt.len, sum));
}

/* A segment without a pcb: SYN-ACKs from a listener, resets, challenge ACKs. */
static int build_raw(struct tcp_batch *b, const struct netaddr *src, const struct netaddr *dst, uint8_t flags,
                     uint32_t seq, uint32_t ack, uint32_t wnd, uint16_t mss_option)
{
    if (b->n == ARRAY_SIZE(b->seg))
        return -ENOSPC;
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return -ENOMEM;
    unsigned hlen = sizeof(struct tcp_hdr) + (mss_option ? 4 : 0);
    struct tcp_hdr *th = (struct tcp_hdr *)m->data;
    memset(th, 0, hlen);
    th->sport = htons(src->port);
    th->dport = htons(dst->port);
    th->seq = htonl(seq);
    th->ack = htonl(ack);
    th->doff = (uint8_t)((hlen / 4) << 4);
    th->flags = flags;
    th->win = htons((uint16_t)(wnd > TCP_MAX_WINDOW ? TCP_MAX_WINDOW : wnd));
    if (mss_option) {
        uint8_t *opt = m->data + sizeof(*th);
        opt[0] = 2;
        opt[1] = 4;
        opt[2] = (uint8_t)(mss_option >> 8);
        opt[3] = (uint8_t)mss_option;
    }
    m->len = m->pkt.len = hlen;
    seg_checksum(m, th, src, dst);
    return batch_push(b, m, src, dst);
}

/* pcb lock held. Build one segment into the batch; data comes from sndbuf at
 * (seq - snd_una), copied straight from the ring. */
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
    uint32_t wnd = pcb->rcv_wnd > TCP_MAX_WINDOW ? TCP_MAX_WINDOW : pcb->rcv_wnd;
    th->win = htons((uint16_t)wnd);
    if (mss_option) {
        uint8_t *opt = m->data + sizeof(*th);
        uint16_t mss = pcb->path_mss;
        opt[0] = 2;
        opt[1] = 4;
        opt[2] = (uint8_t)(mss >> 8);
        opt[3] = (uint8_t)mss;
    }
    m->len = m->pkt.len = hlen;
    if (datalen && netbuf_append_mbuf(&pcb->sndbuf, seq - pcb->snd_una, datalen, m)) {
        m_freem(m);
        return -ENOMEM;
    }
    seg_checksum(m, th, &pcb->local, &pcb->remote);
    pcb->segs_out++;
    return batch_push(b, m, &pcb->local, &pcb->remote);
}

/* Reset in reply to a segment for which there is no connection. */
static void build_rst(struct tcp_batch *b, const struct netaddr *src, const struct netaddr *dst, const struct tcp_hdr *th,
                      uint32_t seglen)
{
    if (th->flags & TH_RST)
        return;
    int rc;
    if (th->flags & TH_ACK)
        rc = build_raw(b, src, dst, TH_RST, ntohl(th->ack), 0, 0, 0);
    else
        rc = build_raw(b, src, dst, TH_RST | TH_ACK, 0,
                       ntohl(th->seq) + seglen + ((th->flags & TH_SYN) ? 1 : 0) + ((th->flags & TH_FIN) ? 1 : 0), 0, 0);
    if (rc == 0)
        STAT(rsts_out);
}

/* RFC 5961: at most TCP_CHALLENGE_PER_SEC challenge ACKs a second, host-wide. */
static bool challenge_allowed(void)
{
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_chal_lock);
    if (now - g_chal_window_ns >= 1000000000ull) {
        g_chal_window_ns = now;
        g_chal_count = 0;
    }
    bool ok = g_chal_count < TCP_CHALLENGE_PER_SEC;
    if (ok)
        g_chal_count++;
    spin_unlock_irqrestore(&g_chal_lock, s);
    return ok;
}

/* pcb lock held. */
static void challenge_ack(struct tcp_pcb *pcb, struct tcp_batch *b)
{
    STAT(challenge_acks);
    if (challenge_allowed())
        build_segment(pcb, b, TH_ACK, pcb->snd_nxt, 0, false);
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

static void arm_keep(struct tcp_pcb *pcb, uint64_t ns)
{
    timer_cancel(&pcb->keep);
    timer_start(&pcb->keep, ns);
}

/* pcb lock held. The connection is established: start the idle clock. */
static void established_locked(struct tcp_pcb *pcb, uint64_t now)
{
    set_state(pcb, TCP_ESTABLISHED);
    pcb->rexmit_count = 0;
    pcb->keep_probes = 0;
    pcb->last_rx_ns = now;
    STAT(conns_established);
    arm_keep(pcb, g_keep_idle_ns);
}

/* pcb lock held. Send what the windows allow, a queued FIN, or a pending ACK. */
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

/* pcb lock held: take a reference to wake after unlocking. The socket's
 * release detaches pcb->sock under the pcb lock, but its count is already
 * zero when it starts, so a plain get could race it: tryget. */
static struct socket *sock_ref(struct tcp_pcb *pcb)
{
    if (pcb->sock && kobject_tryget(&pcb->sock->obj))
        return pcb->sock;
    return NULL;
}

/* pcb lock held. The pcb is out of the socket's hands and in FIN_WAIT_2. */
static void orphan_fin_wait2(struct tcp_pcb *pcb)
{
    if (pcb->sock == NULL && pcb->state == TCP_FIN_WAIT_2)
        arm_keep(pcb, g_fin_wait2_ns);
}

/* --- worker-side timer handling ----------------------------------------------------- */

/* pcb lock held. The keep timer: an orphaned FIN_WAIT_2 ends, an idle
 * connection is probed, an unanswered one times out. */
static void keep_fire(struct tcp_pcb *pcb, struct tcp_batch *b, struct socket **wake, bool *killed)
{
    if (pcb->state == TCP_FIN_WAIT_2) {
        if (pcb->sock == NULL) {
            STAT(fin_wait2_timeouts);
            *killed = pcb_end_locked(pcb);
        }
        return;
    }
    if (pcb->state != TCP_ESTABLISHED && pcb->state != TCP_CLOSE_WAIT)
        return;
    uint64_t now = clock_now_ns();
    uint64_t idle = now - pcb->last_rx_ns;
    if (idle < g_keep_idle_ns) {
        arm_keep(pcb, g_keep_idle_ns - idle);
        return;
    }
    if (pcb->keep_probes >= g_keep_cnt) {
        STAT(timeouts);
        pcb->error = -ETIMEDOUT;
        *wake = sock_ref(pcb);
        *killed = pcb_end_locked(pcb);
        return;
    }
    pcb->keep_probes++;
    STAT(keepalive_probes);
    build_segment(pcb, b, TH_ACK, pcb->snd_nxt - 1, 0, false);   /* RFC 1122 4.2.3.6 */
    arm_keep(pcb, g_keep_intvl_ns);
}

static void pcb_work(void *arg)
{
    struct tcp_pcb *pcb = arg;
    struct tcp_batch b = { .n = 0 };
    struct socket *wake = NULL;
    bool killed = false;

    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    unsigned flags = __atomic_exchange_n(&pcb->work_flags, 0, __ATOMIC_ACQ_REL);
    if (pcb->state == TCP_CLOSED)
        goto out;   /* retired or ended: nothing to run */
    if ((flags & WORK_TIMEWAIT) && pcb->state == TCP_TIME_WAIT) {
        /* Under a socket that was shut down but not closed the pcb is
         * retired, not freed; it lives until close. */
        wake = sock_ref(pcb);
        killed = pcb_end_locked(pcb);
        goto out;
    }
    if (flags & WORK_KEEP) {
        keep_fire(pcb, &b, &wake, &killed);
        if (killed || pcb->state == TCP_CLOSED)
            goto out;
    }
    if (flags & WORK_DELACK)
        tcp_output_locked(pcb, &b);
    if ((flags & WORK_REXMIT) && pcb->state != TCP_LISTEN && pcb->state != TCP_TIME_WAIT) {
        if (SEQ_GT(pcb->snd_max, pcb->snd_una) || pcb->state == TCP_SYN_SENT || pcb->state == TCP_SYN_RCVD ||
            (pcb->sndbuf.len && pcb->snd_wnd == 0)) {
            pcb->rexmit_count++;
            pcb->retransmits++;
            STAT(retransmits);
            if (pcb->rexmit_count > TCP_MAX_REXMIT) {
                STAT(timeouts);
                pcb->error = -ETIMEDOUT;
                wake = sock_ref(pcb);
                killed = pcb_end_locked(pcb);
                goto out;
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
out:
    spin_unlock_irqrestore(&pcb->lock, s);
    batch_send(&b);
    sock_wake_after(wake);
    if (killed)
        pcb_put(pcb);   /* the state machine's */
    pcb_put(pcb);       /* the work item's */
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
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    if (pcb->local.port != 0 || pcb->state != TCP_CLOSED) {
        spin_unlock_irqrestore(&pcb->lock, s);
        return -EINVAL;
    }
    int rc = table_bind(pcb, local);
    spin_unlock_irqrestore(&pcb->lock, s);
    return rc;
}

int tcp_listen(struct tcp_pcb *pcb, unsigned backlog)
{
    struct tcp_syncache *sc = kzalloc(sizeof(*sc));
    if (sc == NULL)
        return -ENOMEM;
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    if (pcb->local.port == 0 || pcb->state != TCP_CLOSED || pcb->syncache) {
        spin_unlock_irqrestore(&pcb->lock, s);
        kfree(sc);
        return -EINVAL;
    }
    pcb->syncache = sc;
    pcb->backlog = backlog == 0 ? 1 : (backlog > TCP_MAX_BACKLOG ? TCP_MAX_BACKLOG : backlog);
    set_state(pcb, TCP_LISTEN);
    spin_unlock_irqrestore(&pcb->lock, s);
    return 0;
}

/*
 * Dequeue an established child and attach `owner` to it under both locks
 * (listener, then child): between the dequeue and the attach the child
 * would otherwise have neither a listener nor a socket, and a reset
 * arriving then would end it under the accepting thread (N-L2). The
 * queue's reference becomes the socket's. Children that ended while
 * queued are discarded here.
 */
struct tcp_pcb *tcp_accept(struct tcp_pcb *pcb, struct socket *owner)
{
    struct tcp_pcb *dead[TCP_MAX_BACKLOG];
    unsigned ndead = 0;
    struct tcp_pcb *child = NULL, *c, *tmp;
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    list_for_each_entry_safe(c, tmp, &pcb->accept_queue, accept_link) {
        if (state_of(c) == TCP_CLOSED) {
            list_remove(&c->accept_link);
            list_init(&c->accept_link);
            pcb->nr_queued--;
            KASSERT(ndead < ARRAY_SIZE(dead));   /* nr_queued never exceeds the backlog */
            dead[ndead++] = c;
            continue;
        }
        child = c;
        break;
    }
    if (child) {
        list_remove(&child->accept_link);
        list_init(&child->accept_link);
        pcb->nr_queued--;
        arch_irq_state_t s2 = spin_lock_irqsave_nested(&child->lock, 1);
        child->listener = NULL;
        child->sock = owner;
        spin_unlock_irqrestore(&child->lock, s2);
    }
    spin_unlock_irqrestore(&pcb->lock, s);
    for (unsigned i = 0; i < ndead; i++)
        pcb_put(dead[i]);
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
    /* Decided here, before the lock: it reads the netif registry. */
    uint16_t path_mss = tcp_path_mss(local.family, remote);
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    if (pcb->state != TCP_CLOSED) {
        int rc = pcb->state == TCP_SYN_SENT ? -EALREADY : -EISCONN;
        spin_unlock_irqrestore(&pcb->lock, s);
        return rc;
    }
    if (pcb->syncache) {
        spin_unlock_irqrestore(&pcb->lock, s);
        return -EINVAL;   /* a listener */
    }
    int rc = table_bind(pcb, &local);   /* a bound port is kept, port 0 picks one */
    if (rc) {
        spin_unlock_irqrestore(&pcb->lock, s);
        return rc;
    }
    conn_reset_locked(pcb);
    pcb->remote = *remote;
    pcb->iss = (uint32_t)random_u64();
    pcb->snd_una = pcb->iss;
    pcb->snd_nxt = pcb->snd_max = pcb->iss + 1;
    pcb->path_mss = path_mss;
    pcb->mss = path_mss;
    pcb->snd_wnd = pcb->mss;
    pcb->cwnd = 2 * pcb->mss;
    set_state(pcb, TCP_SYN_SENT);
    STAT(conns_active);
    build_segment(pcb, &b, TH_SYN, pcb->iss, 0, true);
    arm_rexmit(pcb);
    spin_unlock_irqrestore(&pcb->lock, s);
    batch_send(&b);
    return 0;
}

int64_t tcp_send(struct tcp_pcb *pcb, const void *data, size_t len)
{
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    if (pcb->error) {
        int e = pcb->error;
        spin_unlock_irqrestore(&pcb->lock, s);
        return e;
    }
    if (pcb->state != TCP_ESTABLISHED && pcb->state != TCP_CLOSE_WAIT) {
        int e = pcb->state == TCP_SYN_SENT || pcb->state == TCP_SYN_RCVD ? -EAGAIN : -EPIPE;
        spin_unlock_irqrestore(&pcb->lock, s);
        return e;
    }
    if (pcb->fin_queued) {
        spin_unlock_irqrestore(&pcb->lock, s);
        return -EPIPE;
    }
    uint32_t n = netbuf_put(&pcb->sndbuf, data, (uint32_t)(len > 0xffffffffu ? 0xffffffffu : len));
    if (n)
        tcp_output_locked(pcb, &b);
    spin_unlock_irqrestore(&pcb->lock, s);
    batch_send(&b);
    return (int64_t)n;
}

int64_t tcp_recv(struct tcp_pcb *pcb, void *data, size_t len, bool *peer_closed)
{
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
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
    spin_unlock_irqrestore(&pcb->lock, s);
    batch_send(&b);
    if (n == 0 && err && !*peer_closed)
        return err;
    return (int64_t)n;
}

int tcp_shutdown_write(struct tcp_pcb *pcb)
{
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    switch (pcb->state) {
    case TCP_ESTABLISHED:
    case TCP_SYN_RCVD:
        set_state(pcb, TCP_FIN_WAIT_1);
        break;
    case TCP_CLOSE_WAIT:
        set_state(pcb, TCP_LAST_ACK);
        break;
    default: {
        int rc = pcb->fin_queued ? 0 : -ENOTCONN;
        spin_unlock_irqrestore(&pcb->lock, s);
        return rc;
    }
    }
    pcb->fin_queued = true;
    tcp_output_locked(pcb, &b);
    spin_unlock_irqrestore(&pcb->lock, s);
    batch_send(&b);
    return 0;
}

uint32_t tcp_send_space(struct tcp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    uint32_t n = netbuf_space(&pcb->sndbuf);
    spin_unlock_irqrestore(&pcb->lock, s);
    return n;
}

uint32_t tcp_recv_avail(struct tcp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    uint32_t n = pcb->rcvbuf.len;
    spin_unlock_irqrestore(&pcb->lock, s);
    return n;
}

/* pcb lock held. */
static bool accept_ready_locked(struct tcp_pcb *pcb)
{
    struct tcp_pcb *c;
    list_for_each_entry(c, &pcb->accept_queue, accept_link) {
        if (state_of(c) != TCP_CLOSED)
            return true;
    }
    return false;
}

bool tcp_accept_ready(struct tcp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    bool ready = pcb->state == TCP_LISTEN && accept_ready_locked(pcb);
    spin_unlock_irqrestore(&pcb->lock, s);
    return ready;
}

unsigned tcp_ready(struct tcp_pcb *pcb)
{
    unsigned r = 0;
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    switch (pcb->state) {
    case TCP_LISTEN:
        if (accept_ready_locked(pcb))
            r |= COSMO_IO_READABLE;
        break;
    case TCP_SYN_SENT:
    case TCP_SYN_RCVD:
        break;   /* connecting: nothing would complete */
    case TCP_CLOSED:
        r |= COSMO_IO_READABLE | COSMO_IO_WRITABLE | COSMO_IO_HANGUP;
        if (pcb->error)
            r |= COSMO_IO_ERROR;
        break;
    default:
        if (pcb->rcvbuf.len)
            r |= COSMO_IO_READABLE;
        if (pcb->fin_rcvd)
            r |= COSMO_IO_READABLE | COSMO_IO_HANGUP;
        if (pcb->error)
            r |= COSMO_IO_READABLE | COSMO_IO_WRITABLE | COSMO_IO_ERROR;
        if (pcb->fin_queued)
            r |= COSMO_IO_WRITABLE;   /* a write fails at once */
        else if ((pcb->state == TCP_ESTABLISHED || pcb->state == TCP_CLOSE_WAIT) && netbuf_space(&pcb->sndbuf))
            r |= COSMO_IO_WRITABLE;
        break;
    }
    spin_unlock_irqrestore(&pcb->lock, s);
    return r;
}

void tcp_close(struct tcp_pcb *pcb)
{
    struct tcp_batch b = { .n = 0 };
    struct tcp_pcb *kids[TCP_MAX_BACKLOG];
    bool kid_killed[TCP_MAX_BACKLOG];
    unsigned nkids = 0;
    bool killed = false;

    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    pcb->sock = NULL;
    switch (pcb->state) {
    case TCP_CLOSED:
    case TCP_SYN_SENT:
        killed = pcb_kill_locked(pcb);
        break;
    case TCP_LISTEN: {
        struct tcp_pcb *c, *tmp;
        list_for_each_entry_safe(c, tmp, &pcb->accept_queue, accept_link) {
            list_remove(&c->accept_link);
            list_init(&c->accept_link);
            arch_irq_state_t s2 = spin_lock_irqsave_nested(&c->lock, 1);
            c->listener = NULL;
            bool ck = false;
            if (c->state != TCP_CLOSED) {
                /* A reset carrying the child's own sequence number: build_rst
                 * takes the peer's view, whose ack is our snd_nxt. */
                struct tcp_hdr peer = { .flags = TH_ACK, .sport = htons(c->remote.port), .dport = htons(c->local.port),
                                        .ack = htonl(c->snd_nxt) };
                build_rst(&b, &c->local, &c->remote, &peer, 0);
                ck = pcb_kill_locked(c);
            }
            spin_unlock_irqrestore(&c->lock, s2);
            KASSERT(nkids < ARRAY_SIZE(kids));
            kids[nkids] = c;
            kid_killed[nkids++] = ck;
        }
        pcb->nr_queued = 0;
        killed = pcb_kill_locked(pcb);
        break;
    }
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        if (pcb->rcvbuf.len > 0 || pcb->ooo_n > 0) {
            /* Unread data: the peer learns through a reset (RFC 2525 2.17). */
            build_segment(pcb, &b, TH_RST | TH_ACK, pcb->snd_nxt, 0, false);
            STAT(rsts_out);
            killed = pcb_kill_locked(pcb);
            break;
        }
        set_state(pcb, pcb->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT_1);
        pcb->fin_queued = true;
        tcp_output_locked(pcb, &b);
        break;
    case TCP_FIN_WAIT_2:
        orphan_fin_wait2(pcb);
        break;
    default:
        /* Already closing; it ends on its own (FIN_WAIT_2 arms its timeout when reached). */
        break;
    }
    spin_unlock_irqrestore(&pcb->lock, s);
    batch_send(&b);
    for (unsigned i = 0; i < nkids; i++) {
        if (kid_killed[i])
            pcb_put(kids[i]);   /* the child's state machine */
        pcb_put(kids[i]);       /* the queue's */
    }
    if (killed)
        pcb_put(pcb);   /* the state machine's */
    pcb_put(pcb);       /* the caller's (the socket's) */
}

/* --- SYN cache and cookies ------------------------------------------------------- */

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

static uint64_t addr_hash(const struct netaddr *a, uint64_t h)
{
    h = mix64(h ^ ((uint64_t)a->family << 16 | a->port));
    if (a->family == COSMO_AF_INET)
        return mix64(h ^ a->v4);
    uint64_t lo, hi;
    memcpy(&lo, a->v6.s6_addr, 8);
    memcpy(&hi, a->v6.s6_addr + 8, 8);
    return mix64(mix64(h ^ lo) ^ hi);
}

static uint64_t tuple_hash(const struct netaddr *local, const struct netaddr *remote, uint64_t salt)
{
    return addr_hash(remote, addr_hash(local, salt));
}

static bool syn_entry_live(const struct tcp_syn_entry *e, uint64_t now)
{
    return e->ts_ns != 0 && now - e->ts_ns < TCP_SYNCACHE_TTL_NS;
}

/* Listener lock held. */
static struct tcp_syn_entry *syncache_find(struct tcp_pcb *l, const struct netaddr *local,
                                           const struct netaddr *remote, uint64_t now)
{
    unsigned h = (unsigned)tuple_hash(local, remote, 0x5ca1ab1eull) % TCP_SYNCACHE_SIZE;
    for (unsigned i = 0; i < 8; i++) {
        struct tcp_syn_entry *e = &l->syncache->e[(h + i) % TCP_SYNCACHE_SIZE];
        if (syn_entry_live(e, now) && netaddr_equal(&e->remote, remote) && netaddr_equal(&e->local, local))
            return e;
    }
    return NULL;
}

/* Listener lock held. A free or expired slot in the probe range, or NULL. */
static struct tcp_syn_entry *syncache_alloc(struct tcp_pcb *l, const struct netaddr *local,
                                            const struct netaddr *remote, uint64_t now)
{
    unsigned h = (unsigned)tuple_hash(local, remote, 0x5ca1ab1eull) % TCP_SYNCACHE_SIZE;
    for (unsigned i = 0; i < 8; i++) {
        struct tcp_syn_entry *e = &l->syncache->e[(h + i) % TCP_SYNCACHE_SIZE];
        if (!syn_entry_live(e, now))
            return e;
    }
    return NULL;
}

static const uint16_t g_cookie_mss[8] = { 536, 1220, 1440, 1460, 2048, 4096, 8960, 16384 };
#define COOKIE_SLOT_SHIFT 33   /* ~8.6 s */

static uint32_t cookie_hash(const struct netaddr *local, const struct netaddr *remote, uint64_t slot)
{
    return (uint32_t)mix64(tuple_hash(local, remote, g_cookie_secret) ^ slot);
}

static uint32_t cookie_make(const struct netaddr *local, const struct netaddr *remote, uint64_t now, uint16_t peer_mss)
{
    unsigned idx = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(g_cookie_mss); i++)
        if (g_cookie_mss[i] <= peer_mss)
            idx = i;
    return (cookie_hash(local, remote, now >> COOKIE_SLOT_SHIFT) & ~7u) | idx;
}

static bool cookie_check(const struct netaddr *local, const struct netaddr *remote, uint64_t now, uint32_t iss,
                         uint16_t *peer_mss)
{
    uint64_t slot = now >> COOKIE_SLOT_SHIFT;
    for (uint64_t d = 0; d < 2; d++) {
        if ((cookie_hash(local, remote, slot - d) & ~7u) == (iss & ~7u)) {
            *peer_mss = g_cookie_mss[iss & 7];
            return true;
        }
    }
    return false;
}

/* --- input -------------------------------------------------------------------- */

static uint16_t parse_mss(const uint8_t *opts, unsigned optlen, uint16_t dflt)
{
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
    set_state(pcb, TCP_TIME_WAIT);
    disarm_rexmit(pcb);
    timer_cancel(&pcb->delack);
    timer_cancel(&pcb->keep);
    timer_cancel(&pcb->timewait);
    timer_start(&pcb->timewait, TCP_TIMEWAIT_NS);
}

/* Everything tcp_input parsed from one segment. */
struct seg {
    struct netaddr src, dst;   /* remote, local */
    struct tcp_hdr hdr;
    uint8_t opts[40];
    unsigned optlen;
    uint32_t seq, ack, len;
    uint16_t win;
    uint8_t flags;
    bool via_lo;
};

/*
 * Listener lock held. A SYN is answered from the SYN cache or with a
 * cookie and creates nothing; the ACK that completes a handshake creates
 * the child, ESTABLISHED, queued for accept and in the table, and returns
 * it referenced for the caller to process the segment on (it may carry
 * data). Anything else returns NULL.
 */
static struct tcp_pcb *listen_input(struct tcp_pcb *l, struct seg *g, struct tcp_batch *b,
                                    struct socket **wake_listener)
{
    uint64_t now = clock_now_ns();
    const struct netaddr *local = &g->dst, *remote = &g->src;
    uint8_t flags = g->flags;

    if (flags & TH_RST) {
        /* A reset for a half-open connection returns it to nothing. */
        struct tcp_syn_entry *e = syncache_find(l, local, remote, now);
        if (e && g->seq == e->irs + 1)
            e->ts_ns = 0;
        return NULL;
    }
    if (flags & TH_ACK) {
        if (flags & TH_SYN) {
            build_rst(b, local, remote, &g->hdr, g->len);
            return NULL;
        }
        uint32_t iss, irs;
        uint16_t peer_mss, path_mss;
        struct tcp_syn_entry *e = syncache_find(l, local, remote, now);
        if (e) {
            if (g->ack != e->iss + 1 || SEQ_LT(g->seq, e->irs + 1)) {
                STAT(syn_bad_ack);
                build_rst(b, local, remote, &g->hdr, g->len);
                return NULL;
            }
            iss = e->iss;
            irs = e->irs;
            peer_mss = e->peer_mss;
            path_mss = e->path_mss;
        } else if (cookie_check(local, remote, now, g->ack - 1, &peer_mss)) {
            /* The first segment after the handshake starts at irs + 1 (a
             * client retransmits from snd_una, which nothing has moved). */
            iss = g->ack - 1;
            irs = g->seq - 1;
            path_mss = g->via_lo ? TCP_MSS_LO : family_mss(local->family);
            STAT(syn_cookies_ok);
        } else {
            STAT(syn_bad_ack);
            build_rst(b, local, remote, &g->hdr, g->len);
            return NULL;
        }
        if (l->nr_queued >= l->backlog)
            return NULL;   /* accept queue full: the client retransmits */
        struct tcp_pcb *c = tcp_pcb_new(local->family);
        if (c == NULL)
            return NULL;
        c->local = *local;
        c->remote = *remote;
        c->irs = irs;
        c->rcv_nxt = irs + 1;
        c->iss = iss;
        c->snd_una = iss + 1;
        c->snd_nxt = c->snd_max = iss + 1;
        c->snd_wnd = g->win;
        c->snd_wl1 = g->seq;
        c->snd_wl2 = g->ack;
        c->path_mss = path_mss;
        c->mss = peer_mss < path_mss ? peer_mss : path_mss;
        c->cwnd = 2 * c->mss;
        c->listener = l;
        established_locked(c, now);
        STAT(conns_passive);
        if (e)
            e->ts_ns = 0;
        list_push_back(&l->accept_queue, &c->accept_link);
        l->nr_queued++;
        pcb_get(c);          /* the queue's reference */
        table_insert(c);     /* visible from here; the caller locks it before touching it */
        *wake_listener = sock_ref(l);
        return c;            /* the creator's reference goes to the caller */
    }
    if (!(flags & TH_SYN))
        return NULL;

    /* A SYN: remember it, or answer statelessly. */
    uint16_t path_mss = g->via_lo ? TCP_MSS_LO : family_mss(local->family);
    uint16_t peer_mss = parse_mss(g->opts, g->optlen, family_mss(local->family));
    uint32_t iss;
    struct tcp_syn_entry *e = syncache_find(l, local, remote, now);
    if (e) {
        iss = e->iss;   /* the client's SYN retransmit: answer again */
    } else if ((e = syncache_alloc(l, local, remote, now)) != NULL) {
        e->remote = *remote;
        e->local = *local;
        e->iss = (uint32_t)random_u64();
        e->irs = g->seq;
        e->peer_mss = peer_mss;
        e->path_mss = path_mss;
        e->ts_ns = now;
        iss = e->iss;
        STAT(syn_cached);
    } else {
        iss = cookie_make(local, remote, now, peer_mss);
        STAT(syn_cookies_sent);
    }
    build_raw(b, local, remote, TH_SYN | TH_ACK, iss, g->seq + 1, TCP_RCVBUF, path_mss);
    return NULL;
}

void tcp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6)
{
    struct seg g;
    memset(&g, 0, sizeof(g));
    /* A segment that arrived through `lo` came from this host (ipv4/6_route
     * deliver every packet to one of our own addresses that way), so a
     * connection it opens is a local one: the MSS cap is decided from the
     * interface here, never by a registry lookup under a lock. */
    g.via_lo = (nif->flags & NETIF_LOOPBACK) != 0;
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

    uint32_t sum;
    if (ip4) {
        g.src.family = g.dst.family = COSMO_AF_INET;
        g.src.v4 = ip4->src;
        g.dst.v4 = ip4->dst;
        sum = cksum_pseudo4(ip4->src, ip4->dst, IPPROTO_TCP, (uint16_t)len);
    } else {
        g.src.family = g.dst.family = COSMO_AF_INET6;
        g.src.v6 = ip6->src;
        g.dst.v6 = ip6->dst;
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
    g.src.port = ntohs(th->sport);
    g.dst.port = ntohs(th->dport);
    memcpy(&g.hdr, th, sizeof(g.hdr));
    g.optlen = hlen - sizeof(g.hdr);
    memcpy(g.opts, m->data + sizeof(g.hdr), g.optlen);
    m_adj(m, (int)hlen);
    g.len = m->pkt.len;
    g.seq = ntohl(g.hdr.seq);
    g.ack = ntohl(g.hdr.ack);
    g.flags = g.hdr.flags;
    g.win = ntohs(g.hdr.win);
    uint32_t seq = g.seq, ack = g.ack, seglen = g.len;
    uint8_t flags = g.flags;
    uint16_t win = g.win;

    struct tcp_batch b = { .n = 0 };
    struct socket *wake = NULL, *wake_listener = NULL;
    bool killed = false;

    struct tcp_pcb *pcb = lookup(&g.dst, &g.src);
    arch_irq_state_t s;
    if (pcb) {
        s = spin_lock_irqsave(&pcb->lock);
        if (pcb->state == TCP_CLOSED) {
            spin_unlock_irqrestore(&pcb->lock, s);   /* ended between the lookup and the lock */
            pcb_put(pcb);
            pcb = NULL;
        }
    }
    if (pcb == NULL) {
        STAT(dropped_no_pcb);
        build_rst(&b, &g.dst, &g.src, &g.hdr, seglen);
        m_freem(m);
        batch_send(&b);
        return;
    }
    pcb->segs_in++;

    if (pcb->state == TCP_LISTEN) {
        struct tcp_pcb *child = listen_input(pcb, &g, &b, &wake_listener);
        spin_unlock_irqrestore(&pcb->lock, s);
        pcb_put(pcb);
        if (child == NULL) {
            m_freem(m);
            batch_send(&b);
            sock_wake_after(wake_listener);
            return;
        }
        pcb = child;
        s = spin_lock_irqsave(&pcb->lock);
        pcb->segs_in++;
        /* The completing ACK is processed below as an ordinary segment of
         * the new connection (it may carry data). */
    }

    if (pcb->state == TCP_SYN_SENT) {
        if (flags & TH_ACK) {
            if (SEQ_LEQ(ack, pcb->iss) || SEQ_GT(ack, pcb->snd_nxt)) {
                if (!(flags & TH_RST))
                    build_rst(&b, &g.dst, &g.src, &g.hdr, seglen);
                goto out;
            }
        }
        if (flags & TH_RST) {
            if (flags & TH_ACK) {
                STAT(rsts_in);
                pcb->error = -ECONNREFUSED;
                wake = sock_ref(pcb);
                killed = pcb_end_locked(pcb);
            }
            goto out;
        }
        if (!(flags & TH_SYN))
            goto out;
        pcb->irs = seq;
        pcb->rcv_nxt = seq + 1;
        pcb->mss = parse_mss(g.opts, g.optlen, pcb->mss);
        if (pcb->mss > pcb->path_mss)
            pcb->mss = pcb->path_mss;
        pcb->cwnd = 2 * pcb->mss;
        pcb->snd_wnd = win;
        pcb->snd_wl1 = seq;
        pcb->snd_wl2 = ack;
        if (flags & TH_ACK) {
            pcb->snd_una = ack;
            established_locked(pcb, clock_now_ns());
            disarm_rexmit(pcb);
            pcb->delack_pending = true;
            tcp_output_locked(pcb, &b);
            wake = sock_ref(pcb);
        } else {
            set_state(pcb, TCP_SYN_RCVD);   /* simultaneous open */
            build_segment(pcb, &b, TH_SYN | TH_ACK, pcb->iss, 0, true);
            arm_rexmit(pcb);
        }
        goto out;
    }

    /* --- synchronized states --- */
    uint64_t now = clock_now_ns();
    pcb->last_rx_ns = now;   /* the peer is alive, whatever it sent */

    if (pcb->state == TCP_TIME_WAIT) {
        if (flags & TH_RST)
            goto out;   /* RFC 1337: no TIME_WAIT assassination */
        if (flags & TH_SYN) {
            challenge_ack(pcb, &b);
            goto out;
        }
        /* Only a retransmitted FIN is acknowledged and restarts 2 MSL. */
        if ((flags & TH_FIN) && seq + seglen + 1 == pcb->rcv_nxt) {
            build_segment(pcb, &b, TH_ACK, pcb->snd_nxt, 0, false);
            timer_cancel(&pcb->timewait);
            timer_start(&pcb->timewait, TCP_TIMEWAIT_NS);
        }
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
        if (!(flags & TH_RST))
            build_segment(pcb, &b, TH_ACK, pcb->snd_nxt, 0, false);
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

    /* RFC 5961 §3: a reset must name exactly rcv_nxt; elsewhere in the
     * window it earns a challenge ACK. */
    if (flags & TH_RST) {
        if (seq != pcb->rcv_nxt) {
            challenge_ack(pcb, &b);
            goto out;
        }
        STAT(rsts_in);
        pcb->error = pcb->state == TCP_SYN_RCVD ? -ECONNREFUSED : -ECONNRESET;
        wake = sock_ref(pcb);
        killed = pcb_end_locked(pcb);
        goto out;
    }
    /* RFC 5961 §4: a SYN in a synchronized state is never a reset. */
    if (flags & TH_SYN) {
        challenge_ack(pcb, &b);
        goto out;
    }
    if (!(flags & TH_ACK))
        goto out;
    /* RFC 5961 §5: an ACK outside [snd_una - max window, snd_max]. */
    if (SEQ_LT(ack, pcb->snd_una - TCP_MAX_WINDOW) || SEQ_GT(ack, pcb->snd_max)) {
        challenge_ack(pcb, &b);
        goto out;
    }
    pcb->keep_probes = 0;

    if (pcb->state == TCP_SYN_RCVD) {
        if (SEQ_LT(ack, pcb->snd_una) || SEQ_GT(ack, pcb->snd_nxt)) {
            build_rst(&b, &g.dst, &g.src, &g.hdr, seglen);
            goto out;
        }
        pcb->snd_una = ack;
        pcb->snd_wnd = win;
        pcb->snd_wl1 = seq;
        pcb->snd_wl2 = ack;
        disarm_rexmit(pcb);
        established_locked(pcb, now);
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
            rtt_sample(pcb, now);
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
            if (pcb->state == TCP_FIN_WAIT_1) {
                set_state(pcb, TCP_FIN_WAIT_2);
                orphan_fin_wait2(pcb);
            } else if (pcb->state == TCP_CLOSING) {
                enter_time_wait(pcb);
            } else if (pcb->state == TCP_LAST_ACK) {
                killed = pcb_end_locked(pcb);
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
    bool fin_now = (flags & TH_FIN) != 0;
    bool drained_fin = false;
    if (seglen && (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_FIN_WAIT_1 || pcb->state == TCP_FIN_WAIT_2)) {
        if (seq == pcb->rcv_nxt) {
            uint32_t n = netbuf_put_mbuf(&pcb->rcvbuf, m, 0, seglen);
            pcb->rcv_nxt += n;
            if (n < seglen)
                fin_now = false;   /* the FIN is beyond what we took */
            else
                ooo_drain(pcb, &drained_fin);
            pcb->rcv_wnd = netbuf_space(&pcb->rcvbuf);
            wake = wake ? wake : sock_ref(pcb);
            if (pcb->delack_pending || pcb->ooo_n) {
                tcp_output_locked(pcb, &b);   /* every second segment, or a gap behind: ACK now */
            } else {
                pcb->delack_pending = true;
                timer_cancel(&pcb->delack);
                timer_start(&pcb->delack, TCP_DELACK_NS);
            }
        } else {
            /* A hole before this segment: keep it and tell the sender where we are. */
            STAT(out_of_order);
            ooo_insert(pcb, m, seq, seglen, fin_now);
            m = NULL;
            fin_now = false;
            build_segment(pcb, &b, TH_ACK, pcb->snd_nxt, 0, false);
        }
    } else if (seglen) {
        fin_now = false;   /* data in a state that takes none: the FIN would be beyond it */
    }

    /* FIN. */
    bool fin_here = fin_now && seq + seglen == pcb->rcv_nxt;
    if ((fin_here || drained_fin) && !pcb->fin_rcvd) {
        pcb->fin_rcvd = true;
        pcb->rcv_nxt++;
        wake = wake ? wake : sock_ref(pcb);
        switch (pcb->state) {
        case TCP_ESTABLISHED: set_state(pcb, TCP_CLOSE_WAIT); break;
        case TCP_FIN_WAIT_1:
            if (pcb->fin_sent && pcb->snd_una == pcb->snd_max)
                enter_time_wait(pcb);
            else
                set_state(pcb, TCP_CLOSING);
            break;
        case TCP_FIN_WAIT_2: enter_time_wait(pcb); break;
        default: break;
        }
        pcb->delack_pending = false;
        build_segment(pcb, &b, TH_ACK, pcb->snd_nxt, 0, false);
        timer_cancel(&pcb->delack);
    } else if (pcb->state != TCP_CLOSED && pcb->state != TCP_TIME_WAIT) {
        tcp_output_locked(pcb, &b);   /* new window or ack may allow more data */
    }

out:
    spin_unlock_irqrestore(&pcb->lock, s);
    if (m)
        m_freem(m);
    batch_send(&b);
    sock_wake_after(wake);
    sock_wake_after(wake_listener);
    if (killed)
        pcb_put(pcb);   /* the state machine's */
    pcb_put(pcb);       /* the lookup's (or the creator's, for a new child) */
}

void tcp_pmtu_notify(const struct netaddr *local, const struct netaddr *remote, uint32_t seq, uint16_t mtu)
{
    struct tcp_pcb *pcb = lookup(local, remote);
    if (pcb == NULL)
        return;
    struct tcp_batch b = { .n = 0 };
    arch_irq_state_t s = spin_lock_irqsave(&pcb->lock);
    unsigned overhead = local->family == COSMO_AF_INET ? 40 : 60;
    uint32_t mss = mtu > overhead + 256 ? mtu - overhead : 256;
    if (pcb->state == TCP_LISTEN || pcb->state == TCP_CLOSED || pcb->state == TCP_TIME_WAIT ||
        pcb->state == TCP_SYN_SENT) {
        spin_unlock_irqrestore(&pcb->lock, s);
        pcb_put(pcb);
        return;
    }
    /* RFC 5927: the quoted segment must be one of ours still in flight. */
    if (!(SEQ_GEQ(seq, pcb->snd_una) && SEQ_LT(seq, pcb->snd_max)) || mss >= pcb->path_mss) {
        spin_unlock_irqrestore(&pcb->lock, s);
        pcb_put(pcb);
        return;
    }
    STAT(pmtu_updates);
    pcb->path_mss = (uint16_t)mss;
    if (pcb->mss > mss)
        pcb->mss = (uint16_t)mss;
    if (pcb->cwnd < pcb->mss)
        pcb->cwnd = pcb->mss;
    /* Resend what was too large, in the new size. */
    pcb->snd_nxt = pcb->snd_una;
    pcb->fin_sent = false;
    tcp_output_locked(pcb, &b);
    spin_unlock_irqrestore(&pcb->lock, s);
    batch_send(&b);
    pcb_put(pcb);
}

enum tcp_state tcp_state_of(struct tcp_pcb *pcb)
{
    return state_of(pcb);
}

void tcp_set_keepalive(uint64_t idle_ns, uint64_t intvl_ns, unsigned cnt)
{
    g_keep_idle_ns = idle_ns ? idle_ns : TCP_KEEPIDLE_NS;
    g_keep_intvl_ns = intvl_ns ? intvl_ns : TCP_KEEPINTVL_NS;
    g_keep_cnt = cnt ? cnt : TCP_KEEPCNT;
}

void tcp_set_fin_wait2(uint64_t ns)
{
    g_fin_wait2_ns = ns ? ns : TCP_FIN_WAIT2_NS;
}

void tcp_init(void)
{
    for (unsigned i = 0; i < TCP_HASH_SIZE; i++)
        list_init(&g_hash[i]);
    g_cookie_secret = random_u64() | 1;
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
