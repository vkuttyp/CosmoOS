/*
 * nettest.c - Network self-tests: mbufs, checksums, ARP table, UDP and
 * TCP over loopback (with and without injected loss), and the
 * harness-driven echo services over the real interface.
 */

#include <kernel/errno.h>
#include <kernel/fwcfg.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

static struct netaddr v4addr(uint32_t ip, uint16_t port)
{
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.v4 = ip;
    a.port = port;
    return a;
}

static struct netaddr v6loop(uint16_t port)
{
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET6;
    a.v6.s6_addr[15] = 1;
    a.port = port;
    return a;
}

/* --- mbufs ------------------------------------------------------------- */

bool selftest_net_mbuf(const char **reason)
{
    struct mbuf_stats s0, s1;
    mbuf_get_stats(&s0);

    struct mbuf *m = m_getcl();
    CHECK(m != NULL && (m->flags & (M_PKTHDR | M_EXT)) == (M_PKTHDR | M_EXT));
    CHECK(m_leadingspace(m) == NET_HEADROOM && m->len == 0);
    uint8_t pat[3000];
    for (unsigned i = 0; i < sizeof(pat); i++)
        pat[i] = (uint8_t)(i * 13);
    CHECK(m_append(m, pat, sizeof(pat)) == 0);
    CHECK(m->pkt.len == 3000 && m_length(m) == 3000 && m->next != NULL);   /* two clusters */
    uint8_t out[3000];
    CHECK(m_copydata(m, 0, 3000, out) && memcmp(out, pat, 3000) == 0);
    CHECK(m_copydata(m, 2990, 10, out) && memcmp(out, pat + 2990, 10) == 0);
    CHECK(!m_copydata(m, 2990, 11, out));

    /* prepend within headroom, then beyond it. */
    m = m_prepend(m, 14);
    CHECK(m != NULL && m->pkt.len == 3014 && m_leadingspace(m) == NET_HEADROOM - 14);
    m = m_prepend(m, 100);
    CHECK(m != NULL && m->pkt.len == 3114 && m->len == 100);   /* new leading buffer */
    m_adj(m, 114);
    CHECK(m->pkt.len == 3000 && m_copydata(m, 0, 3000, out) && memcmp(out, pat, 3000) == 0);

    /* pullup across buffers. */
    m = m_pullup(m, 2000);   /* the first buffer holds 1984: this crosses into the second */
    CHECK(m != NULL && m->len >= 2000 && memcmp(m->data, pat, 2000) == 0 && m_length(m) == 3000);
    CHECK(m_pullup(m_get(), 2049) == NULL);   /* beyond one cluster is refused */
    m_adj(m, -1000);
    CHECK(m->pkt.len == 2000 && m_length(m) == 2000);

    /* shared clusters survive the original's free. */
    struct mbuf *r = m_ref(m);
    CHECK(r != NULL && r->data == m->data);
    m_freem(m);
    CHECK(r->data[0] == pat[0]);
    m_freem(r);

    struct mbuf *lin = m_getcl();
    CHECK(lin != NULL && m_append(lin, pat, 1500) == 0);
    struct mbuf *copy = m_copypacket(lin);
    CHECK(copy != NULL && copy->len == 1500 && memcmp(copy->data, pat, 1500) == 0);
    m_freem(lin);
    m_freem(copy);

    struct mbufq q;
    mbufq_init(&q, 2, "test");
    CHECK(mbufq_enqueue(&q, m_get()) && mbufq_enqueue(&q, m_get()) && !mbufq_enqueue(&q, m_get()));
    CHECK(mbufq_len(&q) == 2);
    mbufq_drain(&q);
    CHECK(mbufq_dequeue(&q) == NULL);

    mbuf_get_stats(&s1);
    CHECK(s1.mbufs_alive == s0.mbufs_alive && s1.clusters_alive == s0.clusters_alive);
    return true;
}

/* --- checksum ---------------------------------------------------------------- */

bool selftest_net_cksum(const char **reason)
{
    /* RFC 1071 example: 0001 f203 f4f5 f6f7 -> ~sum = 220d. */
    static const uint8_t ex[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    CHECK(in_cksum(ex, sizeof(ex)) == htons(0x220d));
    /* Odd length and a verified checksum folds to zero. */
    uint8_t buf[11] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    uint16_t c = in_cksum(buf, 9);
    CHECK(cksum_fold(cksum_partial(&c, 2, cksum_partial(buf, 9, 0))) == 0);
    /* The chain sum equals the flat sum across odd buffer boundaries. */
    struct mbuf *m = m_getcl();
    CHECK(m != NULL);
    uint8_t data[1000];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7 + 1);
    m->len = m->pkt.len = 0;
    CHECK(m_append(m, data, 333) == 0);
    struct mbuf *t = m_get();
    CHECK(t != NULL);
    memcpy(t->data, data + 333, 100);
    t->len = 100;
    m->next = t;
    m->pkt.len = 433;
    CHECK(m_append(m, data + 433, 567) == 0);
    CHECK(m_length(m) == 1000);
    CHECK(cksum_fold(m_cksum_partial(m, 0, 1000, 0)) == in_cksum(data, 1000));
    CHECK(cksum_fold(m_cksum_partial(m, 7, 900, 0)) == in_cksum(data + 7, 900));
    m_freem(m);
    return true;
}

/* --- ARP table ------------------------------------------------------------------ */

bool selftest_net_arp(const char **reason)
{
    uint8_t mac[6];
    uint32_t ip = IPV4_ADDR(10, 99, 0, 7);
    CHECK(!arp_lookup(ip, mac));
    struct netif *nif = netif_default();
    if (nif == NULL) {
        kinfo("selftest: net-arp: no ethernet interface; table logic only");
        return true;
    }
    struct arp_stats s0, s1;
    arp_get_stats(&s0);
    struct mbuf *m = m_getcl();
    CHECK(m != NULL);
    m->len = m->pkt.len = 20;
    /* Unknown host: request sent, packet parked, then aged out. */
    CHECK(arp_resolve(nif, ip, mac, m) == -EINPROGRESS);
    arp_get_stats(&s1);
    CHECK(s1.requests_sent == s0.requests_sent + 1 && s1.entries == s0.entries + 1);
    uint64_t now = clock_now_ns();
    arp_age(now + 2ull * 1000000000ull);   /* second try */
    arp_age(now + 4ull * 1000000000ull);   /* third try */
    arp_age(now + 6ull * 1000000000ull);   /* give up: pending packet freed */
    arp_get_stats(&s1);
    CHECK(s1.timeouts == s0.timeouts + 1 && s1.entries == s0.entries && s1.pending_dropped == s0.pending_dropped + 1);
    CHECK(!arp_lookup(ip, mac));

    /* Admission: an unsolicited reply teaches nothing; a request addressed
     * to us records the asker. Frames are handed straight to arp_input. */
    static const uint8_t forged_mac[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };
    uint32_t liar = IPV4_ADDR(10, 99, 0, 8), asker = IPV4_ADDR(10, 99, 0, 9);
    struct mbuf *f = m_getcl();
    CHECK(f != NULL);
    f->len = f->pkt.len = 28;
    memset(f->data, 0, 28);
    f->data[1] = 1;   /* Ethernet */
    f->data[2] = 0x08; /* IPv4 */
    f->data[4] = 6;
    f->data[5] = 4;
    f->data[7] = 2;   /* reply */
    memcpy(f->data + 8, forged_mac, 6);
    memcpy(f->data + 14, &liar, 4);
    memcpy(f->data + 18, nif->mac, 6);
    memcpy(f->data + 24, &nif->ip4.addr, 4);
    arp_input(nif, f);
    CHECK(!arp_lookup(liar, mac));
    arp_get_stats(&s1);
    CHECK(s1.unsolicited == s0.unsolicited + 1 && s1.entries == s0.entries);
    f = m_getcl();
    CHECK(f != NULL);
    f->len = f->pkt.len = 28;
    memset(f->data, 0, 28);
    f->data[1] = 1;
    f->data[2] = 0x08;
    f->data[4] = 6;
    f->data[5] = 4;
    f->data[7] = 1;   /* request */
    memcpy(f->data + 8, forged_mac, 6);
    memcpy(f->data + 14, &asker, 4);
    memcpy(f->data + 24, &nif->ip4.addr, 4);
    arp_input(nif, f);
    CHECK(arp_lookup(asker, mac) && memcmp(mac, forged_mac, 6) == 0);
    arp_get_stats(&s1);
    CHECK(s1.replies_sent == s0.replies_sent + 1 && s1.entries == s0.entries + 1);
    arp_flush(nif);   /* the test's entries; a real gateway entry is re-learned below */
    /* The gateway resolves for real when a NIC is present (asynchronous). */
    if (nif->ip4.gateway) {
        struct mbuf *probe = m_getcl();
        CHECK(probe != NULL);
        probe->len = probe->pkt.len = 20;
        int rc = arp_resolve(nif, nif->ip4.gateway, mac, probe);
        CHECK(rc == 0 || rc == -EINPROGRESS);
        for (unsigned i = 0; i < 50 && !arp_lookup(nif->ip4.gateway, mac); i++)
            thread_sleep_ms(10);
        if (arp_lookup(nif->ip4.gateway, mac))
            kinfo("selftest: net-arp: gateway is %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
        else {
            struct arp_stats as;
            arp_get_stats(&as);
            kinfo("selftest: net-arp: gateway did not answer (requests %llu, replies %llu)",
                  (unsigned long long)as.requests_sent, (unsigned long long)as.replies_rcvd);
            netif_dump();
        }
    }
    return true;
}

/* --- UDP over loopback ------------------------------------------------------------ */

static bool udp_roundtrip(const char **reason, struct netaddr srv_addr, struct netaddr cli_addr)
{
    struct socket *srv, *cli;
    CHECK(ksock_create(srv_addr.family, COSMO_SOCK_DGRAM, 0, &srv) == 0);
    CHECK(ksock_create(cli_addr.family, COSMO_SOCK_DGRAM, 0, &cli) == 0);
    CHECK(ksock_bind(srv, &srv_addr) == 0);
    CHECK(ksock_bind(srv, &srv_addr) == -EINVAL);   /* twice */
    struct socket *dup;
    CHECK(ksock_create(srv_addr.family, COSMO_SOCK_DGRAM, 0, &dup) == 0);
    CHECK(ksock_bind(dup, &srv_addr) == -EADDRINUSE);
    ksock_put(dup);
    struct netaddr srv_name;
    CHECK(ksock_getsockname(srv, &srv_name) == 0 && srv_name.port == srv_addr.port);

    char msg[1400];
    for (unsigned i = 0; i < sizeof(msg); i++)
        msg[i] = (char)('a' + i % 26);
    CHECK(ksock_sendto(cli, msg, 5, &srv_addr) == 5);
    CHECK(ksock_sendto(cli, msg, sizeof(msg), &srv_addr) == (int64_t)sizeof(msg));
    char buf[1500];
    struct netaddr from;
    CHECK(ksock_recvfrom(srv, buf, sizeof(buf), &from) == 5 && memcmp(buf, msg, 5) == 0);
    CHECK(from.family == cli_addr.family && from.port >= NET_EPHEMERAL_LO);
    CHECK(ksock_recvfrom(srv, buf, sizeof(buf), &from) == (int64_t)sizeof(msg) && memcmp(buf, msg, sizeof(msg)) == 0);
    /* Reply to the sender's ephemeral port. */
    CHECK(ksock_sendto(srv, "pong", 4, &from) == 4);
    CHECK(ksock_recvfrom(cli, buf, sizeof(buf), NULL) == 4 && memcmp(buf, "pong", 4) == 0);
    /* Truncation on a short buffer. */
    CHECK(ksock_sendto(cli, msg, 100, &srv_addr) == 100);
    CHECK(ksock_recvfrom(srv, buf, 10, NULL) == 10);
    /* Errors. */
    struct netaddr bad = srv_addr;
    bad.port = 0;
    CHECK(ksock_sendto(cli, msg, 1, &bad) == -EINVAL);
    CHECK(ksock_sendto(cli, msg, 70000, &srv_addr) == -EMSGSIZE || sizeof(msg) < 70000);
    struct socket *unbound;
    CHECK(ksock_create(srv_addr.family, COSMO_SOCK_DGRAM, 0, &unbound) == 0);
    CHECK(ksock_recvfrom(unbound, buf, 10, NULL) == -EINVAL);
    ksock_put(unbound);
    /* A datagram to a closed port is dropped (v4 replies with ICMP). */
    struct netaddr closed = srv_addr;
    closed.port = 9;
    CHECK(ksock_sendto(cli, msg, 3, &closed) == 3);
    thread_sleep_ms(20);
    ksock_put(cli);
    ksock_put(srv);
    return true;
}

bool selftest_net_lo_udp(const char **reason)
{
    unsigned socks0 = socket_count();
    struct udp_stats u0, u1;
    udp_get_stats(&u0);
    if (!udp_roundtrip(reason, v4addr(INADDR_LOOPBACK_N, 5000), v4addr(0, 0)))
        return false;
    if (!udp_roundtrip(reason, v6loop(5001), v6loop(0)))
        return false;
    /* A privileged port needs uid 0. */
    struct socket *s;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 1000, &s) == 0);
    struct netaddr low = v4addr(0, 80);
    CHECK(ksock_bind(s, &low) == -EPERM);
    ksock_put(s);
    udp_get_stats(&u1);
    CHECK(u1.rx_no_port >= u0.rx_no_port + 2 && u1.rx_bad_cksum == u0.rx_bad_cksum);
    thread_sleep_ms(10);
    CHECK(socket_count() == socks0);
    return true;
}

/* --- TCP over loopback -------------------------------------------------------------- */

#define TCP_TEST_BYTES (1024u * 1024u)

struct tcp_server {
    struct netaddr addr;
    uint32_t bytes_seen;
    uint32_t bytes_echoed;
    int result;
    bool done;
    bool echo;               /* echo mode for the harness */
    volatile bool stop;
};

static uint8_t pattern(uint32_t i)
{
    return (uint8_t)((i * 2654435761u) >> 24);
}

/* Accept one connection, verify the pattern stream, send the count back. */
static void tcp_sink_thread(void *arg)
{
    struct tcp_server *srv = arg;
    struct socket *ls, *c;
    srv->result = ksock_create(srv->addr.family, COSMO_SOCK_STREAM, 0, &ls);
    if (srv->result)
        goto done;
    srv->result = ksock_bind(ls, &srv->addr);
    if (srv->result == 0)
        srv->result = ksock_listen(ls, 4);
    if (srv->result == 0)
        srv->result = ksock_accept(ls, &c, NULL);
    if (srv->result) {
        ksock_put(ls);
        goto done;
    }
    uint8_t *buf = kmalloc(8192, 0);
    for (;;) {
        int64_t n = ksock_recvfrom(c, buf, 8192, NULL);
        if (n <= 0) {
            if (n < 0)
                srv->result = (int)n;
            break;
        }
        for (int64_t i = 0; i < n; i++) {
            if (buf[i] != pattern(srv->bytes_seen + (uint32_t)i)) {
                srv->result = -EIO;
                break;
            }
        }
        srv->bytes_seen += (uint32_t)n;
    }
    uint32_t total = srv->bytes_seen;
    ksock_sendto(c, &total, sizeof(total), NULL);
    ksock_shutdown(c, COSMO_SHUT_WR);
    /* Wait for the peer's close so both sides run the full sequence. */
    ksock_recvfrom(c, buf, 16, NULL);
    kfree(buf);
    ksock_put(c);
    ksock_put(ls);
done:
    srv->done = true;
    thread_exit(0);
}

static bool tcp_transfer(const char **reason, struct netaddr addr, uint32_t bytes, unsigned linger_ms)
{
    struct tcp_server srv;
    memset(&srv, 0, sizeof(srv));
    srv.addr = addr;
    struct thread *t = thread_create(tcp_sink_thread, &srv, "tcp-sink", 32);
    CHECK(t != NULL);
    thread_sleep_ms(20);   /* let it listen */

    struct socket *c;
    CHECK(ksock_create(addr.family, COSMO_SOCK_STREAM, 0, &c) == 0);
    CHECK(ksock_connect(c, &addr) == 0);
    struct netaddr peer, me;
    CHECK(ksock_getpeername(c, &peer) == 0 && peer.port == addr.port);
    CHECK(ksock_getsockname(c, &me) == 0 && me.port >= NET_EPHEMERAL_LO);
    CHECK(ksock_connect(c, &addr) == -EISCONN);

    uint8_t *chunk = kmalloc(9000, 0);
    CHECK(chunk != NULL);
    uint32_t sent = 0;
    unsigned step = 1;
    while (sent < bytes) {
        uint32_t n = (step * 613u) % 9000u + 1;
        if (n > bytes - sent)
            n = bytes - sent;
        for (uint32_t i = 0; i < n; i++)
            chunk[i] = pattern(sent + i);
        int64_t w = ksock_sendto(c, chunk, n, NULL);
        if (w != (int64_t)n) {
            kfree(chunk);
            *reason = "tcp send failed";
            return false;
        }
        sent += n;
        step++;
    }
    kfree(chunk);
    CHECK(ksock_shutdown(c, COSMO_SHUT_WR) == 0);
    uint32_t total = 0;
    uint8_t tmp[8];
    int64_t got = 0;
    while (got < 4) {
        int64_t n = ksock_recvfrom(c, tmp + got, 4 - (size_t)got, NULL);
        if (n <= 0)
            break;
        got += n;
    }
    memcpy(&total, tmp, 4);
    CHECK(got == 4 && total == bytes);
    CHECK(ksock_recvfrom(c, tmp, 8, NULL) == 0);   /* EOF */
    CHECK(ksock_sendto(c, "x", 1, NULL) == -EPIPE);
    /* Keep the socket past TIME_WAIT: the pcb must stay valid until close. */
    for (unsigned i = 0; i < linger_ms; i += 100) {
        thread_sleep_ms(100);
        sched_watchdog_kick();
    }
    if (linger_ms) {
        CHECK(ksock_getsockname(c, &me) == 0 && me.port >= NET_EPHEMERAL_LO);
        CHECK(ksock_recvfrom(c, tmp, 8, NULL) == 0);
        CHECK(ksock_sendto(c, "x", 1, NULL) == -EPIPE);
    }
    ksock_put(c);
    for (unsigned i = 0; i < 500 && !srv.done; i++) {
        thread_sleep_ms(10);
        sched_watchdog_kick();
    }
    CHECK(srv.done && srv.result == 0 && srv.bytes_seen == bytes);
    return true;
}

bool selftest_net_lo_tcp(const char **reason)
{
    unsigned socks0 = socket_count();
    struct tcp_stats t0, t1;
    tcp_get_stats(&t0);

    /* A connection to a closed port is refused with a reset. */
    struct socket *c;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    struct netaddr closed = v4addr(INADDR_LOOPBACK_N, 5999);
    CHECK(ksock_connect(c, &closed) == -ECONNREFUSED);
    ksock_put(c);
    tcp_get_stats(&t1);
    CHECK(t1.rsts_in == t0.rsts_in + 1);

    if (!tcp_transfer(reason, v4addr(INADDR_LOOPBACK_N, 6000), TCP_TEST_BYTES, 0))
        return false;
    if (!tcp_transfer(reason, v6loop(6001), 256u * 1024u, 2500))
        return false;

    /* Listen backlog: a listener that never accepts still completes the
     * handshake for `backlog` clients; the next SYN is ignored (the client
     * times out, so only check the queue fills). */
    struct socket *ls;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0);
    struct netaddr la = v4addr(INADDR_LOOPBACK_N, 6002);
    CHECK(ksock_bind(ls, &la) == 0 && ksock_listen(ls, 2) == 0);
    CHECK(ksock_listen(ls, 2) == -EINVAL);
    struct socket *c1, *c2;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c1) == 0 && ksock_connect(c1, &la) == 0);
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c2) == 0 && ksock_connect(c2, &la) == 0);
    struct socket *a1;
    struct netaddr peer;
    CHECK(ksock_accept(ls, &a1, &peer) == 0 && peer.port >= NET_EPHEMERAL_LO);
    CHECK(ksock_sendto(a1, "hi", 2, NULL) == 2);
    char b[4];
    CHECK(ksock_recvfrom(c1, b, 4, NULL) == 2 && memcmp(b, "hi", 2) == 0);
    /* Closing the listener resets the still-queued connection. */
    ksock_put(ls);
    thread_sleep_ms(20);
    CHECK(ksock_recvfrom(c2, b, 4, NULL) < 0 || ksock_sendto(c2, "x", 1, NULL) < 0);
    ksock_put(a1);
    ksock_put(c1);
    ksock_put(c2);

    thread_sleep_ms(50);
    CHECK(socket_count() == socks0);
    tcp_get_stats(&t1);
    CHECK(t1.conns_established >= t0.conns_established + 4 && t1.bad_cksum == t0.bad_cksum);
    kinfo("selftest: net-lo-tcp: %llu segments, %llu retransmits", (unsigned long long)(t1.segs_out - t0.segs_out),
          (unsigned long long)(t1.retransmits - t0.retransmits));
    return true;
}

/* Loss injection: drop every `drop_every`th TCP data segment. */
static unsigned g_seen, g_drop_every, g_dropped;

static bool lossy_filter(struct mbuf *m, void *arg)
{
    (void)arg;
    if (m->pkt.proto != ETH_P_IP || m->pkt.len < 40)
        return true;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)m->data;
    if (ip->proto != IPPROTO_TCP)
        return true;
    unsigned ihl = IPV4_HDR_LEN(ip);
    uint16_t total = ntohs(ip->len);
    if (total <= ihl + 20)
        return true;   /* keep pure ACKs, SYNs and FINs */
    if (++g_seen % g_drop_every == 0) {
        g_dropped++;
        return false;
    }
    return true;
}

bool selftest_net_lo_tcp_loss(const char **reason)
{
    struct tcp_stats t0, t1;
    tcp_get_stats(&t0);
    g_seen = g_dropped = 0;
    g_drop_every = 7;
    loopback_set_filter(lossy_filter, NULL);
    bool ok = tcp_transfer(reason, v4addr(INADDR_LOOPBACK_N, 6010), 256u * 1024u, 0);
    loopback_set_filter(NULL, NULL);
    if (!ok)
        return false;
    tcp_get_stats(&t1);
    CHECK(g_dropped > 0);
    CHECK(t1.retransmits > t0.retransmits);
    kinfo("selftest: net-lo-tcp-loss: dropped %u data segments, %llu retransmissions", g_dropped,
          (unsigned long long)(t1.retransmits - t0.retransmits));
    return true;
}

/* --- harness-driven echo over the real interface --------------------------------------- */

static volatile bool g_h_quit, g_h_stop;
static volatile int g_h_tcp_conns, g_h_udp_pkts;

static void h_tcp_echo_thread(void *arg)
{
    struct socket *ls = arg;
    for (;;) {
        struct socket *c;
        if (ksock_accept(ls, &c, NULL) != 0)
            break;
        g_h_tcp_conns++;
        uint8_t *buf = kmalloc(8192, 0);
        bool quit = false;
        for (;;) {
            int64_t n = ksock_recvfrom(c, buf, 8192, NULL);
            if (n <= 0)
                break;
            if (n >= 4 && memcmp(buf, "QUIT", 4) == 0) {
                quit = true;
                break;
            }
            if (ksock_sendto(c, buf, (size_t)n, NULL) != n)
                break;
        }
        kfree(buf);
        ksock_put(c);
        if (quit) {
            g_h_quit = true;
            break;
        }
    }
    ksock_put(ls);
    thread_exit(0);
}

static void h_udp_echo_thread(void *arg)
{
    struct socket *s = arg;
    uint8_t *buf = kmalloc(2048, 0);
    while (!g_h_stop) {
        struct netaddr from;
        int64_t n = ksock_recvfrom(s, buf, 2048, &from);
        if (n <= 0)
            break;
        if (g_h_stop)
            break;
        g_h_udp_pkts++;
        ksock_sendto(s, buf, (size_t)n, &from);
    }
    kfree(buf);
    ksock_put(s);
    thread_exit(0);
}

bool selftest_net_harness(const char **reason)
{
    char cfg[64];
    if (!fwcfg_get_string("nettest", cfg, sizeof(cfg))) {
        kinfo("selftest: net-harness: no opt/cosmo/nettest parameter; skipping");
        return true;
    }
    struct netif *nif = netif_default();
    CHECK(nif != NULL && nif->ip4.addr != 0);
    unsigned hostport = 0;
    if (strncmp(cfg, "tcp=", 4) == 0) {
        for (const char *p = cfg + 4; *p >= '0' && *p <= '9'; p++)
            hostport = hostport * 10 + (unsigned)(*p - '0');
    }
    CHECK(hostport > 0 && hostport < 65536);

    /* Echo services on port 7 for the harness's port forwards. */
    struct socket *tls, *us;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &tls) == 0);
    struct netaddr any7 = v4addr(0, 7);
    CHECK(ksock_bind(tls, &any7) == 0 && ksock_listen(tls, 4) == 0);
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &us) == 0);
    CHECK(ksock_bind(us, &any7) == 0);
    g_h_quit = g_h_stop = false;
    g_h_tcp_conns = g_h_udp_pkts = 0;
    ksock_get(tls);
    ksock_get(us);
    CHECK(thread_create(h_tcp_echo_thread, tls, "nettest-tcp", 32) != NULL);
    CHECK(thread_create(h_udp_echo_thread, us, "nettest-udp", 32) != NULL);
    kprintf("NETTEST: ready tcp=7 udp=7\n");

    /* Connect back to the harness through the gateway (QEMU forwards
     * 10.0.2.2 to the host's loopback). */
    bool client_ok = false;
    struct socket *c;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    struct netaddr host = v4addr(nif->ip4.gateway, (uint16_t)hostport);
    int rc = ksock_connect(c, &host);
    if (rc == 0 && ksock_sendto(c, "cosmo hello\n", 12, NULL) == 12) {
        char buf[32];
        int64_t n = ksock_recvfrom(c, buf, sizeof(buf), NULL);
        client_ok = n == 12 && memcmp(buf, "cosmo world\n", 12) == 0;
    }
    ksock_put(c);
    kprintf(client_ok ? "NETTEST: client ok\n" : "NETTEST: client failed (%d)\n", rc);

    /* Serve echo until the harness sends QUIT (60 s budget). */
    for (unsigned i = 0; i < 6000 && !g_h_quit; i++) {
        thread_sleep_ms(10);
        sched_watchdog_kick();
    }
    g_h_stop = true;
    ksock_shutdown(tls, COSMO_SHUT_RD);   /* accept returns */
    struct netaddr self = v4addr(INADDR_LOOPBACK_N, 7);
    ksock_sendto(us, "x", 1, &self);       /* the UDP thread wakes and exits */
    thread_sleep_ms(50);
    kprintf("NETTEST: done tcp_conns=%d udp_pkts=%d quit=%d\n", g_h_tcp_conns, g_h_udp_pkts, g_h_quit ? 1 : 0);
    ksock_put(tls);
    ksock_put(us);
    CHECK(client_ok);
    CHECK(g_h_quit);
    return true;
}
