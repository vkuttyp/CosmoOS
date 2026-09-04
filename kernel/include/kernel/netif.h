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
#include <kernel/spinlock.h>
#include <kernel/types.h>

#define NETIF_UP       (1u << 0)
#define NETIF_LOOPBACK (1u << 1)
#define NETIF_NOARP    (1u << 2)

struct netif;

struct netif_ops {
    int (*transmit)(struct netif *nif, struct mbuf *m);   /* takes the packet; any errno */
};

struct netif_stats {
    uint64_t rx_packets, rx_bytes, rx_dropped, rx_errors;
    uint64_t tx_packets, tx_bytes, tx_dropped, tx_errors;
};

struct netif {
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
    struct netif_stats stats;
    struct list_node link;
    spinlock_t lock;
};

void net_init(void);          /* mbufs, worker thread, loopback, protocols */

/* Register an interface prepared by the driver (name, mac, mtu, ops).
 * Assigns index and the IPv6 link-local address. -EEXIST on a duplicate name. */
int netif_register(struct netif *nif);
void netif_unregister(struct netif *nif);
struct netif *netif_find(const char *name);
struct netif *netif_default(void);            /* the non-loopback interface, or NULL */
struct netif *netif_loopback(void);
void netif_set_ipv4(struct netif *nif, uint32_t addr, uint32_t mask, uint32_t gateway);
void netif_set_up(struct netif *nif, bool up);
bool netif_owns_ipv4(uint32_t addr);          /* one of our addresses (any interface) */
bool netif_owns_ipv6(const struct in6_addr *a);

/* Driver -> stack: takes the packet, any context. */
void netif_rx(struct netif *nif, struct mbuf *m);
/* Stack -> driver: takes the packet. Thread context. */
int netif_transmit(struct netif *nif, struct mbuf *m);

/* Deferred work on the worker thread (timers hand off through this). */
typedef void (*net_work_fn)(void *arg);
struct net_work {
    struct list_node link;
    net_work_fn fn;
    void *arg;
    bool queued;
};
void net_work_init(struct net_work *w, net_work_fn fn, void *arg);
void net_work_queue(struct net_work *w);      /* any context; idempotent while queued */

/* Test hook for the loopback path: called for every packet before it
 * is queued; return false to drop it. */
typedef bool (*lo_filter_fn)(struct mbuf *m, void *arg);
void loopback_set_filter(lo_filter_fn fn, void *arg);

void loopback_init(void);
void netif_dump(void);

#endif /* KERNEL_NETIF_H */
