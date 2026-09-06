/*
 * netif.h - Network interfaces and the receive path.
 *
 * A driver implements netif_ops and calls netif_rx() from its completion
 * path; the stack never names a driver (invariant 5). All protocol input
 * runs on the network worker thread; output runs on the caller.
 */

#ifndef KERNEL_NETIF_H
#define KERNEL_NETIF_H

#include <kernel/list.h>
#include <kernel/mbuf.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

#define NETIF_UP       (1u << 0)
#define NETIF_LOOPBACK (1u << 1)
#define NETIF_NOARP    (1u << 2)
#define NETIF_GONE     (1u << 3)   /* netif_unregister ran: no transmit, no receive */

/* nif->caps, set by the driver before netif_register (unit 11). */
#define NETIF_CAP_TXCSUM (1u << 0)   /* finishes NET_CSUM_* transport checksums (virtio NEEDS_CSUM) */
#define NETIF_CAP_RXCSUM (1u << 1)   /* may mark received packets M_CSUM_OK */

struct netif;

struct netif_ops {
    /* Takes the packet; any errno. Runs inside a read-side section
     * (docs/kernel/quiesce/): must not sleep. */
    int (*transmit)(struct netif *nif, struct mbuf *m);
    /* Mandatory: last reference dropped after netif_unregister; free the
     * memory the netif is embedded in (netif_release_static for static). */
    void (*release)(struct netif *nif);
};

struct netif_stats {
    uint64_t rx_packets, rx_bytes, rx_dropped, rx_errors;
    uint64_t tx_packets, tx_bytes, tx_dropped, tx_errors;
};

struct netif {
    struct kobject obj;
    char name[8];
    unsigned index;
    uint8_t mac[6];
    uint32_t mtu;
    unsigned flags;
    struct {
        uint32_t addr, mask, gateway;   /* network byte order; 0 = unset */
    } ip4;
    struct in6_addr ip6_ll;
    const struct netif_ops *ops;
    void *priv;
    unsigned caps;                      /* NETIF_CAP_* */
    struct netif_stats stats;
    struct list_node link;
    spinlock_t lock;
};

void net_init(void);          /* mbufs, CPU 0's worker, loopback, protocols */
void net_start_workers(void); /* after SMP bring-up: one pinned worker per online CPU (unit 11) */

/* Register an interface prepared by the driver (name, mac, mtu, ops with
 * transmit and release). Assigns index and the IPv6 link-local address;
 * the registry takes its own reference, the creator keeps reference 1.
 * -EINVAL (bad fields, no release), -EEXIST on a duplicate name. */
int netif_register(struct netif *nif);
/* Take the interface down for good: no transmit or receive after this
 * returns, no packet of its left in the receive queue or on the worker,
 * its ARP and ND entries gone, the registry's reference dropped. The
 * driver then tears its queues down and drops its own reference; the
 * release frees the memory when the last holder is gone. Sleeps. */
void netif_unregister(struct netif *nif);
void netif_release_static(struct netif *nif);

/* Lookups return a referenced pointer (netif_put when done) or NULL. */
struct netif *netif_find(const char *name);
struct netif *netif_default(void);            /* the non-loopback interface that is up, or NULL */
struct netif *netif_loopback(void);
static inline void netif_get(struct netif *nif) { kobject_get(&nif->obj); }
static inline void netif_put(struct netif *nif) { kobject_put(&nif->obj); }
void netif_set_ipv4(struct netif *nif, uint32_t addr, uint32_t mask, uint32_t gateway);
void netif_set_up(struct netif *nif, bool up);
bool netif_owns_ipv4(uint32_t addr);          /* one of our addresses (any interface) */
bool netif_owns_ipv6(const struct in6_addr *a);

/* Driver -> stack: takes the packet, any context (dropped once GONE).
 * The packet is steered to a CPU's receive queue by its flow hash, so
 * one flow's packets are processed in order by one worker (unit 11). */
void netif_rx(struct netif *nif, struct mbuf *m);
/* The same for a driver whose receive queue is bound to `cpu` (a
 * multi-queue device that steered already): no hash, that CPU's queue. */
void netif_rx_on(struct netif *nif, struct mbuf *m, unsigned cpu);
/* The steering hash: Ethernet (when `ether`) / IPv4 / IPv6 addresses and
 * TCP/UDP ports; 0 for a packet it cannot classify. Reads at most the
 * first 96 bytes; any context. */
uint32_t net_flow_hash(const struct mbuf *m, bool ether);
/* Steering on (the default) or every packet to CPU 0's queue (the
 * architecture before unit 11; the benchmark's baseline). */
void netif_set_steering(bool on);
bool netif_steering(void);
struct net_cpu_stats {
    uint64_t rx_queued, rx_dropped, rx_steered_here, work_runs;
};
bool netif_cpu_stats(unsigned cpu, struct net_cpu_stats *out);   /* false: no such worker */
/* Test hook: called on the worker for every received packet before input;
 * return false to take the packet (the hook then owns it). */
typedef bool (*netif_rx_hook_fn)(struct netif *nif, struct mbuf *m, void *arg);
void netif_set_rx_hook(netif_rx_hook_fn fn, void *arg);
/* Stack -> driver: takes the packet. Thread context; -ENETUNREACH when
 * down, -ENODEV once unregistered. */
int netif_transmit(struct netif *nif, struct mbuf *m);
/* Test hook: -1, or the number of queued receive packets from `nif`. */
unsigned netif_rxq_count(const struct netif *nif);

/* Deferred work on the worker thread (timers hand off through this). */
typedef void (*net_work_fn)(void *arg);
struct net_work {
    struct list_node link;
    net_work_fn fn;
    void *arg;
    bool queued;
};
void net_work_init(struct net_work *w, net_work_fn fn, void *arg);
bool net_work_queue(struct net_work *w);      /* any context; true if newly queued, false while already queued */

/* Test hook for the loopback path: called for every packet before it
 * is queued; return false to drop it. */
typedef bool (*lo_filter_fn)(struct mbuf *m, void *arg);
void loopback_set_filter(lo_filter_fn fn, void *arg);

void loopback_init(void);
void netif_dump(void);

#endif /* KERNEL_NETIF_H */
