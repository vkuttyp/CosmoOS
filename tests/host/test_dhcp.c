/*
 * test_dhcp.c - The kernel's DHCP client (kernel-services/network/dhcp.c) on
 * the host: roadmap 2.0 SA6. A digest over seeded histories, recorded from
 * the C, of a host with four autoconfigured interfaces, each on its own
 * link with two DHCP servers, a router and a server beyond it:
 *
 *   - the servers' replies to what each client last sent: offers, ACKs and
 *     NAKs, mostly as a server would answer and otherwise not -- the wrong
 *     xid, chaddr, op, cookie or server identifier, no server identifier, a
 *     message type a client never takes, a yiaddr of 0 or the broadcast,
 *     sent to the broadcast, the offered address, the address held or
 *     another; leases of a second to infinite with T1 and T2 ordered,
 *     unordered or absent, masks, routers, DNS servers (none to too many,
 *     a length that is not a multiple of four), host names a server should
 *     not be trusted with; UDP checksums absent or wrong, ports wrong, IP
 *     options, messages too short and too long, options running past the
 *     end;
 *   - datagrams handed to dhcp_client_input directly that ipv4_input would
 *     not pass whole (too short for the ports or the destination);
 *   - ARP for a renewal's next hop answered or not; transmit rings full;
 *   - time passing and the clients' timers firing (retransmission and its
 *     backoff, the give-up in REQUESTING, T1, T2, the lease's end), kicks,
 *     clients stopped and started again, a fifth refused, the boot wait
 *     (its sleep moves the clock), the lease list at every bound, the
 *     statistics.
 *
 * The rest of the stack (Ethernet, ARP, IPv4, UDP, mbufs, the checksum)
 * runs as the kernel's, over tests/fuzz/shim_net.c: timers, work items, the
 * clock and the random source are the shim's, seeded. Every frame the host
 * transmits, every line the client logs (harness_klog_hook), every return,
 * lease list and the interfaces' addresses are folded into the digest;
 * every transmitted frame has correct checksums, and every DHCP message is
 * a well-formed BOOTREQUEST from the interface's MAC; an interface holds an
 * address exactly while its client holds a lease, and then the lease's;
 * after each history nothing is allocated beyond the baseline, no mbuf is
 * alive and no timer is pending but ARP's age timer.
 *
 * Not reached: a slot stopped and reused between client_of and the lock (a
 * second CPU), m_getcl and kmalloc failing, and CONFIG_SELFTEST's
 * dhcp_test_fire and dhcp_test_state (the kernel's self-tests drive them).
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>
#include <kernel/net/dhcp.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/netif.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "harness.h"
#include "netpkt.h"
#include "shim_net.h"

/* The digest of the C (dhcp.c, 2026-11-03), which the CeeSafe port gives. */
#define DHCP_DIGEST 0x10330ab4022bbc46ull
#define HISTORIES 4u
#define OPS 2500u
#define SEED 0xD4C9C11E2026B00Dull
#define NIF 4u

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void mix32(uint32_t v) { mix(&v, sizeof(v)); }
static void mix64(uint64_t v) { mix(&v, sizeof(v)); }

static bool g_benching;

static void note(uint32_t which, uint64_t value)
{
    if (g_benching)
        return;
    mix32(0x6400u + which);
    mix64(value);
}

/* shim_net.c's assertions land in the harness. */
void fuzz_fail(const char *file, int line, const char *expr) { harness_fail(file, line, expr); }

/* dhcp_boot_wait's sleep: the clock moves, and what falls due runs. */
void thread_sleep_ns(uint64_t ns) { fz_clock_advance(ns); }

static void on_klog(int level, const char *line)
{
    note(3, (uint64_t)level);
    if (!g_benching)
        mix(line, strlen(line));
}

/* ---- the links ---- */

static struct fz_netif g_f[NIF];
static struct netif g_extra;   /* a fifth interface, for the full table */
static bool g_running[NIF];
static unsigned g_bad_frames;

static uint32_t net_addr(unsigned i, unsigned host) { return IPV4_ADDR(10, 80 + i, 0, host); }
static uint32_t server_a(unsigned i) { return net_addr(i, 1); }
static uint32_t server_b(unsigned i) { return net_addr(i, 2); }
static uint32_t router(unsigned i) { return net_addr(i, 254); }
static uint32_t far_server(unsigned i) { return IPV4_ADDR(10, 99, i, 1); }

static void mac_of(uint32_t ip, uint8_t mac[6])
{
    mac[0] = 0x02;
    mac[1] = 0x5e;
    memcpy(mac + 2, &ip, 4);
}

static void host_mac(unsigned i, uint8_t mac[6])
{
    static const uint8_t base[6] = { 0x02, 0xdc, 0x00, 0x00, 0x00, 0x00 };
    memcpy(mac, base, 6);
    mac[5] = (uint8_t)(0x10 + i);
}

/* What each client last sent, as its server reads it. */
struct seen {
    bool any;
    uint8_t type;
    uint8_t xid[4];
    uint32_t ciaddr, reqip, server_opt, dst;
};
static struct seen g_seen[NIF];
static uint32_t g_arp_want[NIF];   /* the next hop an ARP request asked for, or 0 */

static unsigned index_of(const struct fz_netif *f) { return (unsigned)(f - g_f); }

static void on_frame(struct fz_netif *f, const uint8_t *frame, uint32_t len)
{
    unsigned i = index_of(f);
    if (!g_benching && np_check_checksums(frame, len) != 0)
        g_bad_frames++;
    note(1, ((uint64_t)i << 32) | len);
    if (!g_benching)
        mix(frame, len);
    if (len < NP_ETH + 28)
        return;
    uint16_t type = np_get16(frame + 12);
    if (type == ETH_P_ARP) {
        const uint8_t *a = frame + NP_ETH;
        if (np_get16(a + 6) == 1)
            memcpy(&g_arp_want[i], a + 24, 4);
        return;
    }
    if (type != ETH_P_IP || len < NP_ETH + NP_IPV4 + NP_UDP)
        return;
    const uint8_t *ip = frame + NP_ETH;
    const uint8_t *uh = ip + NP_IPV4;
    if (ip[9] != IPPROTO_UDP || np_get16(uh) != DHCP_CPORT || np_get16(uh + 2) != DHCP_SPORT)
        return;
    const uint8_t *dh = uh + NP_UDP;
    uint32_t dhlen = np_get16(uh + 4) - NP_UDP;
    uint8_t mac[6];
    host_mac(i, mac);
    uint8_t l = 0;
    const uint8_t *t = dhlen >= DHCP_FIXED ? dhcp_opt(dh + DHCP_FIXED, dhlen - DHCP_FIXED, OPT_MSGTYPE, &l) : NULL;
    if (ip[0] != 0x45 || dhlen < 300 || NP_ETH + NP_IPV4 + NP_UDP + dhlen > len || dh[0] != BOOTP_REQUEST ||
        memcmp(dh + BOOTP_OFF_CHADDR, mac, 6) != 0 || np_get32(dh + BOOTP_LEN) != DHCP_MAGIC || t == NULL || l != 1 ||
        (*t != DHCP_DISCOVER && *t != DHCP_REQUEST)) {
        g_bad_frames++;
        return;
    }
    struct seen *s = &g_seen[i];
    s->any = true;
    s->type = *t;
    memcpy(s->xid, dh + BOOTP_OFF_XID, 4);
    memcpy(&s->ciaddr, dh + BOOTP_OFF_CIADDR, 4);
    memcpy(&s->dst, ip + 16, 4);
    s->reqip = s->server_opt = 0;
    const uint8_t *v = dhcp_opt(dh + DHCP_FIXED, dhlen - DHCP_FIXED, OPT_REQIP, &l);
    if (v != NULL && l == 4)
        memcpy(&s->reqip, v, 4);
    v = dhcp_opt(dh + DHCP_FIXED, dhlen - DHCP_FIXED, OPT_SERVERID, &l);
    if (v != NULL && l == 4)
        memcpy(&s->server_opt, v, 4);
}

/* An interface holds an address exactly while its client holds a lease,
 * and then the lease's; the lease's times are ordered. */
static void check_leases(void)
{
    struct dhcp_lease_info l[DHCP_MAX_CLIENTS];
    unsigned n = dhcp_lease_list(l, DHCP_MAX_CLIENTS);
    unsigned running = 0;
    for (unsigned i = 0; i < NIF; i++)
        running += g_running[i];
    EXPECT(n == running);
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (unsigned)(l[k].ifname[2] - '0');
        EXPECT(i < NIF && g_running[i]);
        if (i >= NIF)
            continue;
        const struct netif *nif = &g_f[i].nif;
        bool held = l[k].state == DHCP_S_BOUND || l[k].state == DHCP_S_RENEWING || l[k].state == DHCP_S_REBINDING;
        if (held)
            EXPECT(nif->ip4.addr == l[k].addr && l[k].addr != 0 && nif->ip4.mask == l[k].mask &&
                   nif->ip4.gateway == l[k].router && l[k].ndns <= DHCP_MAX_DNS && l[k].t1_s <= l[k].t2_s &&
                   l[k].t2_s <= l[k].lease_s);
        else
            EXPECT(nif->ip4.addr == 0 && l[k].addr == 0);
    }
}

static void note_addresses(void)
{
    for (unsigned i = 0; i < NIF; i++) {
        const struct netif *nif = &g_f[i].nif;
        note(4, ((uint64_t)nif->ip4.addr << 32) | nif->ip4.mask);
        note(5, nif->ip4.gateway);
    }
}

/* ---- the servers ---- */

static const uint32_t k_leases[] = { 1, 2, 3, 8, 30, 120, 600, 3600, 86400, 0xffffffffu };

static const char *const k_names[] = {
    "cosmo-guest", "a", "bad name", "dot.ted-1",
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",    /* 63 */
    "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",   /* 64 */
    "", "\001ctl", "under_score", "-dash",
};

static uint8_t *opt4(uint8_t *o, uint8_t code, uint32_t net_order)
{
    *o++ = code;
    *o++ = 4;
    memcpy(o, &net_order, 4);
    return o + 4;
}

static uint8_t *opt32(uint8_t *o, uint8_t code, uint32_t host_order)
{
    *o++ = code;
    *o++ = 4;
    np_put32(o, host_order);
    return o + 4;
}

/* An IPv4/UDP frame, its IP header `ihl` bytes (20, or 24 with a NOP option). */
static size_t udp_frame(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], uint32_t src, uint32_t dst,
                        uint16_t sport, uint16_t dport, const uint8_t *payload, size_t n, unsigned ihl)
{
    size_t o = np_eth(f, dmac, smac, ETH_P_IP);
    uint8_t *ip = f + o;
    memset(ip, 0, ihl);
    ip[0] = (uint8_t)(0x40 | (ihl / 4));
    np_put16(ip + 2, (uint16_t)(ihl + NP_UDP + n));
    np_put16(ip + 4, 0x2222);
    ip[8] = 64;
    ip[9] = IPPROTO_UDP;
    memcpy(ip + 12, &src, 4);
    memcpy(ip + 16, &dst, 4);
    if (ihl > NP_IPV4)
        memset(ip + NP_IPV4, 1, ihl - NP_IPV4);
    uint16_t c = in_cksum(ip, ihl);
    memcpy(ip + 10, &c, 2);
    size_t ul = np_udp(ip + ihl, sport, dport, payload, n);
    np_l4_cksum4(ip + ihl, (uint32_t)ul, src, dst, IPPROTO_UDP, 6);
    return o + ihl + ul;
}

/* A server's reply to client `i`: as a server would answer what it last
 * sent, mostly, and otherwise not. */
static void op_reply(void)
{
    unsigned i = pick(NIF);
    const struct seen *s = &g_seen[i];
    uint8_t mac[6];
    host_mac(i, mac);
    bool proper = pick(8) != 0;

    uint8_t mt;
    if (proper && s->any)
        mt = s->type == DHCP_DISCOVER ? DHCP_OFFER : pick(10) == 0 ? DHCP_NAK : DHCP_ACK;
    else {
        static const uint8_t types[] = { DHCP_OFFER, DHCP_ACK, DHCP_NAK, DHCP_DECLINE, DHCP_RELEASE, 0, 9, DHCP_ACK };
        mt = types[pick(sizeof(types))];
    }
    /* The server: the one the client named, or its lease's, or another. */
    uint32_t server;
    unsigned sk = pick(20);
    if (s->server_opt != 0 && sk < 14)
        server = s->server_opt;
    else if (s->ciaddr != 0 && s->dst != INADDR_BROADCAST_N && sk < 14)
        server = s->dst;   /* a renewal, unicast to the server that granted the lease */
    else if (sk < 15)
        server = server_a(i);
    else if (sk < 17)
        server = server_b(i);
    else if (sk < 18)
        server = far_server(i);
    else
        server = 0;   /* sk 19: no server identifier at all; 18: a zero one */
    uint32_t src_ip = server != 0 ? server : server_a(i);
    uint32_t yi;
    if (s->reqip != 0 && pick(6) != 0)
        yi = s->reqip;
    else if (s->ciaddr != 0 && pick(6) != 0)
        yi = s->ciaddr;
    else
        yi = net_addr(i, 10 + pick(5));
    unsigned yk = pick(32);
    if (yk == 0)
        yi = 0;
    else if (yk == 1)
        yi = INADDR_BROADCAST_N;

    uint8_t dh[1800];
    memset(dh, 0, sizeof(dh));
    dh[BOOTP_OFF_OP] = pick(40) == 0 ? BOOTP_REQUEST : BOOTP_REPLY;
    dh[BOOTP_OFF_HTYPE] = DHCP_HTYPE_ETHER;
    dh[BOOTP_OFF_HLEN] = 6;
    memcpy(dh + BOOTP_OFF_XID, s->xid, 4);
    if (!proper && pick(3) == 0)
        dh[BOOTP_OFF_XID + pick(4)] ^= (uint8_t)(1 + pick(255));
    memcpy(dh + BOOTP_OFF_YIADDR, &yi, 4);
    memcpy(dh + BOOTP_OFF_CHADDR, mac, 6);
    if (!proper && pick(4) == 0)
        dh[BOOTP_OFF_CHADDR + pick(6)] ^= 0x40;
    np_put32(dh + BOOTP_LEN, pick(40) == 0 ? 0x63825364u : DHCP_MAGIC);
    uint8_t *o = dh + DHCP_FIXED;
    if (pick(4) == 0)
        *o++ = OPT_PAD;
    if (mt != 0) {
        *o++ = OPT_MSGTYPE;
        *o++ = 1;
        *o++ = mt;
    }
    if (sk != 19)
        o = opt4(o, OPT_SERVERID, server);
    if (mt != DHCP_NAK || pick(4) == 0) {
        if (pick(16) != 0)
            o = opt32(o, OPT_LEASE, k_leases[pick(sizeof(k_leases) / sizeof(k_leases[0]))]);
        unsigned mk = pick(8);
        if (mk < 6)
            o = opt32(o, OPT_SUBNET, 0xffffff00u);
        else if (mk == 6)
            o = opt32(o, OPT_SUBNET, 0xffff0000u);
        unsigned rk = pick(8);
        if (rk < 5)
            o = opt4(o, OPT_ROUTER, router(i));
        else if (rk == 5) {
            *o++ = OPT_ROUTER;
            *o++ = 8;
            uint32_t r0 = net_addr(i, 253), r1 = router(i);
            memcpy(o, &r0, 4);
            memcpy(o + 4, &r1, 4);
            o += 8;
        }
        unsigned nd = pick(7);
        if (nd > 0) {
            unsigned dl = nd == 6 ? 6 : 4 * (nd - 1);   /* 0..16 bytes, or 6: not a multiple of four */
            *o++ = OPT_DNS;
            *o++ = (uint8_t)dl;
            for (unsigned k = 0; k < dl; k++)
                o[k] = (uint8_t)(k < 4 ? (k == 0 ? 10 : k == 1 ? 80 + i : k == 2 ? 0 : 53) : 8 + k);
            o += dl;
        }
        if (pick(2) == 0) {
            const char *h = k_names[pick(sizeof(k_names) / sizeof(k_names[0]))];
            size_t hl = strlen(h);
            *o++ = OPT_HOSTNAME;
            *o++ = (uint8_t)hl;
            memcpy(o, h, hl);
            o += hl;
        }
        unsigned tk = pick(6);
        if (tk == 1 || tk == 2 || tk == 3) {
            uint32_t t1 = tk == 1 ? 1 : tk == 2 ? 50 : 4000, t2 = tk == 1 ? 2 : tk == 2 ? 40 : 7000;
            o = opt32(o, OPT_T1, t1);
            o = opt32(o, OPT_T2, t2);
        } else if (tk == 4)
            o = opt32(o, OPT_T1, 1);
    }
    if (pick(48) == 0) {   /* an option running past the end */
        *o++ = OPT_DNS;
        *o++ = 200;
    } else
        *o++ = OPT_END;
    size_t dhlen = (size_t)(o - dh);
    unsigned lk = pick(64);
    if (lk == 0)
        dhlen = pick(DHCP_FIXED);           /* too short for the fixed part */
    else if (lk == 1)
        dhlen = 1600;                       /* past the client's 1600 bytes */
    else if (lk < 8 && dhlen < 300)
        dhlen = 300;

    /* The destination: the broadcast, mostly, or the unicast a renewal's answer takes. */
    uint32_t dst;
    unsigned dk = pick(16);
    if (dk < 9)
        dst = s->ciaddr != 0 && s->dst != INADDR_BROADCAST_N ? s->ciaddr : INADDR_BROADCAST_N;
    else if (dk < 12)
        dst = INADDR_BROADCAST_N;
    else if (dk < 13)
        dst = yi;
    else if (dk < 14)
        dst = g_f[i].nif.ip4.addr != 0 ? g_f[i].nif.ip4.addr : INADDR_BROADCAST_N;
    else if (dk < 15)
        dst = net_addr(i, 77);
    else
        dst = net_addr(i, 255);
    uint16_t sport = DHCP_SPORT, dport = DHCP_CPORT;
    unsigned pk = pick(40);
    if (pk == 0)
        dport = 69;
    else if (pk == 1)
        sport = 66;
    uint8_t smac[6];
    mac_of(src_ip, smac);
    static uint8_t frame[2048];
    size_t n = udp_frame(frame, pick(3) == 0 ? mac : eth_broadcast, smac, src_ip, dst, sport, dport, dh, dhlen,
                         pick(24) == 0 ? 24 : 20);
    uint8_t *uh = frame + n - dhlen - NP_UDP;
    unsigned ck = pick(24);
    if (ck == 0)
        uh[6] = uh[7] = 0;   /* no checksum: taken as it is */
    else if (ck == 1)
        uh[7] ^= 0x5a;
    note(6, ((uint64_t)i << 40) | ((uint64_t)mt << 32) | dhlen);
    fz_deliver(&g_f[i], frame, (uint32_t)n);
}

/* Datagrams ipv4_input would never pass whole, to dhcp_client_input itself. */
static void op_direct(void)
{
    unsigned i = pick(NIF);
    struct netif *nif = pick(8) == 0 ? &g_extra : &g_f[i].nif;
    uint8_t b[64];
    memset(b, 0, sizeof(b));
    unsigned k = pick(4), ihl = 0;
    size_t len;
    if (k == 0) {
        len = 3;   /* too short for the ports */
        ihl = 0;
    } else if (k == 1) {
        np_put16(b, DHCP_SPORT);
        np_put16(b + 2, DHCP_CPORT);
        len = 12;   /* the ports, and too short for the destination */
    } else if (k == 2) {
        np_put16(b, DHCP_SPORT);
        np_put16(b + 2, 70);
        len = 32;
    } else {
        /* A header, ports, and a destination of this host -- and nothing more. */
        b[0] = 0x45;
        uint32_t d = INADDR_BROADCAST_N;
        memcpy(b + 16, &d, 4);
        np_put16(b + 20, DHCP_SPORT);
        np_put16(b + 22, DHCP_CPORT);
        ihl = 20;
        len = 20 + 8 + pick(20);
    }
    struct mbuf *m = m_getcl();
    EXPECT(m != NULL);
    if (m == NULL)
        return;
    EXPECT(m_append(m, b, (uint32_t)len) == 0);
    bool claimed = dhcp_client_input(nif, m, ihl);
    note(7, ((uint64_t)k << 8) | claimed);
    if (!claimed)
        m_freem(m);
}

static void op_arp(void)
{
    unsigned i = pick(NIF);
    if (g_arp_want[i] == 0 || pick(4) == 0)
        return;
    uint8_t mac[6], peer[6], f[64];
    host_mac(i, mac);
    mac_of(g_arp_want[i], peer);
    size_t n = np_frame_arp(f, mac, peer, 2, peer, g_arp_want[i], mac, g_f[i].nif.ip4.addr);
    note(8, ((uint64_t)i << 32) | g_arp_want[i]);
    g_arp_want[i] = 0;
    fz_deliver(&g_f[i], f, (uint32_t)n);
}

static void op_time(void)
{
    static const uint64_t limits[] = { 1000000ull, 500000000ull, 4000000000ull, 61000000000ull, 700000000000ull,
                                       90000000000000ull };
    unsigned k = pick(8);
    if (k < 6) {
        unsigned fired = fz_fire_until(limits[k], 1 + pick(6));
        note(9, fired);
    } else {
        fz_clock_advance(1000000ull * (1 + pick(3000)));
    }
    note(10, fz_now());
}

/* Long waits, in steps: what falls due in a step runs at its end, late (ARP's
 * one-second age timer would take every fire of fz_fire_until's). */
static void op_wait(void)
{
    static const uint64_t steps[] = { 1000000000ull, 5000000000ull, 10000000000ull, 60000000000ull, 600000000000ull, 3600000000000ull,
                                      20000000000000ull };
    uint64_t step = steps[pick(sizeof(steps) / sizeof(steps[0]))];
    unsigned n = 1 + pick(8);
    for (unsigned k = 0; k < n; k++)
        fz_clock_advance(step);
    note(19, fz_now());
}

static void op_lifecycle(void)
{
    unsigned i = pick(NIF);
    unsigned k = pick(8);
    if (k == 1 && g_running[i])
        dhcp_client_kick(&g_f[i].nif);   /* work queued as the client stops: it runs, and finds it stopping */
    if (k == 0) {
        unsigned all = 0;
        for (unsigned j = 0; j < NIF; j++)
            all += g_running[j];
        if (all == NIF) {
            int rc = dhcp_client_start(&g_extra);
            EXPECT(rc == -ENOSPC);
            note(11, (uint64_t)(int64_t)rc);
        } else {
            dhcp_client_stop(&g_extra);   /* no client: nothing */
            note(11, 1);
        }
        return;
    }
    if (g_running[i]) {
        dhcp_client_stop(&g_f[i].nif);
        g_running[i] = false;
        memset(&g_seen[i], 0, sizeof(g_seen[i]));
        note(12, i);
        /* What a stopped client leaves: its address, if it held a lease (netif_unregister follows in the kernel). */
        g_f[i].nif.ip4.addr = g_f[i].nif.ip4.mask = g_f[i].nif.ip4.gateway = 0;
        arp_flush(&g_f[i].nif);
    } else {
        int rc = dhcp_client_start(&g_f[i].nif);
        EXPECT(rc == 0);
        g_running[i] = rc == 0;
        note(13, ((uint64_t)i << 32) | (uint32_t)rc);
    }
}

static void op_kick(void)
{
    unsigned i = pick(NIF + 1);
    dhcp_client_kick(i == NIF ? &g_extra : &g_f[i].nif);
    note(14, i);
}

static void op_ring(void)
{
    unsigned i = pick(NIF);
    g_f[i].ring_full = !g_f[i].ring_full && pick(3) == 0;
    note(15, ((uint64_t)i << 1) | g_f[i].ring_full);
}

static void op_list(void)
{
    struct dhcp_lease_info l[DHCP_MAX_CLIENTS + 2];
    unsigned max = pick(DHCP_MAX_CLIENTS + 3);
    unsigned n = dhcp_lease_list(l, max);
    EXPECT(n <= max);
    note(16, ((uint64_t)max << 32) | n);
    for (unsigned k = 0; k < n; k++) {
        const struct dhcp_lease_info *o = &l[k];
        mix(o->ifname, sizeof(o->ifname));
        mix32(((uint32_t)o->state << 8) | o->ndns);
        mix32(o->addr);
        mix32(o->mask);
        mix32(o->router);
        mix32(o->server);
        for (unsigned d = 0; d < DHCP_MAX_DNS; d++)
            mix32(o->dns[d]);
        mix32(o->lease_s);
        mix32(o->t1_s);
        mix32(o->t2_s);
        mix32(o->remaining_ms);
        mix32(o->t1_ms);
        mix32(o->t2_ms);
        mix(o->hostname, sizeof(o->hostname));
    }
}

static void note_stats(void)
{
    struct dhcp_stats st;
    dhcp_get_stats(&st);
    note(17, st.discovers);
    note(17, st.requests);
    note(17, st.offers);
    note(17, st.acks);
    note(17, st.naks);
    note(17, st.renews);
    note(17, st.rebinds);
    note(17, st.expiries);
    note(17, st.ignored);
}

static void op_boot_wait(void)
{
    dhcp_boot_wait();
    note(18, fz_now());
}

/* A server's reply of type `mt` to what client `i` last sent: from server A, the address .10, a lease of
 * `lease` seconds, broadcast. */
static size_t reply_to(unsigned i, uint8_t *f, uint8_t mt, uint32_t lease)
{
    const struct seen *s = &g_seen[i];
    uint8_t mac[6], smac[6], dh[300];
    host_mac(i, mac);
    mac_of(server_a(i), smac);
    memset(dh, 0, sizeof(dh));
    dh[0] = BOOTP_REPLY;
    dh[1] = DHCP_HTYPE_ETHER;
    dh[2] = 6;
    memcpy(dh + BOOTP_OFF_XID, s->xid, 4);
    uint32_t yi = net_addr(i, 10);
    memcpy(dh + BOOTP_OFF_YIADDR, &yi, 4);
    memcpy(dh + BOOTP_OFF_CHADDR, mac, 6);
    np_put32(dh + BOOTP_LEN, DHCP_MAGIC);
    uint8_t *o = dh + DHCP_FIXED;
    *o++ = OPT_MSGTYPE;
    *o++ = 1;
    *o++ = mt;
    o = opt4(o, OPT_SERVERID, server_a(i));
    if (mt != DHCP_NAK) {
        o = opt32(o, OPT_LEASE, lease);
        o = opt32(o, OPT_SUBNET, 0xffffff00u);
        o = opt4(o, OPT_ROUTER, router(i));
        o = opt4(o, OPT_DNS, server_a(i));
    }
    *o++ = OPT_END;
    return udp_frame(f, mac, smac, server_a(i), INADDR_BROADCAST_N, DHCP_SPORT, DHCP_CPORT, dh, 300, 20);
}

/* Each history opens with a renewal that reaches its server: client 0 bound for an hour, T1, the ARP for the
 * server answered, the REQUEST unicast once ARP has it, and its retransmission to the cached address. */
static void scripted(void)
{
    if (!g_running[0])
        return;
    static uint8_t f[2048];
    fz_deliver(&g_f[0], f, (uint32_t)reply_to(0, f, DHCP_OFFER, 3600));
    fz_deliver(&g_f[0], f, (uint32_t)reply_to(0, f, DHCP_ACK, 3600));
    EXPECT(g_f[0].nif.ip4.addr == net_addr(0, 10));
    for (unsigned k = 0; k < 5; k++) {
        fz_clock_advance(600000000000ull);   /* T1 at 1800 s; the retransmission 675 s later */
        if (g_arp_want[0] != 0) {
            uint8_t mac[6], peer[6], a[64];
            host_mac(0, mac);
            mac_of(g_arp_want[0], peer);
            size_t n = np_frame_arp(a, mac, peer, 2, peer, g_arp_want[0], mac, g_f[0].nif.ip4.addr);
            g_arp_want[0] = 0;
            fz_deliver(&g_f[0], a, (uint32_t)n);
        }
    }
    note(20, g_seen[0].dst);
}

/* ---- the histories ---- */

static void setup(unsigned h)
{
    fz_random_seed(SEED ^ h);
    for (unsigned i = 0; i < NIF; i++) {
        char name[8];
        snprintf(name, sizeof(name), "dh%u", i);
        uint8_t mac[6];
        host_mac(i, mac);
        fz_netif_register(&g_f[i], name, mac, 0, 0, 0, NETIF_NODEFAULT | NETIF_AUTOCONF);
        g_f[i].on_frame = on_frame;
        memset(&g_seen[i], 0, sizeof(g_seen[i]));
        g_arp_want[i] = 0;
    }
    for (unsigned i = 0; i < NIF; i++) {
        g_running[i] = h % 2 == 0 || i % 2 == 0;
        if (g_running[i])
            EXPECT(dhcp_client_start(&g_f[i].nif) == 0);
    }
    fz_run_work();   /* the first DISCOVERs */
    scripted();
}

static void teardown(size_t baseline)
{
    check_leases();
    for (unsigned i = 0; i < NIF; i++) {
        if (g_running[i])
            dhcp_client_stop(&g_f[i].nif);
        g_running[i] = false;
        g_f[i].ring_full = false;
    }
    struct dhcp_lease_info gone[DHCP_MAX_CLIENTS];
    EXPECT(dhcp_lease_list(gone, DHCP_MAX_CLIENTS) == 0);
    fz_netif_unregister_all();
    fz_run_work();
    EXPECT(harness_locks_held() == 0);
    EXPECT(fz_timers_pending() <= 1);   /* ARP's age timer, which runs for good */
    if (fz_timers_pending() > 1)
        fz_dump_timers();
    struct mbuf_stats ms;
    mbuf_get_stats(&ms);
    EXPECT(ms.mbufs_alive == 0 && ms.clusters_alive == 0);
    EXPECT(fz_allocs_live() == baseline);
    if (fz_allocs_live() != baseline)
        fz_dump_live();
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    fz_net_init();
    memset(&g_extra, 0, sizeof(g_extra));
    strcpy(g_extra.name, "dh9");
    g_extra.mtu = 1500;
    static const struct kobject_type extra_type = { .name = "extra" };
    kobject_init(&g_extra.obj, &extra_type);
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the lines are in the digest; the warnings are expected */
    size_t baseline = fz_allocs_live();
    dhcp_client_kick(&g_extra);   /* before any client: no table to look in */
    struct dhcp_lease_info none[1];
    EXPECT(dhcp_lease_list(none, 1) == 0);
    for (unsigned h = 0; h < HISTORIES; h++) {
        setup(h);
        for (unsigned r = 0; r < OPS; r++) {
            unsigned k = pick(32);
            if (k < 12)
                op_reply();
            else if (k < 13)
                op_direct();
            else if (k < 16)
                op_arp();
            else if (k < 21)
                op_time();
            else if (k < 23)
                op_wait();
            else if (k < 24)
                op_lifecycle();
            else if (k < 25)
                op_kick();
            else if (k < 26)
                op_ring();
            else if (k < 29)
                op_list();
            else if (k < 30)
                note_stats();
            else if (k < 31 && r % 4 == 0)
                op_boot_wait();
            else
                op_time();
            fz_run_work();
            check_leases();
            note_addresses();
            EXPECT(harness_locks_held() == 0);
        }
        note_stats();
        teardown(baseline);
        note(60, h);
    }
    harness_klog_hook = NULL;
    EXPECT(g_bad_frames == 0);
    printf("  dhcp digest 0x%016llx over %u histories of %u operations\n", (unsigned long long)g_digest, HISTORIES, OPS);
    EXPECT(g_digest == DHCP_DIGEST);
}

/* DHCP_BENCH=1: the median of nine runs of 20,000 exchanges on one interface -- a NAK (the client starts over and
 * sends a DISCOVER), the OFFER, the client's REQUEST, the ACK that binds it -- through the whole host stack. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void dhcp_bench(void)
{
    g_rng = SEED;
    fz_net_init();
    size_t baseline = fz_allocs_live();
    setup(1);   /* dh0 and dh2 running */
    dhcp_client_stop(&g_f[2].nif);
    g_running[2] = false;
    g_benching = true;
    static uint8_t f[2048];
    fz_deliver(&g_f[0], f, (uint32_t)reply_to(0, f, DHCP_OFFER, 3600));
    fz_deliver(&g_f[0], f, (uint32_t)reply_to(0, f, DHCP_ACK, 3600));
    EXPECT(g_f[0].nif.ip4.addr == net_addr(0, 10));
    double t[9];
    for (unsigned r = 0; r < 9; r++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned k = 0; k < 20000; k++) {
            fz_deliver(&g_f[0], f, (uint32_t)reply_to(0, f, DHCP_NAK, 3600));
            fz_deliver(&g_f[0], f, (uint32_t)reply_to(0, f, DHCP_OFFER, 3600));
            fz_deliver(&g_f[0], f, (uint32_t)reply_to(0, f, DHCP_ACK, 3600));
        }
        t[r] = ns_since(&t0) / 20000;
    }
    EXPECT(g_f[0].nif.ip4.addr == net_addr(0, 10));
    qsort(t, 9, sizeof(double), cmp_double);
    teardown(baseline);
    g_benching = false;
    printf("  bench an exchange (NAK, OFFER, ACK) %.1f ns\n", t[4]);
}

static const struct host_test tests[] = {
    { "dhcp digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("DHCP_BENCH") != NULL)
        dhcp_bench();
    return rc;
}
