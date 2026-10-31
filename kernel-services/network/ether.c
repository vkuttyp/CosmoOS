/*
 * ether.c - Ethernet framing and dispatch.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/netparse.h>
#include <kernel/string.h>

const uint8_t eth_broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

void ether_input(struct netif *nif, struct mbuf *m)
{
    m = m_pullup(m, ETH_HLEN);
    if (m == NULL) {
        __atomic_fetch_add(&nif->stats.rx_errors, 1, __ATOMIC_RELAXED);
        return;
    }
    uint16_t type;
    switch (netparse_ether(m->data, nif->mac, &type)) {
    case NETPARSE_ETH_OTHER:   /* promiscuous devices hand us others' frames */
    case NETPARSE_ETH_OWN:     /* our own frame reflected: never accept it */
        m_freem(m);
        return;
    case NETPARSE_ETH_BROADCAST:
        m->flags |= M_BCAST;
        break;
    case NETPARSE_ETH_MULTICAST:
        m->flags |= M_MCAST;
        break;
    default:
        break;
    }
    m->pkt.proto = type;
    m_adj(m, ETH_HLEN);
    switch (type) {
    case ETH_P_IP:   ipv4_input(nif, m); break;
    case ETH_P_ARP:  arp_input(nif, m); break;
    case ETH_P_IPV6: ipv6_input(nif, m); break;
    default:
        m_freem(m);
        break;
    }
}

int ether_output(struct netif *nif, struct mbuf *m, const uint8_t dst[ETH_ALEN], uint16_t type)
{
    m = m_prepend(m, ETH_HLEN);
    if (m == NULL)
        return -ENOMEM;
    struct eth_hdr *eh = (struct eth_hdr *)m->data;
    memcpy(eh->dst, dst, ETH_ALEN);
    memcpy(eh->src, nif->mac, ETH_ALEN);
    eh->type = htons(type);
    if (m->pkt.csum_flags & NET_CSUM_TX)
        m->pkt.csum_start += ETH_HLEN;
    /* Minimum frame size (60 bytes before the FCS): switches and virtual
     * backends drop runts, and QEMU's user-mode network is one of them. */
    if (m->pkt.len < ETH_ZLEN) {
        static const uint8_t zeros[ETH_ZLEN];
        if (m_append(m, zeros, ETH_ZLEN - m->pkt.len)) {
            m_freem(m);
            return -ENOMEM;
        }
    }
    return netif_transmit(nif, m);
}
