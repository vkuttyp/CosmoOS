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
#include <kernel/net/fw.h>
#include <kernel/net/nat.h>
#include <kernel/net/tapsvc.h>
#include <uapi/cosmo/netctl.h>
#include <kernel/vfs.h>
#include <kernel/net/tap.h>
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
    /* Wait for the message to be processed, bounded, rather than a fixed
     * 30 ms: the check is that the counters moved by exactly one, not
     * that the netrx worker made a window on a loaded host (it missed
     * one once, on `no-iommu x86_64`, with debug page poisoning adding
     * a fill and a scan to every cluster). */
    for (unsigned i = 0; i < 100; i++) {
        tcp_get_stats(&t1);
        if (t1.pmtu_updates != t0.pmtu_updates)
            break;
        settle(10);
    }
    settle(10);   /* and let the IP side's record land too */
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
     * The peer never reads here, so its window closes and the socket ends
     * up unwritable for good -- but getting there is not instant, because
     * an ACK still in flight (processed on another worker since unit 11)
     * frees send space again. So converge on the state instead of
     * sampling it: keep sending while readiness says there is room, and
     * stop the moment it says there is none. The observation is the exit
     * condition, so no ACK can arrive between seeing the state and
     * asserting it. A 4 KiB send that fails while readiness still says
     * writable is not a failure -- WRITABLE is a low-water predicate and
     * a byte might still fit -- it just means more waiting. The positive
     * direction (WRITABLE returns once the peer drains) is checked after
     * the drain loop below. */
    memset(buf, 'f', sizeof(buf));
    uint64_t pushed = 0;
    bool full = false;
    for (unsigned i = 0; i < 400 && !full; i++) {
        if (!(ksock_ready(c) & COSMO_IO_WRITABLE)) {
            full = true;
            break;
        }
        int64_t n = ksock_sendto(c, buf, sizeof(buf), NULL);
        if (n > 0)
            pushed += (uint64_t)n;
        else if (n == -EAGAIN)
            settle(10);
        else
            CHECK(n > 0);
    }
    CHECK(full && pushed > 0);
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

/* --- a second interface (docs/drivers/e1000e/testing.md) -----------------------
 *
 * The half of the two-interface question a driver can be checked on:
 * when the default interface goes down, the other becomes the default
 * and carries traffic through its own rings. Written against no driver
 * in particular -- it finds whatever second non-loopback interface the
 * machine has -- because that is the claim being tested.
 */
static struct netif *find_other_interface(struct netif *not_this)
{
    for (unsigned i = 0; i < 16; i++) {
        char name[8];
        ksnprintf(name, sizeof(name), "eth%u", i);
        struct netif *n = netif_find(name);
        if (n == NULL)
            continue;
        if (n != not_this && !(n->flags & NETIF_LOOPBACK))
            return n;
        netif_put(n);
    }
    return NULL;
}

static bool second_nic_body(const char **reason, struct netif *second)
{
    /* The other takes over as soon as the first is down. */
    struct netif *now = netif_default();
    CHECK(now == second);
    netif_put(now);

    /* The ARP cache is keyed by address alone, and both backends answer
     * the same gateway address, so an entry learned through the first
     * interface would make this pass without a frame ever crossing the
     * second. Age everything out first. */
    arp_age(clock_now_ns() + 3600ull * NS_PER_SEC);
    uint8_t mac[ETH_ALEN];
    uint32_t gw = second->ip4.gateway;
    CHECK(gw != 0);
    CHECK(!arp_lookup(gw, mac));

    uint64_t rx0 = second->stats.rx_packets, tx0 = second->stats.tx_packets;
    struct mbuf *m = m_getcl();
    CHECK(m != NULL);
    m->len = m->pkt.len = 20;
    int rc = arp_resolve(second, gw, mac, m);   /* the request leaves through `second` */
    CHECK(rc == -EINPROGRESS || rc == 0);
    bool resolved = rc == 0;
    for (unsigned waited = 0; waited < 2000 && !resolved; waited += 10) {
        thread_sleep_ms(10);
        resolved = arp_lookup(gw, mac);
    }
    CHECK(resolved);   /* the reply came back: a frame out and a frame in through its rings */
    CHECK(second->stats.tx_packets > tx0);
    CHECK(second->stats.rx_packets > rx0);
    return true;
}

bool selftest_net_second_nic(const char **reason)
{
    struct netif *first = netif_default();
    if (first == NULL) {
        kinfo("selftest: net-second-nic: no ethernet interface; skipping");
        return true;
    }
    struct netif *second = find_other_interface(first);
    if (second == NULL) {
        netif_put(first);
        kinfo("selftest: net-second-nic: one interface; skipping");
        return true;
    }
    netif_set_up(first, false);
    bool ok = second_nic_body(reason, second);
    /* Whatever happened, the machine leaves with its default interface
     * back: a later test that finds it down would fail for this test's
     * reason and say nothing about its own. */
    netif_set_up(first, true);
    struct netif *again = netif_default();
    if (ok) {
        ok = again == first;
        if (!ok)
            *reason = "the first interface did not become the default again";
    }
    if (again)
        netif_put(again);
    if (ok)
        kinfo("selftest: net-second-nic: %s took over from %s and resolved the gateway through its own rings",
              second->name, first->name);
    netif_put(second);
    netif_put(first);
    return ok;
}

/* --- the NIC-path benchmark (design.md, "The NIC-path benchmark") ----------------
 *
 * Traffic that leaves the machine, per interface: ARP round trips
 * counted at the driver boundary, UDP sends through the whole stack,
 * and the software checksum's share of a send -- the number that gates
 * a driver's transmit offload.
 */
#define NICBENCH_ARP     2000u
#define NICBENCH_WINDOW  64u      /* requests in flight: the receive queue is short, and an open loop overruns it */
#define NICBENCH_UDP     10000u
#define NICBENCH_UDP_LEN 1024u
#define NICBENCH_PORT    33434u   /* nobody listens on the host; the send is the measurement */

struct arp_frame {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[ETH_ALEN];
    uint8_t spa[4];
    uint8_t tha[ETH_ALEN];
    uint8_t tpa[4];
} __packed;

struct nicbench_hook {
    struct netif *nif;
    uint32_t gateway;   /* the reply we asked for is about this address */
    uint32_t sent;      /* requests out so far: replies are never counted past it */
    uint32_t replies;   /* atomic: the worker of whichever CPU the flow hashes to */
};

/* At the driver boundary, before any protocol layer: a reply to *us*
 * about the *gateway*, from the interface under test, is counted and
 * taken. Anything else -- another interface, an unsolicited reply, a
 * reply about some other host -- goes on to the ARP layer untouched. */
static bool nicbench_rx_hook(struct netif *nif, struct mbuf *m, void *arg)
{
    struct nicbench_hook *h = arg;
    if (nif != h->nif || m->pkt.len < ETH_HLEN + sizeof(struct arp_frame))
        return true;
    uint8_t hdr[ETH_HLEN + sizeof(struct arp_frame)];
    if (!m_copydata(m, 0, sizeof(hdr), hdr))
        return true;
    uint16_t type = (uint16_t)((hdr[12] << 8) | hdr[13]);
    const struct arp_frame *a = (const struct arp_frame *)(hdr + ETH_HLEN);
    if (type != ETH_P_ARP || a->op != htons(2))
        return true;
    if (memcmp(a->spa, &h->gateway, 4) != 0 || memcmp(a->tha, nif->mac, ETH_ALEN) != 0)
        return true;   /* not the reply this benchmark asked for */
    /* Never past what was sent: a duplicate cannot make the window
     * arithmetic go negative or the count exceed the requests. */
    uint32_t seen = __atomic_load_n(&h->replies, __ATOMIC_RELAXED);
    if (seen >= __atomic_load_n(&h->sent, __ATOMIC_ACQUIRE))
        return true;
    __atomic_fetch_add(&h->replies, 1u, __ATOMIC_RELAXED);
    m_freem(m);
    return false;
}

/* Receive-queue drops across every CPU's worker: where an open-loop
 * sender's replies go when they arrive faster than they are taken. */
static uint64_t rxq_drops_total(void)
{
    uint64_t total = 0;
    for (unsigned cpu = 0; cpu < cpu_count(); cpu++) {
        struct net_cpu_stats st;
        if (netif_cpu_stats(cpu, &st))
            total += st.rx_dropped;
    }
    return total;
}

/*
 * Closed loop: at most NICBENCH_WINDOW requests outstanding. The first
 * version sent all 2000 at once; the driver received every reply and
 * the receive queue kept 512 of them, which measured the queue's depth
 * and nothing about the NIC. A round trip is only a round trip if the
 * reply is waited for.
 */
static bool nicbench_arp(const char **reason, struct netif *nif, unsigned *rt_per_s, uint64_t *ns_per_rt)
{
    static const uint8_t bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    /* One context per round, on this frame: netif_set_rx_hook(NULL)
     * returns only after a grace period, so no worker is still inside
     * the hook when the round ends -- a reply from this interface's
     * round cannot be counted into the next interface's. */
    struct nicbench_hook ctx = { .nif = nif, .gateway = nif->ip4.gateway };
    struct nicbench_hook *h = &ctx;
    uint64_t rx0 = nif->stats.rx_packets, drops0 = rxq_drops_total();
    netif_set_rx_hook(nicbench_rx_hook, h);
    uint64_t t0 = clock_now_ns();
    unsigned sent = 0;
    for (unsigned i = 0; i < NICBENCH_ARP; i++) {
        /* Wait for the window to open; give up on this round if it never does. */
        uint64_t wait_until = clock_now_ns() + 200ull * 1000000ull;
        unsigned spins = 0;
        while (sent - __atomic_load_n(&h->replies, __ATOMIC_RELAXED) >= NICBENCH_WINDOW) {
            if (clock_now_ns() > wait_until)
                goto stop;
            if (++spins < 64)
                sched_yield();
            else
                thread_sleep_ms(1);
        }
        struct mbuf *m = m_getcl();
        if (m == NULL)
            break;
        m->data = m->buf + 64;   /* headroom for the Ethernet header */
        struct arp_frame *a = (struct arp_frame *)m->data;
        a->htype = htons(1);
        a->ptype = htons(ETH_P_IP);
        a->hlen = ETH_ALEN;
        a->plen = 4;
        a->op = htons(1);
        memcpy(a->sha, nif->mac, ETH_ALEN);
        memcpy(a->spa, &nif->ip4.addr, 4);
        memset(a->tha, 0, ETH_ALEN);
        memcpy(a->tpa, &nif->ip4.gateway, 4);
        m->len = m->pkt.len = sizeof(*a);
        if (ether_output(nif, m, bcast, ETH_P_ARP) != 0)
            break;
        sent++;
        __atomic_store_n(&h->sent, sent, __ATOMIC_RELEASE);
    }
stop:;
    /* The clock stops when the replies have caught up, or when it is
     * clear they are not going to. */
    uint64_t deadline = clock_now_ns() + 500ull * 1000000ull;
    while (__atomic_load_n(&h->replies, __ATOMIC_RELAXED) < sent && clock_now_ns() < deadline)
        thread_sleep_ms(1);
    uint64_t dt = clock_now_ns() - t0;
    netif_set_rx_hook(NULL, NULL);
    unsigned got = __atomic_load_n(&h->replies, __ATOMIC_RELAXED);
    kinfo("selftest: net-nicbench: %s: %u ARP requests sent, %u replies counted at the boundary, %llu frames received by the driver, %llu dropped at the receive queue",
          nif->name, sent, got, (unsigned long long)(nif->stats.rx_packets - rx0),
          (unsigned long long)(rxq_drops_total() - drops0));
    CHECK(sent > 0);
    CHECK(got * 2 >= sent);   /* fewer than half back is a broken path, not a slow one */
    *rt_per_s = dt ? (unsigned)(((uint64_t)got * 1000000000ull) / dt) : 0;
    *ns_per_rt = got ? dt / got : 0;
    return true;
}

static bool nicbench_udp(const char **reason, struct netif *nif, unsigned *sends_per_s, uint64_t *ns_per_send,
                         uint64_t *frames_out, unsigned *accepted)
{
    struct socket *tx;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &tx) == 0);
    struct netaddr to = v4addr(nif->ip4.gateway, NICBENCH_PORT);
    static uint8_t payload[NICBENCH_UDP_LEN];
    /* Warm up: the first send resolves the gateway and parks behind it. */
    for (unsigned i = 0; i < 8; i++)
        (void)ksock_sendto(tx, payload, sizeof(payload), &to);
    thread_sleep_ms(20);
    uint64_t tx0 = nif->stats.tx_packets;
    uint64_t t0 = clock_now_ns();
    unsigned sent = 0;
    for (unsigned i = 0; i < NICBENCH_UDP; i++) {
        if (ksock_sendto(tx, payload, sizeof(payload), &to) == (int64_t)sizeof(payload))
            sent++;
        if ((i & 63) == 63)
            sched_yield();
    }
    uint64_t dt = clock_now_ns() - t0;
    thread_sleep_ms(20);   /* the driver's completions and counters settle */
    *frames_out = nif->stats.tx_packets - tx0;
    ksock_put(tx);
    *accepted = sent;
    *sends_per_s = dt ? (unsigned)(((uint64_t)sent * 1000000000ull) / dt) : 0;
    *ns_per_send = sent ? dt / sent : 0;
    return true;
}

/* in_cksum over a datagram's worth, by itself: what a transmit checksum
 * offload could save per packet, and no more. */
static uint64_t nicbench_cksum_ns(void)
{
    static uint8_t buf[NICBENCH_UDP_LEN];
    volatile uint32_t sink = 0;
    uint64_t t0 = clock_now_ns();
    for (unsigned i = 0; i < NICBENCH_UDP; i++) {
        buf[i & (NICBENCH_UDP_LEN - 1)] = (uint8_t)i;   /* defeat a hoisted result */
        sink += in_cksum(buf, sizeof(buf));
    }
    return (clock_now_ns() - t0) / NICBENCH_UDP;
}

static bool nicbench_one(const char **reason, struct netif *nif, uint64_t cksum_ns)
{
    unsigned rt_s = 0, sends_s = 0, accepted = 0;
    uint64_t ns_rt = 0, ns_send = 0, frames = 0;
    if (!nicbench_arp(reason, nif, &rt_s, &ns_rt))
        return false;
    if (!nicbench_udp(reason, nif, &sends_s, &ns_send, &frames, &accepted))
        return false;
    unsigned share = ns_send ? (unsigned)((cksum_ns * 100) / ns_send) : 0;
    kinfo("selftest: net-nicbench: %s (caps 0x%x): arp %u rt/s (%llu ns per round trip); udp %u sends/s (%llu ns per send, "
          "%llu of %u frames left the driver); sw checksum of 1 KiB %llu ns = %u%% of a send",
          nif->name, nif->caps, rt_s, (unsigned long long)ns_rt, sends_s, (unsigned long long)ns_send,
          (unsigned long long)frames, accepted, (unsigned long long)cksum_ns, share);
    return true;
}

/*
 * The guarantee the benchmark's per-round context rests on: once
 * netif_set_rx_hook(NULL) has returned, the hook it removed is not
 * running on any worker. The hook lingers on purpose after announcing
 * itself, the test removes it while it is lingering, and the removal
 * must not return before the hook has left.
 */
struct rxhook_grace_state {
    uint32_t entered, exited;
};

static bool rxhook_grace_hook(struct netif *nif, struct mbuf *m, void *arg)
{
    struct rxhook_grace_state *st = arg;
    (void)nif;
    __atomic_store_n(&st->entered, 1u, __ATOMIC_RELEASE);
    /* Long enough for the test thread, on another CPU, to see `entered`
     * and call netif_set_rx_hook(NULL) while this is still running. A
     * spin, not a sleep: a hook runs inside a read-side section. */
    uint64_t until = clock_now_ns() + 10ull * 1000000ull;
    while (clock_now_ns() < until)
        arch_cpu_relax();
    m_freem(m);
    __atomic_store_n(&st->exited, 1u, __ATOMIC_RELEASE);
    return false;
}

bool selftest_net_rxhook_grace(const char **reason)
{
    struct netif *lo = netif_loopback();
    CHECK(lo != NULL);
    struct rxhook_grace_state st = { 0, 0 };
    netif_set_rx_hook(rxhook_grace_hook, &st);
    struct mbuf *m = m_getcl();
    CHECK(m != NULL);
    m->data = m->buf + 64;
    m->len = m->pkt.len = 64;
    memset(m->data, 0, 64);
    m->pkt.proto = ETH_P_IP;   /* never reaches IP: the hook takes it */
    /* To another CPU's worker when there is one. On the same CPU the
     * hook, which does not yield, finishes before this thread runs
     * again, and the removal has nothing to wait for. */
    unsigned ncpu = cpu_count();
    netif_rx_on(lo, m, ncpu > 1 ? (arch_cpu_id() + 1) % ncpu : 0);
    for (unsigned i = 0; i < 1000 && !__atomic_load_n(&st.entered, __ATOMIC_ACQUIRE); i++)
        thread_sleep_ms(1);
    CHECK(__atomic_load_n(&st.entered, __ATOMIC_ACQUIRE) == 1);
    netif_set_rx_hook(NULL, NULL);
    /* Returned: the hook has finished, and `st` -- this stack frame --
     * may go. Without the grace period this fails on two or more CPUs. */
    CHECK(__atomic_load_n(&st.exited, __ATOMIC_ACQUIRE) == 1);
    netif_put(lo);
    return true;
}

bool selftest_net_nicbench(const char **reason)
{
    struct netif *first = netif_default();
    if (first == NULL) {
        kinfo("selftest: net-nicbench: no ethernet interface; skipping");
        return true;
    }
    uint64_t cksum_ns = nicbench_cksum_ns();
    bool ok = nicbench_one(reason, first, cksum_ns);
    struct netif *second = ok ? find_other_interface(first) : NULL;
    if (second != NULL) {
        /* The same numbers over the other driver, on the same host and
         * the same kind of backend: bring the default down so the stack
         * routes through the second, as net-second-nic does. */
        netif_set_up(first, false);
        arp_age(clock_now_ns() + 3600ull * NS_PER_SEC);
        ok = nicbench_one(reason, second, cksum_ns);
        netif_set_up(first, true);
        netif_put(second);
    }
    netif_put(first);
    return ok;
}

/* tap: an interface whose far end is userland. A frame the stack transmits
 * out the tap is read back; an ARP request injected for the tap's own IP is
 * answered by the stack out the tap; the transmit queue caps rather than
 * grows without bound. The owner's frame channel and the guest are proved
 * elsewhere (the /dev/net/tap device and el2-tap-host); this is the tap
 * itself against the real stack. */
bool selftest_tap(const char **reason)
{
    static const uint8_t host_mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x01 };
    uint32_t host_ip = IPV4_ADDR(10, 0, 3, 1), guest_ip = IPV4_ADDR(10, 0, 3, 15);
    struct tap *t = tap_create("taptest", host_ip, htonl(0xffffff00u), host_mac);
    CHECK(t != NULL);
    struct netif *nif = tap_netif(t);

    /* a tap is a point-to-point owner link, never the machine's default
     * interface -- even brought up, a tap cannot swallow the host's outbound
     * traffic. */
    struct netif *def = netif_default();
    CHECK(def != nif);
    if (def)
        netif_put(def);

    /* (1) stack -> tap: a frame transmitted out the tap is there to read. */
    struct mbuf *m = m_getcl();
    CHECK(m != NULL);
    uint8_t probe[64];
    memset(probe, 0, sizeof(probe));
    memcpy(probe, guest_mac, 6);
    memcpy(probe + 6, host_mac, 6);
    probe[12] = 0x08;
    for (unsigned i = 14; i < 64; i++)
        probe[i] = (uint8_t)i;
    memcpy(m->data, probe, 64);
    m->len = m->pkt.len = 64;
    CHECK(netif_transmit(nif, m) == 0);
    struct mbuf *got = tap_recv(t);
    CHECK(got != NULL);
    uint8_t out[64];
    CHECK(m_copydata(got, 0, 64, out) && memcmp(out, probe, 64) == 0);
    m_freem(got);
    CHECK(tap_recv(t) == NULL);

    /* (2) tap -> stack -> tap: an ARP request for the tap's IP is answered. */
    uint8_t req[42];
    memset(req, 0, sizeof(req));
    memset(req, 0xff, 6);                 /* dst broadcast */
    memcpy(req + 6, guest_mac, 6);        /* src */
    req[12] = 0x08; req[13] = 0x06;       /* ethertype ARP */
    req[15] = 1;                          /* htype Ethernet */
    req[16] = 0x08;                       /* ptype IPv4 */
    req[18] = 6; req[19] = 4;             /* hlen, plen */
    req[21] = 1;                          /* op request */
    memcpy(req + 22, guest_mac, 6);       /* sha */
    memcpy(req + 28, &guest_ip, 4);       /* spa */
    memcpy(req + 38, &host_ip, 4);        /* tpa */
    CHECK(tap_inject(t, req, sizeof(req)) == 0);
    struct mbuf *reply = NULL;
    for (unsigned i = 0; i < 50 && reply == NULL; i++) {
        reply = tap_recv(t);
        if (reply == NULL)
            thread_sleep_ms(10);
    }
    CHECK(reply != NULL);
    uint8_t r[42];
    CHECK(m_copydata(reply, 0, 42, r));
    CHECK(memcmp(r, guest_mac, 6) == 0);         /* to the guest */
    CHECK(r[12] == 0x08 && r[13] == 0x06);       /* ARP */
    CHECK(r[21] == 2);                            /* reply */
    CHECK(memcmp(r + 22, host_mac, 6) == 0);      /* sha = the tap's MAC */
    CHECK(memcmp(r + 28, &host_ip, 4) == 0);      /* spa = the tap's IP */
    CHECK(memcmp(r + 32, guest_mac, 6) == 0);     /* tha = the asker */
    m_freem(reply);

    /* (3) the transmit queue caps rather than growing without bound. */
    for (unsigned i = 0; i < TAP_TXQ_MAX + 8; i++) {
        struct mbuf *f = m_getcl();
        if (f == NULL)
            break;
        f->len = f->pkt.len = 64;
        netif_transmit(nif, f);
    }
    unsigned held = 0;
    struct mbuf *d;
    while ((d = tap_recv(t)) != NULL) {
        m_freem(d);
        held++;
    }
    CHECK(held == TAP_TXQ_MAX);

    tap_destroy(t);
    kinfo("selftest: tap: a transmitted frame was read back, an injected ARP was answered by the stack, "
          "and the queue capped at %u", TAP_TXQ_MAX);
    return true;
}

/* net-route: connected routing picks the longest prefix, not the first
 * interface registered. Two taps with overlapping subnets -- a broad /16
 * first, a specific /24 second -- and an address in both must route to the
 * /24. An address only in the /16 routes to it; loopback is unchanged. */
bool selftest_net_route(const char **reason)
{
    static const uint8_t mac_a[6] = { 0x52, 0x54, 0x00, 0x0a, 0x00, 0x01 };
    static const uint8_t mac_b[6] = { 0x52, 0x54, 0x00, 0x0a, 0x00, 0x02 };
    struct tap *a = tap_create("rtbroad", IPV4_ADDR(10, 9, 0, 1), htonl(0xffff0000u), mac_a);   /* 10.9.0.0/16 */
    CHECK(a != NULL);
    struct tap *b = tap_create("rtnarrow", IPV4_ADDR(10, 9, 5, 1), htonl(0xffffff00u), mac_b);  /* 10.9.5.0/24 (later) */
    CHECK(b != NULL);

    struct netif *r = ipv4_route(IPV4_ADDR(10, 9, 5, 7));   /* in both: the /24 wins */
    CHECK(r == tap_netif(b));
    if (r) netif_put(r);
    r = ipv4_route(IPV4_ADDR(10, 9, 9, 9));                 /* only the /16 */
    CHECK(r == tap_netif(a));
    if (r) netif_put(r);
    r = ipv4_route(IPV4_ADDR(127, 0, 0, 1));                /* loopback unchanged */
    CHECK(r != NULL && (r->flags & NETIF_LOOPBACK));
    if (r) netif_put(r);

    tap_destroy(b);
    tap_destroy(a);
    kinfo("selftest: net-route: longest-prefix connected routing picks the /24 over the /16");
    return true;
}

/* --- IP forwarding -------------------------------------------------------- */

/* Teach the stack a neighbour `peer_ip`->`peer_mac` reachable on tap `t`, by
 * feeding arp_input a request from that peer to the tap's own IP (a request
 * addressed to us records the asker). The stack answers with a reply queued
 * for the tap reader; nettest_recv_ip skips it. */
static void nettest_seed_arp(struct netif *nif, uint32_t peer_ip, const uint8_t peer_mac[6])
{
    struct mbuf *f = m_getcl();
    if (f == NULL)
        return;
    memset(f->data, 0, 28);
    f->len = f->pkt.len = 28;
    f->data[1] = 1;            /* htype Ethernet */
    f->data[2] = 0x08;         /* ptype IPv4 */
    f->data[4] = 6; f->data[5] = 4;
    f->data[7] = 1;            /* op request */
    memcpy(f->data + 8, peer_mac, 6);     /* sha = the neighbour */
    memcpy(f->data + 14, &peer_ip, 4);    /* spa */
    memcpy(f->data + 24, &nif->ip4.addr, 4);  /* tpa = us */
    arp_input(nif, f);
}

/* Build an Ethernet+IPv4+UDP frame into buf; returns its length. The IP
 * header checksum is valid (ipv4_input verifies it); the UDP checksum is
 * left 0 (optional over IPv4) since forwarding does not inspect it. */
static uint32_t nettest_build_udp(uint8_t *buf, const uint8_t dstmac[6], const uint8_t srcmac[6],
                                  uint32_t sip, uint32_t dip, uint8_t ttl,
                                  uint16_t sport, uint16_t dport, const uint8_t *pl, uint32_t pllen)
{
    struct eth_hdr *eh = (struct eth_hdr *)buf;
    memcpy(eh->dst, dstmac, 6);
    memcpy(eh->src, srcmac, 6);
    eh->type = htons(ETH_P_IP);
    struct ipv4_hdr *iph = (struct ipv4_hdr *)(buf + ETH_HLEN);
    uint16_t total = (uint16_t)(sizeof(*iph) + sizeof(struct udp_hdr) + pllen);
    iph->vhl = 0x45; iph->tos = 0; iph->len = htons(total);
    iph->id = htons(0x1234); iph->frag = 0; iph->ttl = ttl;
    iph->proto = IPPROTO_UDP; iph->cksum = 0; iph->src = sip; iph->dst = dip;
    iph->cksum = in_cksum(iph, sizeof(*iph));
    struct udp_hdr *uh = (struct udp_hdr *)(buf + ETH_HLEN + sizeof(*iph));
    uh->sport = htons(sport); uh->dport = htons(dport);
    uh->len = htons((uint16_t)(sizeof(*uh) + pllen)); uh->cksum = 0;
    memcpy(buf + ETH_HLEN + sizeof(*iph) + sizeof(*uh), pl, pllen);
    return ETH_HLEN + total;
}

/* The next IPv4 frame read back from tap `t`, skipping ARP the stack queued
 * while resolving; NULL after ~500 ms of nothing. Caller frees it. */
static struct mbuf *nettest_recv_ip(struct tap *t)
{
    for (unsigned i = 0; i < 50; i++) {
        struct mbuf *m;
        while ((m = tap_recv(t)) != NULL) {
            uint8_t type[2];
            if (m_copydata(m, 12, 2, type) && type[0] == 0x08 && type[1] == 0x00)
                return m;   /* IPv4 */
            m_freem(m);     /* an ARP reply from seeding: skip */
        }
        thread_sleep_ms(10);
    }
    return NULL;
}

bool selftest_net_forward(const char **reason)
{
    static const uint8_t g_gw_mac[6]  = { 0x52, 0x54, 0x00, 0x03, 0x00, 0x01 };  /* guest-side tap */
    static const uint8_t u_gw_mac[6]  = { 0x52, 0x54, 0x00, 0x02, 0x00, 0x01 };  /* uplink tap */
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x03, 0x00, 0x0f };  /* the guest neighbour */
    static const uint8_t peer_mac[6]  = { 0x52, 0x54, 0x00, 0x02, 0x00, 0x63 };  /* a host on the uplink */
    uint32_t mask = htonl(0xffffff00u);
    /* Private subnets chosen to collide with neither the QEMU user-net NIC
     * (10.0.2.0/24) nor the real tap0 (10.0.3.0/24), so netif_connected
     * routes to these test taps and not to a live interface. */
    uint32_t gw_ip = IPV4_ADDR(10, 77, 3, 1), guest_ip = IPV4_ADDR(10, 77, 3, 15);
    uint32_t up_ip = IPV4_ADDR(10, 77, 4, 1), peer_ip = IPV4_ADDR(10, 77, 4, 99);

    struct tap *g = tap_create("fwdg", gw_ip, mask, g_gw_mac);
    CHECK(g != NULL);
    struct tap *u = tap_create("fwdu", up_ip, mask, u_gw_mac);
    CHECK(u != NULL);
    netif_set_forward(tap_netif(g), true);   /* guest side forwards; uplink deliberately does not */

    nettest_seed_arp(tap_netif(u), peer_ip, peer_mac);    /* so forwarding transmits at once */
    nettest_seed_arp(tap_netif(g), guest_ip, guest_mac);

    uint8_t payload[16];
    for (unsigned i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(0xa0 + i);
    uint8_t frame[128];

    /* (1) a guest datagram to the uplink subnet is forwarded out the uplink,
     * its TTL down one, its addresses and payload intact (no NAT yet). */
    struct ip_stats s0, s1;
    ipv4_get_stats(&s0);
    uint32_t len = nettest_build_udp(frame, g_gw_mac, guest_mac, guest_ip, peer_ip, 64,
                                     4000, 5000, payload, sizeof(payload));
    CHECK(tap_inject(g, frame, len) == 0);
    struct mbuf *out = nettest_recv_ip(u);
    CHECK(out != NULL);
    uint8_t hdr[ETH_HLEN + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + sizeof(payload)];
    CHECK(m_copydata(out, 0, sizeof(hdr), hdr));
    struct ipv4_hdr *oiph = (struct ipv4_hdr *)(hdr + ETH_HLEN);
    CHECK(oiph->src == guest_ip && oiph->dst == peer_ip);
    CHECK(oiph->ttl == 63);                                   /* decremented by one */
    CHECK(in_cksum(oiph, sizeof(*oiph)) == 0);                /* checksum recomputed */
    CHECK(memcmp(hdr + ETH_HLEN + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr),
                 payload, sizeof(payload)) == 0);
    m_freem(out);
    ipv4_get_stats(&s1);
    CHECK(s1.fwd == s0.fwd + 1);

    /* (2) a TTL-1 datagram dies in transit: no frame on the uplink, an ICMP
     * time-exceeded back to the guest instead. */
    ipv4_get_stats(&s0);
    len = nettest_build_udp(frame, g_gw_mac, guest_mac, guest_ip, peer_ip, 1,
                            4001, 5000, payload, sizeof(payload));
    CHECK(tap_inject(g, frame, len) == 0);
    struct mbuf *err = nettest_recv_ip(g);
    CHECK(err != NULL);
    uint8_t ebuf[ETH_HLEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr)];
    CHECK(m_copydata(err, 0, sizeof(ebuf), ebuf));
    struct ipv4_hdr *eiph = (struct ipv4_hdr *)(ebuf + ETH_HLEN);
    CHECK(eiph->proto == IPPROTO_ICMP && eiph->dst == guest_ip);
    struct icmp_hdr *eic = (struct icmp_hdr *)(ebuf + ETH_HLEN + sizeof(struct ipv4_hdr));
    CHECK(eic->type == ICMP_TIME_EXCEEDED && eic->code == ICMP_TIMXCEED_INTRANS);
    m_freem(err);
    CHECK(nettest_recv_ip(u) == NULL);                       /* nothing forwarded */
    ipv4_get_stats(&s1);
    CHECK(s1.fwd_ttl_exceeded == s0.fwd_ttl_exceeded + 1);

    /* (3) the gate: a datagram arriving on the non-forwarding uplink, bound
     * for the guest subnet, is dropped as not-for-us -- never forwarded, so
     * a real NIC's ingress cannot turn the host into a router. */
    ipv4_get_stats(&s0);
    len = nettest_build_udp(frame, u_gw_mac, peer_mac, peer_ip, guest_ip, 64,
                            5000, 4000, payload, sizeof(payload));
    CHECK(tap_inject(u, frame, len) == 0);
    CHECK(nettest_recv_ip(g) == NULL);                       /* not forwarded to the guest */
    ipv4_get_stats(&s1);
    CHECK(s1.rx_not_for_us == s0.rx_not_for_us + 1 && s1.fwd == s0.fwd);

    tap_destroy(u);
    tap_destroy(g);
    kinfo("selftest: net-forward: a guest datagram was forwarded (TTL 64->63), a TTL-1 datagram "
          "drew a time-exceeded, and non-forwarding ingress stayed a non-router");
    return true;
}

/* --- masquerade NAT ------------------------------------------------------- */

/* The transport checksum (network order) for an L4 buffer of `len` bytes
 * carrying its own zeroed checksum field, under the IPv4 pseudo-header. */
static uint16_t nettest_l4cksum(uint32_t sip, uint32_t dip, uint8_t proto, const void *l4, uint16_t len)
{
    uint32_t sum = cksum_pseudo4(sip, dip, proto, len);
    return cksum_fold(cksum_partial(l4, len, sum));
}

/* True if an L4 buffer's checksum (in place) is valid under the pseudo-header. */
static bool nettest_l4_ok(uint32_t sip, uint32_t dip, uint8_t proto, const void *l4, uint16_t len)
{
    uint32_t sum = cksum_pseudo4(sip, dip, proto, len);
    return cksum_fold(cksum_partial(l4, len, sum)) == 0;
}

/* Wrap an already-built L4 buffer in Ethernet+IPv4; returns frame length. */
static uint32_t nettest_wrap(uint8_t *frame, const uint8_t dmac[6], const uint8_t smac[6],
                             uint32_t sip, uint32_t dip, uint8_t ttl, uint8_t proto,
                             const void *l4, uint16_t l4len)
{
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    memcpy(eh->dst, dmac, 6);
    memcpy(eh->src, smac, 6);
    eh->type = htons(ETH_P_IP);
    struct ipv4_hdr *iph = (struct ipv4_hdr *)(frame + ETH_HLEN);
    uint16_t total = (uint16_t)(sizeof(*iph) + l4len);
    iph->vhl = 0x45; iph->tos = 0; iph->len = htons(total);
    iph->id = htons(0x2000); iph->frag = 0; iph->ttl = ttl;
    iph->proto = proto; iph->cksum = 0; iph->src = sip; iph->dst = dip;
    iph->cksum = in_cksum(iph, sizeof(*iph));
    memcpy(frame + ETH_HLEN + sizeof(*iph), l4, l4len);
    return ETH_HLEN + total;
}

/* Build a UDP datagram (header+payload) into l4, checksum valid. */
static uint16_t nettest_mk_udp(uint8_t *l4, uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp,
                               const uint8_t *pl, uint16_t pllen)
{
    struct udp_hdr *uh = (struct udp_hdr *)l4;
    uint16_t len = (uint16_t)(sizeof(*uh) + pllen);
    uh->sport = htons(sp); uh->dport = htons(dp); uh->len = htons(len); uh->cksum = 0;
    memcpy(l4 + sizeof(*uh), pl, pllen);
    uint16_t c = nettest_l4cksum(sip, dip, IPPROTO_UDP, l4, len);
    uh->cksum = c ? c : 0xffff;
    return len;
}

/* Build a bare TCP segment (flags given, no payload) into l4, checksum valid. */
static uint16_t nettest_mk_tcp(uint8_t *l4, uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, uint8_t flags)
{
    struct tcp_hdr *th = (struct tcp_hdr *)l4;
    memset(th, 0, sizeof(*th));
    th->sport = htons(sp); th->dport = htons(dp);
    th->seq = htonl(0x11223344); th->doff = 5 << 4; th->flags = flags; th->win = htons(64240);
    th->cksum = nettest_l4cksum(sip, dip, IPPROTO_TCP, l4, sizeof(*th));
    return sizeof(*th);
}

/* Build an ICMP echo (type 8) or reply (type 0) into l4, checksum valid. */
static uint16_t nettest_mk_icmp(uint8_t *l4, uint8_t type, uint16_t id, uint16_t seq,
                                const uint8_t *pl, uint16_t pllen)
{
    struct icmp_hdr *ic = (struct icmp_hdr *)l4;
    ic->type = type; ic->code = 0; ic->cksum = 0; ic->id = htons(id); ic->seq = htons(seq);
    memcpy(l4 + sizeof(*ic), pl, pllen);
    uint16_t len = (uint16_t)(sizeof(*ic) + pllen);
    ic->cksum = in_cksum(l4, len);   /* ICMPv4: no pseudo-header */
    return len;
}

bool selftest_net_nat(const char **reason)
{
    static const uint8_t g_mac[6]     = { 0x52, 0x54, 0x00, 0x03, 0x00, 0x01 };
    static const uint8_t u_mac[6]     = { 0x52, 0x54, 0x00, 0x04, 0x00, 0x01 };
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x03, 0x00, 0x0f };
    static const uint8_t peer_mac[6]  = { 0x52, 0x54, 0x00, 0x04, 0x00, 0x63 };
    uint32_t mask = htonl(0xffffff00u);
    uint32_t g_ip = IPV4_ADDR(10, 77, 3, 1), guest = IPV4_ADDR(10, 77, 3, 15);
    uint32_t u_ip = IPV4_ADDR(10, 77, 4, 1), peer = IPV4_ADDR(10, 77, 4, 99);

    struct tap *g = tap_create("natg", g_ip, mask, g_mac);
    CHECK(g != NULL);
    struct tap *u = tap_create("natu", u_ip, mask, u_mac);
    CHECK(u != NULL);
    netif_set_forward(tap_netif(g), true);
    netif_set_masquerade(tap_netif(g), true);   /* masquerade flows forwarded from the guest tap */
    nettest_seed_arp(tap_netif(u), peer, peer_mac);
    nettest_seed_arp(tap_netif(g), guest, guest_mac);
    nat_flush();

    uint8_t payload[12];
    for (unsigned i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(0x40 + i);
    uint8_t frame[256], l4[128], hdr[ETH_HLEN + 20 + 40];

    /* (1) UDP round trip: out masqueraded, reply un-masqueraded. */
    uint16_t l4len = nettest_mk_udp(l4, guest, peer, 6001, 7001, payload, sizeof(payload));
    uint32_t flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    struct mbuf *out = nettest_recv_ip(u);
    CHECK(out != NULL);
    CHECK(m_copydata(out, 0, ETH_HLEN + 20 + (int)sizeof(payload) + 8, hdr));
    struct ipv4_hdr *oi = (struct ipv4_hdr *)(hdr + ETH_HLEN);
    CHECK(oi->src == u_ip && oi->dst == peer && oi->ttl == 63);     /* source masqueraded */
    uint8_t *ol4 = hdr + ETH_HLEN + 20;
    uint16_t nat_port = (uint16_t)(ol4[0] << 8 | ol4[1]);           /* the lent source port */
    CHECK(nat_port >= NAT_PORT_MIN && nat_port <= NAT_PORT_MAX);
    CHECK((uint16_t)(ol4[2] << 8 | ol4[3]) == 7001);               /* dest port unchanged */
    CHECK(nettest_l4_ok(u_ip, peer, IPPROTO_UDP, ol4, (uint16_t)(sizeof(struct udp_hdr) + sizeof(payload))));
    m_freem(out);

    l4len = nettest_mk_udp(l4, peer, u_ip, 7001, nat_port, payload, sizeof(payload));
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    struct mbuf *back = nettest_recv_ip(g);
    CHECK(back != NULL);
    CHECK(m_copydata(back, 0, ETH_HLEN + 20 + (int)sizeof(payload) + 8, hdr));
    struct ipv4_hdr *bi = (struct ipv4_hdr *)(hdr + ETH_HLEN);
    CHECK(bi->src == peer && bi->dst == guest && bi->ttl == 63);    /* delivered to the guest */
    uint8_t *bl4 = hdr + ETH_HLEN + 20;
    CHECK((uint16_t)(bl4[2] << 8 | bl4[3]) == 6001);               /* original guest port restored */
    CHECK(nettest_l4_ok(peer, guest, IPPROTO_UDP, bl4, (uint16_t)(sizeof(struct udp_hdr) + sizeof(payload))));
    CHECK(memcmp(bl4 + sizeof(struct udp_hdr), payload, sizeof(payload)) == 0);
    m_freem(back);

    /* (2) TCP SYN out, SYN-ACK back. */
    l4len = nettest_mk_tcp(l4, guest, peer, 6002, 80, TH_SYN);
    flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    out = nettest_recv_ip(u);
    CHECK(out != NULL);
    CHECK(m_copydata(out, 0, ETH_HLEN + 20 + 20, hdr));
    ol4 = hdr + ETH_HLEN + 20;
    uint16_t tcp_nat = (uint16_t)(ol4[0] << 8 | ol4[1]);
    CHECK(((struct ipv4_hdr *)(hdr + ETH_HLEN))->src == u_ip);
    CHECK(nettest_l4_ok(u_ip, peer, IPPROTO_TCP, ol4, sizeof(struct tcp_hdr)));
    m_freem(out);

    l4len = nettest_mk_tcp(l4, peer, u_ip, 80, tcp_nat, TH_SYN | TH_ACK);
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    back = nettest_recv_ip(g);
    CHECK(back != NULL);
    CHECK(m_copydata(back, 0, ETH_HLEN + 20 + 20, hdr));
    bl4 = hdr + ETH_HLEN + 20;
    CHECK(((struct ipv4_hdr *)(hdr + ETH_HLEN))->dst == guest);
    CHECK((uint16_t)(bl4[2] << 8 | bl4[3]) == 6002);
    CHECK(nettest_l4_ok(peer, guest, IPPROTO_TCP, bl4, sizeof(struct tcp_hdr)));
    m_freem(back);

    /* (3) ICMP echo out, echo reply back (the id is the NAT identifier). */
    l4len = nettest_mk_icmp(l4, ICMP_ECHO, 0x4321, 1, payload, sizeof(payload));
    flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_ICMP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    out = nettest_recv_ip(u);
    CHECK(out != NULL);
    CHECK(m_copydata(out, 0, ETH_HLEN + 20 + (int)sizeof(payload) + 8, hdr));
    ol4 = hdr + ETH_HLEN + 20;
    uint16_t icmp_nat = (uint16_t)(ol4[4] << 8 | ol4[5]);
    CHECK(((struct ipv4_hdr *)(hdr + ETH_HLEN))->src == u_ip && ol4[0] == ICMP_ECHO);
    CHECK(in_cksum(ol4, (uint16_t)(sizeof(struct icmp_hdr) + sizeof(payload))) == 0);
    m_freem(out);

    l4len = nettest_mk_icmp(l4, ICMP_ECHO_REPLY, icmp_nat, 1, payload, sizeof(payload));
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_ICMP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    back = nettest_recv_ip(g);
    CHECK(back != NULL);
    CHECK(m_copydata(back, 0, ETH_HLEN + 20 + (int)sizeof(payload) + 8, hdr));
    bl4 = hdr + ETH_HLEN + 20;
    CHECK(((struct ipv4_hdr *)(hdr + ETH_HLEN))->dst == guest && bl4[0] == ICMP_ECHO_REPLY);
    CHECK((uint16_t)(bl4[4] << 8 | bl4[5]) == 0x4321);            /* original id restored */
    CHECK(in_cksum(bl4, (uint16_t)(sizeof(struct icmp_hdr) + sizeof(payload))) == 0);
    m_freem(back);

    /* (4) ICMP error quoting a NAT'd packet is translated back to the guest.
     * First open a UDP flow, then deliver a dest-unreach whose quote is the
     * packet we sent out. */
    nat_flush();
    l4len = nettest_mk_udp(l4, guest, peer, 6100, 53, payload, sizeof(payload));
    flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    out = nettest_recv_ip(u);
    CHECK(out != NULL);
    CHECK(m_copydata(out, 0, ETH_HLEN + 20 + 8, hdr));
    ol4 = hdr + ETH_HLEN + 20;
    uint16_t err_nat = (uint16_t)(ol4[0] << 8 | ol4[1]);
    m_freem(out);
    /* Build the ICMP error: [icmp hdr][quoted IP: u_ip->peer UDP][8 bytes of
     * that UDP: sport=err_nat, dport=53]. */
    uint8_t icmperr[8 + 20 + 8];
    memset(icmperr, 0, sizeof(icmperr));
    icmperr[0] = ICMP_DEST_UNREACH; icmperr[1] = ICMP_UNREACH_PORT;
    struct ipv4_hdr *q = (struct ipv4_hdr *)(icmperr + 8);
    q->vhl = 0x45; q->len = htons(20 + 8); q->ttl = 63; q->proto = IPPROTO_UDP;
    q->src = u_ip; q->dst = peer; q->cksum = in_cksum(q, 20);
    uint8_t *qudp = icmperr + 8 + 20;
    qudp[0] = (uint8_t)(err_nat >> 8); qudp[1] = (uint8_t)err_nat;   /* sport = the lent port */
    qudp[2] = 0; qudp[3] = 53;                                        /* dport */
    uint16_t *ecs = (uint16_t *)(icmperr + 2);
    *ecs = 0; *ecs = in_cksum(icmperr, sizeof(icmperr));
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_ICMP, icmperr, sizeof(icmperr));
    CHECK(tap_inject(u, frame, flen) == 0);
    back = nettest_recv_ip(g);
    CHECK(back != NULL);
    CHECK(m_copydata(back, 0, ETH_HLEN + 20 + 8 + 20 + 8, hdr));
    bi = (struct ipv4_hdr *)(hdr + ETH_HLEN);
    CHECK(bi->dst == guest && bi->proto == IPPROTO_ICMP);
    struct ipv4_hdr *iq = (struct ipv4_hdr *)(hdr + ETH_HLEN + 20 + 8);
    CHECK(iq->src == guest);                                         /* inner src un-NAT'd */
    uint8_t *iu = hdr + ETH_HLEN + 20 + 8 + 20;
    CHECK((uint16_t)(iu[0] << 8 | iu[1]) == 6100);                  /* inner source port restored */
    m_freem(back);

    /* (4b) Anti-spoof: a guest frame sourced from the uplink subnet (not the
     * guest tap's own) is dropped on forwarding, never masqueraded or
     * emitted with the forged source. */
    struct ip_stats is0, is1;
    ipv4_get_stats(&is0);
    l4len = nettest_mk_udp(l4, IPV4_ADDR(10, 77, 4, 5), peer, 6200, 53, payload, sizeof(payload));
    flen = nettest_wrap(frame, g_mac, guest_mac, IPV4_ADDR(10, 77, 4, 5), peer, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    CHECK(nettest_recv_ip(u) == NULL);                             /* forged source never leaves */
    ipv4_get_stats(&is1);
    CHECK(is1.fwd_spoofed > is0.fwd_spoofed);

    /* (4c) An ICMP error with a bad checksum is not translated (the normal
     * receive path would drop it; nat_in must not launder it). Reuse the
     * error frame but corrupt the ICMP checksum. */
    *ecs = (uint16_t)(*ecs ^ htons(0x1));
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_ICMP, icmperr, sizeof(icmperr));
    CHECK(tap_inject(u, frame, flen) == 0);
    CHECK(nettest_recv_ip(g) == NULL);                             /* corrupt error not forwarded */

    /* (5) Table exhaustion: many distinct flows fill the table; further ones
     * are dropped, and the table does not grow past its bound. */
    nat_flush();
    struct nat_stats ns0, ns1;
    nat_get_stats(&ns0);
    for (unsigned i = 0; i < NAT_TABLE_SIZE + 8; i++) {
        l4len = nettest_mk_udp(l4, guest, peer, (uint16_t)(10000 + i), 9, payload, 4);
        flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_UDP, l4, l4len);
        tap_inject(g, frame, flen);
        if ((i & 31) == 31) {                 /* keep the uplink queue drained */
            struct mbuf *d;
            while ((d = tap_recv(u)) != NULL)
                m_freem(d);
        }
    }
    for (unsigned i = 0; i < 200; i++) {
        struct mbuf *d;
        while ((d = tap_recv(u)) != NULL)
            m_freem(d);
        nat_get_stats(&ns1);
        if (ns1.out_new + ns1.out_drop_full >= NAT_TABLE_SIZE + 8)
            break;
        thread_sleep_ms(10);
    }
    nat_get_stats(&ns1);
    CHECK(ns1.entries == NAT_QUOTA_PER_GUEST);            /* one guest is capped at its quota */
    CHECK(ns1.out_drop_full > ns0.out_drop_full);        /* new flows dropped once full */

    /* (6) Expiry: aging past the timeout reclaims the entries. */
    nat_get_stats(&ns0);
    CHECK(ns0.entries > 0);
    nat_age(clock_now_ns() + 2ull * NAT_TIMEOUT_UDP_NS);
    nat_get_stats(&ns1);
    CHECK(ns1.entries == 0 && ns1.expired > ns0.expired);

    nat_flush();
    tap_destroy(u);
    tap_destroy(g);
    kinfo("selftest: net-nat: UDP/TCP/ICMP round trips masqueraded and restored (checksums valid), "
          "an ICMP error translated back, the table bounded at %u and its entries expiring", NAT_TABLE_SIZE);
    return true;
}

/* --- tap input filter (the DHCP responder's ingress/egress mechanism) ----- */

/* Claim frames of a private ethertype and answer each with a canned reply
 * built and sent back out the tap; pass everything else to the stack. */
#define TAPFILT_ETYPE 0x88b5u
static const uint8_t tapfilt_reply[4] = { 0xC0, 0xDE, 0xCA, 0xFE };

static bool tapfilt_hook(struct tap *t, const void *frame, uint32_t len, void *arg)
{
    (void)len;
    unsigned *calls = arg;
    const uint8_t *f = frame;
    uint16_t etype = (uint16_t)(f[12] << 8 | f[13]);
    if (etype != TAPFILT_ETYPE)
        return false;                       /* not ours: let the stack have it */
    (*calls)++;
    struct mbuf *m = m_getcl();
    if (m != NULL) {
        memcpy(m->data, tapfilt_reply, sizeof(tapfilt_reply));
        m->len = m->pkt.len = sizeof(tapfilt_reply);
        uint8_t dst[6];
        memcpy(dst, f + 6, 6);              /* reply to the injector's source MAC */
        ether_output(tap_netif(t), m, dst, TAPFILT_ETYPE);
    }
    return true;                            /* claimed */
}

bool selftest_tap_filter(const char **reason)
{
    static const uint8_t host_mac[6]  = { 0x52, 0x54, 0x00, 0x05, 0x00, 0x01 };
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x05, 0x00, 0x0f };
    uint32_t host_ip = IPV4_ADDR(10, 88, 0, 1), guest_ip = IPV4_ADDR(10, 88, 0, 15);
    struct tap *t = tap_create("filt", host_ip, htonl(0xffffff00u), host_mac);
    CHECK(t != NULL);
    unsigned calls = 0;
    tap_set_input_filter(t, tapfilt_hook, &calls);

    /* (1) a claimed frame is answered out the tap, and the stack never sees it. */
    uint8_t f[60];
    memset(f, 0, sizeof(f));
    memcpy(f, host_mac, 6);
    memcpy(f + 6, guest_mac, 6);
    f[12] = 0x88; f[13] = 0xb5;
    CHECK(tap_inject(t, f, sizeof(f)) == 0);
    CHECK(calls == 1);
    struct mbuf *r = tap_recv(t);
    CHECK(r != NULL);
    uint8_t out[18];
    CHECK(m_copydata(r, 0, 18, out));
    CHECK(memcmp(out, guest_mac, 6) == 0);              /* to the injector */
    CHECK(out[12] == 0x88 && out[13] == 0xb5);
    CHECK(memcmp(out + 14, tapfilt_reply, 4) == 0);
    m_freem(r);
    CHECK(tap_recv(t) == NULL);

    /* (2) an unclaimed frame reaches the stack: an ARP request for the tap IP
     * is answered by the stack itself, not the filter. */
    uint8_t req[42];
    memset(req, 0, sizeof(req));
    memset(req, 0xff, 6);
    memcpy(req + 6, guest_mac, 6);
    req[12] = 0x08; req[13] = 0x06;
    req[15] = 1; req[16] = 0x08; req[18] = 6; req[19] = 4; req[21] = 1;
    memcpy(req + 22, guest_mac, 6);
    memcpy(req + 28, &guest_ip, 4);
    memcpy(req + 38, &host_ip, 4);
    CHECK(tap_inject(t, req, sizeof(req)) == 0);
    struct mbuf *arp = NULL;
    for (unsigned i = 0; i < 50 && arp == NULL; i++) {
        arp = tap_recv(t);
        if (arp == NULL)
            thread_sleep_ms(10);
    }
    CHECK(arp != NULL);
    uint8_t a[42];
    CHECK(m_copydata(arp, 0, 42, a) && a[12] == 0x08 && a[13] == 0x06 && a[21] == 2);
    m_freem(arp);
    CHECK(calls == 1);                                  /* the filter did not touch the ARP */

    /* (3) clearing the filter lets a formerly-claimed frame reach the stack. */
    tap_set_input_filter(t, NULL, NULL);
    CHECK(tap_inject(t, f, sizeof(f)) == 0);
    CHECK(calls == 1 && tap_recv(t) == NULL);           /* no reply: the stack drops the unknown type */

    tap_destroy(t);
    kinfo("selftest: tap-filter: a claimed frame answered out the tap, an unclaimed frame reached the stack");
    return true;
}

/* --- DHCP server (tapsvc) -------------------------------------------------- */

/* Build a DHCP client frame (Ethernet+IP+UDP+BOOTP+cookie+options) into buf.
 * The DHCP server (a tap input filter) does not verify the UDP checksum, so
 * it is left 0; the IP header checksum is valid. Returns the frame length. */
static uint32_t nettest_mk_dhcp(uint8_t *buf, const uint8_t smac[6], uint8_t msgtype,
                                uint32_t xid, bool bcast_flag, const uint8_t chaddr[6],
                                uint32_t req_ip)
{
    memset(buf, 0, 400);
    struct eth_hdr *eh = (struct eth_hdr *)buf;
    memset(eh->dst, 0xff, 6);
    memcpy(eh->src, smac, 6);
    eh->type = htons(ETH_P_IP);
    uint8_t *dh = buf + ETH_HLEN + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr);
    dh[0] = 1;              /* op BOOTREQUEST */
    dh[1] = 1; dh[2] = 6;   /* htype ETHER, hlen 6 */
    memcpy(dh + 4, &xid, 4);
    uint16_t flags = htons(bcast_flag ? 0x8000u : 0u);
    memcpy(dh + 10, &flags, 2);
    memcpy(dh + 28, chaddr, 6);
    uint32_t cookie = htonl(0x63825363u);
    memcpy(dh + 236, &cookie, 4);
    uint8_t *o = dh + 240;
    *o++ = 53; *o++ = 1; *o++ = msgtype;
    if (req_ip) { *o++ = 50; *o++ = 4; memcpy(o, &req_ip, 4); o += 4; }
    *o++ = 255;
    uint32_t dhlen = (uint32_t)(o - dh);
    uint32_t udplen = (uint32_t)sizeof(struct udp_hdr) + dhlen;
    uint32_t iplen = (uint32_t)sizeof(struct ipv4_hdr) + udplen;
    struct udp_hdr *uh = (struct udp_hdr *)(buf + ETH_HLEN + sizeof(struct ipv4_hdr));
    uh->sport = htons(68); uh->dport = htons(67); uh->len = htons((uint16_t)udplen); uh->cksum = 0;
    struct ipv4_hdr *iph = (struct ipv4_hdr *)(buf + ETH_HLEN);
    iph->vhl = 0x45; iph->tos = 0; iph->len = htons((uint16_t)iplen); iph->id = 0; iph->frag = 0;
    iph->ttl = 64; iph->proto = IPPROTO_UDP; iph->cksum = 0;
    iph->src = 0; iph->dst = INADDR_BROADCAST_N;
    iph->cksum = in_cksum(iph, sizeof(*iph));
    return ETH_HLEN + iplen;
}

/* Find DHCP option `code` in a received reply frame; NULL or a pointer+len. */
static const uint8_t *nettest_dhcp_opt(const uint8_t *dh, uint32_t dhlen, uint8_t code, uint8_t *olen)
{
    uint32_t i = 240;   /* past BOOTP + cookie */
    while (i < dhlen) {
        uint8_t c = dh[i++];
        if (c == 255) break;
        if (c == 0) continue;
        if (i >= dhlen) break;
        uint8_t l = dh[i++];
        if (i + l > dhlen) break;
        if (c == code) { *olen = l; return dh + i; }
        i += l;
    }
    return NULL;
}

bool selftest_net_dhcp(const char **reason)
{
    static const uint8_t host_mac[6]  = { 0x52, 0x54, 0x00, 0x06, 0x00, 0x01 };
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x06, 0x00, 0x0f };
    static const uint8_t other_mac[6] = { 0x52, 0x54, 0x00, 0x06, 0x00, 0xaa };
    uint32_t host_ip = IPV4_ADDR(10, 88, 1, 1), mask = htonl(0xffffff00u);
    uint32_t guest_ip = IPV4_ADDR(10, 88, 1, 15);
    struct tap *t = tap_create("dhcp", host_ip, mask, host_mac);
    CHECK(t != NULL);
    struct tapsvc *svc = tapsvc_start(t);
    CHECK(svc != NULL);

    uint8_t frame[400], rx[400];

    /* (1) DISCOVER with the broadcast flag -> OFFER as the limited broadcast
     * at both layers, carrying the guest address and the right options. */
    uint32_t len = nettest_mk_dhcp(frame, guest_mac, DHCP_DISCOVER, 0xAABBCCDD, true, guest_mac, 0);
    CHECK(tap_inject(t, frame, len) == 0);
    struct mbuf *r = tap_recv(t);
    CHECK(r != NULL);
    uint32_t rl = m_length(r);
    CHECK(rl <= sizeof(rx) && m_copydata(r, 0, rl, rx));
    m_freem(r);
    CHECK(memcmp(rx, "\xff\xff\xff\xff\xff\xff", 6) == 0);          /* Ethernet broadcast */
    struct ipv4_hdr *oi = (struct ipv4_hdr *)(rx + ETH_HLEN);
    CHECK(oi->dst == INADDR_BROADCAST_N && oi->src == host_ip);     /* IP limited broadcast */
    uint8_t *dh = rx + ETH_HLEN + 20 + 8;
    uint32_t dhlen = rl - (ETH_HLEN + 20 + 8);
    CHECK(dh[0] == 2);                                              /* BOOTREPLY */
    CHECK(memcmp(dh + 16, &guest_ip, 4) == 0);                     /* yiaddr = the guest */
    uint8_t l; const uint8_t *op;
    op = nettest_dhcp_opt(dh, dhlen, 53, &l); CHECK(op && l == 1 && op[0] == DHCP_OFFER);
    op = nettest_dhcp_opt(dh, dhlen, 54, &l); CHECK(op && l == 4 && memcmp(op, &host_ip, 4) == 0);
    op = nettest_dhcp_opt(dh, dhlen, 1,  &l); CHECK(op && l == 4 && memcmp(op, &mask, 4) == 0);
    op = nettest_dhcp_opt(dh, dhlen, 3,  &l); CHECK(op && l == 4 && memcmp(op, &host_ip, 4) == 0);
    op = nettest_dhcp_opt(dh, dhlen, 6,  &l); CHECK(op && l == 4 && memcmp(op, &host_ip, 4) == 0);
    op = nettest_dhcp_opt(dh, dhlen, 51, &l); CHECK(op && l == 4);

    /* (2) REQUEST for that address -> ACK. */
    len = nettest_mk_dhcp(frame, guest_mac, DHCP_REQUEST, 0xAABBCCDD, true, guest_mac, guest_ip);
    CHECK(tap_inject(t, frame, len) == 0);
    r = tap_recv(t); CHECK(r != NULL);
    rl = m_length(r); CHECK(rl <= sizeof(rx) && m_copydata(r, 0, rl, rx)); m_freem(r);
    dh = rx + ETH_HLEN + 20 + 8; dhlen = rl - (ETH_HLEN + 20 + 8);
    op = nettest_dhcp_opt(dh, dhlen, 53, &l); CHECK(op && op[0] == DHCP_ACK);

    /* (3) REQUEST for a different address -> NAK (always broadcast). */
    len = nettest_mk_dhcp(frame, guest_mac, DHCP_REQUEST, 0xAABBCCDD, true, guest_mac,
                          IPV4_ADDR(10, 88, 1, 200));
    CHECK(tap_inject(t, frame, len) == 0);
    r = tap_recv(t); CHECK(r != NULL);
    rl = m_length(r); CHECK(rl <= sizeof(rx) && m_copydata(r, 0, rl, rx)); m_freem(r);
    dh = rx + ETH_HLEN + 20 + 8; dhlen = rl - (ETH_HLEN + 20 + 8);
    op = nettest_dhcp_opt(dh, dhlen, 53, &l); CHECK(op && op[0] == DHCP_NAK);

    /* (4) A DISCOVER with the flag clear -> OFFER unicast to chaddr, IP to yiaddr. */
    len = nettest_mk_dhcp(frame, guest_mac, DHCP_DISCOVER, 0x11223344, false, guest_mac, 0);
    CHECK(tap_inject(t, frame, len) == 0);
    r = tap_recv(t); CHECK(r != NULL);
    rl = m_length(r); CHECK(rl <= sizeof(rx) && m_copydata(r, 0, rl, rx)); m_freem(r);
    CHECK(memcmp(rx, guest_mac, 6) == 0);                          /* link-unicast to the client */
    oi = (struct ipv4_hdr *)(rx + ETH_HLEN);
    CHECK(oi->dst == guest_ip);                                    /* IP unicast to yiaddr */

    /* (5) A second, different client is offered nothing (the lease is taken). */
    struct tapsvc_stats s0, s1;
    tapsvc_get_stats(&s0);
    len = nettest_mk_dhcp(frame, other_mac, DHCP_DISCOVER, 0x99999999, true, other_mac, 0);
    CHECK(tap_inject(t, frame, len) == 0);
    CHECK(tap_recv(t) == NULL);                                    /* no reply */
    tapsvc_get_stats(&s1);
    CHECK(s1.dhcp_ignored > s0.dhcp_ignored);

    tapsvc_stop(svc);
    tap_destroy(t);
    kinfo("selftest: net-dhcp: DORA completes with the guest's config, a wrong REQUEST is NAK'd, "
          "the flag-clear reply is a chaddr unicast, a second client is refused");
    return true;
}

/* --- DNS proxy (tapsvc) ---------------------------------------------------- */

/* A fixed DNS query for "www.example.com" A IN, with a given id and RD set. */
static uint32_t nettest_mk_dns(uint8_t *msg, uint16_t id)
{
    static const uint8_t qname[] = { 3,'w','w','w', 7,'e','x','a','m','p','l','e', 3,'c','o','m', 0 };
    msg[0] = (uint8_t)(id >> 8); msg[1] = (uint8_t)id;
    msg[2] = 0x01; msg[3] = 0x00;         /* flags: RD */
    msg[4] = 0; msg[5] = 1;               /* qdcount 1 */
    msg[6] = 0; msg[7] = 0; msg[8] = 0; msg[9] = 0; msg[10] = 0; msg[11] = 0;
    memcpy(msg + 12, qname, sizeof(qname));
    uint32_t o = 12 + (uint32_t)sizeof(qname);
    msg[o++] = 0; msg[o++] = 1;           /* qtype A */
    msg[o++] = 0; msg[o++] = 1;           /* qclass IN */
    return o;
}

/* The test upstream resolver: echo each query as an answer with one A record. */
static const uint8_t dns_answer_ip[4] = { 93, 184, 216, 34 };
static struct { struct socket *sock; struct socket *spoof; volatile bool running; volatile bool spoofing; } g_dnsresp;

static void dns_responder_main(void *arg)
{
    (void)arg;
    uint8_t q[512];
    while (g_dnsresp.running) {
        struct netaddr from;
        int64_t n = ksock_recvfrom(g_dnsresp.sock, q, sizeof(q), &from);
        if (n < 12) { if (n <= 0) break; continue; }
        /* Build the answer in place: keep id + question, set response flags,
         * ancount 1, append an A record pointing at the question name. */
        q[2] = 0x81; q[3] = 0x80;         /* QR + RD + RA */
        q[6] = 0; q[7] = 1;               /* ancount 1 */
        uint32_t o = (uint32_t)n;
        q[o++] = 0xc0; q[o++] = 0x0c;     /* name pointer to offset 12 */
        q[o++] = 0; q[o++] = 1;           /* type A */
        q[o++] = 0; q[o++] = 1;           /* class IN */
        q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 4;   /* ttl */
        q[o++] = 0; q[o++] = 4;           /* rdlength */
        memcpy(q + o, dns_answer_ip, 4); o += 4;
        /* Normally reply from the configured upstream; in spoof mode reply from
         * a different source, which the proxy must reject. */
        ksock_sendto(g_dnsresp.spoofing ? g_dnsresp.spoof : g_dnsresp.sock, q, o, &from);
    }
    thread_exit(0);
}

bool selftest_net_dns(const char **reason)
{
    static const uint8_t host_mac[6]  = { 0x52, 0x54, 0x00, 0x07, 0x00, 0x01 };
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x07, 0x00, 0x0f };
    uint32_t host_ip = IPV4_ADDR(10, 88, 2, 1), mask = htonl(0xffffff00u);
    uint32_t guest_ip = IPV4_ADDR(10, 88, 2, 15);
    struct tap *t = tap_create("dns", host_ip, mask, host_mac);
    CHECK(t != NULL);
    nettest_seed_arp(tap_netif(t), guest_ip, guest_mac);   /* so replies reach the guest */
    struct tapsvc *svc = tapsvc_start(t);
    CHECK(svc != NULL);

    /* A loopback upstream resolver the proxy will forward to. */
    struct netaddr rl;
    memset(&rl, 0, sizeof(rl));
    rl.family = COSMO_AF_INET; rl.v4 = IPV4_ADDR(127, 0, 0, 1); rl.port = 5300;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &g_dnsresp.sock) == 0);
    CHECK(ksock_bind(g_dnsresp.sock, &rl) == 0);
    struct netaddr sp;
    memset(&sp, 0, sizeof(sp));
    sp.family = COSMO_AF_INET; sp.v4 = IPV4_ADDR(127, 0, 0, 1); sp.port = 5301;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &g_dnsresp.spoof) == 0);
    CHECK(ksock_bind(g_dnsresp.spoof, &sp) == 0);
    g_dnsresp.running = true;
    struct thread *rth = thread_create(dns_responder_main, NULL, "dns-resp", SCHED_PRIO_DEFAULT);
    CHECK(rth != NULL);
    tapsvc_test_set_upstream(svc, IPV4_ADDR(127, 0, 0, 1), 5300);

    uint8_t msg[64], l4[128], frame[256], rx[256];

    /* (1) a guest query is relayed and the answer returns with the guest's
     * original id and the upstream's A record. */
    uint32_t mlen = nettest_mk_dns(msg, 0x1234);
    uint16_t l4len = nettest_mk_udp(l4, guest_ip, host_ip, 4444, 53, msg, (uint16_t)mlen);
    uint32_t flen = nettest_wrap(frame, host_mac, guest_mac, guest_ip, host_ip, 64,
                                 IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(t, frame, flen) == 0);
    struct mbuf *r = nettest_recv_ip(t);
    CHECK(r != NULL);
    uint32_t rl2 = m_length(r);
    CHECK(rl2 <= sizeof(rx) && m_copydata(r, 0, rl2, rx));
    m_freem(r);
    struct ipv4_hdr *ri = (struct ipv4_hdr *)(rx + ETH_HLEN);
    CHECK(ri->dst == guest_ip && ri->proto == IPPROTO_UDP);
    uint8_t *rudp = rx + ETH_HLEN + 20;
    CHECK((uint16_t)(rudp[2] << 8 | rudp[3]) == 4444);           /* back to the guest's port */
    uint8_t *dns = rudp + 8;
    CHECK((uint16_t)(dns[0] << 8 | dns[1]) == 0x1234);           /* the guest's original id */
    CHECK((dns[2] & 0x80) && (uint16_t)(dns[6] << 8 | dns[7]) >= 1);   /* a response with answers */
    uint32_t anofs = rl2 - (ETH_HLEN + 20 + 8);
    CHECK(anofs >= 4 && memcmp(rx + rl2 - 4, dns_answer_ip, 4) == 0);   /* the A record's address */

    /* (2) two queries sharing an id but different source ports stay
     * unambiguous: both answers come back to the right ports. */
    mlen = nettest_mk_dns(msg, 0x5555);
    l4len = nettest_mk_udp(l4, guest_ip, host_ip, 5001, 53, msg, (uint16_t)mlen);
    flen = nettest_wrap(frame, host_mac, guest_mac, guest_ip, host_ip, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(t, frame, flen) == 0);
    l4len = nettest_mk_udp(l4, guest_ip, host_ip, 5002, 53, msg, (uint16_t)mlen);
    flen = nettest_wrap(frame, host_mac, guest_mac, guest_ip, host_ip, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(t, frame, flen) == 0);
    unsigned seen_ports = 0;
    for (unsigned got = 0; got < 2; got++) {
        r = nettest_recv_ip(t);
        CHECK(r != NULL);
        CHECK(m_copydata(r, 0, ETH_HLEN + 20 + 8 + 2, rx)); m_freem(r);
        rudp = rx + ETH_HLEN + 20;
        uint16_t port = (uint16_t)(rudp[2] << 8 | rudp[3]);
        dns = rudp + 8;
        CHECK((uint16_t)(dns[0] << 8 | dns[1]) == 0x5555);
        if (port == 5001) seen_ports |= 1;
        if (port == 5002) seen_ports |= 2;
    }
    CHECK(seen_ports == 3);                                     /* both, unambiguous */

    /* (3) with no upstream configured, the proxy answers SERVFAIL itself. */
    tapsvc_test_set_upstream(svc, 0, 0);                            /* clear the upstream */
    mlen = nettest_mk_dns(msg, 0x7777);
    l4len = nettest_mk_udp(l4, guest_ip, host_ip, 6001, 53, msg, (uint16_t)mlen);
    flen = nettest_wrap(frame, host_mac, guest_mac, guest_ip, host_ip, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(t, frame, flen) == 0);
    r = nettest_recv_ip(t);
    CHECK(r != NULL);
    CHECK(m_copydata(r, 0, ETH_HLEN + 20 + 8 + 4, rx)); m_freem(r);
    dns = rx + ETH_HLEN + 20 + 8;
    CHECK((uint16_t)(dns[0] << 8 | dns[1]) == 0x7777);          /* id preserved */
    CHECK((dns[2] & 0x80) && (dns[3] & 0x0f) == 2);            /* QR + RCODE 2 (SERVFAIL) */

    /* (3b) a response from a source other than the configured upstream is
     * rejected -- an off-path attacker cannot race a forged answer into the
     * guest even if it guesses the id. */
    tapsvc_test_set_upstream(svc, IPV4_ADDR(127, 0, 0, 1), 5300);
    struct tapsvc_stats sp0, sp1;
    tapsvc_get_stats(&sp0);
    g_dnsresp.spoofing = true;                                 /* responder replies from :5301 */
    mlen = nettest_mk_dns(msg, 0x2468);
    l4len = nettest_mk_udp(l4, guest_ip, host_ip, 6002, 53, msg, (uint16_t)mlen);
    flen = nettest_wrap(frame, host_mac, guest_mac, guest_ip, host_ip, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(t, frame, flen) == 0);
    CHECK(nettest_recv_ip(t) == NULL);                         /* the forged-source answer is dropped */
    tapsvc_get_stats(&sp1);
    CHECK(sp1.dns_answer == sp0.dns_answer);                   /* nothing was relayed */
    g_dnsresp.spoofing = false;

    /* (4) table bound: with the upstream a black hole, a flood fills the
     * pending table and further queries drop; it never exceeds the bound. */
    tapsvc_test_set_upstream(svc, IPV4_ADDR(127, 0, 0, 1), 1);      /* nothing answers there */
    struct tapsvc_stats s0, s1;
    tapsvc_get_stats(&s0);
    for (unsigned i = 0; i < 400; i++) {
        mlen = nettest_mk_dns(msg, (uint16_t)(0x8000 + i));
        l4len = nettest_mk_udp(l4, guest_ip, host_ip, (uint16_t)(20000 + i), 53, msg, (uint16_t)mlen);
        flen = nettest_wrap(frame, host_mac, guest_mac, guest_ip, host_ip, 64, IPPROTO_UDP, l4, l4len);
        tap_inject(t, frame, flen);
        if ((i & 15) == 15)
            thread_sleep_ms(5);                               /* let the guest thread drain */
    }
    for (unsigned i = 0; i < 100; i++) {
        tapsvc_get_stats(&s1);
        if (s1.dns_drop_full > s0.dns_drop_full)
            break;
        thread_sleep_ms(10);
    }
    tapsvc_get_stats(&s1);
    CHECK(s1.dns_pending <= 128);                              /* never exceeds the bound */
    CHECK(s1.dns_drop_full > s0.dns_drop_full);                /* the flood was dropped */

    /* (5) expiry reclaims the pending entries. */
    tapsvc_get_stats(&s0);
    CHECK(s0.dns_pending > 0);
    tapsvc_dns_age(clock_now_ns() + 2ull * 5ull * NS_PER_SEC);
    tapsvc_get_stats(&s1);
    CHECK(s1.dns_pending == 0 && s1.dns_expired > s0.dns_expired);

    /* Tear down the responder and the service. */
    g_dnsresp.running = false;
    ksock_shutdown(g_dnsresp.sock, COSMO_SHUT_RD);
    thread_join(rth);
    ksock_put(g_dnsresp.sock);
    ksock_put(g_dnsresp.spoof);
    g_dnsresp.sock = NULL;
    g_dnsresp.spoof = NULL;
    tapsvc_stop(svc);
    tap_destroy(t);
    kinfo("selftest: net-dns: a query is relayed and its answer restored to the guest, two queries "
          "sharing an id stay unambiguous, an unconfigured upstream is SERVFAIL, the table bounds and expires");
    return true;
}

/* --- inbound port forwarding (DNAT) --------------------------------------- */

bool selftest_net_dnat(const char **reason)
{
    static const uint8_t g_mac[6]     = { 0x52, 0x54, 0x00, 0x08, 0x00, 0x01 };
    static const uint8_t u_mac[6]     = { 0x52, 0x54, 0x00, 0x09, 0x00, 0x01 };
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x08, 0x00, 0x0f };
    static const uint8_t client_mac[6]= { 0x52, 0x54, 0x00, 0x09, 0x00, 0x63 };
    uint32_t mask = htonl(0xffffff00u);
    uint32_t g_ip = IPV4_ADDR(10, 77, 5, 1), guest = IPV4_ADDR(10, 77, 5, 15);
    uint32_t u_ip = IPV4_ADDR(10, 77, 6, 1), client = IPV4_ADDR(10, 77, 6, 99);

    struct tap *g = tap_create("dnatg", g_ip, mask, g_mac);
    CHECK(g != NULL);
    struct tap *u = tap_create("dnatu", u_ip, mask, u_mac);
    CHECK(u != NULL);
    netif_set_forward(tap_netif(g), true);        /* the guest side forwards its replies out */
    netif_set_masquerade(tap_netif(g), true);
    nettest_seed_arp(tap_netif(g), guest, guest_mac);    /* DNAT'd packets reach the guest */
    nettest_seed_arp(tap_netif(u), client, client_mac);  /* replies reach the client */
    nat_flush();
    nat_pf_clear();
    CHECK(nat_pf_add(IPPROTO_TCP, 8080, guest, 80) == 0);
    CHECK(nat_pf_add(IPPROTO_UDP, 9090, guest, 53) == 0);
    CHECK(nat_pf_add(IPPROTO_TCP, 8081, guest, 80) == 0);   /* same guest endpoint as 8080 */
    /* A rule whose target is not on a connected subnet (would route out the
     * default uplink and stall) is refused. */
    CHECK(nat_pf_add(IPPROTO_TCP, 7777, IPV4_ADDR(203, 0, 113, 5), 7777) != 0);

    uint8_t l4[128], frame[256], rx[256];

    /* (1) a client SYN to the host's uplink port 8080 is DNAT'd to the guest. */
    uint16_t l4len = nettest_mk_tcp(l4, client, u_ip, 12345, 8080, TH_SYN);
    uint32_t flen = nettest_wrap(frame, u_mac, client_mac, client, u_ip, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    struct mbuf *r = nettest_recv_ip(g);
    CHECK(r != NULL);
    CHECK(m_copydata(r, 0, ETH_HLEN + 20 + 20, rx)); m_freem(r);
    struct ipv4_hdr *ri = (struct ipv4_hdr *)(rx + ETH_HLEN);
    CHECK(ri->src == client && ri->dst == guest);          /* dst rewritten to the guest */
    uint8_t *rl4 = rx + ETH_HLEN + 20;
    CHECK((uint16_t)(rl4[0] << 8 | rl4[1]) == 12345);      /* source port intact */
    CHECK((uint16_t)(rl4[2] << 8 | rl4[3]) == 80);         /* dest port rewritten to the guest's */
    CHECK(nettest_l4_ok(client, guest, IPPROTO_TCP, rl4, sizeof(struct tcp_hdr)));

    /* (2) the guest's SYN-ACK is un-DNAT'd back to the client from host:8080. */
    l4len = nettest_mk_tcp(l4, guest, client, 80, 12345, TH_SYN | TH_ACK);
    flen = nettest_wrap(frame, g_mac, guest_mac, guest, client, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    r = nettest_recv_ip(u);
    CHECK(r != NULL);
    CHECK(m_copydata(r, 0, ETH_HLEN + 20 + 20, rx)); m_freem(r);
    ri = (struct ipv4_hdr *)(rx + ETH_HLEN);
    CHECK(ri->src == u_ip && ri->dst == client);           /* source rewritten to the host */
    rl4 = rx + ETH_HLEN + 20;
    CHECK((uint16_t)(rl4[0] << 8 | rl4[1]) == 8080);       /* source port = what the client dialed */
    CHECK((uint16_t)(rl4[2] << 8 | rl4[3]) == 12345);
    CHECK(nettest_l4_ok(u_ip, client, IPPROTO_TCP, rl4, sizeof(struct tcp_hdr)));

    /* (3) a UDP round trip through the udp rule. */
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    l4len = nettest_mk_udp(l4, client, u_ip, 5555, 9090, payload, sizeof(payload));
    flen = nettest_wrap(frame, u_mac, client_mac, client, u_ip, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    r = nettest_recv_ip(g);
    CHECK(r != NULL);
    CHECK(m_copydata(r, 0, ETH_HLEN + 20 + 8 + (int)sizeof(payload), rx)); m_freem(r);
    ri = (struct ipv4_hdr *)(rx + ETH_HLEN);
    rl4 = rx + ETH_HLEN + 20;
    CHECK(ri->dst == guest && (uint16_t)(rl4[2] << 8 | rl4[3]) == 53);
    CHECK(nettest_l4_ok(client, guest, IPPROTO_UDP, rl4, (uint16_t)(8 + sizeof(payload))));
    /* the guest's UDP reply is un-DNAT'd back to the client from host:9090. */
    l4len = nettest_mk_udp(l4, guest, client, 53, 5555, payload, sizeof(payload));
    flen = nettest_wrap(frame, g_mac, guest_mac, guest, client, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    r = nettest_recv_ip(u);
    CHECK(r != NULL);
    CHECK(m_copydata(r, 0, ETH_HLEN + 20 + 8 + (int)sizeof(payload), rx)); m_freem(r);
    ri = (struct ipv4_hdr *)(rx + ETH_HLEN);
    rl4 = rx + ETH_HLEN + 20;
    CHECK(ri->src == u_ip && (uint16_t)(rl4[0] << 8 | rl4[1]) == 9090);
    CHECK(nettest_l4_ok(u_ip, client, IPPROTO_UDP, rl4, (uint16_t)(8 + sizeof(payload))));

    /* (4) a connection to an unruled port is delivered to the host, not the guest. */
    l4len = nettest_mk_tcp(l4, client, u_ip, 22222, 1234, TH_SYN);
    flen = nettest_wrap(frame, u_mac, client_mac, client, u_ip, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    CHECK(nettest_recv_ip(g) == NULL);                     /* never forwarded to the guest */

    /* (4b) two forwards to the same guest endpoint cannot alias: a client
     * reusing its source tuple on the second forward (8081, same guest:80 as
     * the live 8080 flow from part 1) is refused, since the reply could not
     * be told from the first flow's. */
    struct nat_stats as0, as1;
    nat_get_stats(&as0);
    l4len = nettest_mk_tcp(l4, client, u_ip, 12345, 8081, TH_SYN);
    flen = nettest_wrap(frame, u_mac, client_mac, client, u_ip, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    CHECK(nettest_recv_ip(g) == NULL);                     /* ambiguous: not forwarded */
    nat_get_stats(&as1);
    CHECK(as1.dnat_drop_full > as0.dnat_drop_full);

    /* (5) one guest's inbound flood is bounded to its share of the table, not
     * the whole table: from an empty table, a flood of distinct client flows
     * against one guest's forward settles at NAT_QUOTA_PER_GUEST DNAT entries
     * (so a peer's slots survive) and further ones drop; then aging reclaims
     * them. */
    struct nat_stats ns0, ns1;
    nat_flush();
    nat_get_stats(&ns0);
    for (unsigned i = 0; i < NAT_TABLE_SIZE + 16; i++) {
        l4len = nettest_mk_tcp(l4, client, u_ip, (uint16_t)(30000 + i), 8080, TH_SYN);
        flen = nettest_wrap(frame, u_mac, client_mac, client, u_ip, 64, IPPROTO_TCP, l4, l4len);
        tap_inject(u, frame, flen);
        if ((i & 31) == 31) {
            struct mbuf *d;
            while ((d = tap_recv(g)) != NULL) m_freem(d);
        }
    }
    for (unsigned i = 0; i < 100; i++) {
        struct mbuf *d;
        while ((d = tap_recv(g)) != NULL) m_freem(d);
        nat_get_stats(&ns1);
        if (ns1.dnat_drop_full > ns0.dnat_drop_full) break;
        thread_sleep_ms(10);
    }
    nat_get_stats(&ns1);
    CHECK(ns1.entries == NAT_QUOTA_PER_GUEST);            /* capped to the guest's share, not 256 */
    CHECK(ns1.dnat_drop_full > ns0.dnat_drop_full);        /* the flood past the share dropped */
    nat_get_stats(&ns0);
    CHECK(ns0.entries > 0);
    nat_age(clock_now_ns() + 2ull * NAT_TIMEOUT_TCP_NS);
    nat_get_stats(&ns1);
    CHECK(ns1.entries == 0 && ns1.expired > ns0.expired);

    nat_pf_clear();
    nat_flush();
    tap_destroy(u);
    tap_destroy(g);
    kinfo("selftest: net-dnat: a client connection was forwarded to the guest and its reply "
          "un-DNAT'd back from host:P (TCP and UDP), an unruled port stayed local, the table bounded");
    return true;
}

/* --- the runtime network control channel (/dev/net/tapctl) ---------------- */

/* The byte length a /dev/net/tapctl snapshot should have given `pf_rules`
 * port-forwards: the port-forward list, then the filter section (ABI version
 * 2 and later) whose counts are read from the buffer itself (attached guests and rules
 * vary with what other tests left open). */
static int64_t netctl_snapshot_len(const uint8_t *buf, unsigned pf_rules)
{
    size_t off = sizeof(struct cosmo_netctl_list) + (size_t)pf_rules * sizeof(struct cosmo_netctl_rule);
    struct cosmo_netctl_filter_list fh;
    memcpy(&fh, buf + off, sizeof(fh));
    return (int64_t)(off + sizeof(fh) + (size_t)fh.guest_count * sizeof(struct cosmo_netctl_filter_guest) +
                     (size_t)fh.rule_count * sizeof(struct cosmo_netctl_filter_rule));
}

bool selftest_net_tapctl(const char **reason)
{
    static const uint8_t g_mac[6]     = { 0x52, 0x54, 0x00, 0x0a, 0x00, 0x01 };
    static const uint8_t u_mac[6]     = { 0x52, 0x54, 0x00, 0x0b, 0x00, 0x01 };
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x0a, 0x00, 0x0f };
    static const uint8_t client_mac[6]= { 0x52, 0x54, 0x00, 0x0b, 0x00, 0x63 };
    uint32_t mask = htonl(0xffffff00u);
    uint32_t g_ip = IPV4_ADDR(10, 77, 7, 1), guest = IPV4_ADDR(10, 77, 7, 15);
    uint32_t u_ip = IPV4_ADDR(10, 77, 8, 1), client = IPV4_ADDR(10, 77, 8, 99);

    struct tap *g = tap_create("nctlg", g_ip, mask, g_mac);
    CHECK(g != NULL);
    struct tap *u = tap_create("nctlu", u_ip, mask, u_mac);
    CHECK(u != NULL);
    netif_set_forward(tap_netif(g), true);        /* the guest tap: the only valid target */
    netif_set_masquerade(tap_netif(g), true);
    nettest_seed_arp(tap_netif(g), guest, guest_mac);
    nettest_seed_arp(tap_netif(u), client, client_mac);
    nat_flush();
    nat_pf_clear();

    struct file *f = NULL;
    CHECK(vfs_open(NULL, "/dev/net/tapctl", COSMO_O_RDWR, 0, &f) == 0 && f != NULL);

    struct cosmo_netctl cmd;
    /* Room for the port-forward list and the filter section that follows it
     * (ABI version 2 and later: its header, up to every guest's policy, and a
     * few rules). */
    uint8_t rbuf[sizeof(struct cosmo_netctl_list) + NAT_PF_MAX * sizeof(struct cosmo_netctl_rule) +
                 sizeof(struct cosmo_netctl_filter_list) + (FW_MAX_GUESTS + 1) * sizeof(struct cosmo_netctl_filter_guest) +
                 16 * sizeof(struct cosmo_netctl_filter_rule)];

    /* (1) FORWARD_ADD through the device installs a rule. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_NETCTL_VERSION; cmd.op = COSMO_NETCTL_FORWARD_ADD;
    cmd.proto = COSMO_NETCTL_PROTO_TCP; cmd.host_port = 8080; cmd.guest_port = 80; cmd.guest_addr = guest;
    CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));

    /* (2) the read listing shows exactly that rule. */
    int64_t rn = file_read(f, rbuf, sizeof(rbuf));
    CHECK(rn == netctl_snapshot_len(rbuf, 1));      /* one forward, then the filter section */
    struct cosmo_netctl_list *hdr = (struct cosmo_netctl_list *)rbuf;
    CHECK(hdr->version == COSMO_NETCTL_VERSION && hdr->count == 1);
    struct cosmo_netctl_rule *r0 = (struct cosmo_netctl_rule *)(rbuf + sizeof(*hdr));
    CHECK(r0->proto == COSMO_NETCTL_PROTO_TCP && r0->host_port == 8080 &&
          r0->guest_port == 80 && r0->guest_addr == guest);

    /* (3) a client SYN to the host port is DNAT'd to the guest (the rule took
     * effect through the device). */
    uint8_t l4[128], frame[256], rx[256];
    uint16_t l4len = nettest_mk_tcp(l4, client, u_ip, 40001, 8080, TH_SYN);
    uint32_t flen = nettest_wrap(frame, u_mac, client_mac, client, u_ip, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    struct mbuf *m = nettest_recv_ip(g);
    CHECK(m != NULL);
    CHECK(m_copydata(m, 0, ETH_HLEN + 20 + 20, rx)); m_freem(m);
    CHECK(((struct ipv4_hdr *)(rx + ETH_HLEN))->dst == guest);
    CHECK((uint16_t)(rx[ETH_HLEN + 22] << 8 | rx[ETH_HLEN + 23]) == 80);   /* dport -> 80 */

    /* (4) FORWARD_DEL removes it and reaps the flow it created. */
    struct nat_stats ns0, ns1;
    nat_get_stats(&ns0);
    CHECK(ns0.entries >= 1);                        /* the flow from (3) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_NETCTL_VERSION; cmd.op = COSMO_NETCTL_FORWARD_DEL;
    cmd.proto = COSMO_NETCTL_PROTO_TCP; cmd.host_port = 8080;
    CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));
    nat_get_stats(&ns1);
    CHECK(ns1.entries < ns0.entries);               /* the rule's entries reaped */

    /* (5) the listing is empty and a client SYN now stays local. */
    rn = file_read(f, rbuf, sizeof(rbuf));
    CHECK(rn == netctl_snapshot_len(rbuf, 0));      /* no forwards, then the filter section */
    CHECK(((struct cosmo_netctl_list *)rbuf)->count == 0);
    l4len = nettest_mk_tcp(l4, client, u_ip, 40002, 8080, TH_SYN);
    flen = nettest_wrap(frame, u_mac, client_mac, client, u_ip, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    CHECK(nettest_recv_ip(g) == NULL);              /* no rule: not forwarded */

    /* (6) refusals: a duplicate, an off-tap target, a short write, a bad
     * version -- each refused, the table unchanged. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_NETCTL_VERSION; cmd.op = COSMO_NETCTL_FORWARD_ADD;
    cmd.proto = COSMO_NETCTL_PROTO_TCP; cmd.host_port = 8080; cmd.guest_port = 80; cmd.guest_addr = guest;
    CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));     /* re-add ok */
    CHECK(file_write(f, &cmd, sizeof(cmd)) == -EEXIST);                  /* duplicate */
    struct cosmo_netctl bad = cmd;
    bad.host_port = 9000; bad.guest_addr = u_ip;                        /* off the guest tap */
    CHECK(file_write(f, &bad, sizeof(bad)) == -EINVAL);                  /* off-tap: distinct from dup */
    CHECK(file_write(f, &cmd, 4) == -EINVAL);                            /* short */
    uint8_t big[sizeof(cmd) + 8];
    memcpy(big, &cmd, sizeof(cmd)); memset(big + sizeof(cmd), 0, 8);
    CHECK(file_write(f, big, sizeof(big)) == -EINVAL);                   /* oversized: not applied */
    bad = cmd; bad.version = 99;
    CHECK(file_write(f, &bad, sizeof(bad)) == -ENOTSUP);                 /* wrong version */
    rn = file_read(f, rbuf, sizeof(rbuf));
    CHECK(((struct cosmo_netctl_list *)rbuf)->count == 1);              /* only the re-added rule */

    file_put(f);
    nat_pf_clear();
    nat_flush();
    tap_destroy(u);
    tap_destroy(g);
    kinfo("selftest: net-tapctl: a forward added through /dev/net/tapctl took effect and listed, "
          "delete reaped its flow, and duplicate/off-tap/short/bad-version commands were refused");
    return true;
}

/* --- many guests: a tap per open of /dev/net/tap ---------------------------- */

#define MG_MAX 8u

/* An ARP request from `smac` for the host address `tpa`, 42 bytes. */
static void mg_arp_req(uint8_t *req, const uint8_t smac[6], uint32_t spa, uint32_t tpa)
{
    memset(req, 0, 42);
    memset(req, 0xff, 6); memcpy(req + 6, smac, 6);
    req[12] = 0x08; req[13] = 0x06; req[15] = 1; req[16] = 0x08; req[18] = 6; req[19] = 4; req[21] = 1;
    memcpy(req + 22, smac, 6); memcpy(req + 28, &spa, 4); memcpy(req + 38, &tpa, 4);
}

bool selftest_net_multiguest(const char **reason)
{
    struct file *f[MG_MAX];
    memset(f, 0, sizeof(f));

    /* (1) eight opens are eight taps on eight distinct subnets; a ninth is refused. */
    for (unsigned k = 0; k < MG_MAX; k++) {
        CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &f[k]) == 0 && f[k] != NULL);
        char name[8];
        ksnprintf(name, sizeof(name), "tap%u", k);
        struct netif *n = netif_find(name);
        CHECK(n != NULL && n->ip4.addr == IPV4_ADDR(10, 0, 3 + k, 1) && (n->flags & NETIF_FORWARD));
        netif_put(n);
    }
    struct file *ninth = NULL;
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &ninth) == -ENOSPC && ninth == NULL);

    /* (2) a frame written to one file reaches only that file's tap: an ARP
     * for tap0's address answered on file 0, nothing on file 1. */
    static const uint8_t gmac[6] = { 0x52, 0x54, 0x00, 0x0c, 0x00, 0x0f };
    uint8_t req[42], rep[64];
    mg_arp_req(req, gmac, IPV4_ADDR(10, 0, 3, 15), IPV4_ADDR(10, 0, 3, 1));
    CHECK(file_write(f[0], req, sizeof(req)) == (int64_t)sizeof(req));
    int64_t n = 0;
    for (unsigned i = 0; i < 50 && n <= 0; i++) {
        n = file_read(f[0], rep, sizeof(rep));
        if (n <= 0) thread_sleep_ms(10);
    }
    CHECK(n >= 42 && rep[12] == 0x08 && rep[13] == 0x06 && rep[21] == 2);   /* an ARP reply (padded to 60) */
    CHECK(memcmp(rep + 28, "\x0a\x00\x03\x01", 4) == 0);                    /* from 10.0.3.1 */
    CHECK(file_read(f[1], rep, sizeof(rep)) == 0);                            /* tap1 saw nothing */

    /* (2b) guest-to-guest is routed with real addresses, not masqueraded:
     * a datagram from tap0's guest to tap1's guest leaves tap1 with its
     * source intact (both taps forward, so the egress is not the uplink). */
    static const uint8_t g1mac[6] = { 0x52, 0x54, 0x00, 0x0c, 0x00, 0x1f };
    static const uint8_t tap0mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };
    uint32_t ga = IPV4_ADDR(10, 0, 3, 15), gb = IPV4_ADDR(10, 0, 4, 15);
    /* The forwarding firewall drops inter-guest traffic by default (its own
     * unit, net-firewall, proves that); open A's to-guest policy here so this
     * step asserts routing and the absence of masquerade, not policy. */
    CHECK(fw_policy_set(ga, FW_DIR_TO_GUEST, FW_ACCEPT) == 0);
    { struct netif *n1 = netif_find("tap1"); CHECK(n1 != NULL); nettest_seed_arp(n1, gb, g1mac); netif_put(n1); }
    uint8_t pl[4] = { 9, 8, 7, 6 }, l4[64], frame[128], rx[128];
    uint16_t l4len = nettest_mk_udp(l4, ga, gb, 7000, 7001, pl, sizeof(pl));
    uint32_t flen = nettest_wrap(frame, tap0mac, gmac, ga, gb, 64, IPPROTO_UDP, l4, l4len);
    CHECK(file_write(f[0], frame, flen) == (int64_t)flen);
    /* read f[1], skipping the ARP the stack queues while resolving, to the
     * forwarded IPv4 datagram. */
    int64_t gn = 0; bool got = false;
    for (unsigned i = 0; i < 60 && !got; i++) {
        gn = file_read(f[1], rx, sizeof(rx));
        if (gn >= ETH_HLEN + 20 && rx[12] == 0x08 && rx[13] == 0x00)
            got = true;
        else if (gn <= 0)
            thread_sleep_ms(10);
    }
    CHECK(got);
    CHECK(((struct ipv4_hdr *)(rx + ETH_HLEN))->src == ga);   /* real source, not masqueraded */

    /* (2b') a guest cannot forge a same-subnet identity toward a peer: a frame
     * from tap0's guest sourced as 10.0.3.50 (not its assigned .15) is dropped
     * as spoofed and never reaches tap1. */
    struct ip_stats mgs0, mgs1;
    ipv4_get_stats(&mgs0);
    uint32_t forged = IPV4_ADDR(10, 0, 3, 50);
    l4len = nettest_mk_udp(l4, forged, gb, 7000, 7001, pl, sizeof(pl));
    flen = nettest_wrap(frame, tap0mac, gmac, forged, gb, 64, IPPROTO_UDP, l4, l4len);
    CHECK(file_write(f[0], frame, flen) == (int64_t)flen);
    bool leaked = false;
    for (unsigned i = 0; i < 30 && !leaked; i++) {
        int64_t sn = file_read(f[1], rx, sizeof(rx));
        if (sn >= ETH_HLEN + 20 && rx[12] == 0x08 && rx[13] == 0x00)
            leaked = true;                       /* the forged datagram reached the peer */
        else if (sn <= 0)
            thread_sleep_ms(10);
    }
    ipv4_get_stats(&mgs1);
    CHECK(!leaked && mgs1.fwd_spoofed > mgs0.fwd_spoofed);

    /* (2c) a forward rule per guest; closing tap0's owner purges only its
     * guest's rule, tap1's remains. */
    nat_pf_clear();
    CHECK(nat_pf_add(IPPROTO_TCP, 2222, ga, 22) == 0);
    CHECK(nat_pf_add(IPPROTO_TCP, 3333, gb, 22) == 0);
    struct nat_pf_rule rules[NAT_PF_MAX];
    CHECK(nat_pf_list(rules, NAT_PF_MAX) == 2);

    /* (3) the last close of one file destroys only its tap, purges only its
     * guest's NAT state, and frees its slot; the others live on. */
    file_put(f[0]); f[0] = NULL;
    struct netif *gone = netif_find("tap0"), *kept = netif_find("tap1");
    CHECK(gone == NULL && kept != NULL);
    netif_put(kept);
    unsigned nr = nat_pf_list(rules, NAT_PF_MAX);
    CHECK(nr == 1 && rules[0].guest_ip == gb);   /* tap0's guest purged, tap1's kept */
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &f[0]) == 0);
    struct netif *again = netif_find("tap0");
    CHECK(again != NULL && again->ip4.addr == IPV4_ADDR(10, 0, 3, 1));
    netif_put(again);

    for (unsigned k = 0; k < MG_MAX; k++)
        if (f[k]) file_put(f[k]);
    CHECK(netif_find("tap0") == NULL && netif_find("tap7") == NULL);

    kinfo("selftest: net-multiguest: eight opens gave eight taps on eight subnets, a ninth was refused, "
          "frames stayed on their own tap, a close destroyed only its own and its slot was reused");
    return true;
}

/* --- the forwarding firewall (docs/audit/next-subsystem-firewall.md) ----- */

/* Inject one datagram from a guest (its file) toward the stack, addressed to
 * its tap's MAC as a real guest would send to its gateway. */
static bool fwt_send(struct file *from, const uint8_t tapmac[6], const uint8_t gmac[6],
                     uint32_t sip, uint32_t dip, uint8_t proto, const uint8_t *l4, uint16_t l4len)
{
    uint8_t frame[128];
    uint32_t flen = nettest_wrap(frame, tapmac, gmac, sip, dip, 64, proto, l4, l4len);
    return file_write(from, frame, flen) == (int64_t)flen;
}

/* Poll a guest's file for a forwarded IPv4 datagram, skipping anything else
 * the stack emits on the tap (ARP, ND). Length, or 0 when none arrived. */
static int64_t fwt_recv(struct file *to, uint8_t *rx, size_t cap, unsigned tries)
{
    for (unsigned i = 0; i < tries; i++) {
        int64_t n = file_read(to, rx, cap);
        if (n >= ETH_HLEN + 20 && rx[12] == 0x08 && rx[13] == 0x00)
            return n;
        if (n <= 0)
            thread_sleep_ms(10);
    }
    return 0;
}

/* An ICMP echo of `type` with identifier `id` (checksum left zero: the
 * forwarding path does not validate it). 16 bytes. */
static uint16_t fwt_mk_icmp(uint8_t *l4, uint8_t type, uint16_t id)
{
    memset(l4, 0, 16);
    l4[0] = type;
    l4[4] = (uint8_t)(id >> 8);
    l4[5] = (uint8_t)id;
    l4[7] = 1;                              /* sequence */
    return 16;
}

bool selftest_net_firewall(const char **reason)
{
    *reason = NULL;
    fw_flush();
    nat_flush();

    /* Two guests through /dev/net/tap, as vmctl opens them (so each attaches
     * to the firewall): A on tap0 (10.0.3.15), B on tap1 (10.0.4.15). */
    struct file *fa = NULL, *fb = NULL;
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &fa) == 0 && fa != NULL);
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &fb) == 0 && fb != NULL);
    uint32_t ga = IPV4_ADDR(10, 0, 3, 15), gb = IPV4_ADDR(10, 0, 4, 15);
    static const uint8_t amac[6] = { 0x52, 0x54, 0x00, 0x0d, 0x00, 0x0a };
    static const uint8_t bmac[6] = { 0x52, 0x54, 0x00, 0x0d, 0x00, 0x0b };
    static const uint8_t tap0mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };
    static const uint8_t tap1mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcd };
    { struct netif *n = netif_find("tap0"); CHECK(n != NULL && n->ip4.addr == IPV4_ADDR(10, 0, 3, 1)); nettest_seed_arp(n, ga, amac); netif_put(n); }
    { struct netif *n = netif_find("tap1"); CHECK(n != NULL && n->ip4.addr == IPV4_ADDR(10, 0, 4, 1)); nettest_seed_arp(n, gb, bmac); netif_put(n); }
    uint32_t attached[FW_MAX_GUESTS];
    unsigned na = fw_guest_list(attached, FW_MAX_GUESTS);
    bool has_a = false, has_b = false;
    for (unsigned i = 0; i < na; i++) { has_a |= attached[i] == ga; has_b |= attached[i] == gb; }
    CHECK(has_a && has_b);                               /* both opens attached */

    uint8_t pl[4] = { 1, 2, 3, 4 }, l4[32], rx[128];
    uint16_t l4len;
    struct fw_stats fs0, fs1;
    struct ip_stats is0, is1;

    /* (1) the default: a guest cannot reach its neighbour. A -> B is dropped
     * by the default policy, counted on both the filter and the IP side. */
    fw_get_stats(&fs0); ipv4_get_stats(&is0);
    l4len = nettest_mk_udp(l4, ga, gb, 7000, 7001, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fb, rx, sizeof(rx), 15) == 0);
    fw_get_stats(&fs1); ipv4_get_stats(&is1);
    CHECK(fs1.drop_default > fs0.drop_default && is1.fwd_filtered > is0.fwd_filtered);

    /* (2) the default toward the uplink is unchanged: A -> the world is
     * accepted (the verdict is what is asserted; the NIC carries it on). */
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, IPV4_ADDR(10, 0, 2, 2), 7000, 53, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, IPV4_ADDR(10, 0, 2, 2), IPPROTO_UDP, l4, l4len));
    for (unsigned i = 0; i < 20; i++) { fw_get_stats(&fs1); if (fs1.accept_default > fs0.accept_default) break; thread_sleep_ms(10); }
    CHECK(fs1.accept_default > fs0.accept_default);

    /* (3) one rule punches one hole: A -> B udp/7001 accepted, udp/7002 still dropped. */
    struct fw_rule allow_udp = { .direction = FW_DIR_TO_GUEST, .proto = IPPROTO_UDP, .dst_prefix = 32,
                                 .verdict = FW_ACCEPT, .dst_ip = gb, .dst_port = 7001 };
    CHECK(fw_rule_add(ga, 0, &allow_udp) == 0);
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, gb, 7000, 7001, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fb, rx, sizeof(rx), 40) > 0);
    CHECK(((struct ipv4_hdr *)(rx + ETH_HLEN))->src == ga && ((struct ipv4_hdr *)(rx + ETH_HLEN))->dst == gb);
    fw_get_stats(&fs1);
    CHECK(fs1.accept_rule > fs0.accept_rule && fs1.flow_new > fs0.flow_new);
    l4len = nettest_mk_udp(l4, ga, gb, 7000, 7002, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fb, rx, sizeof(rx), 15) == 0);

    /* (4) stateful return, the guest-to-guest direction: B's reply to the
     * accepted flow reaches A with no rule for B; an unsolicited B -> A does not. */
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, gb, ga, 7001, 7000, pl, sizeof(pl));
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 40) > 0);
    CHECK(((struct ipv4_hdr *)(rx + ETH_HLEN))->src == gb);
    fw_get_stats(&fs1);
    CHECK(fs1.accept_established > fs0.accept_established);
    l4len = nettest_mk_udp(l4, gb, ga, 9000, 9001, pl, sizeof(pl));
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 15) == 0);

    /* (4b) TCP: a reverse-direction bare SYN is a new connection, not a
     * reply. On an accepted A -> B flow, B's SYN-ACK and ACK are established;
     * a SYN from B on the reversed ports is B opening a connection and takes
     * B's default drop, whatever the tuple says. */
    struct fw_rule allow_tcp = { .direction = FW_DIR_TO_GUEST, .proto = IPPROTO_TCP, .dst_prefix = 32,
                                 .verdict = FW_ACCEPT, .dst_ip = gb, .dst_port = 8443 };
    CHECK(fw_rule_add(ga, 0, &allow_tcp) == 0);
    l4len = nettest_mk_tcp(l4, ga, gb, 40000, 8443, TH_SYN);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_TCP, l4, l4len));
    CHECK(fwt_recv(fb, rx, sizeof(rx), 40) > 0);                        /* A's SYN, by rule */
    l4len = nettest_mk_tcp(l4, gb, ga, 8443, 40000, TH_SYN | TH_ACK);
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_TCP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 40) > 0);                        /* B's SYN-ACK: a reply */
    l4len = nettest_mk_tcp(l4, gb, ga, 8443, 40000, TH_SYN);
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_TCP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 15) == 0);                       /* B's bare SYN: not a reply */
    l4len = nettest_mk_tcp(l4, gb, ga, 8443, 40000, TH_ACK);
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_TCP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 40) > 0);                        /* B's ACK: still established */
    CHECK(fw_rule_del(ga, &allow_tcp) == 0);

    /* (5) ICMP echo is stateful on the identifier, not a bare reverse tuple. */
    struct fw_rule allow_icmp = { .direction = FW_DIR_TO_GUEST, .proto = IPPROTO_ICMP, .dst_prefix = 32,
                                  .verdict = FW_ACCEPT, .dst_ip = gb, .dst_port = FW_ICMP_TYPE_ANY };
    CHECK(fw_rule_add(ga, 99, &allow_icmp) == 0);       /* out-of-range index appends */
    l4len = fwt_mk_icmp(l4, ICMP_ECHO, 0x1234);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_ICMP, l4, l4len));
    CHECK(fwt_recv(fb, rx, sizeof(rx), 40) > 0);        /* the request, by rule */
    l4len = fwt_mk_icmp(l4, ICMP_ECHO_REPLY, 0x1234);
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_ICMP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 40) > 0);        /* the reply: established */
    l4len = fwt_mk_icmp(l4, ICMP_ECHO, 0x1234);
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_ICMP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 15) == 0);       /* a reverse *request* is not a reply */
    l4len = fwt_mk_icmp(l4, ICMP_ECHO_REPLY, 0x9999);
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_ICMP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 15) == 0);       /* wrong id: no flow */

    /* (6) ordering and identity: a DROP inserted at index 0 wins first-match
     * for a new flow; deleting it by tuple restores the ACCEPT; a duplicate
     * tuple is refused. */
    struct fw_rule deny_udp = allow_udp;
    deny_udp.verdict = FW_DROP;
    CHECK(fw_rule_add(ga, 0, &deny_udp) == 0);
    struct fw_rule listed[FW_RULES_PER_GUEST];
    /* The list: the DROP just inserted at 0, the UDP ACCEPT it pushed to 1, the
     * two TO_HOST seeds attach installed (DNS, echo-request), and the ICMP
     * ACCEPT appended last. */
    CHECK(fw_rule_list(ga, listed, FW_RULES_PER_GUEST) == 5 && listed[0].verdict == FW_DROP &&
          listed[1].verdict == FW_ACCEPT && listed[1].proto == IPPROTO_UDP &&
          listed[2].direction == FW_DIR_TO_HOST && listed[3].direction == FW_DIR_TO_HOST &&
          listed[4].proto == IPPROTO_ICMP && listed[4].direction == FW_DIR_TO_GUEST);
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, gb, 7100, 7001, pl, sizeof(pl));   /* a new flow, not the live one */
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fb, rx, sizeof(rx), 15) == 0);
    fw_get_stats(&fs1);
    CHECK(fs1.drop_rule > fs0.drop_rule);
    CHECK(fw_rule_del(ga, &deny_udp) == 0);
    CHECK(fw_rule_del(ga, &deny_udp) == -ENOENT);
    CHECK(fw_rule_add(ga, 0, &allow_udp) == -EEXIST);
    l4len = nettest_mk_udp(l4, ga, gb, 7101, 7001, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fb, rx, sizeof(rx), 40) > 0);

    /* (7) a rule is bound to a guest by address, not to the control handle:
     * added for B through /dev/net/tapctl, it takes effect after that handle
     * is closed; B's release purges it, an add for the departed B is refused,
     * and a fresh tap at B's address starts with no rules. */
    struct file *fctl = NULL;
    CHECK(vfs_open(NULL, "/dev/net/tapctl", COSMO_O_RDWR, 0, &fctl) == 0 && fctl != NULL);
    struct cosmo_netctl_filter c = { .version = COSMO_NETCTL_VERSION, .op = COSMO_NETCTL_FILTER_ADD,
                                     .guest_addr = gb, .direction = COSMO_NETCTL_DIR_TO_GUEST,
                                     .proto = COSMO_NETCTL_PROTO_UDP, .dst_prefix = 32,
                                     .verdict = COSMO_NETCTL_VERDICT_ACCEPT, .dst_addr = ga, .dst_port = 5000 };
    CHECK(file_write(fctl, &c, sizeof(c)) == (int64_t)sizeof(c));
    file_put(fctl); fctl = NULL;                         /* the transient handle vmctl would close */
    l4len = nettest_mk_udp(l4, gb, ga, 5001, 5000, pl, sizeof(pl));
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 40) > 0);        /* the rule outlived its handle */
    file_put(fb); fb = NULL;                             /* B departs: purged and detached */
    struct fw_rule b_rule = { .direction = FW_DIR_TO_GUEST, .proto = IPPROTO_UDP, .dst_prefix = 32,
                              .verdict = FW_ACCEPT, .dst_ip = ga, .dst_port = 5000 };
    CHECK(fw_rule_add(gb, 0, &b_rule) == -ENOENT);       /* an add after teardown is refused */
    CHECK(fw_rule_list(gb, listed, FW_RULES_PER_GUEST) == 0);
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &fb) == 0 && fb != NULL);   /* slot 1 reused */
    { struct netif *n = netif_find("tap1"); CHECK(n != NULL && n->ip4.addr == IPV4_ADDR(10, 0, 4, 1)); nettest_seed_arp(n, gb, bmac); netif_put(n); }
    CHECK(fw_rule_list(gb, listed, FW_RULES_PER_GUEST) == 2 &&   /* the reused address inherits nothing: */
          listed[0].direction == FW_DIR_TO_HOST && listed[1].direction == FW_DIR_TO_HOST);   /* only the fresh seeds */
    l4len = nettest_mk_udp(l4, gb, ga, 5001, 5000, pl, sizeof(pl));
    CHECK(fwt_send(fb, tap1mac, bmac, gb, ga, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fa, rx, sizeof(rx), 15) == 0);       /* no stale rule, no stale flow */

    /* (8) the control round trip: an ADD is listed with its guest and index;
     * POLICY flips the default and the next verdict follows; bad writes change nothing. */
    CHECK(vfs_open(NULL, "/dev/net/tapctl", COSMO_O_RDWR, 0, &fctl) == 0 && fctl != NULL);
    c.guest_addr = ga; c.proto = COSMO_NETCTL_PROTO_TCP; c.dst_addr = gb; c.dst_port = 445; c.at_index = 0;
    CHECK(file_write(fctl, &c, sizeof(c)) == (int64_t)sizeof(c));
    uint8_t snap[512];
    int64_t sn = file_read(fctl, snap, sizeof(snap));
    CHECK(sn > 0);
    {
        struct cosmo_netctl_list ph; memcpy(&ph, snap, sizeof(ph));
        size_t off = sizeof(ph) + (size_t)ph.count * sizeof(struct cosmo_netctl_rule);
        struct cosmo_netctl_filter_list fh; memcpy(&fh, snap + off, sizeof(fh));
        CHECK(fh.version == COSMO_NETCTL_VERSION && fh.guest_count >= 2 && fh.rule_count >= 5);   /* incl. two seeds per guest */
        off += sizeof(fh) + (size_t)fh.guest_count * sizeof(struct cosmo_netctl_filter_guest);
        bool seen = false;
        for (unsigned i = 0; i < fh.rule_count; i++) {
            struct cosmo_netctl_filter_rule fr; memcpy(&fr, snap + off + i * sizeof(fr), sizeof(fr));
            if (fr.guest_addr == ga && fr.proto == COSMO_NETCTL_PROTO_TCP && fr.dst_port == 445)
                seen = fr.index == 0 && fr.verdict == COSMO_NETCTL_VERDICT_ACCEPT && fr.dst_addr == gb;
        }
        CHECK(seen);
        CHECK(sn == netctl_snapshot_len(snap, ph.count));
    }
    struct cosmo_netctl_filter pol = { .version = COSMO_NETCTL_VERSION, .op = COSMO_NETCTL_FILTER_POLICY,
                                       .guest_addr = ga, .direction = COSMO_NETCTL_DIR_TO_GUEST,
                                       .verdict = COSMO_NETCTL_VERDICT_ACCEPT };
    CHECK(file_write(fctl, &pol, sizeof(pol)) == (int64_t)sizeof(pol));
    l4len = nettest_mk_udp(l4, ga, gb, 7200, 7777, pl, sizeof(pl));   /* no rule: the default decides */
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fb, rx, sizeof(rx), 40) > 0);
    pol.verdict = COSMO_NETCTL_VERDICT_DROP;
    CHECK(file_write(fctl, &pol, sizeof(pol)) == (int64_t)sizeof(pol));
    l4len = nettest_mk_udp(l4, ga, gb, 7201, 7777, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_UDP, l4, l4len));
    CHECK(fwt_recv(fb, rx, sizeof(rx), 15) == 0);
    CHECK(file_write(fctl, &c, 4) == -EINVAL);                          /* short */
    struct cosmo_netctl_filter bad = c;
    bad.version = 99;
    CHECK(file_write(fctl, &bad, sizeof(bad)) == -ENOTSUP);             /* wrong version */
    bad = c; bad.guest_addr = IPV4_ADDR(10, 0, 9, 15);
    CHECK(file_write(fctl, &bad, sizeof(bad)) == -ENOENT);              /* not an attached guest */
    bad = c; bad.op = COSMO_NETCTL_FILTER_DEL; bad.dst_port = 446;
    CHECK(file_write(fctl, &bad, sizeof(bad)) == -ENOENT);              /* no such tuple */
    bad = c; bad.proto = COSMO_NETCTL_PROTO_ICMP; bad.dst_port = 300;
    CHECK(file_write(fctl, &bad, sizeof(bad)) == -EINVAL);              /* an ICMP selector is a type <= 255 or ANY */
    file_put(fctl);

    file_put(fa);
    file_put(fb);
    fw_flush();
    kinfo("selftest: net-firewall: inter-guest dropped by default and uplink accepted, one rule punched one hole, "
          "a reply was admitted by state (echo by id, not a reverse request), a DROP inserted first won and was "
          "deleted by tuple, a rule outlived its handle but not its guest, and the control listing round-tripped");
    return true;
}

/* --- the INPUT chain (docs/audit/next-subsystem-input-chain.md) ---------- */

/* The ones-complement checksum of `n` bytes as big-endian words, complemented
 * -- stored big-endian it is what icmp_input verifies. */
static uint16_t fwt_ones_sum(const uint8_t *p, unsigned n)
{
    uint32_t s = 0;
    for (unsigned i = 0; i + 1 < n; i += 2)
        s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1)
        s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

/* An ICMP echo with a valid checksum: the host answers only a well-formed one. */
static uint16_t fwt_mk_icmp_ck(uint8_t *l4, uint8_t type, uint16_t id)
{
    uint16_t n = fwt_mk_icmp(l4, type, id);
    uint16_t ck = fwt_ones_sum(l4, n);
    l4[2] = (uint8_t)(ck >> 8);
    l4[3] = (uint8_t)ck;
    return n;
}

/* An ICMP Need-Fragmentation (type 3 code 4, next-hop MTU `mtu`) quoting a
 * host -> guest datagram: what a guest would send to shrink the host's path
 * MTU toward it. 36 bytes, checksummed. */
static uint16_t fwt_mk_needfrag(uint8_t *l4, uint32_t host_ip, uint32_t guest_ip, uint16_t mtu)
{
    memset(l4, 0, 36);
    l4[0] = ICMP_DEST_UNREACH;
    l4[1] = 4;
    l4[6] = (uint8_t)(mtu >> 8);
    l4[7] = (uint8_t)mtu;
    struct ipv4_hdr *in = (struct ipv4_hdr *)(l4 + 8);
    in->vhl = 0x45; in->len = htons(1500); in->ttl = 64; in->proto = IPPROTO_UDP;
    in->src = host_ip; in->dst = guest_ip;
    uint16_t ck = fwt_ones_sum(l4, 36);
    l4[2] = (uint8_t)(ck >> 8);
    l4[3] = (uint8_t)ck;
    return 36;
}

/* Did the host answer an echo on this guest's tap? Skips everything else the
 * tap carries (ARP, the DNS proxy's replies, a RST). */
static bool fwt_recv_echo_reply(struct file *f, unsigned tries)
{
    uint8_t rx[160];
    for (unsigned i = 0; i < tries; i++) {
        int64_t n = file_read(f, rx, sizeof(rx));
        if (n >= ETH_HLEN + 20 + 8 && rx[12] == 0x08 && rx[13] == 0x00 &&
            rx[ETH_HLEN + 9] == IPPROTO_ICMP && rx[ETH_HLEN + 20] == ICMP_ECHO_REPLY)
            return true;
        if (n <= 0)
            thread_sleep_ms(10);
    }
    return false;
}

/* A verdict is taken on the network worker after file_write returns, so a
 * counter is awaited, not read at once. True once `st.field` exceeds `base`. */
#define FWT_RISES(fn, st, field, base) ({                                   \
    bool r_ = false;                                                        \
    for (unsigned i_ = 0; i_ < 40 && !r_; i_++) {                           \
        fn(&(st));                                                          \
        if ((st).field > (base)) r_ = true; else thread_sleep_ms(10);       \
    }                                                                       \
    r_; })

bool selftest_net_input(const char **reason)
{
    *reason = NULL;
    fw_flush();
    nat_flush();

    /* Two guests through /dev/net/tap (so each attaches): A on tap0, B on tap1. */
    struct file *fa = NULL, *fb = NULL;
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &fa) == 0 && fa != NULL);
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &fb) == 0 && fb != NULL);
    uint32_t ga = IPV4_ADDR(10, 0, 3, 15), gwa = IPV4_ADDR(10, 0, 3, 1);
    uint32_t gb = IPV4_ADDR(10, 0, 4, 15), gwb = IPV4_ADDR(10, 0, 4, 1);
    static const uint8_t amac[6] = { 0x52, 0x54, 0x00, 0x0e, 0x00, 0x0a };
    static const uint8_t bmac[6] = { 0x52, 0x54, 0x00, 0x0e, 0x00, 0x0b };
    static const uint8_t tap0mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };
    static const uint8_t tap1mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcd };
    { struct netif *n = netif_find("tap0"); CHECK(n != NULL); nettest_seed_arp(n, ga, amac); netif_put(n); }
    { struct netif *n = netif_find("tap1"); CHECK(n != NULL); nettest_seed_arp(n, gb, bmac); netif_put(n); }

    uint8_t pl[4] = { 1, 2, 3, 4 }, l4[48];
    uint16_t l4len;
    struct fw_stats fs0, fs1;
    struct ip_stats is0, is1;
    struct fw_rule listed[FW_RULES_PER_GUEST];

    /* (1) attach seeded exactly the tap's two services, as TO_HOST rules on
     * the gateway, and the TO_HOST default is DROP. */
    CHECK(fw_rule_list(ga, listed, FW_RULES_PER_GUEST) == 2);
    CHECK(listed[0].direction == FW_DIR_TO_HOST && listed[0].proto == IPPROTO_UDP &&
          listed[0].dst_ip == gwa && listed[0].dst_prefix == 32 && listed[0].dst_port == 53);
    CHECK(listed[1].direction == FW_DIR_TO_HOST && listed[1].proto == IPPROTO_ICMP &&
          listed[1].dst_ip == gwa && listed[1].dst_port == ICMP_ECHO);
    uint8_t pu, pg, ph, pw;
    CHECK(fw_policy_get(ga, &pu, &pg, &ph, &pw) == 0 && ph == FW_DROP && pu == FW_ACCEPT && pg == FW_DROP);

    /* (2) the seeds reach the host: a DNS query to the gateway is accepted by
     * rule; an echo request to the gateway draws an echo reply. */
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, gwa, 4000, 53, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_accept_rule, fs0.in_accept_rule));
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO, 0x4242);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_ICMP, l4, l4len));
    CHECK(fwt_recv_echo_reply(fa, 40));

    /* (3) everything else is closed by default: UDP to gateway:7000, a TCP SYN
     * to gateway:2222, and a datagram to the host's uplink address -- counted
     * on the filter and on the IP side. */
    fw_get_stats(&fs0); ipv4_get_stats(&is0);
    l4len = nettest_mk_udp(l4, ga, gwa, 4001, 7000, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_drop_default, fs0.in_drop_default));
    CHECK(FWT_RISES(ipv4_get_stats, is1, in_filtered, is0.in_filtered));
    fw_get_stats(&fs0);
    l4len = nettest_mk_tcp(l4, ga, gwa, 40000, 2222, TH_SYN);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_drop_default, fs0.in_drop_default));
    /* A datagram to the host's *uplink* address is not this tap's to deliver:
     * the off-link invariant drops it before any chain (the host chain's
     * unit), so INPUT's default is not what closes it any more. */
    uint32_t uplink = IPV4_ADDR(10, 0, 2, 15);
    if (netif_owns_ipv4(uplink)) {                    /* the NIC's autoconfigured address, when present */
        fw_get_stats(&fs0); ipv4_get_stats(&is0);
        l4len = nettest_mk_udp(l4, ga, uplink, 4002, 7000, pl, sizeof(pl));
        CHECK(fwt_send(fa, tap0mac, amac, ga, uplink, IPPROTO_UDP, l4, l4len));
        CHECK(FWT_RISES(ipv4_get_stats, is1, rx_offlink, is0.rx_offlink));
        fw_get_stats(&fs1);
        CHECK(fs1.in_drop_default == fs0.in_drop_default);
    }

    /* (4) a rule opens a service, per datagram (stateless): a SYN and then a
     * bare ACK to the ruled port are both admitted by the same rule. */
    struct fw_rule open_tcp = { .direction = FW_DIR_TO_HOST, .proto = IPPROTO_TCP, .dst_prefix = 32,
                                .verdict = FW_ACCEPT, .dst_ip = gwa, .dst_port = 2222 };
    CHECK(fw_rule_add(ga, 0, &open_tcp) == 0);
    fw_get_stats(&fs0);
    l4len = nettest_mk_tcp(l4, ga, gwa, 40001, 2222, TH_SYN);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_accept_rule, fs0.in_accept_rule));
    fw_get_stats(&fs0);
    l4len = nettest_mk_tcp(l4, ga, gwa, 40001, 2222, TH_ACK);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_accept_rule, fs0.in_accept_rule));

    /* (4c) the ANY direction keeps its forwarding-only meaning: a wildcard
     * ACCEPT written to permit forwarding does not open a host port -- host
     * traffic needs an explicit TO_HOST rule -- so no rule written before the
     * INPUT chain existed silently opens the host. */
    struct fw_rule any_udp = { .direction = FW_DIR_ANY, .proto = IPPROTO_UDP, .dst_prefix = 32,
                               .verdict = FW_ACCEPT, .dst_ip = gwa, .dst_port = 7003 };
    CHECK(fw_rule_add(ga, 0, &any_udp) == 0);
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, gwa, 4003, 7003, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_drop_default, fs0.in_drop_default));   /* ANY did not reach the host */
    CHECK(fw_rule_del(ga, &any_udp) == 0);
    struct fw_rule host_udp = any_udp;
    host_udp.direction = FW_DIR_TO_HOST;
    CHECK(fw_rule_add(ga, 0, &host_udp) == 0);
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, gwa, 4004, 7003, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_accept_rule, fs0.in_accept_rule));     /* TO_HOST does */
    CHECK(fw_rule_del(ga, &host_udp) == 0);

    /* (5) the seeds are real rules, not hard-coded holes: deleting the echo
     * seed closes echo; re-adding it reopens it. */
    struct fw_rule seed_echo = { .direction = FW_DIR_TO_HOST, .proto = IPPROTO_ICMP, .dst_prefix = 32,
                                 .verdict = FW_ACCEPT, .dst_ip = gwa, .dst_port = ICMP_ECHO };
    CHECK(fw_rule_del(ga, &seed_echo) == 0);
    fw_get_stats(&fs0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO, 0x4243);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_drop_default, fs0.in_drop_default));
    CHECK(!fwt_recv_echo_reply(fa, 10));
    CHECK(fw_rule_add(ga, 0, &seed_echo) == 0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO, 0x4244);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_ICMP, l4, l4len));
    CHECK(fwt_recv_echo_reply(fa, 40));

    /* (6) the ICMP selector is a type: a guest echo *reply* and a guest
     * Need-Fragmentation toward the gateway are dropped by default (the
     * host's PMTU cache untouched); a type-0 rule admits the reply alone;
     * the wildcard admits Need-Fragmentation too. */
    fw_get_stats(&fs0); ipv4_get_stats(&is0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO_REPLY, 0x4242);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_drop_default, fs0.in_drop_default));
    fw_get_stats(&fs0);
    l4len = fwt_mk_needfrag(l4, gwa, ga, 576);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_drop_default, fs0.in_drop_default));
    ipv4_get_stats(&is1);
    CHECK(is1.pmtu_updates == is0.pmtu_updates);          /* never reached ipv4_pmtu_update */
    struct fw_rule type0 = { .direction = FW_DIR_TO_HOST, .proto = IPPROTO_ICMP, .dst_prefix = 32,
                             .verdict = FW_ACCEPT, .dst_ip = gwa, .dst_port = ICMP_ECHO_REPLY };
    CHECK(fw_rule_add(ga, 0, &type0) == 0);
    fw_get_stats(&fs0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO_REPLY, 0x4242);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_accept_rule, fs0.in_accept_rule));
    fw_get_stats(&fs0);
    l4len = fwt_mk_needfrag(l4, gwa, ga, 576);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_drop_default, fs0.in_drop_default));   /* type 3 still closed */
    CHECK(fw_rule_del(ga, &type0) == 0);
    struct fw_rule anyicmp = type0;
    anyicmp.dst_port = FW_ICMP_TYPE_ANY;
    CHECK(fw_rule_add(ga, 0, &anyicmp) == 0);
    fw_get_stats(&fs0);
    l4len = fwt_mk_needfrag(l4, gwa, ga, 576);
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_accept_rule, fs0.in_accept_rule));
    CHECK(fw_rule_del(ga, &anyicmp) == 0);

    /* (7) anti-spoof on the local path: from A's tap, a source that is not A
     * -- a stray, the host itself, the neighbour -- toward the gateway's DNS
     * (a ruled port) is dropped as spoofed before any rule is read. */
    /* Give B a rule that would admit exactly this datagram if it came from B:
     * without the anti-spoof, A forging B's source would borrow B's policy. */
    struct fw_rule b_dns_any = { .direction = FW_DIR_TO_HOST, .proto = IPPROTO_UDP, .dst_prefix = 0,
                                 .verdict = FW_ACCEPT, .dst_ip = 0, .dst_port = 53 };
    CHECK(fw_rule_add(gb, 0, &b_dns_any) == 0);
    fw_get_stats(&fs0); ipv4_get_stats(&is0);
    uint32_t forged[3] = { IPV4_ADDR(10, 0, 3, 50), gb, IPV4_ADDR(10, 0, 2, 2) };   /* a stray, the neighbour, the world */
    for (unsigned i = 0; i < 3; i++) {
        uint64_t base = fs0.in_spoofed + i;
        l4len = nettest_mk_udp(l4, forged[i], gwa, 4100, 53, pl, sizeof(pl));
        CHECK(fwt_send(fa, tap0mac, amac, forged[i], gwa, IPPROTO_UDP, l4, l4len));
        CHECK(FWT_RISES(fw_get_stats, fs1, in_spoofed, base));
    }
    CHECK(FWT_RISES(ipv4_get_stats, is1, in_filtered, is0.in_filtered));
    CHECK(fw_rule_del(gb, &b_dns_any) == 0);
    /* Forged as the host itself, the datagram never reaches the firewall:
     * ipv4_input drops one of our own addresses arriving from a link as a
     * martian first -- a stronger drop, counted upstream. */
    ipv4_get_stats(&is0);
    l4len = nettest_mk_udp(l4, gwa, gwa, 4100, 53, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, gwa, gwa, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, rx_bad_header, is0.rx_bad_header));

    /* (8) per guest: A's TCP rule does not open B's path; B's release purges
     * its seeds; a reopened B re-seeds cleanly. */
    fw_get_stats(&fs0);
    l4len = nettest_mk_tcp(l4, gb, gwb, 40000, 2222, TH_SYN);
    CHECK(fwt_send(fb, tap1mac, bmac, gb, gwb, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_drop_default, fs0.in_drop_default));
    file_put(fb); fb = NULL;
    CHECK(fw_rule_list(gb, listed, FW_RULES_PER_GUEST) == 0);
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &fb) == 0 && fb != NULL);
    CHECK(fw_rule_list(gb, listed, FW_RULES_PER_GUEST) == 2 &&
          listed[0].direction == FW_DIR_TO_HOST && listed[1].direction == FW_DIR_TO_HOST);

    /* (9) the policy flips: TO_HOST ACCEPT admits an unruled port, DROP
     * closes it again; ANY is not a policy direction. */
    CHECK(fw_policy_set(ga, FW_DIR_TO_HOST, FW_ACCEPT) == 0);
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, gwa, 4200, 7001, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_accept_default, fs0.in_accept_default));
    CHECK(fw_policy_set(ga, FW_DIR_TO_HOST, FW_DROP) == 0);
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, gwa, 4201, 7002, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_drop_default, fs0.in_drop_default));
    CHECK(fw_policy_set(ga, FW_DIR_ANY, FW_ACCEPT) == -EINVAL);

    /* (10) the control channel: a TO_HOST rule written through tapctl is
     * listed with its direction beside a guest record carrying the third
     * policy; the ICMP selector round-trips -- type 0 is writable, the
     * wildcard is not mistaken for it, a type above 255 and ANY-as-policy-
     * direction are refused. */
    struct file *fctl = NULL;
    CHECK(vfs_open(NULL, "/dev/net/tapctl", COSMO_O_RDWR, 0, &fctl) == 0 && fctl != NULL);
    struct cosmo_netctl_filter c = { .version = COSMO_NETCTL_VERSION, .op = COSMO_NETCTL_FILTER_ADD,
                                     .guest_addr = ga, .direction = COSMO_NETCTL_DIR_TO_HOST,
                                     .proto = COSMO_NETCTL_PROTO_TCP, .dst_prefix = 32,
                                     .verdict = COSMO_NETCTL_VERDICT_ACCEPT, .dst_addr = gwa, .dst_port = 8080 };
    CHECK(file_write(fctl, &c, sizeof(c)) == (int64_t)sizeof(c));
    uint8_t snap[1024];
    int64_t sn = file_read(fctl, snap, sizeof(snap));
    CHECK(sn > 0);
    {
        struct cosmo_netctl_list phh; memcpy(&phh, snap, sizeof(phh));
        size_t off = sizeof(phh) + (size_t)phh.count * sizeof(struct cosmo_netctl_rule);
        struct cosmo_netctl_filter_list fh; memcpy(&fh, snap + off, sizeof(fh));
        CHECK(fh.version == COSMO_NETCTL_VERSION);
        off += sizeof(fh);
        bool guest_seen = false;
        for (unsigned i = 0; i < fh.guest_count; i++, off += sizeof(struct cosmo_netctl_filter_guest)) {
            struct cosmo_netctl_filter_guest fg; memcpy(&fg, snap + off, sizeof(fg));
            if (fg.guest_addr == ga)
                guest_seen = fg.policy_to_host == COSMO_NETCTL_VERDICT_DROP &&
                             fg.policy_to_uplink == COSMO_NETCTL_VERDICT_ACCEPT;
        }
        CHECK(guest_seen);
        bool rule_seen = false;
        for (unsigned i = 0; i < fh.rule_count; i++) {
            struct cosmo_netctl_filter_rule fr; memcpy(&fr, snap + off + i * sizeof(fr), sizeof(fr));
            if (fr.guest_addr == ga && fr.proto == COSMO_NETCTL_PROTO_TCP && fr.dst_port == 8080)
                rule_seen = fr.direction == COSMO_NETCTL_DIR_TO_HOST && fr.dst_addr == gwa;
        }
        CHECK(rule_seen);
    }
    struct cosmo_netctl_filter ic = c;
    ic.proto = COSMO_NETCTL_PROTO_ICMP; ic.dst_port = 0;                  /* echo-reply: type 0 is writable */
    CHECK(file_write(fctl, &ic, sizeof(ic)) == (int64_t)sizeof(ic));
    ic.op = COSMO_NETCTL_FILTER_DEL;
    CHECK(file_write(fctl, &ic, sizeof(ic)) == (int64_t)sizeof(ic));
    ic.op = COSMO_NETCTL_FILTER_ADD; ic.dst_port = COSMO_NETCTL_ICMP_TYPE_ANY;   /* the wildcard is distinct */
    CHECK(file_write(fctl, &ic, sizeof(ic)) == (int64_t)sizeof(ic));
    ic.op = COSMO_NETCTL_FILTER_DEL;
    CHECK(file_write(fctl, &ic, sizeof(ic)) == (int64_t)sizeof(ic));
    ic.op = COSMO_NETCTL_FILTER_ADD; ic.dst_port = 256;
    CHECK(file_write(fctl, &ic, sizeof(ic)) == -EINVAL);                  /* not a type */
    struct cosmo_netctl_filter pol = { .version = COSMO_NETCTL_VERSION, .op = COSMO_NETCTL_FILTER_POLICY,
                                       .guest_addr = ga, .direction = COSMO_NETCTL_DIR_ANY,
                                       .verdict = COSMO_NETCTL_VERDICT_ACCEPT };
    CHECK(file_write(fctl, &pol, sizeof(pol)) == -EINVAL);                /* ANY is not a policy direction */
    pol.direction = COSMO_NETCTL_DIR_TO_HOST;
    CHECK(file_write(fctl, &pol, sizeof(pol)) == (int64_t)sizeof(pol));
    pol.verdict = COSMO_NETCTL_VERDICT_DROP;
    CHECK(file_write(fctl, &pol, sizeof(pol)) == (int64_t)sizeof(pol));
    file_put(fctl);

    file_put(fa);
    file_put(fb);
    fw_flush();
    kinfo("selftest: net-input: the seeded DNS and echo reached the host and nothing else did, a rule opened a "
          "port per datagram, the seeds were deletable, the ICMP selector was a type (echo reply and need-frag "
          "dropped; type 0 and the wildcard admitted what they name), forged sources were dropped as spoofed, "
          "policy was per guest and flippable, and the control listing carried the third direction");
    return true;
}

/* --- the host chain (docs/audit/next-subsystem-host-input.md) ------------- */

/* One datagram as read back from the uplink tap: the world's view of what
 * the host answered. For TCP the header fields and up to 64 bytes of payload;
 * for UDP the ports and payload length; for ICMP the type in `flags`. */
struct hin_seg {
    uint32_t src, dst;
    uint8_t  proto, flags;
    uint16_t sport, dport, win, paylen;
    uint32_t seq, ack;
    uint8_t  pay[64];
};

/* A TCP segment with every field the world side needs to drive a connection
 * by hand: sequence, acknowledgment, flags, window and payload, checksummed. */
static uint16_t hin_mk_tcp(uint8_t *l4, uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, uint32_t seq,
                           uint32_t ack, uint8_t flags, uint16_t win, const void *pl, uint16_t pllen)
{
    struct tcp_hdr *th = (struct tcp_hdr *)l4;
    memset(th, 0, sizeof(*th));
    th->sport = htons(sp); th->dport = htons(dp);
    th->seq = htonl(seq); th->ack = htonl(ack);
    th->doff = 5 << 4; th->flags = flags; th->win = htons(win);
    if (pllen)
        memcpy(l4 + sizeof(*th), pl, pllen);
    uint16_t len = (uint16_t)(sizeof(*th) + pllen);
    th->cksum = nettest_l4cksum(sip, dip, IPPROTO_TCP, l4, len);
    return len;
}

/* The world sends one IPv4 datagram in on the uplink tap. */
static bool hin_send(struct tap *u, const uint8_t umac[6], const uint8_t wmac[6], uint32_t sip, uint32_t dip,
                     uint8_t proto, const uint8_t *l4, uint16_t l4len)
{
    uint8_t frame[256];
    uint32_t flen = nettest_wrap(frame, umac, wmac, sip, dip, 64, proto, l4, l4len);
    return tap_inject(u, frame, flen) == 0;
}

static bool hin_parse(struct mbuf *m, struct hin_seg *o)
{
    /* A window big enough for the headers and the first of the payload: a
     * frame may be far longer (a full-sized segment), so the *header fields*
     * say how much data the datagram carries, never how much was copied. */
    uint8_t rx[128];
    uint32_t n = m_length(m);
    uint32_t take = n < sizeof(rx) ? n : (uint32_t)sizeof(rx);
    if (n < ETH_HLEN + 20u || !m_copydata(m, 0, take, rx) || rx[12] != 0x08 || rx[13] != 0x00)
        return false;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)(rx + ETH_HLEN);
    unsigned ihl = IPV4_HDR_LEN(ip);
    uint16_t total = ntohs(ip->len);
    if (total < ihl || (uint32_t)ETH_HLEN + total > n || (uint32_t)ETH_HLEN + ihl + 8u > take)
        return false;
    memset(o, 0, sizeof(*o));
    o->src = ip->src; o->dst = ip->dst; o->proto = ip->proto;
    const uint8_t *l4 = rx + ETH_HLEN + ihl;
    unsigned l4len = total - ihl;
    unsigned hl;
    if (ip->proto == IPPROTO_TCP && l4len >= sizeof(struct tcp_hdr)) {
        const struct tcp_hdr *th = (const struct tcp_hdr *)l4;
        hl = TCP_HDR_LEN(th);
        if (hl > l4len || (uint32_t)ETH_HLEN + ihl + hl > take)
            return false;
        o->sport = ntohs(th->sport); o->dport = ntohs(th->dport);
        o->seq = ntohl(th->seq); o->ack = ntohl(th->ack);
        o->flags = th->flags; o->win = ntohs(th->win);
        o->paylen = (uint16_t)(l4len - hl);
    } else if (ip->proto == IPPROTO_UDP && l4len >= 8) {
        hl = 8;
        o->sport = (uint16_t)(l4[0] << 8 | l4[1]); o->dport = (uint16_t)(l4[2] << 8 | l4[3]);
        o->paylen = (uint16_t)((l4[4] << 8 | l4[5]) - 8);
    } else {
        if (ip->proto == IPPROTO_ICMP && l4len >= 4)
            o->flags = l4[0];
        return true;
    }
    /* As much of the payload as the window holds, up to what `pay` takes. */
    uint32_t have = take - (uint32_t)(ETH_HLEN + ihl + hl);
    uint32_t cp = o->paylen < have ? o->paylen : have;
    memcpy(o->pay, l4 + hl, cp < sizeof(o->pay) ? cp : sizeof(o->pay));
    return true;
}

/* The next datagram of `proto` the host emitted on the uplink tap toward
 * world port `wport` (0 = any), within tries x 10 ms; everything else the tap
 * carries (ARP, another connection's segments) is skipped. False when none
 * arrived -- the check a silent DROP is proved by. */
static bool hin_recv(struct tap *u, uint8_t proto, uint16_t wport, struct hin_seg *o, unsigned tries)
{
    for (unsigned i = 0; i < tries; i++) {
        struct mbuf *m;
        while ((m = tap_recv(u)) != NULL) {
            bool ok = hin_parse(m, o);
            m_freem(m);
            if (ok && o->proto == proto && (wport == 0 || o->dport == wport))
                return true;
        }
        thread_sleep_ms(10);
    }
    return false;
}

static void hin_drain(struct tap *u)
{
    struct mbuf *m;
    thread_sleep_ms(20);
    while ((m = tap_recv(u)) != NULL)
        m_freem(m);
}

/* A non-blocking accept, awaited. */
static struct socket *hin_accept(struct socket *ls)
{
    for (unsigned i = 0; i < 100; i++) {
        struct socket *a = NULL;
        struct netaddr peer;
        if (ksock_accept(ls, &a, &peer) == 0)
            return a;
        thread_sleep_ms(10);
    }
    return NULL;
}

/* A non-blocking receive, awaited: bytes, 0 at EOF, or the error (-EAGAIN
 * when nothing came in tries x 10 ms). */
static int64_t hin_recv_sock(struct socket *s, void *buf, size_t cap, unsigned tries)
{
    int64_t n = -EAGAIN;
    for (unsigned i = 0; i < tries && n == -EAGAIN; i++) {
        n = ksock_recvfrom(s, buf, cap, NULL);
        if (n == -EAGAIN)
            thread_sleep_ms(10);
    }
    return n;
}

/* The host's own outbound connection, on its own thread (connect blocks
 * until the peer's SYN+ACK is admitted). */
struct hin_conn {
    struct netaddr peer;
    struct socket *s;
    int rc;
    volatile bool done;
};

static void hin_connect_thread(void *arg)
{
    struct hin_conn *c = arg;
    c->rc = ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c->s);
    if (c->rc == 0)
        c->rc = ksock_connect(c->s, &c->peer);
    c->done = true;
    thread_exit(0);
}

static bool hin_udp_listener(struct socket **out, uint32_t ip, uint16_t port)
{
    if (ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, out) != 0)
        return false;
    struct netaddr a = v4addr(ip, port);
    if (ksock_bind(*out, &a) != 0)
        return false;
    ksock_set_nonblock(*out, true);
    return true;
}

static bool hin_tcp_listener(struct socket **out, uint16_t port)
{
    if (ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, out) != 0)
        return false;
    struct netaddr a = v4addr(0, port);
    if (ksock_bind(*out, &a) != 0 || ksock_listen(*out, 4) != 0)
        return false;
    ksock_set_nonblock(*out, true);
    return true;
}

#define HIN_RULE(dir_, proto_, sip_, sp_, port_, verdict_) \
    ((struct fw_rule){ .direction = (dir_), .proto = (proto_), .verdict = (verdict_), .dst_port = (port_), \
                       .src_ip = (sip_), .src_prefix = (sp_) })

bool selftest_net_hostinput(const char **reason)
{
    *reason = NULL;
    fw_flush();
    nat_flush();

    /* The uplink: a real, non-guest link (neither masquerading nor loopback),
     * as net-dnat builds one, with the world on the far side. */
    static const uint8_t umac[6] = { 0x52, 0x54, 0x00, 0x1a, 0x00, 0x01 };
    static const uint8_t wmac[5][6] = {
        { 0x52, 0x54, 0x00, 0x1a, 0x00, 0x63 }, { 0x52, 0x54, 0x00, 0x1a, 0x00, 0x62 },
        { 0x52, 0x54, 0x00, 0x1a, 0x00, 0x61 }, { 0x52, 0x54, 0x00, 0x1a, 0x00, 0x60 },
        { 0x52, 0x54, 0x00, 0x1a, 0x00, 0x5f },
    };
    uint32_t u_ip = IPV4_ADDR(10, 77, 8, 1);
    uint32_t w[5] = { IPV4_ADDR(10, 77, 8, 99), IPV4_ADDR(10, 77, 8, 98), IPV4_ADDR(10, 77, 8, 97),
                      IPV4_ADDR(10, 77, 8, 96), IPV4_ADDR(10, 77, 8, 95) };
    struct tap *u = tap_create("hinu", u_ip, htonl(0xffffff00u), umac);
    CHECK(u != NULL);
    for (unsigned i = 0; i < 5; i++)
        nettest_seed_arp(tap_netif(u), w[i], wmac[i]);
    /* And one guest through /dev/net/tap (so it attaches): A on tap0. */
    struct file *fa = NULL;
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &fa) == 0 && fa != NULL);
    uint32_t ga = IPV4_ADDR(10, 0, 3, 15), gwa = IPV4_ADDR(10, 0, 3, 1);
    static const uint8_t amac[6] = { 0x52, 0x54, 0x00, 0x0f, 0x00, 0x0a };
    static const uint8_t tap0mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };
    { struct netif *n = netif_find("tap0"); CHECK(n != NULL); nettest_seed_arp(n, ga, amac); netif_put(n); }

    /* The host's services: two TCP listeners, a UDP listener, a UDP socket
     * connected to a world peer, and a loopback-bound UDP listener. */
    struct socket *ls1 = NULL, *ls2 = NULL, *us = NULL, *uc = NULL, *ul = NULL;
    CHECK(hin_tcp_listener(&ls1, 2222) && hin_tcp_listener(&ls2, 2223));
    CHECK(hin_udp_listener(&us, 0, 7000));
    CHECK(hin_udp_listener(&uc, 0, 7001));
    { struct netaddr p = v4addr(w[0], 5555); CHECK(ksock_connect(uc, &p) == 0); }
    CHECK(hin_udp_listener(&ul, INADDR_LOOPBACK_N, 7002));

    uint8_t l4[160], buf[256], pl[4] = { 'a', 'b', 'c', 'd' };
    uint16_t l4len;
    struct hin_seg sg;
    struct fw_stats fs0, fs1;
    struct ip_stats is0, is1;
    struct tcp_stats ts0, ts1;
    struct udp_stats us0, us1;
    hin_drain(u);

    /* (1) the host object ships with default ACCEPT and no rules, and the
     * world reaches the host's services under it. */
    uint8_t pu, pg, ph, pw;
    CHECK(fw_policy_get(FW_HOST_GUEST_IP, &pu, &pg, &ph, &pw) == 0 && pw == FW_ACCEPT);
    { struct fw_rule none[1]; CHECK(fw_rule_list(FW_HOST_GUEST_IP, none, 1) == 0); }
    fw_get_stats(&fs0);
    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1000, 0, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 40001, &sg, 50) && (sg.flags & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK) &&
          sg.ack == 1001);
    uint32_t iss1 = sg.seq;
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_accept_default, fs0.hin_accept_default));
    l4len = nettest_mk_udp(l4, w[0], u_ip, 6000, 7000, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(us, buf, sizeof(buf), 50) == 4 && memcmp(buf, pl, 4) == 0);

    /* (2) a DROP rule with a source: the prefix is matched, in both senses. */
    struct fw_rule r_src = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_TCP, IPV4_ADDR(10, 77, 8, 0), 24, 2222, FW_DROP);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_src) == 0);
    fw_get_stats(&fs0); ipv4_get_stats(&is0); tcp_get_stats(&ts0);
    hin_drain(u);
    l4len = hin_mk_tcp(l4, w[1], u_ip, 40002, 2222, 2000, 0, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[1], w[1], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_drop_rule, fs0.hin_drop_rule));
    CHECK(FWT_RISES(ipv4_get_stats, is1, hin_quiet, is0.hin_quiet));
    CHECK(FWT_RISES(tcp_get_stats, ts1, quiet_dropped, ts0.quiet_dropped));
    CHECK(!hin_recv(u, IPPROTO_TCP, 40002, &sg, 15));                 /* no SYN-ACK, no RST */
    fw_get_stats(&fs0); tcp_get_stats(&ts0);
    uint32_t far = IPV4_ADDR(10, 0, 9, 9);                            /* outside the prefix */
    l4len = hin_mk_tcp(l4, far, u_ip, 40009, 2222, 2000, 0, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[1], far, u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_accept_default, fs0.hin_accept_default));
    tcp_get_stats(&ts1);
    CHECK(ts1.quiet_dropped == ts0.quiet_dropped);
    CHECK(fw_rule_del(FW_HOST_GUEST_IP, &r_src) == 0);

    /* (3) quiet delivery: the transport decides, and answers nothing. C1's
     * handshake completes under ACCEPT; then a DROP rule covers its port. */
    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1001, iss1 + 1, TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    struct socket *a1 = hin_accept(ls1);
    CHECK(a1 != NULL);
    ksock_set_nonblock(a1, true);
    struct fw_rule r_2222 = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_TCP, 0, 0, 2222, FW_DROP);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_2222) == 0);
    hin_drain(u);
    ipv4_get_stats(&is0); tcp_get_stats(&ts0);
    /* the established connection's data is delivered and acknowledged */
    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1001, iss1 + 1, TH_ACK | TH_PSH, 64240, "hi", 2);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv_sock(a1, buf, sizeof(buf), 50) == 2 && memcmp(buf, "hi", 2) == 0);
    CHECK(FWT_RISES(ipv4_get_stats, is1, hin_quiet, is0.hin_quiet));
    CHECK(hin_recv(u, IPPROTO_TCP, 40001, &sg, 50) && (sg.flags & TH_ACK) && sg.ack == 1003);
    tcp_get_stats(&ts1);
    CHECK(ts1.quiet_dropped == ts0.quiet_dropped);
    /* an ACK-only probe, and a SYN+ACK, from a source with no connection: freed, no RST */
    tcp_get_stats(&ts0);
    l4len = hin_mk_tcp(l4, w[2], u_ip, 4444, 2222, 999, 777, TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[2], w[2], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(tcp_get_stats, ts1, quiet_dropped, ts0.quiet_dropped));
    CHECK(!hin_recv(u, IPPROTO_TCP, 4444, &sg, 15));
    tcp_get_stats(&ts0);
    l4len = hin_mk_tcp(l4, w[2], u_ip, 4445, 2222, 999, 777, TH_SYN | TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[2], w[2], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(tcp_get_stats, ts1, quiet_dropped, ts0.quiet_dropped));
    CHECK(!hin_recv(u, IPPROTO_TCP, 4445, &sg, 15));
    /* a new SYN: no SYN-cache entry, no cookie, no SYN-ACK, no RST */
    tcp_get_stats(&ts0);
    l4len = hin_mk_tcp(l4, w[2], u_ip, 4446, 2222, 3333, 0, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[2], w[2], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(tcp_get_stats, ts1, quiet_dropped, ts0.quiet_dropped));
    CHECK(ts1.syn_cached == ts0.syn_cached && ts1.syn_cookies_sent == ts0.syn_cookies_sent);
    CHECK(!hin_recv(u, IPPROTO_TCP, 4446, &sg, 15));
    /* UDP under a DROP rule: a connected socket's peer is delivered; a
     * listener and an unbound port get nothing back -- no port-unreachable. */
    struct fw_rule r_udp = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_UDP, 0, 0, 0, FW_DROP);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_udp) == 0);
    udp_get_stats(&us0);
    l4len = nettest_mk_udp(l4, w[0], u_ip, 5555, 7001, (const uint8_t *)"conn", 4);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(uc, buf, sizeof(buf), 50) == 4 && memcmp(buf, "conn", 4) == 0);
    udp_get_stats(&us1);
    CHECK(us1.quiet_dropped == us0.quiet_dropped);
    hin_drain(u);
    l4len = nettest_mk_udp(l4, w[0], u_ip, 6001, 7000, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(us, buf, sizeof(buf), 10) == -EAGAIN);          /* the listener hears nothing */
    CHECK(FWT_RISES(udp_get_stats, us1, quiet_dropped, us0.quiet_dropped));
    udp_get_stats(&us0);
    l4len = nettest_mk_udp(l4, w[0], u_ip, 6002, 7009, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(udp_get_stats, us1, quiet_dropped, us0.quiet_dropped));
    CHECK(!hin_recv(u, IPPROTO_ICMP, 0, &sg, 15));                    /* no port-unreachable */
    CHECK(fw_rule_del(FW_HOST_GUEST_IP, &r_udp) == 0);
    l4len = nettest_mk_udp(l4, w[0], u_ip, 6003, 7009, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_ICMP, 0, &sg, 50) && sg.flags == ICMP_DEST_UNREACH);   /* ...which ACCEPT draws */

    /* (4) TCP's own validation, silenced: on C1 under the DROP rule, an
     * out-of-window segment draws no window ACK, a mis-positioned reset, an
     * in-window SYN and an out-of-range ACK draw no challenge ACK -- all are
     * in-window rejections past the window test -- and the connection lives. */
    hin_drain(u);
    static const struct { uint32_t seq, ack; uint8_t flags; } bad[4] = {
        { 1003 + 100000, 0, TH_ACK },              /* out of window */
        { 1003 + 10, 0, TH_RST },                  /* in window, not rcv_nxt */
        { 1003, 0, TH_SYN },                       /* in-window SYN */
        { 1003, 0x40000000u, TH_ACK },             /* ACK outside [snd_una - max window, snd_max] */
    };
    for (unsigned i = 0; i < 4; i++) {
        tcp_get_stats(&ts0);
        uint32_t ack = bad[i].flags == TH_RST ? 0 : iss1 + 1 + bad[i].ack;
        l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, bad[i].seq, ack, bad[i].flags, 64240, NULL, 0);
        CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
        CHECK(!hin_recv(u, IPPROTO_TCP, 40001, &sg, 10));               /* no window ACK, no challenge */
        tcp_get_stats(&ts1);
        CHECK(ts1.challenge_acks == ts0.challenge_acks);
        CHECK(ts1.quiet_dropped > ts0.quiet_dropped);
    }
    CHECK(hin_recv_sock(a1, buf, sizeof(buf), 2) == -EAGAIN);         /* alive, not reset */
    /* Rejected probes consume no shared budget: more than TCP_CHALLENGE_PER_SEC
     * in-window SYNs under the rule, then a legitimate in-window SYN on C2
     * (port 2223, no rule) still draws its RFC 5961 challenge ACK. */
    tcp_get_stats(&ts0);
    for (unsigned i = 0; i < TCP_CHALLENGE_PER_SEC + 20; i++) {
        l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1003, iss1 + 1, TH_SYN, 64240, NULL, 0);
        CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
        if (i % 20 == 19)
            thread_sleep_ms(10);   /* paced under the receive queue's depth, well inside one budget second */
    }
    CHECK(FWT_RISES(tcp_get_stats, ts1, quiet_dropped, ts0.quiet_dropped + TCP_CHALLENGE_PER_SEC + 19));
    CHECK(ts1.challenge_acks == ts0.challenge_acks);
    l4len = hin_mk_tcp(l4, w[1], u_ip, 40002, 2223, 3000, 0, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[1], w[1], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 40002, &sg, 50) && (sg.flags & TH_SYN) && sg.ack == 3001);
    uint32_t iss2 = sg.seq;
    l4len = hin_mk_tcp(l4, w[1], u_ip, 40002, 2223, 3001, iss2 + 1, TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[1], w[1], u_ip, IPPROTO_TCP, l4, l4len));
    struct socket *a2 = hin_accept(ls2);
    CHECK(a2 != NULL);
    ksock_set_nonblock(a2, true);
    tcp_get_stats(&ts0);
    l4len = hin_mk_tcp(l4, w[1], u_ip, 40002, 2223, 3001, iss2 + 1, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[1], w[1], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 40002, &sg, 50) && sg.flags == TH_ACK && sg.ack == 3001);   /* the challenge */
    CHECK(FWT_RISES(tcp_get_stats, ts1, challenge_acks, ts0.challenge_acks));

    /* The keepalive clock is not refreshed by a rejected segment: with a
     * short idle time armed at C4's establishment and rejected probes arriving
     * every 40 ms under a rule covering the peer, the probe still fires. */
    tcp_set_keepalive(150ull * 1000000ull, 50ull * 1000000ull, 3);
    l4len = hin_mk_tcp(l4, w[3], u_ip, 40004, 2223, 4000, 0, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[3], w[3], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 40004, &sg, 50) && (sg.flags & TH_SYN) && sg.ack == 4001);
    uint32_t iss4 = sg.seq;
    /* The DROP rule covering the peer lands between the admitted SYN and its
     * completing ACK: the ACK is delivered quiet, and the SYN-cache completion
     * still creates the child -- the fourth acceptance point. */
    struct fw_rule r_w3 = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_TCP, w[3], 32, 2223, FW_DROP);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_w3) == 0);
    ipv4_get_stats(&is0); tcp_get_stats(&ts0);
    l4len = hin_mk_tcp(l4, w[3], u_ip, 40004, 2223, 4001, iss4 + 1, TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[3], w[3], u_ip, IPPROTO_TCP, l4, l4len));
    struct socket *a4 = hin_accept(ls2);
    CHECK(a4 != NULL);
    CHECK(FWT_RISES(ipv4_get_stats, is1, hin_quiet, is0.hin_quiet));
    tcp_get_stats(&ts1);
    CHECK(ts1.quiet_dropped == ts0.quiet_dropped);
    /* (the short idle stays in force until the probe has fired: the timer
     * re-reads it when it re-arms, so restoring the default now would arm two
     * hours and prove nothing) */
    tcp_get_stats(&ts0);
    for (unsigned i = 0; i < 10; i++) {
        l4len = hin_mk_tcp(l4, w[3], u_ip, 40004, 2223, 4001 + 100000, iss4 + 1, TH_ACK, 64240, NULL, 0);
        CHECK(hin_send(u, umac, wmac[3], w[3], u_ip, IPPROTO_TCP, l4, l4len));
        thread_sleep_ms(40);
    }
    tcp_get_stats(&ts1);
    tcp_set_keepalive(0, 0, 0);
    CHECK(ts1.keepalive_probes > ts0.keepalive_probes);
    CHECK(ts1.quiet_dropped >= ts0.quiet_dropped + 10);
    CHECK(fw_rule_del(FW_HOST_GUEST_IP, &r_w3) == 0);
    ksock_put(a4);

    /* The host's own outbound connection completes under a rule covering the
     * peer (the SYN_SENT acceptance point), and a bare ACK to a tuple with no
     * connection from that peer is freed silently. */
    struct fw_rule r_w4 = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_TCP, w[4], 32, 0, FW_DROP);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_w4) == 0);
    hin_drain(u);
    struct hin_conn cn = { .peer = v4addr(w[4], 9000) };
    struct thread *ct = thread_create(hin_connect_thread, &cn, "hin-connect", SCHED_PRIO_DEFAULT);
    CHECK(ct != NULL);
    CHECK(hin_recv(u, IPPROTO_TCP, 9000, &sg, 50) && sg.flags == TH_SYN);   /* the host's SYN */
    uint32_t hiss = sg.seq;
    uint16_t hport = sg.sport;
    ipv4_get_stats(&is0);
    l4len = hin_mk_tcp(l4, w[4], u_ip, 9000, hport, 5000, hiss + 1, TH_SYN | TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[4], w[4], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, hin_quiet, is0.hin_quiet));
    CHECK(hin_recv(u, IPPROTO_TCP, 9000, &sg, 50) && sg.flags == TH_ACK && sg.ack == 5001);   /* completing ACK */
    for (unsigned i = 0; i < 100 && !cn.done; i++)
        thread_sleep_ms(10);
    CHECK(cn.done && cn.rc == 0);
    thread_join(ct);
    tcp_get_stats(&ts0);
    l4len = hin_mk_tcp(l4, w[4], u_ip, 9001, hport, 5001, hiss + 1, TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[4], w[4], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(tcp_get_stats, ts1, quiet_dropped, ts0.quiet_dropped));
    CHECK(!hin_recv(u, IPPROTO_TCP, 9001, &sg, 15));
    /* The host closes first: its FIN goes out; the peer's ACK and FIN are
     * accepted (TIME_WAIT); a bare ACK there draws nothing; a retransmitted
     * FIN -- an exact-position match on the connection -- is acknowledged. */
    ksock_put(cn.s);
    CHECK(hin_recv(u, IPPROTO_TCP, 9000, &sg, 50) && (sg.flags & TH_FIN) && sg.seq == hiss + 1);
    l4len = hin_mk_tcp(l4, w[4], u_ip, 9000, hport, 5001, hiss + 2, TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[4], w[4], u_ip, IPPROTO_TCP, l4, l4len));
    l4len = hin_mk_tcp(l4, w[4], u_ip, 9000, hport, 5001, hiss + 2, TH_FIN | TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[4], w[4], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 9000, &sg, 50) && sg.flags == TH_ACK && sg.ack == 5002);
    hin_drain(u);
    l4len = hin_mk_tcp(l4, w[4], u_ip, 9000, hport, 5002, hiss + 2, TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[4], w[4], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(!hin_recv(u, IPPROTO_TCP, 9000, &sg, 10));
    l4len = hin_mk_tcp(l4, w[4], u_ip, 9000, hport, 5001, hiss + 2, TH_FIN | TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[4], w[4], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 9000, &sg, 50) && sg.flags == TH_ACK && sg.ack == 5002);
    CHECK(fw_rule_del(FW_HOST_GUEST_IP, &r_w4) == 0);

    /* Accepted segments that do not advance snd_una keep their output (C1,
     * still under the port-2222 DROP rule): three duplicate ACKs build the
     * fast retransmission; a window update from zero re-enables a blocked
     * send; data with an unchanged ACK is delivered and acknowledged; the
     * peer's FIN is acknowledged and the socket reads EOF. */
    hin_drain(u);
    uint8_t data[100];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)i;
    CHECK(ksock_sendto(a1, data, sizeof(data), NULL) == (int64_t)sizeof(data));
    CHECK(hin_recv(u, IPPROTO_TCP, 40001, &sg, 50) && sg.paylen == 100 && sg.seq == iss1 + 1);
    tcp_get_stats(&ts0);
    for (unsigned i = 0; i < 3; i++) {
        l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1003, iss1 + 1, TH_ACK, 64240, NULL, 0);
        CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    }
    CHECK(hin_recv(u, IPPROTO_TCP, 40001, &sg, 20) && sg.paylen == 100 && sg.seq == iss1 + 1);   /* retransmitted */
    tcp_get_stats(&ts1);
    CHECK(ts1.retransmits > ts0.retransmits && ts1.quiet_dropped == ts0.quiet_dropped);
    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1003, iss1 + 101, TH_ACK, 0, NULL, 0);   /* all acked, window 0 */
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    hin_drain(u);
    CHECK(ksock_sendto(a1, data, 50, NULL) == 50);
    CHECK(!(hin_recv(u, IPPROTO_TCP, 40001, &sg, 15) && sg.paylen >= 50));   /* blocked by the zero window */
    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1003, iss1 + 101, TH_ACK, 64240, NULL, 0);   /* window update */
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 40001, &sg, 50) && sg.paylen == 50 && sg.seq == iss1 + 101);
    hin_drain(u);
    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1003, iss1 + 151, TH_ACK, 64240, data, 10);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1013, iss1 + 151, TH_ACK, 64240, data, 10);   /* unchanged ACK */
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    {
        /* 20 bytes, whether the read catches them together or one segment
         * at a time (the worker may still be queuing the second). */
        int64_t got = 0, n;
        while (got < 20 && (n = hin_recv_sock(a1, buf + got, sizeof(buf) - (size_t)got, 50)) > 0)
            got += n;
        CHECK(got == 20);
    }
    {
        /* Both segments are acknowledged: at once, or the first at once and
         * the second by the delayed-ACK timer -- the last ACK names 1023. */
        bool acked = false;
        for (unsigned i = 0; i < 3 && !acked; i++)
            acked = hin_recv(u, IPPROTO_TCP, 40001, &sg, 50) && (sg.flags & TH_ACK) && sg.ack == 1023;
        CHECK(acked);
    }
    l4len = hin_mk_tcp(l4, w[0], u_ip, 40001, 2222, 1023, iss1 + 151, TH_FIN | TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 40001, &sg, 50) && (sg.flags & TH_ACK) && sg.ack == 1024);
    CHECK(hin_recv_sock(a1, buf, sizeof(buf), 50) == 0);                       /* EOF */
    ksock_put(a1);
    /* A valid reset (seq == rcv_nxt) from a peer under a DROP rule still tears
     * C2 down: the socket reports it, and nothing is emitted. */
    struct fw_rule r_w1 = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_TCP, w[1], 32, 2223, FW_DROP);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_w1) == 0);
    hin_drain(u);
    tcp_get_stats(&ts0);
    l4len = hin_mk_tcp(l4, w[1], u_ip, 40002, 2223, 3001, iss2 + 1, TH_RST | TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[1], w[1], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv_sock(a2, buf, sizeof(buf), 50) == -ECONNRESET);
    tcp_get_stats(&ts1);
    CHECK(ts1.rsts_in > ts0.rsts_in && ts1.quiet_dropped == ts0.quiet_dropped);
    CHECK(!hin_recv(u, IPPROTO_TCP, 40002, &sg, 10));
    CHECK(fw_rule_del(FW_HOST_GUEST_IP, &r_w1) == 0);
    CHECK(fw_rule_del(FW_HOST_GUEST_IP, &r_2222) == 0);
    ksock_put(a2);

    /* (5) UDP and ICMP are per datagram: an icmp type-8 DROP drops an echo
     * request -- delivered quiet, then freed by icmp_input, the one place
     * that dispatches ICMP (the host-state unit) -- and no reply comes back;
     * a type-0 datagram passes the default; without the rule echo is
     * answered. */
    struct fw_rule r_echo = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_ICMP, 0, 0, ICMP_ECHO, FW_DROP);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_echo) == 0);
    hin_drain(u);
    fw_get_stats(&fs0); ipv4_get_stats(&is0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO, 0x5151);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_drop_rule, fs0.hin_drop_rule));
    CHECK(FWT_RISES(ipv4_get_stats, is1, icmp_quiet_dropped, is0.icmp_quiet_dropped));
    CHECK(!hin_recv(u, IPPROTO_ICMP, 0, &sg, 15));
    fw_get_stats(&fs0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO_REPLY, 0x5151);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_accept_default, fs0.hin_accept_default));
    CHECK(fw_rule_del(FW_HOST_GUEST_IP, &r_echo) == 0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO, 0x5152);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_ICMP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_ICMP, 0, &sg, 50) && sg.flags == ICMP_ECHO_REPLY);

    /* (6) off-link: a link's datagrams are for that link's address. No rule
     * installed. The world's query to guest A's gateway (the tap's DNS proxy)
     * and its datagram to a loopback-bound listener are dropped before any
     * chain; the loopback listener still hears a loopback sender; a guest's
     * datagram to 127.0.0.1 or to the uplink's address is dropped likewise. */
    fw_get_stats(&fs0); ipv4_get_stats(&is0);
    l4len = nettest_mk_udp(l4, w[0], gwa, 6100, 53, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac[0], w[0], gwa, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, rx_offlink, is0.rx_offlink));
    ipv4_get_stats(&is0);
    l4len = nettest_mk_udp(l4, w[0], INADDR_LOOPBACK_N, 6101, 7002, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac[0], w[0], INADDR_LOOPBACK_N, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, rx_offlink, is0.rx_offlink));
    CHECK(hin_recv_sock(ul, buf, sizeof(buf), 5) == -EAGAIN);
    {
        struct socket *lo = NULL;
        CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &lo) == 0);
        struct netaddr to = v4addr(INADDR_LOOPBACK_N, 7002);
        CHECK(ksock_sendto(lo, pl, sizeof(pl), &to) == 4);
        CHECK(hin_recv_sock(ul, buf, sizeof(buf), 50) == 4);
        ksock_put(lo);
    }
    ipv4_get_stats(&is0);
    l4len = nettest_mk_udp(l4, ga, INADDR_LOOPBACK_N, 6102, 7002, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, INADDR_LOOPBACK_N, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, rx_offlink, is0.rx_offlink));
    ipv4_get_stats(&is0);
    l4len = nettest_mk_udp(l4, ga, u_ip, 6103, 7000, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, rx_offlink, is0.rx_offlink));
    CHECK(is1.in_filtered == is0.in_filtered);                           /* not INPUT's default any more */
    fw_get_stats(&fs1);
    CHECK(fs1.hin_accept_rule == fs0.hin_accept_rule && fs1.hin_drop_rule == fs0.hin_drop_rule &&
          fs1.hin_accept_default == fs0.hin_accept_default && fs1.hin_drop_default == fs0.hin_drop_default);
    /* Not this chain's ingress: a guest-tap datagram to the host still takes
     * the INPUT chain (A's DNS seed admits it), and the host chain's counters
     * do not move for it. */
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, gwa, 6104, 53, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gwa, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, in_accept_rule, fs0.in_accept_rule));
    CHECK(fs1.hin_accept_default == fs0.hin_accept_default && fs1.hin_drop_default == fs0.hin_drop_default);

    /* (7) ordering after nat_in, now provable: with a port-forward to A and
     * the host default DROP, the world's SYN to host:8080 is still DNAT'd to
     * A -- authorized by the pf rule, never re-gated -- and the host chain's
     * counters do not move. Loopback is untouched by the default. */
    CHECK(nat_pf_add(IPPROTO_TCP, 8080, ga, 80) == 0);
    CHECK(fw_policy_set(FW_HOST_GUEST_IP, FW_DIR_FROM_UPLINK, FW_DROP) == 0);
    fw_get_stats(&fs0);
    l4len = hin_mk_tcp(l4, w[0], u_ip, 41000, 8080, 7000, 0, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[0], w[0], u_ip, IPPROTO_TCP, l4, l4len));
    {
        uint8_t rx[160];
        int64_t n = fwt_recv(fa, rx, sizeof(rx), 50);
        CHECK(n >= ETH_HLEN + 40);
        const struct ipv4_hdr *ri = (const struct ipv4_hdr *)(rx + ETH_HLEN);
        CHECK(ri->src == w[0] && ri->dst == ga && ri->proto == IPPROTO_TCP);
        CHECK((uint16_t)(rx[ETH_HLEN + 20 + 2] << 8 | rx[ETH_HLEN + 20 + 3]) == 80);
    }
    fw_get_stats(&fs1);
    CHECK(fs1.hin_drop_default == fs0.hin_drop_default && fs1.hin_drop_rule == fs0.hin_drop_rule);
    {
        struct socket *lo = NULL;
        CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &lo) == 0);
        struct netaddr to = v4addr(INADDR_LOOPBACK_N, 7002);
        CHECK(ksock_sendto(lo, pl, sizeof(pl), &to) == 4);
        CHECK(hin_recv_sock(ul, buf, sizeof(buf), 50) == 4);
        ksock_put(lo);
    }
    /* Default flip: under DROP an unruled SYN drops; an explicit ACCEPT rule
     * readmits it; back to ACCEPT. */
    hin_drain(u);
    fw_get_stats(&fs0);
    l4len = hin_mk_tcp(l4, w[2], u_ip, 40010, 2223, 8000, 0, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[2], w[2], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_drop_default, fs0.hin_drop_default));
    CHECK(!hin_recv(u, IPPROTO_TCP, 40010, &sg, 15));
    struct fw_rule r_open = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_TCP, 0, 0, 2223, FW_ACCEPT);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_open) == 0);
    fw_get_stats(&fs0);
    l4len = hin_mk_tcp(l4, w[2], u_ip, 40011, 2223, 8000, 0, TH_SYN, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac[2], w[2], u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 40011, &sg, 50) && (sg.flags & TH_SYN));
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_accept_rule, fs0.hin_accept_rule));
    CHECK(fw_rule_del(FW_HOST_GUEST_IP, &r_open) == 0);
    CHECK(fw_policy_set(FW_HOST_GUEST_IP, FW_DIR_FROM_UPLINK, FW_ACCEPT) == 0);
    CHECK(nat_pf_del(IPPROTO_TCP, 8080));

    /* (8) scope discipline: the host owns FROM_UPLINK and the source; a guest
     * owns the other three and no source; ANY is nobody's on the host. */
    struct fw_rule bad_host = HIN_RULE(FW_DIR_TO_HOST, IPPROTO_TCP, 0, 0, 22, FW_DROP);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &bad_host) == -EINVAL);
    struct fw_rule bad_any = HIN_RULE(FW_DIR_ANY, IPPROTO_TCP, 0, 0, 22, FW_ACCEPT);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &bad_any) == -EINVAL);
    struct fw_rule bad_guest = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_TCP, 0, 0, 22, FW_DROP);
    CHECK(fw_rule_add(ga, 0, &bad_guest) == -EINVAL);
    struct fw_rule bad_src = HIN_RULE(FW_DIR_TO_HOST, IPPROTO_TCP, IPV4_ADDR(10, 0, 0, 0), 8, 22, FW_ACCEPT);
    CHECK(fw_rule_add(ga, 0, &bad_src) == -EINVAL);
    struct fw_rule bad_wild = HIN_RULE(FW_DIR_FROM_UPLINK, IPPROTO_TCP, IPV4_ADDR(10, 0, 0, 0), 0, 22, FW_DROP);
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &bad_wild) == -EINVAL);          /* "any source" is 0/0 */
    CHECK(fw_policy_set(FW_HOST_GUEST_IP, FW_DIR_TO_HOST, FW_ACCEPT) == -EINVAL);
    CHECK(fw_policy_set(ga, FW_DIR_FROM_UPLINK, FW_DROP) == -EINVAL);
    CHECK(fw_rule_del(FW_HOST_GUEST_IP, &r_2222) == -ENOENT);

    /* (9) the control channel: a host rule with a source written through
     * tapctl (guest_addr 0) is listed in the host's record with its source
     * beside policy_from_uplink; a host rule naming TO_HOST and a version-3
     * sized write are refused. */
    struct file *fctl = NULL;
    CHECK(vfs_open(NULL, "/dev/net/tapctl", COSMO_O_RDWR, 0, &fctl) == 0 && fctl != NULL);
    struct cosmo_netctl_filter c = { .version = COSMO_NETCTL_VERSION, .op = COSMO_NETCTL_FILTER_ADD,
                                     .guest_addr = COSMO_NETCTL_HOST_ADDR, .direction = COSMO_NETCTL_DIR_FROM_UPLINK,
                                     .proto = COSMO_NETCTL_PROTO_TCP, .verdict = COSMO_NETCTL_VERDICT_DROP,
                                     .dst_port = 2224, .src_addr = IPV4_ADDR(10, 77, 8, 0), .src_prefix = 24 };
    CHECK(file_write(fctl, &c, sizeof(c)) == (int64_t)sizeof(c));
    CHECK(file_write(fctl, &c, 20) == -EINVAL);                            /* a version-3 sized command */
    struct cosmo_netctl_filter cb = c;
    cb.direction = COSMO_NETCTL_DIR_TO_HOST; cb.dst_port = 2225;
    CHECK(file_write(fctl, &cb, sizeof(cb)) == -EINVAL);
    cb = c; cb.guest_addr = ga; cb.dst_port = 2226;
    CHECK(file_write(fctl, &cb, sizeof(cb)) == -EINVAL);                   /* a guest may not name FROM_UPLINK */
    static uint8_t snap[COSMO_NETCTL_SNAPSHOT_MAX];
    int64_t sn = file_read(fctl, snap, sizeof(snap));
    CHECK(sn > 0);
    {
        struct cosmo_netctl_list phh; memcpy(&phh, snap, sizeof(phh));
        size_t off = sizeof(phh) + (size_t)phh.count * sizeof(struct cosmo_netctl_rule);
        struct cosmo_netctl_filter_list fh; memcpy(&fh, snap + off, sizeof(fh));
        CHECK(fh.version == COSMO_NETCTL_VERSION && fh.guest_count >= 2);
        off += sizeof(fh);
        bool host_seen = false;
        for (unsigned i = 0; i < fh.guest_count; i++, off += sizeof(struct cosmo_netctl_filter_guest)) {
            struct cosmo_netctl_filter_guest fg; memcpy(&fg, snap + off, sizeof(fg));
            if (fg.guest_addr == COSMO_NETCTL_HOST_ADDR)
                host_seen = fg.policy_from_uplink == COSMO_NETCTL_VERDICT_ACCEPT;
        }
        CHECK(host_seen);
        bool rule_seen = false;
        for (unsigned i = 0; i < fh.rule_count; i++) {
            struct cosmo_netctl_filter_rule fr; memcpy(&fr, snap + off + i * sizeof(fr), sizeof(fr));
            if (fr.guest_addr == COSMO_NETCTL_HOST_ADDR && fr.dst_port == 2224)
                rule_seen = fr.direction == COSMO_NETCTL_DIR_FROM_UPLINK && fr.src_addr == IPV4_ADDR(10, 77, 8, 0) &&
                            fr.src_prefix == 24 && fr.verdict == COSMO_NETCTL_VERDICT_DROP;
        }
        CHECK(rule_seen);
    }
    c.op = COSMO_NETCTL_FILTER_DEL;
    CHECK(file_write(fctl, &c, sizeof(c)) == (int64_t)sizeof(c));
    { struct fw_rule none[1]; CHECK(fw_rule_list(FW_HOST_GUEST_IP, none, 1) == 0); }
    file_put(fctl);

    ksock_put(ls1); ksock_put(ls2); ksock_put(us); ksock_put(uc); ksock_put(ul);
    file_put(fa);
    hin_drain(u);
    tap_destroy(u);
    fw_flush();
    nat_flush();
    kinfo("selftest: net-hostinput: the world reached the host under the default, a sourced DROP matched by "
          "prefix, a DROP was quiet (established data and the host's own connect completed, probes and new "
          "SYNs drew no RST, SYN-ACK, challenge or window ACK, no port-unreachable, no budget spent, no "
          "keepalive refresh; dup-ACKs, a window update, data and a FIN kept their output; a valid reset "
          "applied), ICMP dropped by type, off-link datagrams dropped before any chain, DNAT never re-gated, "
          "the scopes held, and the control listing carried the host record with its source");
    return true;
}

/* --- the host's own flows (docs/audit/next-subsystem-host-state.md) ------- */

/* The host's echo-reply hook: what reached it, and how often. */
static struct { volatile uint32_t src; volatile uint16_t id, seq; volatile unsigned n; } g_hst_echo;

static void hst_echo_hook(uint32_t src, uint16_t id, uint16_t seq)
{
    g_hst_echo.src = src;
    g_hst_echo.id = id;
    g_hst_echo.seq = seq;
    g_hst_echo.n++;
}

static bool hst_echo_arrived(unsigned base, unsigned tries)
{
    for (unsigned i = 0; i < tries; i++) {
        if (g_hst_echo.n > base)
            return true;
        thread_sleep_ms(10);
    }
    return false;
}

/* An ICMP Need-Fragmentation (type 3 code 4, next-hop MTU `mtu`) quoting a
 * TCP segment the host sent: the IP header the host wrote plus the first 8
 * bytes of the segment (ports and sequence), which is all icmp_needfrag
 * reads. 36 bytes, checksummed -- icmp_input validates it. */
static uint16_t hst_mk_needfrag_tcp(uint8_t *l4, uint32_t host_ip, uint32_t peer_ip, uint16_t sport,
                                    uint16_t dport, uint32_t seq, uint16_t mtu)
{
    memset(l4, 0, 36);
    l4[0] = ICMP_DEST_UNREACH;
    l4[1] = ICMP_UNREACH_NEEDFRAG;
    l4[6] = (uint8_t)(mtu >> 8);                 /* the header's last 16 bits: the next-hop MTU */
    l4[7] = (uint8_t)mtu;
    struct ipv4_hdr *q = (struct ipv4_hdr *)(l4 + 8);
    q->vhl = 0x45; q->len = htons(1240); q->ttl = 64; q->proto = IPPROTO_TCP;
    q->src = host_ip; q->dst = peer_ip;
    uint8_t *th = l4 + 8 + 20;
    th[0] = (uint8_t)(sport >> 8); th[1] = (uint8_t)sport;
    th[2] = (uint8_t)(dport >> 8); th[3] = (uint8_t)dport;
    th[4] = (uint8_t)(seq >> 24); th[5] = (uint8_t)(seq >> 16);
    th[6] = (uint8_t)(seq >> 8);  th[7] = (uint8_t)seq;
    uint16_t ck = fwt_ones_sum(l4, 36);
    l4[2] = (uint8_t)(ck >> 8);
    l4[3] = (uint8_t)ck;
    return 36;
}

/* An ICMP port-unreachable quoting a TCP segment: an error with no consumer. */
static uint16_t hst_mk_unreach_tcp(uint8_t *l4, uint32_t host_ip, uint32_t peer_ip, uint16_t sport,
                                   uint16_t dport, uint32_t seq)
{
    uint16_t n = hst_mk_needfrag_tcp(l4, host_ip, peer_ip, sport, dport, seq, 0);
    l4[1] = ICMP_UNREACH_PORT;
    l4[6] = l4[7] = 0;
    l4[2] = l4[3] = 0;
    uint16_t ck = fwt_ones_sum(l4, n);
    l4[2] = (uint8_t)(ck >> 8);
    l4[3] = (uint8_t)ck;
    return n;
}

bool selftest_net_hoststate(const char **reason)
{
    *reason = NULL;
    fw_flush();
    nat_flush();

    /* The uplink: a real, non-guest link, the world on its far side. */
    static const uint8_t umac[6]  = { 0x52, 0x54, 0x00, 0x1b, 0x00, 0x01 };
    static const uint8_t wmac[6]  = { 0x52, 0x54, 0x00, 0x1b, 0x00, 0x63 };
    static const uint8_t gtmac[6] = { 0x52, 0x54, 0x00, 0x1b, 0x00, 0x11 };
    static const uint8_t gmac[6]  = { 0x52, 0x54, 0x00, 0x1b, 0x00, 0x1f };
    uint32_t u_ip = IPV4_ADDR(10, 77, 9, 1);
    uint32_t w = IPV4_ADDR(10, 77, 9, 99), w2 = IPV4_ADDR(10, 77, 9, 98);
    struct tap *u = tap_create("hstu", u_ip, htonl(0xffffff00u), umac);
    CHECK(u != NULL);
    nettest_seed_arp(tap_netif(u), w, wmac);
    nettest_seed_arp(tap_netif(u), w2, wmac);

    /* A DROP rule covering this test's world, so every admission below is
     * the host's own state and nothing else -- and scoped by source rather
     * than left as the machine-wide default, so that a failing assertion
     * here cannot harden the real uplink for every test that follows. (The
     * hardened *default* is exercised at the end, with its verdicts taken
     * before anything is asserted.) */
    struct fw_rule r_world = { .direction = FW_DIR_FROM_UPLINK, .verdict = FW_DROP,
                               .src_ip = IPV4_ADDR(10, 77, 9, 0), .src_prefix = 24 };
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_world) == 0);

    struct socket *cs = NULL, *cs2 = NULL, *cs3 = NULL;
    CHECK(hin_udp_listener(&cs, 0, 7100));       /* bound, never connected */
    CHECK(hin_udp_listener(&cs2, 0, 7101));
    CHECK(hin_udp_listener(&cs3, 0, 7102));
    struct netaddr peer = v4addr(w, 5300);
    uint8_t pl[4] = { 'p', 'i', 'n', 'g' }, l4[160], buf[256], frame[512];
    uint16_t l4len;
    struct hin_seg sg;
    struct fw_stats fs0, fs1;
    struct udp_stats us0, us1;
    struct tcp_stats ts0, ts1;
    struct ip_stats is0, is1;
    hin_drain(u);

    /* (1) an unconnected UDP client's reply survives the DROP. The send is
     * recorded on its way out; the reply on that one tuple is admitted by
     * state, with no rule anywhere. */
    fw_get_stats(&fs0);
    CHECK(ksock_sendto(cs, pl, sizeof(pl), &peer) == (int64_t)sizeof(pl));
    CHECK(hin_recv(u, IPPROTO_UDP, 5300, &sg, 50) && sg.sport == 7100 && sg.src == u_ip);
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_new == fs0.hin_flow_new + 1);
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, w, u_ip, 5300, 7100, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(cs, buf, sizeof(buf), 50) == (int64_t)sizeof(pl));
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_accept_established, fs0.hin_accept_established));

    /* The tuple is exactly one: another port at that peer, another peer, and
     * another local port match no flow and take the default. */
    udp_get_stats(&us0);
    l4len = nettest_mk_udp(l4, w, u_ip, 5301, 7100, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(cs, buf, sizeof(buf), 10) == -EAGAIN);
    CHECK(FWT_RISES(udp_get_stats, us1, quiet_dropped, us0.quiet_dropped));
    udp_get_stats(&us0);
    l4len = nettest_mk_udp(l4, w2, u_ip, 5300, 7100, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac, w2, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(cs, buf, sizeof(buf), 10) == -EAGAIN);
    CHECK(FWT_RISES(udp_get_stats, us1, quiet_dropped, us0.quiet_dropped));
    udp_get_stats(&us0);
    l4len = nettest_mk_udp(l4, w, u_ip, 5300, 7101, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(cs2, buf, sizeof(buf), 10) == -EAGAIN);
    CHECK(FWT_RISES(udp_get_stats, us1, quiet_dropped, us0.quiet_dropped));

    /* (2) one tuple, and no notion of intent: a second unsolicited datagram
     * on the open tuple is admitted too (the socket's own validation is the
     * second line), while the world initiating to a port the host never sent
     * from takes the rules. */
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, w, u_ip, 5300, 7100, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(cs, buf, sizeof(buf), 50) == (int64_t)sizeof(pl));
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_accept_established, fs0.hin_accept_established));
    udp_get_stats(&us0);
    l4len = nettest_mk_udp(l4, w, u_ip, 5400, 7102, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(cs3, buf, sizeof(buf), 10) == -EAGAIN);
    CHECK(FWT_RISES(udp_get_stats, us1, quiet_dropped, us0.quiet_dropped));
    /* The only other match flow_find can report is the *forward* one -- the
     * host's own tuple arriving inbound -- which on a real link means a
     * datagram carrying one of our addresses as its source: ipv4_input drops
     * it as a martian before any chain, which is why the state step need
     * admit on the reverse match alone. */
    ipv4_get_stats(&is0);
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, u_ip, u_ip, 7100, 5300, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac, u_ip, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, rx_bad_header, is0.rx_bad_header));
    fw_get_stats(&fs1);
    CHECK(fs1.hin_accept_established == fs0.hin_accept_established &&
          fs1.hin_drop_rule == fs0.hin_drop_rule && fs1.hin_accept_rule == fs0.hin_accept_rule);

    /* (3) the host can ping under the DROP: its echo request is recorded by
     * identifier and the reply reaches the hook. */
    icmp_set_echo_reply_hook(hst_echo_hook);
    unsigned echoes = g_hst_echo.n;
    fw_get_stats(&fs0);
    CHECK(icmp_send_echo(w, 0x7a7a, 1, "q", 1) == 0);
    CHECK(hin_recv(u, IPPROTO_ICMP, 0, &sg, 50) && sg.flags == ICMP_ECHO);
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_new == fs0.hin_flow_new + 1);
    fw_get_stats(&fs0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO_REPLY, 0x7a7a);
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_ICMP, l4, l4len));
    CHECK(hst_echo_arrived(echoes, 50));
    CHECK(g_hst_echo.id == 0x7a7a && g_hst_echo.src == w);
    CHECK(FWT_RISES(fw_get_stats, fs1, hin_accept_established, fs0.hin_accept_established));
    /* A reply carrying another identifier matches no flow: freed, no hook. */
    echoes = g_hst_echo.n;
    ipv4_get_stats(&is0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO_REPLY, 0x7a7b);
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, icmp_quiet_dropped, is0.icmp_quiet_dropped));
    CHECK(!hst_echo_arrived(echoes, 5));

    /* (4) an echo *request* is a request, not a reply: quiet delivery reaches
     * icmp_input, which answers nothing and does not spend the host-wide
     * echo-reply budget. */
    hin_drain(u);
    ipv4_get_stats(&is0);
    l4len = fwt_mk_icmp_ck(l4, ICMP_ECHO, 0x5050);
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, icmp_quiet_dropped, is0.icmp_quiet_dropped));
    CHECK(!hin_recv(u, IPPROTO_ICMP, 0, &sg, 10));
    CHECK(is1.icmp_echo_replied == is0.icmp_echo_replied && is1.icmp_echo_rcvd == is0.icmp_echo_rcvd);

    /* (5) path-MTU discovery survives the DROP. The host opens a connection
     * outbound (PR #107's SYN_SENT acceptance point admits the SYN+ACK),
     * sends a large segment, and the router's Need-Fragmentation -- delivered
     * quiet -- is consumed only because TCP confirms the quoted segment is
     * one of its own in flight. */
    hin_drain(u);
    fw_get_stats(&fs0);
    struct hin_conn cn = { .peer = v4addr(w, 9100) };
    struct thread *ct = thread_create(hin_connect_thread, &cn, "hst-connect", SCHED_PRIO_DEFAULT);
    CHECK(ct != NULL);
    CHECK(hin_recv(u, IPPROTO_TCP, 9100, &sg, 50) && sg.flags == TH_SYN);
    uint32_t hiss = sg.seq;
    uint16_t hport = sg.sport;
    l4len = hin_mk_tcp(l4, w, u_ip, 9100, hport, 6000, hiss + 1, TH_SYN | TH_ACK, 64240, NULL, 0);
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_TCP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_TCP, 9100, &sg, 50) && sg.flags == TH_ACK && sg.ack == 6001);
    for (unsigned i = 0; i < 100 && !cn.done; i++)
        thread_sleep_ms(10);
    CHECK(cn.done && cn.rc == 0);
    thread_join(ct);
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_new == fs0.hin_flow_new);   /* TCP is not recorded: no lock on that send path */
    static uint8_t big[1200];
    for (unsigned i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)i;
    CHECK(ksock_sendto(cn.s, big, sizeof(big), NULL) == (int64_t)sizeof(big));
    CHECK(hin_recv(u, IPPROTO_TCP, 9100, &sg, 50) && sg.paylen > 600);
    uint32_t qseq = sg.seq;
    /* A Need-Fragmentation quoting a tuple with no connection reaches the
     * consumer (the firewall admitted it to the layer that can tell) and is
     * refused there: the cache does not move. */
    ipv4_get_stats(&is0); tcp_get_stats(&ts0);
    l4len = hst_mk_needfrag_tcp(l4, u_ip, w, 9999, 9100, qseq, 576);
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, icmp_needfrag_rcvd, is0.icmp_needfrag_rcvd));
    tcp_get_stats(&ts1);
    CHECK(ts1.pmtu_updates == ts0.pmtu_updates && is1.pmtu_updates == is0.pmtu_updates);
    /* The one quoting the live connection's in-flight segment is consumed,
     * and the segment is retransmitted inside the new path MTU. */
    hin_drain(u);
    tcp_get_stats(&ts0); ipv4_get_stats(&is0);
    l4len = hst_mk_needfrag_tcp(l4, u_ip, w, hport, 9100, qseq, 576);
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(tcp_get_stats, ts1, pmtu_updates, ts0.pmtu_updates));
    CHECK(FWT_RISES(ipv4_get_stats, is1, pmtu_updates, is0.pmtu_updates));
    CHECK(hin_recv(u, IPPROTO_TCP, 9100, &sg, 50) && sg.seq == qseq && sg.paylen <= 536);
    /* An ICMP error with no consumer is freed under the flag: nothing parses
     * it, nothing is answered. */
    ipv4_get_stats(&is0);
    l4len = hst_mk_unreach_tcp(l4, u_ip, w, hport, 9100, qseq);
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_ICMP, l4, l4len));
    CHECK(FWT_RISES(ipv4_get_stats, is1, icmp_quiet_dropped, is0.icmp_quiet_dropped));
    ksock_put(cn.s);
    hin_drain(u);

    /* (6) the DNS proxy end to end under the DROP: a guest's query, the
     * proxy's relay out the uplink from its *unconnected* socket, the
     * upstream's answer admitted by the host's flow, the guest's answer with
     * its own id. The socket is unconnected by design (it authenticates the
     * sender itself), so nothing but this state could admit the answer. */
    uint32_t g_ip = IPV4_ADDR(10, 88, 9, 1), guest = IPV4_ADDR(10, 88, 9, 15);
    struct tap *gt = tap_create("hstg", g_ip, htonl(0xffffff00u), gtmac);
    CHECK(gt != NULL);
    nettest_seed_arp(tap_netif(gt), guest, gmac);
    struct tapsvc *svc = tapsvc_start(gt);
    CHECK(svc != NULL);
    tapsvc_test_set_upstream(svc, w, 5300);
    uint8_t msg[64];
    uint32_t mlen = nettest_mk_dns(msg, 0x1357);
    l4len = nettest_mk_udp(l4, guest, g_ip, 4444, 53, msg, (uint16_t)mlen);
    uint32_t flen = nettest_wrap(frame, gtmac, gmac, guest, g_ip, 64, IPPROTO_UDP, l4, l4len);
    fw_get_stats(&fs0);
    CHECK(tap_inject(gt, frame, flen) == 0);
    CHECK(hin_recv(u, IPPROTO_UDP, 5300, &sg, 50) && sg.paylen >= 12 && sg.paylen <= sizeof(sg.pay));
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_new == fs0.hin_flow_new + 1);
    {
        /* The upstream's answer to the port the proxy sent from, built from
         * the relayed query as net-dns's responder builds it. */
        uint8_t ans[128];
        uint32_t alen = sg.paylen;
        memcpy(ans, sg.pay, alen);
        ans[2] = 0x81; ans[3] = 0x80;                /* QR + RD + RA */
        ans[6] = 0; ans[7] = 1;                      /* ancount 1 */
        uint32_t o = alen;
        ans[o++] = 0xc0; ans[o++] = 0x0c;            /* name pointer */
        ans[o++] = 0; ans[o++] = 1;                  /* type A */
        ans[o++] = 0; ans[o++] = 1;                  /* class IN */
        ans[o++] = 0; ans[o++] = 0; ans[o++] = 0; ans[o++] = 4;
        ans[o++] = 0; ans[o++] = 4;
        memcpy(ans + o, dns_answer_ip, 4); o += 4;
        fw_get_stats(&fs0);
        l4len = nettest_mk_udp(l4, w, u_ip, 5300, sg.sport, ans, (uint16_t)o);
        CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len));
        CHECK(FWT_RISES(fw_get_stats, fs1, hin_accept_established, fs0.hin_accept_established));
        struct mbuf *r = nettest_recv_ip(gt);
        CHECK(r != NULL);
        uint32_t rl = m_length(r);
        CHECK(rl <= sizeof(buf) && m_copydata(r, 0, rl, buf));
        m_freem(r);
        const struct ipv4_hdr *ri = (const struct ipv4_hdr *)(buf + ETH_HLEN);
        CHECK(ri->dst == guest && ri->proto == IPPROTO_UDP);
        const uint8_t *rudp = buf + ETH_HLEN + 20;
        CHECK((uint16_t)(rudp[2] << 8 | rudp[3]) == 4444);
        const uint8_t *dns = rudp + 8;
        CHECK((uint16_t)(dns[0] << 8 | dns[1]) == 0x1357);            /* the guest's own id */
        CHECK((dns[2] & 0x80) && memcmp(buf + rl - 4, dns_answer_ip, 4) == 0);
    }
    tapsvc_stop(svc);
    tap_destroy(gt);
    hin_drain(u);

    /* (7) a send on a live flow refreshes it rather than recording a second;
     * expiry closes the tuple, and a fresh send opens it again. */
    fw_get_stats(&fs0);
    CHECK(ksock_sendto(cs, pl, sizeof(pl), &peer) == (int64_t)sizeof(pl));
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_new == fs0.hin_flow_new && fs1.flows == fs0.flows);
    fw_age(clock_now_ns() + 31ull * 1000000000ull);
    udp_get_stats(&us0);
    l4len = nettest_mk_udp(l4, w, u_ip, 5300, 7100, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(cs, buf, sizeof(buf), 10) == -EAGAIN);
    CHECK(FWT_RISES(udp_get_stats, us1, quiet_dropped, us0.quiet_dropped));
    fw_get_stats(&fs0);
    CHECK(ksock_sendto(cs, pl, sizeof(pl), &peer) == (int64_t)sizeof(pl));
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_new == fs0.hin_flow_new + 1);
    l4len = nettest_mk_udp(l4, w, u_ip, 5300, 7100, pl, sizeof(pl));
    CHECK(hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv_sock(cs, buf, sizeof(buf), 50) == (int64_t)sizeof(pl));
    hin_drain(u);

    /* (8) neither a forwarded guest flow nor a loopback send is the host's:
     * a masqueraded guest-to-world flow leaves through output_on, not
     * ipv4_output, and loopback never reaches a chain. */
    struct file *fa = NULL, *fb = NULL;
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &fa) == 0 && fa != NULL);
    CHECK(vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR, 0, &fb) == 0 && fb != NULL);
    uint32_t ga = IPV4_ADDR(10, 0, 3, 15), gb = IPV4_ADDR(10, 0, 4, 15);
    static const uint8_t amac[6] = { 0x52, 0x54, 0x00, 0x1b, 0x00, 0x2a };
    static const uint8_t bmac[6] = { 0x52, 0x54, 0x00, 0x1b, 0x00, 0x2b };
    static const uint8_t tap0mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };
    { struct netif *n = netif_find("tap0"); CHECK(n != NULL); nettest_seed_arp(n, ga, amac); netif_put(n); }
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, w, 4500, 5300, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, w, IPPROTO_UDP, l4, l4len));
    CHECK(hin_recv(u, IPPROTO_UDP, 5300, &sg, 50) && sg.src == u_ip && sg.sport != 4500);   /* masqueraded */
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_new == fs0.hin_flow_new);                  /* not the host's flow */
    fw_get_stats(&fs0);
    {
        struct socket *lo = NULL;
        CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &lo) == 0);
        struct netaddr to = v4addr(INADDR_LOOPBACK_N, 7100);
        CHECK(ksock_sendto(lo, pl, sizeof(pl), &to) == (int64_t)sizeof(pl));
        CHECK(hin_recv_sock(cs, buf, sizeof(buf), 50) == (int64_t)sizeof(pl));
        ksock_put(lo);
    }
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_new == fs0.hin_flow_new);                  /* loopback records nothing */

    /* (9) the host's share is its own: 64 flows record, the 65th does not --
     * its datagram still leaves, only its reply then takes the rules -- and
     * the guests' pool is untouched. */
    fw_flush();
    CHECK(fw_rule_add(FW_HOST_GUEST_IP, 0, &r_world) == 0);   /* fw_flush cleared it */
    fw_get_stats(&fs0);
    for (unsigned i = 0; i < FW_FLOW_QUOTA_HOST; i++) {
        struct netaddr to = v4addr(w, (uint16_t)(6000 + i));
        CHECK(ksock_sendto(cs, pl, sizeof(pl), &to) == (int64_t)sizeof(pl));
    }
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_new == fs0.hin_flow_new + FW_FLOW_QUOTA_HOST && fs1.flows == FW_FLOW_QUOTA_HOST);
    hin_drain(u);
    fw_get_stats(&fs0);
    {
        struct netaddr to = v4addr(w, (uint16_t)(6000 + FW_FLOW_QUOTA_HOST));
        CHECK(ksock_sendto(cs, pl, sizeof(pl), &to) == (int64_t)sizeof(pl));
    }
    fw_get_stats(&fs1);
    CHECK(fs1.hin_flow_drop_full == fs0.hin_flow_drop_full + 1 && fs1.hin_flow_new == fs0.hin_flow_new);
    CHECK(hin_recv(u, IPPROTO_UDP, (uint16_t)(6000 + FW_FLOW_QUOTA_HOST), &sg, 50));   /* still sent */
    /* A guest-to-guest flow still records with the host's share full. */
    { struct netif *n = netif_find("tap1"); CHECK(n != NULL); nettest_seed_arp(n, gb, bmac); netif_put(n); }
    struct fw_rule a_to_b = { .direction = FW_DIR_TO_GUEST, .proto = IPPROTO_UDP, .dst_prefix = 32,
                              .verdict = FW_ACCEPT, .dst_ip = gb, .dst_port = 7300 };
    CHECK(fw_rule_add(ga, 0, &a_to_b) == 0);
    fw_get_stats(&fs0);
    l4len = nettest_mk_udp(l4, ga, gb, 4600, 7300, pl, sizeof(pl));
    CHECK(fwt_send(fa, tap0mac, amac, ga, gb, IPPROTO_UDP, l4, l4len));
    CHECK(FWT_RISES(fw_get_stats, fs1, flow_new, fs0.flow_new));
    CHECK(fs1.hin_flow_drop_full == fs0.hin_flow_drop_full);

    /* (10) state beats the hardened *default* too, not only a rule. A clean
     * share first -- step (9) deliberately spent the host's, and a flow that
     * is never recorded is exactly what the default then drops. The verdicts
     * are taken and the default restored before anything is asserted, so no
     * failure here can leave the machine hardened. */
    fw_flush();
    while (hin_recv_sock(cs, buf, sizeof(buf), 1) > 0)
        ;                                      /* nothing of an earlier step's left queued */
    CHECK(fw_policy_set(FW_HOST_GUEST_IP, FW_DIR_FROM_UPLINK, FW_DROP) == 0);
    {
        struct netaddr p2 = v4addr(w, 5500);
        bool sent = ksock_sendto(cs, pl, sizeof(pl), &p2) == (int64_t)sizeof(pl);
        l4len = nettest_mk_udp(l4, w, u_ip, 5500, 7100, pl, sizeof(pl));
        bool injected = hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len);
        int64_t got = hin_recv_sock(cs, buf, sizeof(buf), 50);
        l4len = nettest_mk_udp(l4, w, u_ip, 5501, 7100, pl, sizeof(pl));
        bool unsolicited = hin_send(u, umac, wmac, w, u_ip, IPPROTO_UDP, l4, l4len);
        int64_t none = hin_recv_sock(cs, buf, sizeof(buf), 10);
        int restored = fw_policy_set(FW_HOST_GUEST_IP, FW_DIR_FROM_UPLINK, FW_ACCEPT);
        CHECK(restored == 0);
        CHECK(sent && injected && got == (int64_t)sizeof(pl));
        CHECK(unsolicited && none == -EAGAIN);
    }

    file_put(fa);
    file_put(fb);
    icmp_set_echo_reply_hook(NULL);
    ksock_put(cs); ksock_put(cs2); ksock_put(cs3);
    hin_drain(u);
    tap_destroy(u);
    fw_flush();
    nat_flush();
    kinfo("selftest: net-hoststate: under a hardened host the replies to its own unconnected UDP sends and echo "
          "requests were admitted by state on exactly one tuple (another port, peer or local port was not), the "
          "DNS proxy relayed and answered end to end through its unconnected socket, a Need-Fragmentation was "
          "consumed only where TCP confirmed the quoted segment and the segment came back inside the new MTU, an "
          "echo request drew nothing and spent no budget, a send refreshed rather than re-recorded, expiry closed "
          "the tuple, and the host's share held while the guests' pool stayed its own");
    return true;
}
