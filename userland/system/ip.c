/* ip - show addresses, links, routes and neighbours, as iproute2 prints them
 *
 *   ip [-4|-6] address|link|route|neigh [show [dev] [NAME]]
 *
 * A read-only front end over /dev/net/tapctl's snapshot (netctl version 8:
 * the interface, route and neighbour sections, and the lease section for
 * which addresses DHCP holds; shell completeness S1). BusyBox's ip speaks
 * only netlink, which this kernel does not have, so this program is the
 * deliberate substitute until it does. Objects and commands may be
 * abbreviated as iproute2 allows ("ip a", "ip r", "ip n", "ip addr sh").
 * Every command that would change something is refused: ifconfig and
 * route configure, under PRIV_NET.
 *
 * The lines follow iproute2's layout field for field and leave out what
 * the kernel does not report (qdisc, group, qlen, a link mode), as
 * iproute2 does when a kernel omits an attribute. Reading the snapshot
 * takes PRIV_NET, as every netctl read does.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uapi/cosmo/netctl.h>

static unsigned char g_buf[COSMO_NETCTL_SNAPSHOT_MAX];

static const struct cosmo_netctl_lease *g_leases;
static const struct cosmo_netctl_if *g_ifs;
static const struct cosmo_netctl_route *g_routes;
static const struct cosmo_netctl_neigh *g_neighs;
static unsigned g_nleases, g_nifs, g_nroutes, g_nneighs;

/* Read the snapshot and find the sections, checking every count against
 * the bytes read before trusting it. 0, or -1 with a message. */
static int load(void)
{
    int fd = open("/dev/net/tapctl", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ip: cannot open /dev/net/tapctl: %s\n", strerror(errno));
        return -1;
    }
    ssize_t n = read(fd, g_buf, sizeof(g_buf));
    close(fd);
    if (n < (ssize_t)sizeof(struct cosmo_netctl_list)) {
        fprintf(stderr, "ip: reading /dev/net/tapctl: %s\n", n < 0 ? strerror(errno) : "short");
        return -1;
    }
    size_t len = (size_t)n, off = 0;
    /* One section header at `off`, copied out (the buffer is not aligned for it). */
#define TAKE(hdr_t, hdr)                         \
    hdr_t hdr;                                   \
    if (len < off + sizeof(hdr))                 \
        goto short_snap;                         \
    memcpy(&hdr, g_buf + off, sizeof(hdr));      \
    off += sizeof(hdr);
    TAKE(struct cosmo_netctl_list, ph)
    if (ph.version != COSMO_NETCTL_VERSION) {
        fprintf(stderr, "ip: snapshot version %u, this ip speaks %u\n", ph.version, COSMO_NETCTL_VERSION);
        return -1;
    }
    off += (size_t)ph.count * sizeof(struct cosmo_netctl_rule);
    TAKE(struct cosmo_netctl_filter_list, fh)
    off += (size_t)fh.guest_count * sizeof(struct cosmo_netctl_filter_guest) +
           (size_t)fh.rule_count * sizeof(struct cosmo_netctl_filter_rule);
    TAKE(struct cosmo_netctl_flow_list, wh)
    off += (size_t)wh.count * sizeof(struct cosmo_netctl_flow);
    TAKE(struct cosmo_netctl_lease_list, lh)
    g_leases = (const struct cosmo_netctl_lease *)(g_buf + off);
    g_nleases = lh.count;
    off += (size_t)lh.count * sizeof(struct cosmo_netctl_lease);
    TAKE(struct cosmo_netctl_if_list, ih)
    g_ifs = (const struct cosmo_netctl_if *)(g_buf + off);
    g_nifs = ih.count;
    off += (size_t)ih.count * sizeof(struct cosmo_netctl_if);
    TAKE(struct cosmo_netctl_route_list, rh)
    g_routes = (const struct cosmo_netctl_route *)(g_buf + off);
    g_nroutes = rh.count;
    off += (size_t)rh.count * sizeof(struct cosmo_netctl_route);
    TAKE(struct cosmo_netctl_neigh_list, nh)
    g_neighs = (const struct cosmo_netctl_neigh *)(g_buf + off);
    g_nneighs = nh.count;
    off += (size_t)nh.count * sizeof(struct cosmo_netctl_neigh);
#undef TAKE
    if (len < off)
        goto short_snap;
    return 0;
short_snap:
    fprintf(stderr, "ip: the snapshot is shorter than its counts say\n");
    return -1;
}

static unsigned prefix_of(uint32_t mask)
{
    unsigned p = 0;
    for (uint32_t m = ntohl(mask); m & 0x80000000u; m <<= 1)
        p++;
    return p;
}

/* The lease DHCP holds for this interface, or NULL (an address from
 * fw_cfg, or none yet) -- only while the interface still has the lease's
 * address and mask: one set by hand since (ifconfig) is not the lease's,
 * and is shown as static. */
static const struct cosmo_netctl_lease *lease_of(const char *ifname, uint32_t addr, uint32_t mask)
{
    for (unsigned i = 0; i < g_nleases; i++) {
        const struct cosmo_netctl_lease *l = &g_leases[i];
        bool held = l->state == COSMO_NETCTL_DHCP_BOUND || l->state == COSMO_NETCTL_DHCP_RENEWING ||
                    l->state == COSMO_NETCTL_DHCP_REBINDING;
        if (held && strncmp(l->ifname, ifname, sizeof(l->ifname)) == 0 && l->addr == addr && l->mask == mask)
            return l;
    }
    return NULL;
}

/* The interface named `ifname` in the snapshot, or NULL. */
static const struct cosmo_netctl_if *if_named(const char *ifname)
{
    for (unsigned i = 0; i < g_nifs; i++)
        if (strncmp(g_ifs[i].name, ifname, sizeof(g_ifs[i].name)) == 0)
            return &g_ifs[i];
    return NULL;
}

static void mac_text(const uint8_t *m, char *out, size_t n)
{
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65535 state UNKNOWN" and, unless a
 * family was asked for, the link/ line. */
static void link_lines(const struct cosmo_netctl_if *f, bool link_line)
{
    bool up = f->flags & COSMO_NETCTL_IF_UP, lo = f->flags & COSMO_NETCTL_IF_LOOPBACK;
    char flags[64];
    snprintf(flags, sizeof(flags), "%s%s%s", lo ? "LOOPBACK" : "BROADCAST,MULTICAST",
             (f->flags & COSMO_NETCTL_IF_NOARP) && !lo ? ",NOARP" : "", up ? ",UP,LOWER_UP" : "");
    printf("%u: %.8s: <%s> mtu %u state %s\n", f->index, f->name, flags, f->mtu,
           lo ? "UNKNOWN" : up ? "UP" : "DOWN");
    if (!link_line)
        return;
    char mac[18];
    mac_text(f->mac, mac, sizeof(mac));
    printf("    link/%s %s brd %s\n", lo ? "loopback" : "ether", mac, lo ? "00:00:00:00:00:00" : "ff:ff:ff:ff:ff:ff");
}

static void inet_lines(const struct cosmo_netctl_if *f)
{
    if (f->addr == 0)
        return;
    char a[16], b[16];
    inet_ntop(AF_INET, &f->addr, a, sizeof(a));
    bool lo = f->flags & COSMO_NETCTL_IF_LOOPBACK;
    const struct cosmo_netctl_lease *l = lo ? NULL : lease_of(f->name, f->addr, f->mask);
    if (lo) {
        printf("    inet %s/%u scope host %.8s\n", a, prefix_of(f->mask), f->name);
    } else {
        uint32_t brd = f->addr | ~f->mask;
        inet_ntop(AF_INET, &brd, b, sizeof(b));
        printf("    inet %s/%u brd %s scope global%s %.8s\n", a, prefix_of(f->mask), b, l ? " dynamic" : "", f->name);
    }
    if (l != NULL && l->remaining_ms != 0xffffffffu)
        printf("       valid_lft %usec preferred_lft %usec\n", l->remaining_ms / 1000, l->remaining_ms / 1000);
    else
        printf("       valid_lft forever preferred_lft forever\n");
}

static void inet6_lines(const struct cosmo_netctl_if *f)
{
    static const uint8_t zero[16];
    if (memcmp(f->ip6_ll, zero, sizeof(zero)) == 0)
        return;
    char a[48];
    inet_ntop(AF_INET6, f->ip6_ll, a, sizeof(a));
    bool lo = f->flags & COSMO_NETCTL_IF_LOOPBACK;
    printf("    inet6 %s/%u scope %s\n", a, lo ? 128u : 64u, lo ? "host" : "link");
    printf("       valid_lft forever preferred_lft forever\n");
}

static bool name_is(const struct cosmo_netctl_if *f, const char *dev)
{
    return dev == NULL || strncmp(f->name, dev, sizeof(f->name)) == 0;
}

static int show_addr(int family, const char *dev, bool addresses)
{
    static const uint8_t zero[16];
    bool any = false;
    for (unsigned i = 0; i < g_nifs; i++) {
        const struct cosmo_netctl_if *f = &g_ifs[i];
        if (!name_is(f, dev))
            continue;
        any = true;
        if (addresses && family == 4 && f->addr == 0)
            continue;   /* `ip -4 a` lists only the interfaces it has something to say about */
        if (addresses && family == 6 && memcmp(f->ip6_ll, zero, sizeof(zero)) == 0)
            continue;
        link_lines(f, family == 0);
        if (!addresses)
            continue;
        if (family != 6)
            inet_lines(f);
        if (family != 4)
            inet6_lines(f);
    }
    if (dev != NULL && !any) {
        fprintf(stderr, "Device \"%s\" does not exist.\n", dev);
        return 1;
    }
    return 0;
}

static int show_route(int family, const char *dev)
{
    if (family == 6) {
        /* IPv6 routes only the link: each link-local subnet. */
        for (unsigned i = 0; i < g_nifs; i++) {
            const struct cosmo_netctl_if *f = &g_ifs[i];
            if ((f->flags & COSMO_NETCTL_IF_LOOPBACK) || !(f->flags & COSMO_NETCTL_IF_UP) || !name_is(f, dev))
                continue;
            printf("fe80::/64 dev %.8s proto kernel metric 256 pref medium\n", f->name);
        }
        return 0;
    }
    for (unsigned i = 0; i < g_nroutes; i++) {
        const struct cosmo_netctl_route *r = &g_routes[i];
        if (dev != NULL && strncmp(r->ifname, dev, sizeof(r->ifname)) != 0)
            continue;
        char d[16], g[16], s[16];
        inet_ntop(AF_INET, &r->dst, d, sizeof(d));
        inet_ntop(AF_INET, &r->gateway, g, sizeof(g));
        inet_ntop(AF_INET, &r->src, s, sizeof(s));
        if (r->flags & COSMO_NETCTL_RT_GATEWAY) {
            /* DHCP's while the interface holds the lease and the lease names this router. */
            const struct cosmo_netctl_if *f = if_named(r->ifname);
            const struct cosmo_netctl_lease *l = f ? lease_of(r->ifname, f->addr, f->mask) : NULL;
            printf("default via %s dev %.8s proto %s src %s\n", g, r->ifname,
                   l != NULL && l->router == r->gateway ? "dhcp" : "static", s);
        }
        else
            printf("%s/%u dev %.8s proto kernel scope link src %s\n", d, prefix_of(r->mask), r->ifname, s);
    }
    return 0;
}

static int show_neigh(int family, const char *dev)
{
    if (family == 6)
        return 0;   /* IPv6's neighbours are not in the snapshot */
    for (unsigned i = 0; i < g_nneighs; i++) {
        const struct cosmo_netctl_neigh *n = &g_neighs[i];
        if (dev != NULL && strncmp(n->ifname, dev, sizeof(n->ifname)) != 0)
            continue;
        char a[16], m[18];
        inet_ntop(AF_INET, &n->addr, a, sizeof(a));
        if (n->state == COSMO_NETCTL_NEIGH_REACHABLE) {
            mac_text(n->mac, m, sizeof(m));
            printf("%s dev %.8s lladdr %s REACHABLE\n", a, n->ifname, m);
        } else {
            printf("%s dev %.8s INCOMPLETE\n", a, n->ifname);
        }
    }
    return 0;
}

/* iproute2's abbreviation rule: `word` is a prefix of `full` at least
 * `min` characters long. */
static bool matches(const char *word, const char *full, size_t min)
{
    size_t n = strlen(word);
    return n >= min && n <= strlen(full) && strncmp(word, full, n) == 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: ip [-4|-6] address|link|route|neigh [show [dev] [NAME]]\n");
    return 2;
}

int main(int argc, char **argv)
{
    int family = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-4") == 0)
            family = 4;
        else if (strcmp(argv[i], "-6") == 0)
            family = 6;
        else
            return usage();
    }
    const char *obj = i < argc ? argv[i++] : NULL;
    if (obj == NULL)
        return usage();
    enum { ADDR, LINK, ROUTE, NEIGH } what;
    if (matches(obj, "address", 1))
        what = ADDR;
    else if (matches(obj, "link", 1))
        what = LINK;
    else if (matches(obj, "route", 1))
        what = ROUTE;
    else if (matches(obj, "neighbour", 1) || matches(obj, "neighbor", 1))
        what = NEIGH;
    else {
        fprintf(stderr, "Object \"%s\" is unknown, try \"ip help\".\n", obj);
        return 2;
    }
    const char *dev = NULL;
    if (i < argc) {
        const char *cmd = argv[i++];
        if (!matches(cmd, "show", 1) && !matches(cmd, "list", 1) && strcmp(cmd, "lst") != 0) {
            fprintf(stderr, "ip: \"%s %s\": this ip only shows (the kernel has no netlink); "
                            "ifconfig and route configure\n", obj, cmd);
            return 2;
        }
        if (i < argc && strcmp(argv[i], "dev") == 0)
            i++;
        if (i < argc)
            dev = argv[i++];
        if (i < argc)
            return usage();
    }
    if (load() < 0)
        return 1;
    switch (what) {
    case ADDR:  return show_addr(family, dev, true);
    case LINK:  return show_addr(family, dev, false);
    case ROUTE: return show_route(family, dev);
    default:    return show_neigh(family, dev);
    }
}
