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
#include <kernel/vfs.h>
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
int nettest_vfs_open(const char *path, unsigned flags, struct file **out);
void nettest_file_put(struct file *f);
/* The NAT and firewall state, cleared however the test returns; cleared now. */
bool nettest_defer_nat_state(void);
void nettest_release_nat_state(void);
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
/* net-lo-tcp's listen backlog, unforced (nettest_accept.cee, which forces
 * it for net-accept-order itself). */
bool nettest_lo_tcp_backlog(const char **reason, uint16_t port, bool force, bool *accepted_first);
/* nettest_census under a name CeeSafe can import beside the struct's
 * (selftest.h names both nettest_census). */
struct nettest_census;
void nettest_census_take(struct nettest_census *out);
/* tap.h's after-purge seam, set for nettest_seam.cee (debug builds; NULL
 * disarms). */
void nettest_tap_set_after_purge(void (*fn)(void *), void *arg);

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

/* nettest_fwd.cee: a guest's datagram through its tap file, a forwarded one
 * read back (0: none in `tries` reads), an ICMP echo of 16 bytes. */
bool nettest_fwt_send(struct file *from, const uint8_t *tapmac, const uint8_t *gmac, uint32_t sip, uint32_t dip,
                      uint8_t proto, const uint8_t *l4, uint16_t l4len);
int64_t nettest_fwt_recv(struct file *to, uint8_t *rx, size_t cap, unsigned tries);
uint16_t nettest_fwt_mk_icmp(uint8_t *l4, uint8_t type, uint16_t id);
/* nettest_input.cee: the big-endian ones-complement sum ICMP verifies, a
 * checksummed echo (16 bytes), a checksummed need-frag quoting host ->
 * guest (36 bytes), and whether the host answered an echo on a guest's tap. */
uint16_t nettest_fwt_ones_sum(const uint8_t *p, unsigned n);
uint16_t nettest_fwt_mk_icmp_ck(uint8_t *l4, uint8_t type, uint16_t id);
uint16_t nettest_fwt_mk_needfrag(uint8_t *l4, uint32_t host_ip, uint32_t guest_ip, uint16_t mtu);
bool nettest_fwt_recv_echo_reply(struct file *f, unsigned tries);

/* nettest_hostin.cee: the world side of the uplink tap. One datagram as
 * read back from it: the world's view of what the host answered. For TCP
 * the header fields and up to 64 bytes of payload; for UDP the ports and
 * payload length; for ICMP the type in `flags`. */
struct hin_seg {
    uint32_t src, dst;
    uint8_t  proto, flags;
    uint16_t sport, dport, win, paylen;
    uint32_t seq, ack;
    uint8_t  pay[64];
};
/* A TCP segment with every field the world side needs (its length); one
 * IPv4 datagram in on the tap; the next datagram of `proto` toward world
 * port `wport` (0 = any) in tries x 10 ms; the tap emptied; every byte of
 * [base, base + total) arriving, `covered` already seen and `have` saying
 * `sg` holds an uncounted segment; an awaited accept (NULL: none) and
 * receive (-EAGAIN: nothing in tries x 10 ms); non-blocking listeners,
 * released however the test returns. */
uint16_t nettest_hin_mk_tcp(uint8_t *l4, uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, uint32_t seq,
                            uint32_t ack, uint8_t flags, uint16_t win, const void *pl, uint16_t pllen);
bool nettest_hin_send(struct tap *u, const uint8_t *umac, const uint8_t *wmac, uint32_t sip, uint32_t dip,
                      uint8_t proto, const uint8_t *l4, uint16_t l4len);
bool nettest_hin_recv(struct tap *u, uint8_t proto, uint16_t wport, struct hin_seg *o, unsigned tries);
void nettest_hin_drain(struct tap *u);
bool nettest_hin_recv_stream(struct tap *u, uint16_t wport, uint32_t base, uint32_t covered, uint32_t total,
                             struct hin_seg *sg, bool have);
struct socket *nettest_hin_accept(struct socket *ls);
int64_t nettest_hin_recv_sock(struct socket *s, void *buf, size_t cap, unsigned tries);
bool nettest_hin_udp_listener(struct socket **out, uint32_t ip, uint16_t port);
bool nettest_hin_tcp_listener(struct socket **out, uint16_t port);
/* nettest.c: net-hostinput's outbound connect on its own thread, one at a
 * time: started, done (with its result), and joined (its socket). */
bool nettest_hin_connect(uint32_t ip, uint16_t port, const char *name);
bool nettest_hin_connect_done(int *rc);
struct socket *nettest_hin_connect_join(void);

/* nettest.c: a pcb's retransmit count, read atomically (nettest_hoststate.cee). */
uint64_t nettest_tcp_retransmits(struct tcp_pcb *pcb);

/* nettest_ctl.cee: where a /dev/net/tapctl snapshot with `pf_rules`
 * forwards has its flow section, and the byte length it should have (-1:
 * a section of another version). */
size_t nettest_netctl_flows_off(const uint8_t *buf, unsigned pf_rules);
int64_t nettest_netctl_snapshot_len(const uint8_t *buf, unsigned pf_rules);

#endif
