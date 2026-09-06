/*
 * mbuf.c - Packet buffers.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/panic.h>
#include <kernel/string.h>

static struct kmem_cache *g_mbuf_cache;
static struct kmem_cache *g_cluster_cache;
static struct mbuf_stats g_stats;
static spinlock_t g_stats_lock = SPINLOCK_INIT("mbuf-stats");

static void stat(uint64_t *f, int64_t d)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    *f = (uint64_t)((int64_t)*f + d);
    spin_unlock_irqrestore(&g_stats_lock, s);
}

void mbuf_init(void)
{
    g_mbuf_cache = kmem_cache_create("mbuf", sizeof(struct mbuf), 64);
    g_cluster_cache = kmem_cache_create("mcluster", sizeof(struct mbuf_cluster), 64);
    if (g_mbuf_cache == NULL || g_cluster_cache == NULL)
        panic("mbuf: cannot create caches");
}

struct mbuf *m_get(void)
{
    struct mbuf *m = kmem_cache_alloc(g_mbuf_cache, 0);
    if (m == NULL) {
        stat(&g_stats.alloc_failures, 1);
        return NULL;
    }
    memset(m, 0, offsetof(struct mbuf, inl));
    m->buf = m->inl;
    m->size = MHLEN;
    m->data = m->buf;
    m->refcount = 1;
    stat(&g_stats.mbufs_alive, 1);
    stat(&g_stats.allocs, 1);
    return m;
}

struct mbuf *m_getcl(void)
{
    struct mbuf *m = m_get();
    if (m == NULL)
        return NULL;
    struct mbuf_cluster *cl = kmem_cache_alloc(g_cluster_cache, 0);
    if (cl == NULL) {
        m_free(m);
        stat(&g_stats.alloc_failures, 1);
        return NULL;
    }
    cl->refcount = 1;
    m->cl = cl;
    m->buf = cl->data;
    m->size = MCLBYTES;
    m->data = m->buf + NET_HEADROOM;
    m->flags = M_PKTHDR | M_EXT;
    stat(&g_stats.clusters_alive, 1);
    return m;
}

struct mbuf *m_free(struct mbuf *m)
{
    if (m == NULL)
        return NULL;
    struct mbuf *next = m->next;
    if (__atomic_fetch_sub(&m->refcount, 1, __ATOMIC_ACQ_REL) != 1)
        return next;
    if (m->flags & M_EXT) {
        if (__atomic_fetch_sub(&m->cl->refcount, 1, __ATOMIC_ACQ_REL) == 1) {
            kmem_cache_free(g_cluster_cache, m->cl);
            stat(&g_stats.clusters_alive, -1);
        }
    }
    kmem_cache_free(g_mbuf_cache, m);
    stat(&g_stats.mbufs_alive, -1);
    stat(&g_stats.frees, 1);
    return next;
}

void m_freem(struct mbuf *m)
{
    while (m)
        m = m_free(m);
}

struct mbuf *m_ref(struct mbuf *m)
{
    if ((m->flags & M_EXT) == 0)
        return NULL;
    struct mbuf *n = m_get();
    if (n == NULL)
        return NULL;
    __atomic_fetch_add(&m->cl->refcount, 1, __ATOMIC_ACQ_REL);
    n->cl = m->cl;
    n->buf = m->buf;
    n->size = m->size;
    n->data = m->data;
    n->len = m->len;
    n->flags = (m->flags & ~M_PKTHDR) | M_EXT;
    return n;
}

uint32_t m_length(const struct mbuf *m)
{
    uint32_t n = 0;
    for (; m; m = m->next)
        n += m->len;
    return n;
}

struct mbuf *m_prepend(struct mbuf *m, uint32_t n)
{
    if (m_leadingspace(m) >= n) {
        m->data -= n;
        m->len += n;
        if (m->flags & M_PKTHDR)
            m->pkt.len += n;
        return m;
    }
    struct mbuf *h = n > MHLEN ? m_getcl() : m_get();
    if (h == NULL) {
        m_freem(m);
        return NULL;
    }
    if (h->size - m_leadingspace(h) < n)
        h->data = h->buf;
    h->len = n;
    if (m->flags & M_PKTHDR) {
        h->flags |= M_PKTHDR;
        h->pkt = m->pkt;
        h->pkt.len = m->pkt.len + n;
        m->flags &= ~M_PKTHDR;
    }
    h->next = m;
    return h;
}

struct mbuf *m_pullup(struct mbuf *m, uint32_t n)
{
    if (m->len >= n)
        return m;
    if (n > MCLBYTES) {
        m_freem(m);
        return NULL;
    }
    struct mbuf *h = m_getcl();
    if (h == NULL) {
        m_freem(m);
        return NULL;
    }
    /* Keep the headroom when the pulled-up bytes fit behind it, so a reply
     * built on this packet (ICMP, a TCP RST or ACK) prepends in place. */
    if (n > MCLBYTES - NET_HEADROOM)
        h->data = h->buf;
    if (m->flags & M_PKTHDR) {
        h->pkt = m->pkt;
        m->flags &= ~M_PKTHDR;
    } else {
        h->flags &= ~M_PKTHDR;
    }
    /* Copy n bytes from the chain into h, consuming whole buffers. */
    uint32_t got = 0;
    while (got < n && m) {
        uint32_t take = m->len < n - got ? m->len : n - got;
        memcpy(h->data + got, m->data, take);
        m->data += take;
        m->len -= take;
        got += take;
        if (m->len == 0)
            m = m_free(m);
    }
    if (got < n) {
        m_freem(h);
        m_freem(m);
        return NULL;
    }
    h->len = n;
    h->next = m;
    return h;
}

void m_adj(struct mbuf *m, int n)
{
    if (m == NULL || n == 0)
        return;
    if (n > 0) {
        uint32_t left = (uint32_t)n;
        for (struct mbuf *b = m; b && left; b = b->next) {
            uint32_t take = b->len < left ? b->len : left;
            b->data += take;
            b->len -= take;
            left -= take;
        }
        if (m->flags & M_PKTHDR)
            m->pkt.len -= (uint32_t)n - left;
        return;
    }
    uint32_t left = (uint32_t)(-n);
    uint32_t total = m_length(m);
    if (left > total)
        left = total;
    uint32_t keep = total - left;
    uint32_t seen = 0;
    for (struct mbuf *b = m; b; b = b->next) {
        if (seen + b->len <= keep) {
            seen += b->len;
        } else {
            b->len = keep > seen ? keep - seen : 0;
            seen = keep;
        }
    }
    if (m->flags & M_PKTHDR)
        m->pkt.len -= left;
}

bool m_copydata(const struct mbuf *m, uint32_t off, uint32_t len, void *dst)
{
    uint8_t *out = dst;
    for (; m && off >= m->len; m = m->next)
        off -= m->len;
    while (len > 0 && m) {
        uint32_t take = m->len - off < len ? m->len - off : len;
        memcpy(out, m->data + off, take);
        out += take;
        len -= take;
        off = 0;
        m = m->next;
    }
    return len == 0;
}

int m_append(struct mbuf *m, const void *src, uint32_t len)
{
    const uint8_t *in = src;
    struct mbuf *last = m;
    while (last->next)
        last = last->next;
    while (len > 0) {
        uint32_t room = m_trailingspace(last);
        if (room == 0) {
            struct mbuf *n = m_getcl();
            if (n == NULL)
                return -ENOMEM;
            n->flags &= ~M_PKTHDR;
            n->data = n->buf;
            last->next = n;
            last = n;
            room = m_trailingspace(last);
        }
        uint32_t take = room < len ? room : len;
        memcpy(last->data + last->len, in, take);
        last->len += take;
        in += take;
        len -= take;
        if (m->flags & M_PKTHDR)
            m->pkt.len += take;
    }
    return 0;
}

struct mbuf *m_copypacket(const struct mbuf *m)
{
    uint32_t total = m_length(m);
    struct mbuf *n = m_getcl();
    if (n == NULL)
        return NULL;
    if (total <= m_trailingspace(n)) {
        /* One cluster: linear, with the headroom kept. */
        if (!m_copydata(m, 0, total, n->data)) {
            m_freem(n);
            return NULL;
        }
        n->len = total;
    } else {
        /* Longer: a chain of clusters (unit 11; a 16 KiB loopback segment
         * held by a test filter used to be refused). */
        uint8_t tmp[512];
        for (uint32_t off = 0; off < total;) {
            uint32_t k = total - off < sizeof(tmp) ? total - off : (uint32_t)sizeof(tmp);
            if (!m_copydata(m, off, k, tmp) || m_append(n, tmp, k)) {
                m_freem(n);
                return NULL;
            }
            off += k;
        }
    }
    if (m->flags & M_PKTHDR)
        n->pkt = m->pkt;
    n->pkt.len = total;
    n->flags |= m->flags & (M_BCAST | M_MCAST | M_CSUM_OK);
    return n;
}

/* --- queues ------------------------------------------------------------- */

void mbufq_init(struct mbufq *q, unsigned maxlen, const char *name)
{
    q->head = q->tail = NULL;
    q->len = 0;
    q->maxlen = maxlen;
    spinlock_init(&q->lock, name);
}

bool mbufq_enqueue(struct mbufq *q, struct mbuf *m)
{
    m->nextpkt = NULL;
    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    if (q->len >= q->maxlen) {
        spin_unlock_irqrestore(&q->lock, s);
        m_freem(m);
        return false;
    }
    if (q->tail)
        q->tail->nextpkt = m;
    else
        q->head = m;
    q->tail = m;
    q->len++;
    spin_unlock_irqrestore(&q->lock, s);
    return true;
}

struct mbuf *mbufq_dequeue(struct mbufq *q)
{
    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    struct mbuf *m = q->head;
    if (m) {
        q->head = m->nextpkt;
        if (q->head == NULL)
            q->tail = NULL;
        q->len--;
        m->nextpkt = NULL;
    }
    spin_unlock_irqrestore(&q->lock, s);
    return m;
}

void mbufq_drain(struct mbufq *q)
{
    struct mbuf *m;
    while ((m = mbufq_dequeue(q)) != NULL)
        m_freem(m);
}

unsigned mbufq_len(struct mbufq *q)
{
    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    unsigned n = q->len;
    spin_unlock_irqrestore(&q->lock, s);
    return n;
}

void mbuf_get_stats(struct mbuf_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    *out = g_stats;
    spin_unlock_irqrestore(&g_stats_lock, s);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(m_get);
EXPORT_SYMBOL(m_getcl);
EXPORT_SYMBOL(m_free);
EXPORT_SYMBOL(m_freem);
EXPORT_SYMBOL(m_prepend);
EXPORT_SYMBOL(m_pullup);
EXPORT_SYMBOL(m_adj);
EXPORT_SYMBOL(m_copydata);
EXPORT_SYMBOL(m_append);
EXPORT_SYMBOL(m_length);
EXPORT_SYMBOL(m_copypacket);
