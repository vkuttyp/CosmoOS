/*
 * procnet.c - /proc/net in Linux's formats (shell completeness S1; design.md,
 * "Inspecting the stack"). Each file is what BusyBox's ifconfig, route,
 * netstat and arp parse, laid out as Linux 6.x lays it out, from the
 * stack's listings: a copy under each table's lock, then formatting with
 * no lock held. What the stack does not have is written as Linux writes
 * its absence -- zero counters, uid 0 and inode 0 for every socket (a
 * socket here records no inode and its pcb no owner) -- never invented.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/procnet.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/printf.h>
#include <kernel/string.h>

const char *const procnet_names[PROCNET_COUNT] = {
    [PROCNET_DEV] = "dev",   [PROCNET_ROUTE] = "route", [PROCNET_ARP] = "arp",   [PROCNET_TCP] = "tcp",
    [PROCNET_TCP6] = "tcp6", [PROCNET_UDP] = "udp",     [PROCNET_UDP6] = "udp6", [PROCNET_IF_INET6] = "if_inet6",
};

#define SOCKS_MAX 400u    /* sockets listed per file: what PROCNET_TEXT_MAX holds */

/* The text so far; a line is appended whole or not at all. */
struct text {
    char *buf;
    size_t n, len;
    bool full;
};

static void line(struct text *t, const char *fmt, ...) __printf(2, 3);
static void line(struct text *t, const char *fmt, ...)
{
    if (t->full)
        return;
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int k = kvsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= sizeof(tmp) || t->len + (size_t)k >= t->n) {
        t->full = true;   /* the listing ends at its last whole line */
        return;
    }
    memcpy(t->buf + t->len, tmp, (size_t)k);
    t->len += (size_t)k;
    t->buf[t->len] = '\0';
}

/* An address as Linux's /proc/net files print it: each 32-bit word of the
 * address as stored, as a native-endian number (both architectures here
 * are little-endian, so 10.0.2.15 reads 0F02000A). */
static void hex_addr(const struct netaddr *a, char *out, size_t n)
{
    if (a->family == COSMO_AF_INET6) {
        uint32_t w[4];
        memcpy(w, a->v6.s6_addr, sizeof(w));
        ksnprintf(out, n, "%08X%08X%08X%08X", w[0], w[1], w[2], w[3]);
    } else {
        ksnprintf(out, n, "%08X", a->family == COSMO_AF_INET ? a->v4 : 0u);
    }
}

static void ipv4_text(uint32_t a, char *out, size_t n)
{
    const uint8_t *b = (const uint8_t *)&a;
    ksnprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static int render_dev(struct text *t)
{
    struct netif_info *ifs = kmalloc(NETIF_MAX * sizeof(*ifs), 0);
    if (ifs == NULL)
        return -ENOMEM;
    unsigned n = netif_list(ifs, NETIF_MAX);   /* every interface: the registry holds no more */
    line(t, "Inter-|   Receive                                                |  Transmit\n");
    line(t, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls "
            "carrier compressed\n");
    for (unsigned i = 0; i < n; i++) {
        const struct netif_stats *s = &ifs[i].stats;
        line(t, "%6s:%8llu %7llu %4llu %4llu %4u %5u %10u %9u %8llu %7llu %4llu %4llu %4u %5u %7u %10u\n", ifs[i].name,
             (unsigned long long)s->rx_bytes, (unsigned long long)s->rx_packets, (unsigned long long)s->rx_errors,
             (unsigned long long)s->rx_dropped, 0u, 0u, 0u, 0u, (unsigned long long)s->tx_bytes,
             (unsigned long long)s->tx_packets, (unsigned long long)s->tx_errors, (unsigned long long)s->tx_dropped,
             0u, 0u, 0u, 0u);
    }
    kfree(ifs);
    return 0;
}

/* Linux pads every line of /proc/net/route to 127 characters. */
static int render_route(struct text *t)
{
    struct ipv4_route_info *r = kmalloc(IPV4_ROUTES_MAX * sizeof(*r), 0);
    if (r == NULL)
        return -ENOMEM;
    unsigned n = ipv4_route_list(r, IPV4_ROUTES_MAX);
    line(t, "%-127s\n", "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT");
    for (unsigned i = 0; i < n; i++) {
        char body[128];
        ksnprintf(body, sizeof(body), "%s\t%08X\t%08X\t%04X\t%d\t%u\t%d\t%08X\t%d\t%u\t%u", r[i].ifname, r[i].dst,
                  r[i].gateway, r[i].flags, 0, 0u, 0, r[i].mask, 0, 0u, 0u);
        line(t, "%-127s\n", body);
    }
    kfree(r);
    return 0;
}

static int render_arp(struct text *t)
{
    struct arp_info *a = kmalloc(ARP_TABLE_SIZE * sizeof(*a), 0);
    if (a == NULL)
        return -ENOMEM;
    unsigned n = arp_list(a, ARP_TABLE_SIZE);
    if (n > ARP_TABLE_SIZE)
        n = ARP_TABLE_SIZE;
    line(t, "IP address       HW type     Flags       HW address            Mask     Device\n");
    for (unsigned i = 0; i < n; i++) {
        char ip[16], mac[18];
        ipv4_text(a[i].ip, ip, sizeof(ip));
        ksnprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", a[i].mac[0], a[i].mac[1], a[i].mac[2],
                  a[i].mac[3], a[i].mac[4], a[i].mac[5]);
        /* HW type 1 is Ethernet; flag 0x2 (ATF_COM) a completed entry. */
        line(t, "%-16s 0x%-10x0x%-10x%-17s     %-8s %s\n", ip, 1u, a[i].complete ? 0x2u : 0x0u, mac, "*",
             a[i].ifname);
    }
    kfree(a);
    return 0;
}

/* Linux's TCP_* numbers in /proc/net/tcp's st column. */
static unsigned linux_tcp_state(enum tcp_state s)
{
    switch (s) {
    case TCP_ESTABLISHED: return 0x01;
    case TCP_SYN_SENT:    return 0x02;
    case TCP_SYN_RCVD:    return 0x03;
    case TCP_FIN_WAIT_1:  return 0x04;
    case TCP_FIN_WAIT_2:  return 0x05;
    case TCP_TIME_WAIT:   return 0x06;
    case TCP_CLOSE_WAIT:  return 0x08;
    case TCP_LAST_ACK:    return 0x09;
    case TCP_LISTEN:      return 0x0A;
    case TCP_CLOSING:     return 0x0B;
    default:              return 0x07;   /* TCP_CLOSE */
    }
}

/* One socket line, padded as Linux pads them (to 149 characters for
 * tcp, 127 for udp): sl, local, remote, st, tx_queue:rx_queue, tr:when,
 * retrnsmt, uid, timeout, inode, then the fields no parser here reads. */
static void sock_line(struct text *t, unsigned sl, const struct netaddr *l, const struct netaddr *r, unsigned st,
                      unsigned txq, unsigned rxq, bool tcp, unsigned long long drops)
{
    char la[33], ra[33], body[200];
    hex_addr(l, la, sizeof(la));
    hex_addr(r, ra, sizeof(ra));
    if (tcp) {
        ksnprintf(body, sizeof(body), "%4u: %s:%04X %s:%04X %02X %08X:%08X 00:00000000 00000000 %5u %8d %lu 1 "
                  "0000000000000000 100 0 0 10 0", sl, la, l->port, ra, r->port, st, txq, rxq, 0u, 0, 0ul);
        line(t, "%-149s\n", body);
    } else {
        ksnprintf(body, sizeof(body), "%5u: %s:%04X %s:%04X %02X %08X:%08X 00:00000000 00000000 %5u %8d %lu 2 "
                  "0000000000000000 %llu", sl, la, l->port, ra, r->port, st, txq, rxq, 0u, 0, 0ul, drops);
        line(t, "%-127s\n", body);
    }
}

static int render_tcp(struct text *t, uint16_t family)
{
    struct tcp_sock_info *s = kmalloc(SOCKS_MAX * sizeof(*s), 0);
    if (s == NULL)
        return -ENOMEM;
    unsigned n = tcp_list(s, SOCKS_MAX, family);   /* this family's alone, chosen under the lock */
    if (n > SOCKS_MAX)
        n = SOCKS_MAX;
    line(t, "%-149s\n", "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode");
    unsigned sl = 0;
    for (unsigned i = 0; i < n; i++)
        sock_line(t, sl++, &s[i].local, &s[i].remote, linux_tcp_state(s[i].state), s[i].txq, s[i].rxq, true, 0);
    kfree(s);
    return 0;
}

static int render_udp(struct text *t, uint16_t family)
{
    struct udp_sock_info *s = kmalloc(SOCKS_MAX * sizeof(*s), 0);
    if (s == NULL)
        return -ENOMEM;
    unsigned n = udp_list(s, SOCKS_MAX, family);   /* this family's alone, chosen under the lock */
    if (n > SOCKS_MAX)
        n = SOCKS_MAX;
    line(t, "%-127s\n", "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout "
                       "inode ref pointer drops");
    unsigned sl = 0;
    for (unsigned i = 0; i < n; i++) {
        /* 01 (ESTABLISHED) once connected, else 07 (CLOSE), as Linux shows a UDP socket. */
        unsigned st = s[i].remote.port != 0 ? 0x01 : 0x07;
        struct netaddr r = s[i].remote;
        r.family = family;
        sock_line(t, sl++, &s[i].local, &r, st, 0, s[i].rxq, false, (unsigned long long)s[i].drops);
    }
    kfree(s);
    return 0;
}

/* Address, ifindex, prefix length, scope (0x10 host, 0x20 link) and flags
 * (0x80 permanent), all hex, then the name right-aligned in eight. */
static int render_if_inet6(struct text *t)
{
    struct netif_info *ifs = kmalloc(NETIF_MAX * sizeof(*ifs), 0);
    if (ifs == NULL)
        return -ENOMEM;
    unsigned n = netif_list(ifs, NETIF_MAX);   /* every interface: the registry holds no more */
    for (unsigned i = 0; i < n; i++) {
        struct in6_addr a;
        unsigned prefix, scope;
        if (ifs[i].flags & NETIF_LOOPBACK) {
            memset(&a, 0, sizeof(a));
            a.s6_addr[15] = 1;   /* ::1, which netif_owns_ipv6 answers for */
            prefix = 128;
            scope = 0x10;
        } else if (!in6_is_unspecified(&ifs[i].ip6_ll)) {
            a = ifs[i].ip6_ll;
            prefix = 64;
            scope = 0x20;
        } else {
            continue;
        }
        char hex[33];
        for (unsigned k = 0; k < 16; k++)
            ksnprintf(hex + 2 * k, 3, "%02x", a.s6_addr[k]);
        line(t, "%s %02x %02x %02x %02x %8s\n", hex, ifs[i].index, prefix, scope, 0x80u, ifs[i].name);
    }
    kfree(ifs);
    return 0;
}

int procnet_render(enum procnet_file f, char *out, size_t n)
{
    struct text t = { .buf = out, .n = n, .len = 0, .full = n == 0 };
    if (n)
        out[0] = '\0';
    int rc = 0;
    switch (f) {
    case PROCNET_DEV:      rc = render_dev(&t); break;
    case PROCNET_ROUTE:    rc = render_route(&t); break;
    case PROCNET_ARP:      rc = render_arp(&t); break;
    case PROCNET_TCP:      rc = render_tcp(&t, COSMO_AF_INET); break;
    case PROCNET_TCP6:     rc = render_tcp(&t, COSMO_AF_INET6); break;
    case PROCNET_UDP:      rc = render_udp(&t, COSMO_AF_INET); break;
    case PROCNET_UDP6:     rc = render_udp(&t, COSMO_AF_INET6); break;
    case PROCNET_IF_INET6: rc = render_if_inet6(&t); break;
    default:               return -ENOENT;
    }
    return rc < 0 ? rc : (int)t.len;
}
