/*
 * loopback.c - The `lo` interface: packets go straight back into the
 * receive queue. A test filter can drop or reorder them.
 */

#include <kernel/netif.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/string.h>

static lo_filter_fn g_filter;
static void *g_filter_arg;

static int lo_transmit(struct netif *nif, struct mbuf *m)
{
    if (g_filter && !g_filter(m, g_filter_arg)) {
        m_freem(m);
        return 0;   /* "sent" and lost */
    }
    netif_rx(nif, m);
    return 0;
}

static const struct netif_ops lo_ops = { .transmit = lo_transmit };

static struct netif g_lo = {
    .name = "lo",
    .mtu = 65535,
    .flags = NETIF_LOOPBACK | NETIF_NOARP | NETIF_UP,
    .ops = &lo_ops,
};

void loopback_set_filter(lo_filter_fn fn, void *arg)
{
    g_filter = fn;
    g_filter_arg = arg;
}

void loopback_init(void)
{
    g_lo.ip4.addr = INADDR_LOOPBACK_N;
    g_lo.ip4.mask = htonl(0xff000000u);
    memset(&g_lo.ip6_ll, 0, sizeof(g_lo.ip6_ll));
    g_lo.ip6_ll.s6_addr[15] = 1;   /* ::1 */
    netif_register(&g_lo);
}
