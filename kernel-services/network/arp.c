/*
 * arp.c - Address resolution (RFC 826) with a fixed table.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#define ARP_HW_ETHER   1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2
#define ARP_REACHABLE_NS   (20ull * 60 * 1000000000ull)
#define ARP_RETRY_NS       (1000000000ull)
#define ARP_MAX_TRIES      3

struct arp_pkt {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t op;
    uint8_t sha[ETH_ALEN];
    uint32_t spa;
    uint8_t tha[ETH_ALEN];
    uint32_t tpa;
} __packed;

enum arp_state { ARP_FREE, ARP_INCOMPLETE, ARP_REACHABLE };

struct arp_entry {
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    enum arp_state state;
    uint64_t updated_ns;
    struct mbuf *pending;
    unsigned tries;
    struct netif *nif;
};

static struct arp_entry g_table[ARP_TABLE_SIZE];
static spinlock_t g_lock = SPINLOCK_INIT("arp");
static struct arp_stats g_stats;
static struct timer g_timer;
static struct net_work g_age_work;

static struct arp_entry *find(uint32_t ip)
{
    for (unsigned i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_table[i].state != ARP_FREE && g_table[i].ip == ip)
            return &g_table[i];
    }
    return NULL;
}

/*
 * A free slot, or with `evict` the least recently updated reachable entry
 * (an incomplete one has a resolution in flight). Without `evict` a full
 * table yields NULL: traffic we did not ask for never displaces an entry.
 */
static struct arp_entry *alloc_entry(uint32_t ip, struct netif *nif, bool evict)
{
    struct arp_entry *victim = NULL;
    for (unsigned i = 0; i < ARP_TABLE_SIZE; i++) {
        struct arp_entry *e = &g_table[i];
        if (e->state == ARP_FREE) {
            victim = e;
            break;
        }
        if (!evict)
            continue;
        if (victim == NULL || (victim->state == ARP_INCOMPLETE && e->state == ARP_REACHABLE) ||
            (victim->state == e->state && e->updated_ns < victim->updated_ns))
            victim = e;
    }
    if (victim == NULL)
        return NULL;
    if (victim->state != ARP_FREE) {
        m_freem(victim->pending);
        victim->pending = NULL;
        g_stats.entries--;
    }
    memset(victim, 0, sizeof(*victim));
    victim->ip = ip;
    victim->nif = nif;
    victim->state = ARP_INCOMPLETE;
    victim->updated_ns = clock_now_ns();
    g_stats.entries++;
    return victim;
}

static int send_arp(struct netif *nif, uint16_t op, uint32_t tpa, const uint8_t tha[ETH_ALEN],
                    const uint8_t dst[ETH_ALEN])
{
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return -ENOMEM;
    struct arp_pkt *a = (struct arp_pkt *)m->data;
    a->htype = htons(ARP_HW_ETHER);
    a->ptype = htons(ETH_P_IP);
    a->hlen = ETH_ALEN;
    a->plen = 4;
    a->op = htons(op);
    memcpy(a->sha, nif->mac, ETH_ALEN);
    a->spa = nif->ip4.addr;
    memcpy(a->tha, tha, ETH_ALEN);
    a->tpa = tpa;
    m->len = m->pkt.len = sizeof(*a);
    return ether_output(nif, m, dst, ETH_P_ARP);
}

int arp_resolve(struct netif *nif, uint32_t ip, uint8_t mac[ETH_ALEN], struct mbuf *m)
{
    if (ip == INADDR_BROADCAST_N || (nif->ip4.mask && (ip | nif->ip4.mask) == INADDR_BROADCAST_N)) {
        memcpy(mac, eth_broadcast, ETH_ALEN);
        return 0;
    }
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    struct arp_entry *e = find(ip);
    if (e && e->state == ARP_REACHABLE) {
        memcpy(mac, e->mac, ETH_ALEN);
        spin_unlock_irqrestore(&g_lock, s);
        return 0;
    }
    bool send = false;
    if (e == NULL) {
        e = alloc_entry(ip, nif, true);
        send = true;
    }
    if (m) {
        if (e->pending) {
            m_freem(e->pending);
            g_stats.pending_dropped++;
        }
        e->pending = m;
    }
    if (send) {
        e->tries = 1;
        e->updated_ns = clock_now_ns();
    }
    spin_unlock_irqrestore(&g_lock, s);
    if (send) {
        static const uint8_t zero_mac[ETH_ALEN] = { 0 };
        __atomic_fetch_add(&g_stats.requests_sent, 1, __ATOMIC_RELAXED);
        send_arp(nif, ARP_OP_REQUEST, ip, zero_mac, eth_broadcast);
    }
    return -EINPROGRESS;
}

bool arp_lookup(uint32_t ip, uint8_t mac[ETH_ALEN])
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    struct arp_entry *e = find(ip);
    bool ok = e && e->state == ARP_REACHABLE;
    if (ok)
        memcpy(mac, e->mac, ETH_ALEN);
    spin_unlock_irqrestore(&g_lock, s);
    return ok;
}

void arp_input(struct netif *nif, struct mbuf *m)
{
    m = m_pullup(m, sizeof(struct arp_pkt));
    if (m == NULL)
        return;
    struct arp_pkt a;
    memcpy(&a, m->data, sizeof(a));
    m_freem(m);
    if (ntohs(a.htype) != ARP_HW_ETHER || ntohs(a.ptype) != ETH_P_IP || a.hlen != ETH_ALEN || a.plen != 4)
        return;
    if (a.spa == 0 || a.spa == nif->ip4.addr)
        return;   /* probes and our own address: nothing to learn */

    uint16_t op = ntohs(a.op);
    bool for_us = a.tpa == nif->ip4.addr && nif->ip4.addr != 0;
    struct mbuf *pending = NULL;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    struct arp_entry *e = find(a.spa);
    /*
     * ARP carries no authentication, so the table learns only what RFC 826
     * requires: a request addressed to us records (or refreshes) the asker,
     * and a reply addressed to us completes an entry we are resolving.
     * Unsolicited replies and requests for other hosts change nothing, and
     * learning an asker never evicts an entry in use.
     */
    bool learn = false;
    if (op == ARP_OP_REQUEST) {
        learn = for_us;
    } else if (op == ARP_OP_REPLY) {
        learn = for_us && e != NULL && e->state == ARP_INCOMPLETE;
        if (!learn)
            g_stats.unsolicited++;
    }
    if (learn && e == NULL)
        e = alloc_entry(a.spa, nif, false);   /* NULL when the table is full */
    if (learn && e) {
        memcpy(e->mac, a.sha, ETH_ALEN);
        e->state = ARP_REACHABLE;
        e->updated_ns = clock_now_ns();
        e->nif = nif;
        pending = e->pending;
        e->pending = NULL;
    }
    spin_unlock_irqrestore(&g_lock, s);

    if (op == ARP_OP_REQUEST) {
        __atomic_fetch_add(&g_stats.requests_rcvd, 1, __ATOMIC_RELAXED);
        if (for_us) {
            __atomic_fetch_add(&g_stats.replies_sent, 1, __ATOMIC_RELAXED);
            send_arp(nif, ARP_OP_REPLY, a.spa, a.sha, a.sha);
        }
    } else if (op == ARP_OP_REPLY) {
        __atomic_fetch_add(&g_stats.replies_rcvd, 1, __ATOMIC_RELAXED);
    }
    if (pending)
        ether_output(nif, pending, a.sha, ETH_P_IP);
}

void arp_flush(struct netif *nif)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    for (unsigned i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_table[i].state != ARP_FREE && g_table[i].nif == nif) {
            m_freem(g_table[i].pending);
            memset(&g_table[i], 0, sizeof(g_table[i]));
            g_stats.entries--;
        }
    }
    spin_unlock_irqrestore(&g_lock, s);
}

/* Worker thread: retry incomplete entries, expire stale ones. */
void arp_age(uint64_t now)
{
    struct {
        struct netif *nif;
        uint32_t ip;
    } retry[ARP_TABLE_SIZE];
    unsigned nr_retry = 0;

    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    for (unsigned i = 0; i < ARP_TABLE_SIZE; i++) {
        struct arp_entry *e = &g_table[i];
        if (e->state == ARP_FREE)
            continue;
        if (e->state == ARP_REACHABLE) {
            if (now - e->updated_ns > ARP_REACHABLE_NS) {
                memset(e, 0, sizeof(*e));
                g_stats.entries--;
            }
            continue;
        }
        if (now - e->updated_ns < ARP_RETRY_NS)
            continue;
        if (e->tries >= ARP_MAX_TRIES) {
            m_freem(e->pending);
            if (e->pending)
                g_stats.pending_dropped++;
            g_stats.timeouts++;
            memset(e, 0, sizeof(*e));
            g_stats.entries--;
            continue;
        }
        e->tries++;
        e->updated_ns = now;
        retry[nr_retry].nif = e->nif;
        retry[nr_retry].ip = e->ip;
        nr_retry++;
    }
    spin_unlock_irqrestore(&g_lock, s);

    static const uint8_t zero_mac[ETH_ALEN] = { 0 };
    for (unsigned i = 0; i < nr_retry; i++) {
        __atomic_fetch_add(&g_stats.requests_sent, 1, __ATOMIC_RELAXED);
        send_arp(retry[i].nif, ARP_OP_REQUEST, retry[i].ip, zero_mac, eth_broadcast);
    }
}

static void age_work(void *arg)
{
    (void)arg;
    arp_age(clock_now_ns());
    nd_age(clock_now_ns());
    timer_start(&g_timer, ARP_RETRY_NS);
}

static void age_timer(struct timer *t, void *arg)
{
    (void)t;
    (void)arg;
    net_work_queue(&g_age_work);
}

void arp_init(void)
{
    net_work_init(&g_age_work, age_work, NULL);
    timer_setup(&g_timer, age_timer, NULL);
    timer_start(&g_timer, ARP_RETRY_NS);
}

void arp_get_stats(struct arp_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    *out = g_stats;
    spin_unlock_irqrestore(&g_lock, s);
}
