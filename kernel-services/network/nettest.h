/*
 * nettest.h - What nettest.c shares with its CeeSafe batches (roadmap 2.0
 * SA6, nettest): the loopback test servers' state, the release framework's
 * wrappers a batch calls, and the bulk transfer nettest_lo.cee defines for
 * the C tests still here. Private to kernel-services/network.
 */
#ifndef KERNEL_SERVICES_NETWORK_NETTEST_H
#define KERNEL_SERVICES_NETWORK_NETTEST_H

#include <kernel/net/inet.h>
#include <kernel/netif.h>
#include <kernel/object.h>
#include <kernel/socket.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <kernel/net/tap.h>
#include <kernel/net/tapsvc.h>

/* A loopback test's server thread. Its storage outlives any frame (the
 * thread may run after the test returns), and nettest_tcp_server_start
 * registers its release the moment it exists. */
struct tcp_server {
    struct netaddr addr;
    uint32_t bytes_seen;
    uint32_t bytes_echoed;
    int result;
    volatile bool done;
    bool echo;               /* echo mode for the harness */
    unsigned param;          /* a per-thread parameter (net-tcp-nagle-peer: the reader's delay in ms) */
    volatile bool stop;
    /* For its release (the net-leftover unit): the thread, and the
     * sockets it is blocked on, published under `lock` so the release can
     * take a reference and shut them without racing the thread's put. */
    struct thread *th;
    spinlock_t lock;
    struct socket *ls, *c;
};

/* A reference the test holds on an interface (a creator's, or one
 * netif_find gave it), released however it returns. Keyed by a static
 * holder, never by the interface: the interface's own address is its
 * unregister's key, and netif_find can return the very interface the test
 * registered. */
struct nt_netif_ref {
    struct netif *nif;
};

/* nettest.c's release framework (the nt_* wrappers), for the CeeSafe batches. */
bool nettest_tcp_server_start(struct tcp_server *srv, void (*fn)(void *), const char *name, int prio);
/* Publish the server's listener (or its connection) for the release, under its lock. */
void nettest_tcp_server_publish(struct tcp_server *srv, bool listener, struct socket *sock);
int nettest_ksock_create(int family, int type, uint32_t uid, struct socket **out);
int nettest_ksock_accept(struct socket *s, struct socket **out, struct netaddr *peer);
void nettest_ksock_put(struct socket *s);
void nettest_loopback_set_filter(lo_filter_fn fn, void *arg);
void nettest_netif_set_rx_hook(netif_rx_hook_fn fn, void *arg);
struct tap *nettest_tap_create(const char *name, uint32_t ip, uint32_t mask, const uint8_t *mac);
void nettest_tap_destroy(struct tap *t);
struct tapsvc *nettest_tapsvc_start(struct tap *t);
void nettest_tapsvc_stop(struct tapsvc *svc);
int nettest_netif_register(struct netif *nif);
void nettest_netif_unregister(struct netif *nif);
bool nettest_netif_hold(struct nt_netif_ref *r, struct netif *nif);
void nettest_netif_ref_put(struct nt_netif_ref *r);
void nettest_netif_set_steering(bool on);
/* A packet a fake interface keeps in `*slot`, freed however the test returns. */
bool nettest_defer_mbuf_slot(struct mbuf **slot);
int nettest_ksock_create_raw(int family, int proto, struct socket **out);
void nettest_tcp_set_keepalive(uint64_t idle_ns, uint64_t intvl_ns, unsigned cnt);
void nettest_tcp_set_fin_wait2(uint64_t ns);
/* A raw socket's queued copies; a kernel object's read and write (CeeSafe
 * cannot call the io type's function-pointer fields). */
unsigned nettest_raw_queued(struct socket *s);
int64_t nettest_kobject_read(struct kobject *obj, void *buf, size_t len);
int64_t nettest_kobject_write(struct kobject *obj, const void *buf, size_t len);
/* net-lo-tcp's and net-accept-order's listen backlog (nettest.c). */
bool nettest_lo_tcp_backlog(const char **reason, uint16_t port, bool force, bool *accepted_first);

/* nettest_lo.cee: move `bytes` of a pattern to a sink thread on `addr` and
 * check the connection's end, lingering `linger_ms` past it (net-lo-tcp,
 * net-lo-tcp-loss, net-rx-dup, net-tcp-reorder). */
bool nettest_tcp_transfer(const char **reason, const struct netaddr *addr, uint32_t bytes, uint32_t linger_ms);

/* nettest_tap.cee: the tap tests' frame builders. Each writes exactly the
 * bytes its frame has and returns that length; a MAC is 6 bytes. */
uint16_t nettest_l4cksum(uint32_t sip, uint32_t dip, uint8_t proto, const void *l4, uint16_t len);
bool nettest_l4_ok(uint32_t sip, uint32_t dip, uint8_t proto, const void *l4, uint16_t len);
uint32_t nettest_wrap(uint8_t *frame, const uint8_t *dmac, const uint8_t *smac, uint32_t sip, uint32_t dip, uint8_t ttl,
                      uint8_t proto, const void *l4, uint16_t l4len);
uint16_t nettest_mk_udp(uint8_t *l4, uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, const uint8_t *pl,
                        uint16_t pllen);
uint16_t nettest_mk_tcp(uint8_t *l4, uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, uint8_t flags);
uint16_t nettest_mk_icmp(uint8_t *l4, uint8_t type, uint16_t id, uint16_t seq, const uint8_t *pl, uint16_t pllen);
void nettest_seed_arp(struct netif *nif, uint32_t peer_ip, const uint8_t *peer_mac);
uint32_t nettest_build_udp(uint8_t *buf, const uint8_t *dstmac, const uint8_t *srcmac, uint32_t sip, uint32_t dip,
                           uint8_t ttl, uint16_t sport, uint16_t dport, const uint8_t *pl, uint32_t pllen);
struct mbuf *nettest_recv_ip(struct tap *t);
uint32_t nettest_mk_dns(uint8_t *msg, uint16_t id);

#endif
