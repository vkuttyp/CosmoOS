/*
 * dhcp.h - DHCP (RFC 2131 / 2132): the wire format both ends share, and the
 * kernel's client (dhcp.c; docs/kernel-services/network/design.md, "The DHCP
 * client").
 *
 * The server is the tap's (tapsvc.c); the client configures the machine's own
 * Ethernet interfaces, as Linux's ipconfig does: an interface a driver marks
 * NETIF_AUTOCONF that has no fw_cfg address (opt/cosmo/ipv4) runs
 * DISCOVER/OFFER/REQUEST/ACK at registration, renews at T1 (unicast to the
 * server), rebinds at T2 (broadcast), and loses its address when the lease
 * ends. Replies are claimed in ipv4_input before the address checks, since
 * until the ACK the interface has no address for them to be addressed to.
 */
#ifndef KERNEL_NET_DHCP_H
#define KERNEL_NET_DHCP_H

#include <kernel/types.h>

struct netif;
struct mbuf;

/* --- the wire ------------------------------------------------------------------ */

#define BOOTP_REQUEST 1
#define BOOTP_REPLY   2
#define DHCP_HTYPE_ETHER 1
#define DHCP_MAGIC    0x63825363u

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_DECLINE  4
#define DHCP_ACK      5
#define DHCP_NAK      6
#define DHCP_RELEASE  7

#define OPT_PAD        0
#define OPT_SUBNET     1
#define OPT_ROUTER     3
#define OPT_DNS        6
#define OPT_HOSTNAME   12   /* RFC 2132 §3.14: the client's host name (shell completeness S1) */
#define OPT_REQIP      50
#define OPT_LEASE      51
#define OPT_MSGTYPE    53
#define OPT_SERVERID   54
#define OPT_PARAMS     55
#define OPT_T1         58
#define OPT_T2         59
#define OPT_CLIENTID   61
#define OPT_END        255

#define DHCP_SPORT 67   /* server */
#define DHCP_CPORT 68   /* client */
#define DHCP_FLAG_BROADCAST 0x8000u

/* BOOTP fixed header (236 bytes), then the 4-byte magic cookie, then options. */
#define BOOTP_LEN 236
#define DHCP_FIXED (BOOTP_LEN + 4)
#define BOOTP_OFF_OP     0
#define BOOTP_OFF_HTYPE  1
#define BOOTP_OFF_HLEN   2
#define BOOTP_OFF_XID    4
#define BOOTP_OFF_SECS   8
#define BOOTP_OFF_FLAGS  10
#define BOOTP_OFF_CIADDR 12
#define BOOTP_OFF_YIADDR 16
#define BOOTP_OFF_SIADDR 20
#define BOOTP_OFF_CHADDR 28

/* Find option `code` in [opts, opts+len); its value and length (*olen), or
 * NULL. Stops at OPT_END, skips OPT_PAD, never reads past `len`. */
static inline const uint8_t *dhcp_opt(const uint8_t *opts, uint32_t len, uint8_t code, uint8_t *olen)
{
    uint32_t i = 0;
    while (i < len) {
        uint8_t c = opts[i++];
        if (c == OPT_END)
            break;
        if (c == OPT_PAD)
            continue;
        if (i >= len)
            break;
        uint8_t l = opts[i++];
        if (i + l > len)
            break;
        if (c == code) {
            *olen = l;
            return opts + i;
        }
        i += l;
    }
    return NULL;
}

/* --- the client ---------------------------------------------------------------- */

enum dhcp_state {
    DHCP_S_INIT = 1,      /* no lease: DISCOVER next */
    DHCP_S_SELECTING,     /* DISCOVER sent, waiting for an OFFER */
    DHCP_S_REQUESTING,    /* REQUEST for an offer sent, waiting for the ACK */
    DHCP_S_BOUND,         /* the address is the interface's until T1 */
    DHCP_S_RENEWING,      /* past T1: REQUEST unicast to the server that granted it */
    DHCP_S_REBINDING,     /* past T2: REQUEST broadcast to any server */
};

#define DHCP_MAX_CLIENTS 4
#define DHCP_MAX_DNS     3
#define DHCP_HOSTNAME_MAX 64   /* a kept option 12, NUL included: COSMO_HOST_NAME_MAX */
/* The first DISCOVER/REQUEST retransmission; doubled per retry up to 64 s
 * (RFC 2131 §4.1 starts at 4 s; a lost packet at boot costs half that). */
#define DHCP_RETX_FIRST_NS (2ull * 1000000000ull)
#define DHCP_RETX_MAX_NS   (64ull * 1000000000ull)
#define DHCP_REQUEST_TRIES 4          /* REQUESTING gives up and starts over after this many */
#define DHCP_RENEW_MIN_NS  (60ull * 1000000000ull)   /* RFC 2131 §4.4.5: renew/rebind retransmit floor */
#define DHCP_BOOT_WAIT_NS  (10ull * 1000000000ull)   /* dhcp_boot_wait's bound */

/* One interface's client, as dhcp_lease_list copies it. Addresses network
 * order; times in seconds as the server granted them, remaining_ms from
 * the read's `now` (0 when not bound). */
struct dhcp_lease_info {
    char ifname[8];
    uint8_t state;                 /* enum dhcp_state */
    uint8_t ndns;
    uint32_t addr, mask, router, server;
    uint32_t dns[DHCP_MAX_DNS];
    uint32_t lease_s, t1_s, t2_s;
    uint32_t remaining_ms;         /* to the end of the lease (BOUND/RENEWING/REBINDING) */
    uint32_t t1_ms, t2_ms;         /* to T1 and T2 from the read, 0 once passed */
    char hostname[DHCP_HOSTNAME_MAX];   /* the lease's option 12, or empty */
};

struct dhcp_stats {
    uint64_t discovers, requests, offers, acks, naks, renews, rebinds, expiries;
    uint64_t ignored;              /* a reply that was not for us, malformed, or unexpected in this state */
};

/* netif_register, for an interface marked NETIF_AUTOCONF with no fw_cfg
 * address; self-tests call it for their fixtures. Thread context. -ENOSPC:
 * DHCP_MAX_CLIENTS are running. */
int dhcp_client_start(struct netif *nif);
/* netif_unregister: the client stops and lets go of the interface; on return
 * no timer or work item of its runs. Sleeps. A no-op without a client. */
void dhcp_client_stop(struct netif *nif);
/* netif_set_up(true): a DISCOVER or REQUEST waiting for its retransmission
 * is sent now (one sent while the link was down was lost). A no-op without
 * a client or in any other state. Thread context. */
void dhcp_client_kick(struct netif *nif);
/* ipv4_input, for a UDP datagram arriving on a non-loopback interface,
 * before the address checks: true when it is a server-to-client DHCP
 * datagram for an interface with a client (the packet taken), false
 * otherwise (the packet untouched). `ihl` is the IP header length. */
bool dhcp_client_input(struct netif *nif, struct mbuf *m, unsigned ihl);
/* Every client, at one `now`. Returns how many were written. Any context. */
unsigned dhcp_lease_list(struct dhcp_lease_info *out, unsigned max);
/* Boot composition, after the boot modules: wait (DHCP_BOOT_WAIT_NS at
 * most) for every client to be bound, as Linux's ipconfig waits before
 * mounting root. A client still unbound keeps trying; the boot goes on. */
void dhcp_boot_wait(void);
void dhcp_get_stats(struct dhcp_stats *out);

#if CONFIG_SELFTEST
/* Run the client's next deadline now: T1, T2, the lease's end, or the
 * retransmission it is waiting for -- the time the state machine sees moves
 * to that deadline, so a test reaches each state without waiting a lease
 * out. -ENOENT without a client. */
enum dhcp_test_event { DHCP_TEST_RETX, DHCP_TEST_T1, DHCP_TEST_T2, DHCP_TEST_EXPIRE };
int dhcp_test_fire(struct netif *nif, enum dhcp_test_event ev);
/* The client's state, or 0 without one. */
int dhcp_test_state(struct netif *nif);
#endif

#endif /* KERNEL_NET_DHCP_H */
