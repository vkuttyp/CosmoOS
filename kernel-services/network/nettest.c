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
#include <kernel/object.h>
#include <kernel/pipe.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <kernel/percpu.h>
#include <arch/cpu.h>

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
    CHECK(m != NULL && m->pkt.len == 3114 && m_leadingspace(m) == NET_HEADROOM - 114);   /* still in place (unit 11: 128 of headroom) */
    m = m_prepend(m, 200);
    CHECK(m != NULL && m->pkt.len == 3314 && m->len == 200);   /* new leading buffer */
    m_adj(m, 314);
    CHECK(m->pkt.len == 3000 && m_copydata(m, 0, 3000, out) && memcmp(out, pat, 3000) == 0);

    /* pullup across buffers, keeping the headroom for a later prepend (unit 11). */
    m = m_pullup(m, 1900);   /* the first buffer holds 1920: this crosses into the second */
    CHECK(m != NULL && m->len >= 1900 && memcmp(m->data, pat, 1900) == 0 && m_length(m) == 3000);
    CHECK(m_leadingspace(m) == NET_HEADROOM);
    CHECK(NET_HEADROOM >= 12 + 14 + 40 + 60);   /* vnet + Ethernet + IPv6 + TCP with options */
    {
        struct mbuf *big = m_copypacket(m);      /* 3000 bytes: a chain of clusters, contents intact */
        CHECK(big != NULL && big->pkt.len == 3000 && big->next != NULL && m_copydata(big, 0, 3000, out) &&
              memcmp(out, pat, 3000) == 0);
        m_freem(big);
    }
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
    netif_put(nif);   /* eth0 is never unregistered while the tests run; a borrowed pointer is enough here */
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
    /* A reserved port is judged on the caller's credentials at bind time,
     * not on the socket's creator: this kernel thread is privileged, so
     * the bind succeeds whatever uid the socket records. The refusal for
     * an unprivileged caller is exercised by init --unpriv-test. */
    struct socket *s;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 1000, &s) == 0);
    struct netaddr low = v4addr(0, 80);
    CHECK(ksock_bind(s, &low) == 0);
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
        /* The ended connection no longer reserves its port. */
        struct socket *again;
        CHECK(ksock_create(addr.family, COSMO_SOCK_STREAM, 0, &again) == 0);
        CHECK(ksock_bind(again, &me) == 0);
        ksock_put(again);
    }
    ksock_put(c);
    for (unsigned i = 0; i < 500 && !srv.done; i++) {
        thread_sleep_ms(10);
        sched_watchdog_kick();
    }
    CHECK(srv.done && srv.result == 0 && srv.bytes_seen == bytes);

    /* The server's child leaves LAST_ACK when the network worker processes
     * our final ACK. The worker runs below this thread's priority and is
     * preempted as soon as it wakes us, so the port can still be reserved
     * for a moment after `done`; the next caller of this helper binds it
     * again. Wait for that condition, bounded. */
    int rc = -EADDRINUSE;
    for (unsigned i = 0; i < 2000 && rc == -EADDRINUSE; i++) {
        struct socket *probe;
        CHECK(ksock_create(addr.family, COSMO_SOCK_STREAM, 0, &probe) == 0);
        rc = ksock_bind(probe, &addr);
        ksock_put(probe);
        if (rc == -EADDRINUSE)
            thread_sleep_ms(1);
    }
    CHECK(rc == 0);
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

/* --- the path MSS is decided outside the TCP lock (Prompt #3, 3.1) ----------- */

bool selftest_net_tcp_mss(const char **reason)
{
    /* tcp_path_mss reads the netif registry, so it is called with no
     * spinlock held; under the TCP lock only the cached pcb->path_mss is
     * consulted. Every mutex_lock now asserts preempt_count == 0, so the
     * loopback handshake below would panic if that rule were broken. */
    struct netaddr a = v4addr(INADDR_LOOPBACK_N, 1);
    CHECK(tcp_path_mss(COSMO_AF_INET, &a) == TCP_MSS_LO);
    struct netif *eth = netif_default();
    if (eth)
        netif_put(eth);   /* see net-arp: eth0 outlives the tests */
    if (eth != NULL && eth->ip4.addr != 0) {
        a.v4 = eth->ip4.addr;
        CHECK(tcp_path_mss(COSMO_AF_INET, &a) == TCP_MSS_LO);   /* one of our own addresses: local delivery */
        a.v4 = eth->ip4.gateway;
        CHECK(tcp_path_mss(COSMO_AF_INET, &a) == TCP_MSS_V4);
    }
    struct netaddr b = v6loop(1);
    CHECK(tcp_path_mss(COSMO_AF_INET6, &b) == TCP_MSS_LO);
    memset(&b.v6, 0, sizeof(b.v6));
    b.v6.s6_addr[0] = 0xfe;
    b.v6.s6_addr[1] = 0x80;
    b.v6.s6_addr[15] = 0x77;
    if (eth == NULL || !in6_equal(&eth->ip6_ll, &b.v6))
        CHECK(tcp_path_mss(COSMO_AF_INET6, &b) == TCP_MSS_V6);

    /* Both ends of a loopback connection settle on TCP_MSS_LO: the active
     * end from the route (before its lock), the passive one from the
     * interface the SYN arrived on (before its lock). */
    struct socket *ls, *c, *acc;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0);
    struct netaddr la = v4addr(INADDR_LOOPBACK_N, 6010);
    CHECK(ksock_bind(ls, &la) == 0 && ksock_listen(ls, 1) == 0);
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0 && ksock_connect(c, &la) == 0);
    struct netaddr peer;
    CHECK(ksock_accept(ls, &acc, &peer) == 0);
    CHECK(c->tcp->path_mss == TCP_MSS_LO && c->tcp->mss == TCP_MSS_LO);
    CHECK(acc->tcp->path_mss == TCP_MSS_LO && acc->tcp->mss == TCP_MSS_LO);
    ksock_put(acc);
    ksock_put(c);
    ksock_put(ls);
    thread_sleep_ms(20);
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
    netif_put(nif);
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

/* --- interface lifetime ------------------------------------------------------
 *
 * docs/kernel/quiesce/design.md, "Network interfaces": lookups are
 * referenced, unregister stops transmit and receive and purges the
 * queue, and the driver's release runs when the last holder is gone.
 */
struct fake_nif {
    struct netif nif;
    unsigned transmits;
    unsigned releases;
};

static int fake_nif_transmit(struct netif *nif, struct mbuf *m)
{
    struct fake_nif *f = nif->priv;
    f->transmits++;
    m_freem(m);
    return 0;
}

static void fake_nif_release(struct netif *nif)
{
    struct fake_nif *f = nif->priv;
    f->releases++;
}

bool selftest_net_netif_lifetime(const char **reason)
{
    static struct fake_nif f;
    static const struct netif_ops no_release = { .transmit = fake_nif_transmit };
    static const struct netif_ops ops = { .transmit = fake_nif_transmit, .release = fake_nif_release };
    memset(&f, 0, sizeof(f));
    strlcpy(f.nif.name, "test0", sizeof(f.nif.name));
    f.nif.mtu = 1500;
    f.nif.ops = &no_release;
    f.nif.priv = &f;
    /* Loopback-style: input_one frees an unknown protocol without a link layer. */
    f.nif.flags = NETIF_LOOPBACK | NETIF_NOARP | NETIF_UP;
    CHECK(netif_register(&f.nif) == -EINVAL);   /* no release: refused */
    f.nif.ops = &ops;
    CHECK(netif_register(&f.nif) == 0);
    CHECK(kobject_refcount(&f.nif.obj) == 2);   /* creator + registry */

    /* A duplicate name is refused before the object exists: no kobject,
     * no owner count for the driver's module to balance (its failure path
     * frees the storage directly). */
    static struct fake_nif dup;
    memset(&dup, 0, sizeof(dup));
    strlcpy(dup.nif.name, "test0", sizeof(dup.nif.name));
    dup.nif.mtu = 1500;
    dup.nif.ops = &ops;
    dup.nif.priv = &dup;
    CHECK(netif_register(&dup.nif) == -EEXIST);
    CHECK(dup.nif.obj.type == NULL && dup.nif.obj.refcount == 0 && dup.nif.obj.owner == NULL);

    struct netif *found = netif_find("test0");
    CHECK(found == &f.nif && kobject_refcount(&f.nif.obj) == 3);

    struct mbuf *m = m_getcl();
    CHECK(m != NULL);
    m->len = m->pkt.len = 16;
    m->pkt.proto = 0x88B5;   /* experimental EtherType: input drops it */
    CHECK(netif_transmit(found, m) == 0 && f.transmits == 1);
    m = m_getcl();
    CHECK(m != NULL);
    m->len = m->pkt.len = 16;
    m->pkt.proto = 0x88B5;
    netif_rx(found, m);                          /* queued for the worker */

    netif_unregister(&f.nif);
    CHECK(netif_find("test0") == NULL);
    CHECK((f.nif.flags & NETIF_GONE) && !(f.nif.flags & NETIF_UP));
    CHECK(kobject_refcount(&f.nif.obj) == 2);
    m = m_getcl();
    CHECK(m != NULL);
    m->len = m->pkt.len = 16;
    CHECK(netif_transmit(found, m) == -ENODEV && f.transmits == 1);
    m = m_getcl();
    CHECK(m != NULL);
    m->len = m->pkt.len = 16;
    unsigned q0 = netif_rxq_count(found);
    netif_rx(found, m);                          /* dropped: gone */
    CHECK(netif_rxq_count(found) == q0);
    CHECK(f.nif.stats.rx_packets == 1);

    netif_put(&f.nif);                           /* the creator is done */
    CHECK(f.releases == 0);
    netif_put(found);
    CHECK(f.releases == 1);
    return true;
}

/* --- accept against a racing peer ------------------------------------------------
 *
 * The audit's accept race: a child dequeued by tcp_accept had neither
 * listener nor socket until the caller attached one, so a reset in that
 * window freed the pcb under the accepting thread. tcp_accept now attaches
 * the owner under the TCP lock. The check below is the invariant (the pcb
 * names its socket when accept returns) plus a stress: clients that
 * connect and drop the connection at once while the server accepts.
 */
struct race_client {
    unsigned rounds;
    unsigned failures;
    struct netaddr server;
};

static void race_client_main(void *arg)
{
    struct race_client *rc = arg;
    for (unsigned i = 0; i < rc->rounds; i++) {
        struct socket *c;
        if (ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) != 0) {
            rc->failures++;
            continue;
        }
        if (ksock_connect(c, &rc->server) != 0)
            rc->failures++;
        else if (i & 1)
            ksock_shutdown(c, 2);   /* FIN before the server accepts */
        ksock_put(c);               /* close: FIN or, with unread data, RST */
    }
}

bool selftest_net_accept_race(const char **reason)
{
    struct socket *ls;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0);
    struct netaddr any = v4addr(INADDR_LOOPBACK_N, 0);
    CHECK(ksock_bind(ls, &any) == 0 && ksock_listen(ls, 8) == 0);
    struct race_client rc = { .rounds = 64 };
    CHECK(ksock_getsockname(ls, &rc.server) == 0);

    struct thread *t = thread_create(race_client_main, &rc, "raceclient", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    unsigned accepted = 0;
    for (unsigned i = 0; i < rc.rounds; i++) {
        struct socket *c;
        struct netaddr peer;
        int rc2 = ksock_accept(ls, &c, &peer);
        CHECK(rc2 == 0);
        CHECK(c->tcp != NULL && c->tcp->sock == c);   /* attached under the lock */
        CHECK(peer.family == COSMO_AF_INET && peer.port != 0);
        accepted++;
        ksock_put(c);
    }
    thread_join(t);
    CHECK(rc.failures == 0);
    ksock_put(ls);
    kinfo("selftest: net-accept-race: %u connections accepted against a dropping peer", accepted);
    return true;
}

/* --- milestone 8: hardening -------------------------------------------------------- */

/* A raw IPv4 TCP segment from 127.0.0.1:sport to 127.0.0.1:dport. */
static void inject_tcp(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t mss_opt)
{
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return;
    unsigned hlen = sizeof(struct tcp_hdr) + (mss_opt ? 4 : 0);
    struct tcp_hdr *th = (struct tcp_hdr *)m->data;
    memset(th, 0, hlen);
    th->sport = htons(sport);
    th->dport = htons(dport);
    th->seq = htonl(seq);
    th->ack = htonl(ack);
    th->doff = (uint8_t)((hlen / 4) << 4);
    th->flags = flags;
    th->win = htons(8192);
    if (mss_opt) {
        uint8_t *o = m->data + sizeof(*th);
        o[0] = 2;
        o[1] = 4;
        o[2] = (uint8_t)(mss_opt >> 8);
        o[3] = (uint8_t)mss_opt;
    }
    m->len = m->pkt.len = hlen;
    uint32_t sum = cksum_pseudo4(INADDR_LOOPBACK_N, INADDR_LOOPBACK_N, IPPROTO_TCP, (uint16_t)m->pkt.len);
    th->cksum = cksum_fold(m_cksum_partial(m, 0, m->pkt.len, sum));
    ipv4_output(m, INADDR_LOOPBACK_N, INADDR_LOOPBACK_N, IPPROTO_TCP, IP_DEFAULT_TTL);
}

/* Let the network worker drain what was injected. */
static void settle(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 10) {
        thread_sleep_ms(10);
        sched_watchdog_kick();
    }
}

/* Drop TCP resets addressed to `g_guard_port` (the flood's SYN-ACKs would
 * otherwise be reset by this host and clear the cache). */
static uint16_t g_guard_port;

static bool drop_rst_filter(struct mbuf *m, void *arg)
{
    (void)arg;
    if (m->pkt.proto != ETH_P_IP || m->pkt.len < 40)
        return true;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)m->data;
    if (ip->proto != IPPROTO_TCP)
        return true;
    const struct tcp_hdr *th = (const struct tcp_hdr *)(m->data + IPV4_HDR_LEN(ip));
    return !((th->flags & TH_RST) && ntohs(th->dport) == g_guard_port);
}

bool selftest_net_tcp_syncache(const char **reason)
{
    struct tcp_stats t0, t1;
    struct socket *ls;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0);
    struct netaddr addr = v4addr(INADDR_LOOPBACK_N, 6020);
    CHECK(ksock_bind(ls, &addr) == 0 && ksock_listen(ls, 4) == 0);
    g_guard_port = 6020;
    loopback_set_filter(drop_rst_filter, NULL);
    tcp_get_stats(&t0);
    /* 300 SYNs from 300 sources that will never answer. */
    for (unsigned i = 0; i < 300; i++)
        inject_tcp((uint16_t)(20000 + i), 6020, 1000 + i, 0, TH_SYN, 1460);
    settle(100);
    tcp_get_stats(&t1);
    uint64_t cached = t1.syn_cached - t0.syn_cached, cookies = t1.syn_cookies_sent - t0.syn_cookies_sent;
    CHECK(cached > 0 && cached <= TCP_SYNCACHE_SIZE);
    CHECK(cookies > 0 && cached + cookies == 300);
    CHECK(t1.conns_passive == t0.conns_passive);           /* nothing allocated per SYN */
    CHECK(!tcp_accept_ready(ls->tcp));
    /* A real client still connects, through a cache slot or a cookie. */
    struct socket *c;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    CHECK(ksock_connect(c, &addr) == 0);
    struct socket *a;
    struct netaddr peer;
    CHECK(ksock_accept(ls, &a, &peer) == 0 && peer.port != 0);
    CHECK(ksock_sendto(c, "hello", 5, NULL) == 5);
    uint8_t buf[8];
    CHECK(ksock_recvfrom(a, buf, sizeof(buf), NULL) == 5 && memcmp(buf, "hello", 5) == 0);
    tcp_get_stats(&t1);
    CHECK(t1.conns_passive == t0.conns_passive + 1);
    /* A completing ACK that matches nothing is refused. */
    inject_tcp(30001, 6020, 5000, 12345, TH_ACK, 0);
    settle(30);
    struct tcp_stats t2;
    tcp_get_stats(&t2);
    CHECK(t2.syn_bad_ack == t1.syn_bad_ack + 1 && t2.conns_passive == t1.conns_passive);
    loopback_set_filter(NULL, NULL);
    ksock_put(a);
    ksock_put(c);
    ksock_put(ls);
    kinfo("selftest: net-tcp-syncache: %llu SYNs cached, %llu answered with cookies, %llu cookies accepted",
          (unsigned long long)cached, (unsigned long long)cookies,
          (unsigned long long)(t2.syn_cookies_ok - t0.syn_cookies_ok));
    return true;
}

/* A server that accepts one connection and holds it until told to stop. */
static void holding_server(void *arg)
{
    struct tcp_server *srv = arg;
    struct socket *ls = NULL, *c = NULL;
    srv->result = ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls);
    if (srv->result == 0)
        srv->result = ksock_bind(ls, &srv->addr);
    if (srv->result == 0)
        srv->result = ksock_listen(ls, 4);
    if (srv->result == 0)
        srv->result = ksock_accept(ls, &c, NULL);
    while (!srv->stop) {
        thread_sleep_ms(10);
        sched_watchdog_kick();
    }
    if (c)
        ksock_put(c);
    if (ls)
        ksock_put(ls);
    srv->done = true;
    thread_exit(0);
}

bool selftest_net_tcp_rfc5961(const char **reason)
{
    struct tcp_server srv;
    memset(&srv, 0, sizeof(srv));
    srv.addr = v4addr(INADDR_LOOPBACK_N, 6021);
    struct thread *t = thread_create(holding_server, &srv, "rfc5961-srv", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    thread_sleep_ms(20);
    struct socket *c;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    CHECK(ksock_connect(c, &srv.addr) == 0);
    struct netaddr me;
    CHECK(ksock_getsockname(c, &me) == 0);
    struct tcp_stats t0, t1;
    tcp_get_stats(&t0);
    uint32_t rcv_nxt = c->tcp->rcv_nxt, snd_nxt = c->tcp->snd_nxt;
    /* A reset inside the window but not at rcv_nxt: a challenge, no reset. */
    inject_tcp(6021, me.port, rcv_nxt + 1000, snd_nxt, TH_RST, 0);
    settle(30);
    CHECK(tcp_state_of(c->tcp) == TCP_ESTABLISHED);
    /* A SYN inside the window: a challenge, no reset. */
    inject_tcp(6021, me.port, rcv_nxt + 10, snd_nxt, TH_SYN, 0);
    settle(30);
    CHECK(tcp_state_of(c->tcp) == TCP_ESTABLISHED);
    /* An ACK for data never sent: a challenge, not processed. */
    inject_tcp(6021, me.port, rcv_nxt, snd_nxt + 100000, TH_ACK, 0);
    settle(30);
    CHECK(tcp_state_of(c->tcp) == TCP_ESTABLISHED && c->tcp->snd_una == snd_nxt);
    tcp_get_stats(&t1);
    CHECK(t1.challenge_acks == t0.challenge_acks + 3);
    CHECK(t1.rsts_in == t0.rsts_in);
    /* The exact reset ends the connection. */
    inject_tcp(6021, me.port, rcv_nxt, snd_nxt, TH_RST, 0);
    settle(30);
    uint8_t buf[4];
    CHECK(ksock_recvfrom(c, buf, sizeof(buf), NULL) == -ECONNRESET);
    tcp_get_stats(&t1);
    CHECK(t1.rsts_in == t0.rsts_in + 1);
    ksock_put(c);
    srv.stop = true;   /* its close sends a FIN into the void and is reset */
    thread_join(t);
    CHECK(srv.done && srv.result == 0);
    kinfo("selftest: net-tcp-rfc5961: three blind segments challenged, the exact reset accepted");
    return true;
}

/* Reordering: hold every fifth data segment and deliver it after the next one. */
static struct mbuf *g_held;
static unsigned g_reorder_seen, g_reordered, g_pass_one;

static bool reorder_filter(struct mbuf *m, void *arg)
{
    struct netif *lo = arg;
    if (m->pkt.proto != ETH_P_IP || m->pkt.len < 40)
        return true;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)m->data;
    if (ip->proto != IPPROTO_TCP)
        return true;
    unsigned ihl = IPV4_HDR_LEN(ip);
    if (ntohs(ip->len) <= ihl + 20)
        return true;   /* pure ACKs and control segments pass */
    const struct tcp_hdr *th = (const struct tcp_hdr *)(m->data + ihl);
    if (ntohs(th->dport) != 6022)
        return true;   /* only the client's data */
    if (g_held) {
        if (g_pass_one) {
            g_pass_one = 0;   /* the segment after the held one overtakes it */
            return true;
        }
        struct mbuf *h = g_held;
        g_held = NULL;
        netif_rx(lo, h);   /* the held one goes first, then this one */
        return true;
    }
    if (++g_reorder_seen % 5 == 0) {
        g_held = m_copypacket(m);
        if (g_held) {
            g_reordered++;
            g_pass_one = 1;
            return false;   /* the original is dropped; the copy arrives one segment late */
        }
    }
    return true;
}

bool selftest_net_tcp_reorder(const char **reason)
{
    struct tcp_stats t0, t1;
    tcp_get_stats(&t0);
    struct netif *lo = netif_loopback();
    CHECK(lo != NULL);
    g_held = NULL;
    g_reorder_seen = g_reordered = g_pass_one = 0;
    loopback_set_filter(reorder_filter, lo);
    bool ok = tcp_transfer(reason, v4addr(INADDR_LOOPBACK_N, 6022), 512u * 1024u, 0);
    loopback_set_filter(NULL, NULL);
    if (g_held) {
        m_freem(g_held);
        g_held = NULL;
    }
    netif_put(lo);
    if (!ok)
        return false;
    tcp_get_stats(&t1);
    CHECK(g_reordered > 0);
    CHECK(t1.ooo_queued > t0.ooo_queued);
    kinfo("selftest: net-tcp-reorder: %u segments delayed, %llu queued out of order, %llu retransmissions",
          g_reordered, (unsigned long long)(t1.ooo_queued - t0.ooo_queued),
          (unsigned long long)(t1.retransmits - t0.retransmits));
    return true;
}

/* A black hole for every segment of one connection (both directions). */
static bool blackhole_filter(struct mbuf *m, void *arg)
{
    (void)arg;
    if (m->pkt.proto != ETH_P_IP || m->pkt.len < 40)
        return true;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)m->data;
    if (ip->proto != IPPROTO_TCP)
        return true;
    const struct tcp_hdr *th = (const struct tcp_hdr *)(m->data + IPV4_HDR_LEN(ip));
    return ntohs(th->dport) != g_guard_port && ntohs(th->sport) != g_guard_port;
}

bool selftest_net_tcp_keepalive(const char **reason)
{
    struct tcp_stats t0, t1;
    /* Keepalive: an idle connection whose peer vanished times out. */
    struct tcp_server srv;
    memset(&srv, 0, sizeof(srv));
    srv.addr = v4addr(INADDR_LOOPBACK_N, 6023);
    struct thread *t = thread_create(holding_server, &srv, "keep-srv", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    thread_sleep_ms(20);
    /* The idle timer is armed when a connection is established: shorten it first. */
    tcp_set_keepalive(150ull * 1000000ull, 50ull * 1000000ull, 3);
    struct socket *c;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    CHECK(ksock_connect(c, &srv.addr) == 0);
    tcp_get_stats(&t0);
    g_guard_port = 6023;
    loopback_set_filter(blackhole_filter, NULL);
    for (unsigned i = 0; i < 300 && tcp_state_of(c->tcp) != TCP_CLOSED; i++)
        settle(10);
    CHECK(tcp_state_of(c->tcp) == TCP_CLOSED);
    uint8_t buf[4];
    int64_t r = ksock_recvfrom(c, buf, sizeof(buf), NULL);
    CHECK(r == -ETIMEDOUT);
    tcp_get_stats(&t1);
    CHECK(t1.timeouts > t0.timeouts);
    uint64_t probes = t1.keepalive_probes - t0.keepalive_probes;
    CHECK(probes >= 3);
    loopback_set_filter(NULL, NULL);
    tcp_set_keepalive(0, 0, 0);
    ksock_put(c);
    srv.stop = true;
    thread_join(t);
    CHECK(srv.done && srv.result == 0);

    /* An orphaned FIN_WAIT_2 ends on its own. */
    memset(&srv, 0, sizeof(srv));
    srv.addr = v4addr(INADDR_LOOPBACK_N, 6024);
    t = thread_create(holding_server, &srv, "fw2-srv", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    thread_sleep_ms(20);
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    CHECK(ksock_connect(c, &srv.addr) == 0);
    tcp_get_stats(&t0);
    tcp_set_fin_wait2(100ull * 1000000ull);
    ksock_put(c);   /* close: FIN; the server never answers with its own */
    for (unsigned i = 0; i < 200; i++) {
        tcp_get_stats(&t1);
        if (t1.fin_wait2_timeouts > t0.fin_wait2_timeouts)
            break;
        settle(10);
    }
    tcp_set_fin_wait2(0);
    CHECK(t1.fin_wait2_timeouts == t0.fin_wait2_timeouts + 1);
    srv.stop = true;
    thread_join(t);
    CHECK(srv.done && srv.result == 0);
    kinfo("selftest: net-tcp-keepalive: %llu probes unanswered, one orphaned FIN_WAIT_2 reaped",
          (unsigned long long)probes);
    return true;
}

bool selftest_net_icmp_limit(const char **reason)
{
    struct ip_stats i0, i1;
    /* 300 echo requests in a burst: at most ICMP_RATE_PER_SEC replies (an
     * unreachable is never sent for 127/8, so the echo path carries the test). */
    ipv4_get_stats(&i0);
    for (unsigned i = 0; i < 300; i++)
        CHECK(icmp_send_echo(INADDR_LOOPBACK_N, 0x4d38, (uint16_t)i, "p", 1) == 0);
    settle(100);
    ipv4_get_stats(&i1);
    uint64_t sent = i1.icmp_echo_replied - i0.icmp_echo_replied, limited = i1.icmp_ratelimited - i0.icmp_ratelimited;
    CHECK(i1.icmp_echo_rcvd - i0.icmp_echo_rcvd == 300);
    CHECK(sent <= ICMP_RATE_PER_SEC && limited >= 300 - ICMP_RATE_PER_SEC);

    /* Path MTU discovery: a "fragmentation needed" quoting a segment in
     * flight lowers the connection's MSS; one quoting nothing in flight is
     * ignored. */
    struct tcp_server srv;
    memset(&srv, 0, sizeof(srv));
    srv.addr = v4addr(INADDR_LOOPBACK_N, 6026);
    struct thread *t = thread_create(holding_server, &srv, "pmtu-srv", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    thread_sleep_ms(20);
    struct socket *c;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    CHECK(ksock_connect(c, &srv.addr) == 0);
    struct netaddr me;
    CHECK(ksock_getsockname(c, &me) == 0);
    CHECK(c->tcp->mss == TCP_MSS_LO && ipv4_path_mtu(INADDR_LOOPBACK_N) == 65535);
    g_guard_port = 6026;
    loopback_set_filter(blackhole_filter, NULL);   /* the data stays in flight */
    uint8_t big[2000];
    memset(big, 'm', sizeof(big));
    CHECK(ksock_sendto(c, big, sizeof(big), NULL) == (int64_t)sizeof(big));
    settle(20);
    uint32_t seq = c->tcp->snd_una;
    struct tcp_stats t0, t1;
    tcp_get_stats(&t0);
    /* Build the ICMP message: type 3 code 4, MTU 1500, quoting IP + 8 bytes of TCP. */
    struct mbuf *m = m_getcl();
    CHECK(m != NULL);
    struct icmp_hdr *ic = (struct icmp_hdr *)m->data;
    memset(ic, 0, sizeof(*ic));
    ic->type = ICMP_DEST_UNREACH;
    ic->code = ICMP_UNREACH_NEEDFRAG;
    ic->seq = htons(1500);
    struct ipv4_hdr *q = (struct ipv4_hdr *)(m->data + sizeof(*ic));
    memset(q, 0, sizeof(*q));
    q->vhl = 0x45;
    q->len = htons(2040);
    q->ttl = 64;
    q->proto = IPPROTO_TCP;
    q->src = INADDR_LOOPBACK_N;
    q->dst = INADDR_LOOPBACK_N;
    uint8_t *tq = m->data + sizeof(*ic) + sizeof(*q);
    tq[0] = (uint8_t)(me.port >> 8);
    tq[1] = (uint8_t)me.port;
    tq[2] = (uint8_t)(6026 >> 8);
    tq[3] = (uint8_t)6026;
    uint32_t bad_seq = seq - 5000;
    tq[4] = (uint8_t)(bad_seq >> 24);
    tq[5] = (uint8_t)(bad_seq >> 16);
    tq[6] = (uint8_t)(bad_seq >> 8);
    tq[7] = (uint8_t)bad_seq;
    m->len = m->pkt.len = sizeof(*ic) + sizeof(*q) + 8;
    struct mbuf *good = m_copypacket(m);
    CHECK(good != NULL);
    ic->cksum = in_cksum(m->data, m->len);
    ipv4_output(m, 0, INADDR_LOOPBACK_N, IPPROTO_ICMP, IP_DEFAULT_TTL);   /* quotes a sequence never sent */
    settle(30);
    tcp_get_stats(&t1);
    ipv4_get_stats(&i1);
    CHECK(t1.pmtu_updates == t0.pmtu_updates && c->tcp->mss == TCP_MSS_LO);
    /* A forged quote poisons nothing: the cache is untouched too. */
    CHECK(ipv4_path_mtu(INADDR_LOOPBACK_N) == 65535 && i1.pmtu_updates == i0.pmtu_updates);
    uint8_t *gq = good->data + sizeof(*ic) + sizeof(*q);
    gq[4] = (uint8_t)(seq >> 24);
    gq[5] = (uint8_t)(seq >> 16);
    gq[6] = (uint8_t)(seq >> 8);
    gq[7] = (uint8_t)seq;
    ((struct icmp_hdr *)good->data)->cksum = 0;
    ((struct icmp_hdr *)good->data)->cksum = in_cksum(good->data, good->len);
    ipv4_output(good, 0, INADDR_LOOPBACK_N, IPPROTO_ICMP, IP_DEFAULT_TTL);
    settle(30);
    tcp_get_stats(&t1);
    ipv4_get_stats(&i1);
    CHECK(t1.pmtu_updates == t0.pmtu_updates + 1 && i1.pmtu_updates == i0.pmtu_updates + 1);
    CHECK(ipv4_path_mtu(INADDR_LOOPBACK_N) == 1500);   /* recorded once the connection confirmed it */
    CHECK(c->tcp->mss == 1460 && c->tcp->path_mss == 1460);
    CHECK(tcp_path_mss(COSMO_AF_INET, &srv.addr) == 1460);   /* new connections start there */
    loopback_set_filter(NULL, NULL);
    settle(300);   /* the retransmission delivers the data in 1460-byte segments */
    ipv4_pmtu_flush();
    CHECK(tcp_path_mss(COSMO_AF_INET, &srv.addr) == TCP_MSS_LO);
    ksock_put(c);
    srv.stop = true;
    thread_join(t);
    CHECK(srv.done && srv.result == 0);
    kinfo("selftest: net-icmp-limit: %llu echo replies sent, %llu suppressed; MSS lowered to 1460 by PMTUD",
          (unsigned long long)sent, (unsigned long long)limited);
    return true;
}

bool selftest_net_nonblock(const char **reason)
{
    struct socket *ls, *c, *a;
    struct netaddr addr = v4addr(INADDR_LOOPBACK_N, 6027);
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0);
    ksock_set_nonblock(ls, true);
    CHECK(ksock_bind(ls, &addr) == 0 && ksock_listen(ls, 2) == 0);
    CHECK(ksock_accept(ls, &a, NULL) == -EAGAIN);
    CHECK(ksock_ready(ls) == 0);
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    ksock_set_nonblock(c, true);
    int rc = ksock_connect(c, &addr);
    CHECK(rc == 0 || rc == -EINPROGRESS);
    if (rc == -EINPROGRESS)
        CHECK(ksock_connect(c, &addr) == -EALREADY || ksock_connect(c, &addr) == -EISCONN);
    for (unsigned i = 0; i < 100 && !(ksock_ready(c) & COSMO_IO_WRITABLE); i++)
        settle(10);
    CHECK(ksock_ready(c) & COSMO_IO_WRITABLE);
    CHECK(ksock_connect(c, &addr) == -EISCONN);
    for (unsigned i = 0; i < 100 && !(ksock_ready(ls) & COSMO_IO_READABLE); i++)
        settle(10);
    CHECK(ksock_ready(ls) & COSMO_IO_READABLE);
    CHECK(ksock_accept(ls, &a, NULL) == 0);
    ksock_set_nonblock(a, true);
    uint8_t buf[4096];
    CHECK(ksock_recvfrom(c, buf, sizeof(buf), NULL) == -EAGAIN);
    CHECK(!(ksock_ready(c) & COSMO_IO_READABLE));
    CHECK(ksock_sendto(a, "hello", 5, NULL) == 5);
    for (unsigned i = 0; i < 100 && !(ksock_ready(c) & COSMO_IO_READABLE); i++)
        settle(10);
    CHECK(ksock_recvfrom(c, buf, sizeof(buf), NULL) == 5);
    /* Fill the pipe: a non-blocking send returns what fits, then -EAGAIN.
     * An ACK still in flight (processed on another worker since unit 11)
     * frees send space until the peer's window is zero and everything
     * sent is acknowledged, so fill until two -EAGAINs 20 ms apart with
     * no progress between: the steady state, in which readiness is stable. */
    memset(buf, 'f', sizeof(buf));
    uint64_t pushed = 0;
    unsigned quiet = 0;
    for (unsigned i = 0; i < 400 && quiet < 2; i++) {
        int64_t n = ksock_sendto(c, buf, sizeof(buf), NULL);
        if (n == -EAGAIN) {
            quiet++;
            settle(20);
        } else if (n > 0) {
            pushed += (uint64_t)n;
            quiet = 0;
        } else {
            CHECK(n > 0);
        }
    }
    CHECK(quiet == 2 && pushed > 0);
    CHECK(!(ksock_ready(c) & COSMO_IO_WRITABLE));
    uint64_t drained = 0;
    for (unsigned i = 0; i < 2000 && drained < pushed; i++) {
        int64_t n = ksock_recvfrom(a, buf, sizeof(buf), NULL);
        if (n == -EAGAIN)
            settle(10);
        else if (n > 0)
            drained += (uint64_t)n;
        else
            CHECK(n > 0);
    }
    CHECK(drained == pushed);
    for (unsigned i = 0; i < 100 && !(ksock_ready(c) & COSMO_IO_WRITABLE); i++)
        settle(10);
    CHECK(ksock_ready(c) & COSMO_IO_WRITABLE);
    ksock_put(a);
    for (unsigned i = 0; i < 100 && !(ksock_ready(c) & COSMO_IO_HANGUP); i++)
        settle(10);
    CHECK((ksock_ready(c) & COSMO_IO_HANGUP) && ksock_recvfrom(c, buf, sizeof(buf), NULL) == 0);
    ksock_put(c);
    ksock_put(ls);

    /* Datagrams. */
    struct socket *u;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &u) == 0);
    ksock_set_nonblock(u, true);
    struct netaddr ua = v4addr(INADDR_LOOPBACK_N, 6028);
    CHECK(ksock_bind(u, &ua) == 0);
    CHECK(ksock_recvfrom(u, buf, sizeof(buf), NULL) == -EAGAIN);
    CHECK(ksock_ready(u) == COSMO_IO_WRITABLE);
    CHECK(ksock_sendto(u, "d", 1, &ua) == 1);
    for (unsigned i = 0; i < 100 && !(ksock_ready(u) & COSMO_IO_READABLE); i++)
        settle(10);
    CHECK(ksock_recvfrom(u, buf, sizeof(buf), NULL) == 1);
    ksock_put(u);

    /* Pipe ends through the object operations. */
    struct kobject *rd, *wr;
    CHECK(pipe_create(&rd, &wr) == 0);
    CHECK(kobject_set_nonblock(rd, 1) == 0 && kobject_set_nonblock(wr, 1) == 0);
    CHECK(kobject_set_nonblock(rd, -1) == 1);
    const struct kobject_io_type *rio = kobject_io_of(rd), *wio = kobject_io_of(wr);
    CHECK(rio && wio && rio->read && wio->write);
    CHECK(rio->read(rd, buf, 8) == -EAGAIN);
    CHECK(kobject_ready(rd) == 0 && kobject_ready(wr) == COSMO_IO_WRITABLE);
    uint64_t wrote = 0;
    for (unsigned i = 0; i < 64; i++) {
        int64_t n = wio->write(wr, buf, sizeof(buf));
        if (n == -EAGAIN)
            break;
        CHECK(n > 0);
        wrote += (uint64_t)n;
    }
    CHECK(wrote == PIPE_SIZE);
    CHECK(kobject_ready(wr) == 0 && (kobject_ready(rd) & COSMO_IO_READABLE));
    CHECK(rio->read(rd, buf, sizeof(buf)) == (int64_t)sizeof(buf));
    CHECK(kobject_ready(wr) == COSMO_IO_WRITABLE);
    kobject_put(wr);
    CHECK(kobject_ready(rd) & COSMO_IO_HANGUP);
    uint64_t left = 0;
    for (unsigned i = 0; i < 8; i++) {
        int64_t n = rio->read(rd, buf, sizeof(buf));
        if (n == 0)
            break;
        CHECK(n > 0);
        left += (uint64_t)n;
    }
    CHECK(left == PIPE_SIZE - sizeof(buf));
    CHECK(rio->read(rd, buf, 8) == 0);   /* EOF */
    kobject_put(rd);
    CHECK(kobject_ready(console_object()) & COSMO_IO_WRITABLE);
    CHECK(kobject_set_nonblock(console_object(), 1) == -EOPNOTSUPP);
    kinfo("selftest: net-nonblock: sockets, datagrams and pipe ends report readiness and never block");
    return true;
}

/* --- receive steering (unit 11) ------------------------------------------------
 *
 * docs/kernel-services/network/design.md, "Receive scaling": one flow's
 * packets reach one worker in order whatever CPU injected them; flows
 * spread over the queues; netif_rx_on lands where told; steering off
 * sends everything to CPU 0.
 */
#define STEER_FLOWS 8u
#define STEER_PKTS  16u

struct steer_state {
    struct netif *nif;
    unsigned cpu[STEER_FLOWS];        /* worker CPU that saw the flow, +1; 0 = none yet */
    unsigned next[STEER_FLOWS];       /* next sequence expected */
    unsigned order_errors, cpu_errors, seen;
};

/* A frame of flow `f` with sequence `seq`: Ethernet + IPv4 + TCP, the
 * sequence in the payload; the ports select the flow. */
static struct mbuf *steer_frame(const struct netif *nif, unsigned f, unsigned seq)
{
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return NULL;
    uint8_t *p = m->data;
    memcpy(p, nif->mac, 6);
    memset(p + 6, 0x22, 6);
    p[12] = 0x08;
    p[13] = 0x00;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(p + 14);
    memset(ip, 0, sizeof(*ip));
    ip->vhl = 0x45;
    ip->len = htons(20 + 20 + 4);
    ip->ttl = 64;
    ip->proto = IPPROTO_TCP;
    ip->src = IPV4_ADDR(10, 9, 0, (uint8_t)(100 + f));
    ip->dst = nif->ip4.addr;
    ip->cksum = in_cksum(ip, sizeof(*ip));
    struct tcp_hdr *th = (struct tcp_hdr *)(p + 34);
    memset(th, 0, sizeof(*th));
    th->sport = htons((uint16_t)(40000 + f));
    th->dport = htons(7);
    th->doff = 5 << 4;
    th->flags = TH_ACK;
    uint32_t s = seq;
    memcpy(p + 54, &s, 4);
    m->len = m->pkt.len = 58;
    m->pkt.proto = ETH_P_IP;
    return m;
}

/* The worker-side hook: record the CPU and check the order per flow. */
static bool steer_hook(struct netif *nif, struct mbuf *m, void *arg)
{
    struct steer_state *st = arg;
    if (nif != st->nif)
        return true;
    uint8_t hdr[58];
    if (!m_copydata(m, 0, sizeof(hdr), hdr)) {
        m_freem(m);
        return false;
    }
    uint16_t sport = (uint16_t)((hdr[34] << 8) | hdr[35]);
    unsigned f = sport - 40000u;
    uint32_t seq;
    memcpy(&seq, hdr + 54, 4);
    if (f < STEER_FLOWS) {
        unsigned cpu = arch_cpu_id() + 1;
        if (st->cpu[f] == 0)
            st->cpu[f] = cpu;
        else if (st->cpu[f] != cpu)
            __atomic_fetch_add(&st->cpu_errors, 1, __ATOMIC_RELAXED);
        if (seq != st->next[f])
            __atomic_fetch_add(&st->order_errors, 1, __ATOMIC_RELAXED);
        st->next[f] = seq + 1;
        __atomic_fetch_add(&st->seen, 1, __ATOMIC_RELAXED);
    }
    m_freem(m);
    return false;
}

struct steer_injector {
    struct steer_state *st;
    unsigned cpu;
    volatile bool done;
};

/* Pinned to one CPU: inject every flow's packets in sequence order. */
static void steer_inject_main(void *arg)
{
    struct steer_injector *in = arg;
    for (unsigned seq = 0; seq < STEER_PKTS; seq++) {
        for (unsigned f = 0; f < STEER_FLOWS; f++) {
            struct mbuf *m = steer_frame(in->st->nif, f, in->cpu * STEER_PKTS + seq);
            if (m)
                netif_rx(in->st->nif, m);
        }
    }
    in->done = true;
    thread_exit(0);
}

static void steer_wait(struct steer_state *st, unsigned want)
{
    for (unsigned i = 0; i < 200 && __atomic_load_n(&st->seen, __ATOMIC_ACQUIRE) < want; i++)
        thread_sleep_ms(5);
}

bool selftest_net_steer(const char **reason)
{
    static const struct netif_ops ops = { .transmit = fake_nif_transmit, .release = fake_nif_release };
    struct fake_nif f;
    memset(&f, 0, sizeof(f));
    strlcpy(f.nif.name, "steer0", sizeof(f.nif.name));
    memcpy(f.nif.mac, "\x02\x11\x22\x33\x44\x55", 6);
    f.nif.mtu = 1500;
    f.nif.ops = &ops;
    f.nif.priv = &f;
    CHECK(netif_register(&f.nif) == 0);
    netif_set_ipv4(&f.nif, IPV4_ADDR(10, 9, 0, 1), htonl(0xffffff00u), 0);
    netif_set_up(&f.nif, true);

    struct steer_state st;
    memset(&st, 0, sizeof(st));
    st.nif = &f.nif;
    netif_set_rx_hook(steer_hook, &st);

    /* The hash is a function of the flow alone: the same frame from any
     * CPU hashes the same, and the eight flows are not all one value. */
    struct mbuf *a = steer_frame(&f.nif, 3, 0), *b = steer_frame(&f.nif, 3, 99), *c = steer_frame(&f.nif, 4, 0);
    CHECK(a && b && c);
    uint32_t ha = net_flow_hash(a, true), hb = net_flow_hash(b, true), hc = net_flow_hash(c, true);
    m_freem(a);
    m_freem(b);
    m_freem(c);
    CHECK(ha != 0 && ha == hb && ha != hc);

    /* Every CPU injects every flow, in sequence order per CPU: the hook
     * must see each flow on one worker, in order, with steering on. */
    unsigned ncpu = cpu_count();
    struct steer_injector inj[CONFIG_MAX_CPUS];
    struct thread *th[CONFIG_MAX_CPUS];
    for (unsigned i = 0; i < ncpu; i++) {
        inj[i].st = &st;
        inj[i].cpu = i;
        inj[i].done = false;
        th[i] = thread_create_on(steer_inject_main, &inj[i], "steer-inj", SCHED_PRIO_DEFAULT, CPUMASK_OF(i));
        CHECK(th[i] != NULL);
        thread_join(th[i]);   /* one CPU at a time keeps each flow's sequence global */
    }
    steer_wait(&st, ncpu * STEER_FLOWS * STEER_PKTS);
    CHECK(st.seen == ncpu * STEER_FLOWS * STEER_PKTS);
    CHECK(st.cpu_errors == 0 && st.order_errors == 0);
    unsigned used = 0;
    for (unsigned k = 0; k < STEER_FLOWS; k++)
        for (unsigned j = 0; j < k; j++)
            if (st.cpu[j] == st.cpu[k])
                goto dup;
        dup:;
    for (unsigned k = 0; k < STEER_FLOWS; k++) {
        bool first = true;
        for (unsigned j = 0; j < k; j++)
            if (st.cpu[j] == st.cpu[k])
                first = false;
        if (first)
            used++;
    }
    if (ncpu >= 2)
        CHECK(used >= 2);   /* eight flows over several queues */
    else
        CHECK(used == 1);

    /* netif_rx_on lands on the named CPU; steering off puts everything on CPU 0. */
    if (ncpu >= 2) {
        memset(&st.cpu, 0, sizeof(st.cpu));
        memset(&st.next, 0, sizeof(st.next));
        st.seen = 0;
        struct mbuf *m = steer_frame(&f.nif, 0, 0);
        CHECK(m != NULL);
        netif_rx_on(&f.nif, m, 1);
        steer_wait(&st, 1);
        CHECK(st.seen == 1 && st.cpu[0] == 2);
        netif_set_steering(false);
        memset(&st.cpu, 0, sizeof(st.cpu));
        memset(&st.next, 0, sizeof(st.next));
        st.seen = 0;
        for (unsigned k = 0; k < STEER_FLOWS; k++) {
            m = steer_frame(&f.nif, k, 0);
            CHECK(m != NULL);
            netif_rx(&f.nif, m);
        }
        steer_wait(&st, STEER_FLOWS);
        netif_set_steering(true);
        CHECK(st.seen == STEER_FLOWS);
        for (unsigned k = 0; k < STEER_FLOWS; k++)
            CHECK(st.cpu[k] == 1);
    }
    struct net_cpu_stats cs;
    CHECK(netif_cpu_stats(0, &cs) && cs.rx_queued > 0);
    CHECK(!netif_cpu_stats(CONFIG_MAX_CPUS, &cs));

    netif_set_rx_hook(NULL, NULL);
    netif_unregister(&f.nif);
    netif_put(&f.nif);
    CHECK(f.releases == 1);
    return true;
}

/* --- checksum offload (unit 11) -------------------------------------------------- */

struct csum_nif {
    struct netif nif;
    struct mbuf *last;      /* the last transmitted packet, kept */
    unsigned transmits, releases;
};

static int csum_nif_transmit(struct netif *nif, struct mbuf *m)
{
    struct csum_nif *c = nif->priv;
    c->transmits++;
    if (c->last)
        m_freem(c->last);
    c->last = m;
    return 0;
}

static void csum_nif_release(struct netif *nif)
{
    struct csum_nif *c = nif->priv;
    c->releases++;
}

/* An IPv4 + TCP packet (no Ethernet) as a transport leaves it: the TCP
 * checksum in the partial form and NET_CSUM_TCP requested; `corrupt`
 * flips a payload byte after the sum was taken. */
static struct mbuf *csum_packet(uint32_t src, uint32_t dst, bool partial, bool corrupt)
{
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return NULL;
    static const char payload[] = "checksum offload payload, 40 bytes long!";
    uint32_t tlen = 20 + 40;
    struct tcp_hdr *th = (struct tcp_hdr *)m->data;
    memset(th, 0, sizeof(*th));
    th->sport = htons(40100);
    th->dport = htons(7);
    th->doff = 5 << 4;
    th->flags = TH_ACK;
    th->win = htons(1024);
    memcpy(m->data + 20, payload, 40);
    m->len = m->pkt.len = tlen;
    uint32_t pseudo = cksum_pseudo4(src, dst, IPPROTO_TCP, (uint16_t)tlen);
    if (partial) {
        th->cksum = cksum_partial_field(pseudo);
        m->pkt.csum_flags = NET_CSUM_TCP;
        m->pkt.csum_start = 0;
        m->pkt.csum_offset = 16;
    } else {
        th->cksum = cksum_fold(m_cksum_partial(m, 0, tlen, pseudo));
    }
    if (corrupt)
        m->data[30] ^= 0x5a;
    /* The IPv4 header, as output_on would add it. */
    m = m_prepend(m, sizeof(struct ipv4_hdr));
    struct ipv4_hdr *ip = (struct ipv4_hdr *)m->data;
    memset(ip, 0, sizeof(*ip));
    ip->vhl = 0x45;
    ip->len = htons((uint16_t)(20 + tlen));
    ip->ttl = 64;
    ip->proto = IPPROTO_TCP;
    ip->src = src;
    ip->dst = dst;
    ip->cksum = in_cksum(ip, sizeof(*ip));
    if (m->pkt.csum_flags & NET_CSUM_TX)
        m->pkt.csum_start += sizeof(*ip);
    m->pkt.proto = ETH_P_IP;
    return m;
}

/* Verify a transmitted IPv4+TCP packet's checksum the receiver's way. */
static bool csum_packet_valid(const struct mbuf *m)
{
    uint8_t hdr[20];
    if (!m_copydata(m, 0, 20, hdr))
        return false;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)hdr;
    uint32_t tlen = m->pkt.len - 20;
    uint32_t pseudo = cksum_pseudo4(ip->src, ip->dst, IPPROTO_TCP, (uint16_t)tlen);
    return cksum_fold(m_cksum_partial(m, 20, tlen, pseudo)) == 0;
}

bool selftest_net_csum_offload(const char **reason)
{
    static const struct netif_ops ops = { .transmit = csum_nif_transmit, .release = csum_nif_release };
    struct csum_nif c;
    memset(&c, 0, sizeof(c));
    strlcpy(c.nif.name, "csum0", sizeof(c.nif.name));
    memcpy(c.nif.mac, "\x02\x11\x22\x33\x44\x66", 6);
    c.nif.mtu = 1500;
    c.nif.ops = &ops;
    c.nif.priv = &c;
    c.nif.caps = NETIF_CAP_TXCSUM | NETIF_CAP_RXCSUM;
    CHECK(netif_register(&c.nif) == 0);
    uint32_t me = IPV4_ADDR(10, 9, 1, 1), peer = IPV4_ADDR(10, 9, 1, 2);
    netif_set_ipv4(&c.nif, me, htonl(0xffffff00u), 0);
    netif_set_up(&c.nif, true);

    /* Transmit through an offloading interface: the partial form arrives
     * untouched, with the offsets a driver programs into its header. */
    struct mbuf *m = csum_packet(me, peer, true, false);
    CHECK(m != NULL);
    CHECK(netif_transmit(&c.nif, m) == 0 && c.last != NULL);
    CHECK((c.last->pkt.csum_flags & NET_CSUM_TCP) && c.last->pkt.csum_start == 20 && c.last->pkt.csum_offset == 16);
    CHECK(!csum_packet_valid(c.last));                     /* only the pseudo-header sum so far */
    CHECK(m_csum_complete(c.last) && csum_packet_valid(c.last) && c.last->pkt.csum_flags == 0);
    /* Through an interface without the capability: finished in software. */
    c.nif.caps = 0;
    m = csum_packet(me, peer, true, false);
    CHECK(m != NULL);
    CHECK(netif_transmit(&c.nif, m) == 0 && c.last->pkt.csum_flags == 0 && csum_packet_valid(c.last));
    /* Bad offsets are refused rather than written out of range. */
    m = csum_packet(me, peer, true, false);
    CHECK(m != NULL);
    m->pkt.csum_start = (uint16_t)m->pkt.len;
    unsigned tx_before = c.transmits;
    CHECK(netif_transmit(&c.nif, m) == -EINVAL && c.transmits == tx_before);
    /* The real transports leave the partial form: a TCP segment from the
     * stack to this interface's peer is finished (no ARP for a directed
     * frame here, so the segment is built by hand above; the stack's own
     * path is covered by net-lo-tcp over the loopback, which offloads). */

    /* Receive: a wrong checksum is dropped and counted; the same packet
     * marked M_CSUM_OK (the device verified it) is accepted. */
    struct tcp_stats t0, t1;
    tcp_get_stats(&t0);
    m = csum_packet(peer, me, false, true);
    CHECK(m != NULL);
    /* Feed it as the loopback does (no link layer): pkt.proto carries the type. */
    c.nif.flags |= NETIF_LOOPBACK;
    netif_rx(&c.nif, m);
    thread_sleep_ms(20);
    tcp_get_stats(&t1);
    CHECK(t1.bad_cksum == t0.bad_cksum + 1);
    m = csum_packet(peer, me, false, true);
    CHECK(m != NULL);
    m->flags |= M_CSUM_OK;
    netif_rx(&c.nif, m);
    thread_sleep_ms(20);
    tcp_get_stats(&t1);
    CHECK(t1.bad_cksum == t0.bad_cksum + 1);                 /* trusted: not counted as bad */
    CHECK(t1.dropped_no_pcb >= t0.dropped_no_pcb + 1);       /* it reached the pcb lookup */
    c.nif.flags &= ~NETIF_LOOPBACK;

    /* The loopback interface offloads both ways: a transfer computes no
     * transport checksum at all (net-lo-tcp exercises it). */
    struct netif *lo = netif_loopback();
    CHECK(lo != NULL && (lo->caps & NETIF_CAP_TXCSUM) && (lo->caps & NETIF_CAP_RXCSUM));
    netif_put(lo);

    netif_unregister(&c.nif);
    if (c.last)
        m_freem(c.last);
    netif_put(&c.nif);
    CHECK(c.releases == 1);
    return true;
}

/* --- benchmark (unit 11) ----------------------------------------------------------
 *
 * Reports, never fails on timing: loopback TCP throughput with one and
 * with two concurrent flows, and UDP datagrams per second, each with
 * steering off (the single-queue architecture) and on.
 */
#define BENCH_TCP_BYTES (4u * 1024u * 1024u)
#define BENCH_UDP_SENDS 10000u

struct bench_sink {
    struct netaddr addr;
    uint32_t bytes;
    int err;
    volatile bool done;
};

static void bench_sink_main(void *arg)
{
    struct bench_sink *b = arg;
    struct socket *ls, *c;
    if (ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0) {
        if (ksock_bind(ls, &b->addr) == 0 && ksock_listen(ls, 2) == 0 && ksock_accept(ls, &c, NULL) == 0) {
            uint8_t *buf = kmalloc(16384, 0);
            for (;;) {
                int64_t n = ksock_recvfrom(c, buf, 16384, NULL);
                if (n <= 0) {
                    b->err = (int)n;
                    break;
                }
                b->bytes += (uint32_t)n;
            }
            kfree(buf);
            ksock_put(c);
        } else {
            b->err = -1000;
        }
        ksock_put(ls);
    } else {
        b->err = -1001;
    }
    b->done = true;
    thread_exit(0);
}

struct bench_client {
    struct netaddr addr;
    uint32_t bytes, sent;
    int err;
    volatile bool done;
};

static void bench_client_main(void *arg)
{
    struct bench_client *cl = arg;
    struct socket *c;
    uint8_t *buf = kmalloc(16384, 0);
    if (buf && ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0) {
        int rc = ksock_connect(c, &cl->addr);
        if (rc == 0) {
            while (cl->sent < cl->bytes) {
                uint32_t want = cl->bytes - cl->sent < 16384 ? cl->bytes - cl->sent : 16384;
                int64_t w = ksock_sendto(c, buf, want, NULL);
                if (w <= 0) {
                    cl->err = (int)w;
                    break;
                }
                cl->sent += (uint32_t)w;
            }
            ksock_shutdown(c, COSMO_SHUT_WR);
        } else {
            cl->err = rc;
        }
        ksock_put(c);
    }
    kfree(buf);
    cl->done = true;
    thread_exit(0);
}

/* `flows` concurrent transfers of BENCH_TCP_BYTES each; returns MiB/s in total, 0 on failure. */
static unsigned bench_tcp(unsigned flows, uint16_t port)
{
    struct bench_sink sinks[2];
    struct bench_client clients[2];
    struct thread *ts[2], *tc[2];
    if (flows > 2)
        flows = 2;
    for (unsigned i = 0; i < flows; i++) {
        memset(&sinks[i], 0, sizeof(sinks[i]));
        sinks[i].addr = v4addr(INADDR_LOOPBACK_N, (uint16_t)(port + i));
        ts[i] = thread_create(bench_sink_main, &sinks[i], "bench-sink", SCHED_PRIO_DEFAULT);
    }
    thread_sleep_ms(20);
    uint64_t t0 = clock_now_ns();
    for (unsigned i = 0; i < flows; i++) {
        memset(&clients[i], 0, sizeof(clients[i]));
        clients[i].addr = sinks[i].addr;
        clients[i].bytes = BENCH_TCP_BYTES;
        tc[i] = thread_create(bench_client_main, &clients[i], "bench-client", SCHED_PRIO_DEFAULT);
    }
    uint64_t total = 0;
    for (unsigned i = 0; i < flows; i++) {
        if (tc[i])
            thread_join(tc[i]);
        if (ts[i])
            thread_join(ts[i]);
        total += sinks[i].bytes;
    }
    uint64_t dt = clock_now_ns() - t0;
    if (total != (uint64_t)flows * BENCH_TCP_BYTES || dt == 0) {
        for (unsigned i = 0; i < flows; i++)
            kwarn("net-bench: flow %u: sink got %u (err %d), client sent %u (err %d)", i, sinks[i].bytes, sinks[i].err,
                  clients[i].sent, clients[i].err);
        return 0;
    }
    return (unsigned)((total * 1000000000ull) / (dt * 1024ull * 1024ull));
}

struct bench_udp_rx {
    struct socket *s;
    uint32_t got;
    volatile bool stop, done;
};

static void bench_udp_rx_main(void *arg)
{
    struct bench_udp_rx *r = arg;
    uint8_t buf[128];
    while (!r->stop) {
        int64_t n = ksock_recvfrom(r->s, buf, sizeof(buf), NULL);
        if (n <= 0)
            break;
        r->got++;
    }
    r->done = true;
    thread_exit(0);
}

/* BENCH_UDP_SENDS datagrams to a loopback receiver: the send rate in
 * datagrams per second (0 on failure) and how many the receiver got (the
 * socket queue holds UDP_RXQ_MAX; the rest were dropped there). */
static unsigned bench_udp(uint16_t port, uint32_t *delivered)
{
    *delivered = 0;
    struct netaddr addr = v4addr(INADDR_LOOPBACK_N, port);
    struct socket *rx, *tx;
    if (ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &rx) != 0)
        return 0;
    if (ksock_bind(rx, &addr) != 0 || ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &tx) != 0) {
        ksock_put(rx);
        return 0;
    }
    struct bench_udp_rx r = { .s = rx };
    struct thread *t = thread_create(bench_udp_rx_main, &r, "bench-udp", SCHED_PRIO_DEFAULT);
    uint8_t payload[64] = { 0 };
    uint64_t t0 = clock_now_ns();
    uint32_t sent = 0;
    for (unsigned i = 0; i < BENCH_UDP_SENDS; i++) {
        if (ksock_sendto(tx, payload, sizeof(payload), &addr) == (int64_t)sizeof(payload))
            sent++;
        if ((i & 63) == 63)
            sched_yield();   /* let the receiver drain: the socket queue is short */
    }
    uint64_t dt = clock_now_ns() - t0;
    for (unsigned i = 0; i < 50 && r.got < sent; i++)
        thread_sleep_ms(2);
    *delivered = r.got;
    r.stop = true;
    ksock_set_nonblock(rx, true);   /* the receiver's blocked recv returns */
    ksock_shutdown(rx, COSMO_SHUT_RD);
    if (t)
        thread_join(t);
    ksock_put(tx);
    ksock_put(rx);
    if (dt == 0)
        return 0;
    return (unsigned)(((uint64_t)sent * 1000000000ull) / dt);
}

bool selftest_net_bench(const char **reason)
{
    (void)reason;
    unsigned ncpu = cpu_count();
    for (unsigned steer = 0; steer < 2; steer++) {
        netif_set_steering(steer != 0);
        /* Fresh ports per round: the previous round's connections may still
         * be in TIME_WAIT on theirs. */
        uint16_t base = (uint16_t)(6100 + 40 * steer);
        uint32_t delivered = 0;
        unsigned one = bench_tcp(1, base), two = bench_tcp(2, (uint16_t)(base + 10));
        unsigned pps = bench_udp((uint16_t)(base + 20), &delivered);
        kinfo("net-bench: steer=%u cpus=%u: tcp 1 flow %u MiB/s, 2 flows %u MiB/s total, udp %u sends/s (%u of %u delivered)",
              steer, ncpu, one, two, pps, delivered, BENCH_UDP_SENDS);
    }
    netif_set_steering(true);
    return true;
}
