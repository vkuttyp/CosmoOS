/*
 * netctl.h - The /dev/net/tapctl control ABI (docs/audit/next-subsystem-netctl.md).
 *
 * A privileged owner configures the guest's networking at runtime by writing
 * one fixed-layout command (struct cosmo_netctl) to /dev/net/tapctl, and
 * reads the live rules back as a versioned snapshot (struct cosmo_netctl_list
 * followed by that many struct cosmo_netctl_rule). Ports are host byte order;
 * addresses are network byte order (an IPv4 address as stored on the wire).
 */
#ifndef UAPI_COSMO_NETCTL_H
#define UAPI_COSMO_NETCTL_H

#include <stdint.h>

/* Version 2 added the forwarding firewall (docs/audit/next-subsystem-firewall.md):
 * the FILTER_* commands below and a filter section appended to the read
 * snapshot after the port-forward list. Version 3 adds the INPUT chain
 * (docs/audit/next-subsystem-input-chain.md): the DIR_TO_HOST direction, a
 * third default policy in the per-guest record, and -- a semantic change --
 * an ICMP rule's dst_port is now its ICMP type (version 2 required 0 there
 * and meant "any"; a version-3 rule says a type or ICMP_TYPE_ANY). Version 4
 * adds the host chain (docs/audit/next-subsystem-host-input.md): the
 * DIR_FROM_UPLINK direction, a source prefix in the filter command and rule
 * records (both grow: 20 -> 28 and 16 -> 24 bytes, so a version-3 writer is
 * refused by size), a fourth default in the per-guest record, and the host
 * itself as a policy object under HOST_ADDR (guest_addr 0). Version 5 adds
 * the OUTPUT chain (docs/audit/next-subsystem-output-chain.md): the
 * DIR_OUTPUT direction, an egress `scope` in a filter command and a rule
 * record (one of the three reserved bytes each, so neither changes size),
 * and a fifth default in the per-guest record -- which had no reserved byte
 * left, so that record grows from 8 to 12 bytes and the snapshot with it.
 * Version 6 adds the flow section (docs/audit/next-subsystem-net-flows.md):
 * a third section after the filter section, read-only, listing every live
 * NAT and firewall flow with the shares and refusal counters that explain
 * why a guest's new flows are refused. No command changes. Version 7 adds
 * the lease section (roadmap M4): a fourth section after the flow section,
 * read-only, listing the machine's own DHCP clients and their leases
 * (address, mask, router, DNS servers, lease times), which netcfg turns into
 * /etc/resolv.conf. No command changes. Version 8 (shell completeness S1)
 * gives a lease record the host name the server named (DHCP option 12),
 * which netcfg writes to /etc/hostname -- the record grows from 64 to 128
 * bytes -- and appends three read-only sections after the lease section,
 * what `ip` prints: the interfaces, IPv4's routes and the ARP table. No
 * command changes. */
#define COSMO_NETCTL_VERSION 8
/* A lease section time that is finite but beyond what 32 bits of
 * milliseconds hold (0xffffffff is an infinite lease). */
#define COSMO_NETCTL_LEASE_MS_MAX 0xfffffffeu

/* Opcodes. FORWARD_* are carried by struct cosmo_netctl; FILTER_* by struct
 * cosmo_netctl_filter. Every command is written whole, at its own struct's
 * exact size; the first four bytes (version, op) are common. */
#define COSMO_NETCTL_FORWARD_ADD   1   /* add a port-forward (DNAT) rule */
#define COSMO_NETCTL_FORWARD_DEL   2   /* remove the rule bound to (proto, host_port) */
#define COSMO_NETCTL_FILTER_ADD    3   /* insert a firewall rule for a guest */
#define COSMO_NETCTL_FILTER_DEL    4   /* remove the firewall rule matching a tuple */
#define COSMO_NETCTL_FILTER_POLICY 5   /* set a guest's default verdict for a direction */

/* Protocol values (the IANA IP protocol numbers, stable). PROTO_ANY is a
 * filter wildcard only; FORWARD_* accept TCP/UDP. */
#define COSMO_NETCTL_PROTO_ANY 0
#define COSMO_NETCTL_PROTO_ICMP 1
#define COSMO_NETCTL_PROTO_TCP 6
#define COSMO_NETCTL_PROTO_UDP 17

/* Filter directions: where a guest's datagram is going. TO_UPLINK/TO_GUEST
 * are the FORWARD chain (decided by the egress); TO_HOST is the INPUT chain
 * (a datagram the guest sends to the host itself). FROM_UPLINK is the host
 * chain (version 4): a datagram the *world* -- a real, non-guest link --
 * sends to the host; it belongs to the host object (HOST_ADDR) alone. */
#define COSMO_NETCTL_DIR_ANY       0   /* either forwarding direction (uplink or guest); never TO_HOST or
                                        * FROM_UPLINK -- a version-2 wildcard keeps its meaning; host
                                        * traffic needs an explicit TO_HOST / FROM_UPLINK rule */
#define COSMO_NETCTL_DIR_TO_UPLINK 1   /* egress is not a guest tap (the world) */
#define COSMO_NETCTL_DIR_TO_GUEST  2   /* egress is another guest's tap */
#define COSMO_NETCTL_DIR_TO_HOST   3   /* addressed to the host (one of its own addresses, or broadcast) */
#define COSMO_NETCTL_DIR_FROM_UPLINK 4 /* version 4: arriving on the uplink, addressed to the host */
#define COSMO_NETCTL_DIR_OUTPUT      5 /* version 5: sent by the host itself, out any non-loopback link */

/* The egress an OUTPUT rule applies to (version 5). ANY unless the rule
 * means to separate the two, and ANY is required on every other direction.
 * A destination prefix can also name a guest, but the tap pool reassigns
 * 10.0.(3+k).0/24 as guests come and go, so a prefix follows whoever
 * inherits the subnet while a scope keeps meaning what it said. */
#define COSMO_NETCTL_SCOPE_ANY   0
#define COSMO_NETCTL_SCOPE_WORLD 1   /* a real, non-guest link */
#define COSMO_NETCTL_SCOPE_GUEST 2   /* a guest's tap, the host's replies to it included */

/* The host as a policy object (version 4). guest_addr 0 in a FILTER_* command
 * names the host's own chain -- the one address no guest or forward can mean,
 * refused by every op before version 4. A host-scoped rule or policy names
 * DIR_FROM_UPLINK only, and is the only kind that may carry a source prefix;
 * the listing carries the host as a record with guest_addr 0. Its default is
 * ACCEPT. A DROP on this chain is silent: the host answers nothing -- no
 * SYN-ACK, RST, challenge ACK or port-unreachable -- while the host's own
 * established connections and connected UDP sockets keep working. */
#define COSMO_NETCTL_HOST_ADDR 0

/* For an ICMP rule the transport selector (dst_port) is the ICMP type; this
 * is its wildcard. (Type 0 is Echo Reply, so 0 cannot mean "any".) */
#define COSMO_NETCTL_ICMP_TYPE_ANY 0xffff

/* Filter verdicts. */
#define COSMO_NETCTL_VERDICT_DROP   0
#define COSMO_NETCTL_VERDICT_ACCEPT 1

/* One control command, written whole to /dev/net/tapctl. */
struct cosmo_netctl {
    uint16_t version;      /* COSMO_NETCTL_VERSION */
    uint16_t op;           /* COSMO_NETCTL_FORWARD_* */
    uint8_t  proto;        /* COSMO_NETCTL_PROTO_TCP / UDP */
    uint8_t  reserved;     /* must be 0 */
    uint16_t host_port;    /* host byte order */
    uint16_t guest_port;   /* host byte order (ignored by DEL) */
    uint16_t reserved2;    /* must be 0 */
    uint32_t guest_addr;   /* network byte order (ignored by DEL) */
};

/* The read snapshot: this header, then `count` rules. */
struct cosmo_netctl_list {
    uint16_t version;      /* COSMO_NETCTL_VERSION */
    uint16_t count;        /* number of struct cosmo_netctl_rule following */
};

struct cosmo_netctl_rule {
    uint8_t  proto;
    uint8_t  reserved;
    uint16_t host_port;    /* host byte order */
    uint16_t guest_port;   /* host byte order */
    uint16_t reserved2;
    uint32_t guest_addr;   /* network byte order */
};

/*
 * The forwarding firewall (version 2). A rule is identified by its whole
 * match tuple -- there is no kernel-assigned id, because a write returns
 * only a byte count and could not hand one back; FILTER_DEL therefore names
 * the same tuple FILTER_ADD installed, as FORWARD_DEL names (proto, host_port).
 * Rules are per guest (named by address in the payload, as a forward names
 * its target) and evaluated first-match in list order; `at_index` places a
 * new rule (clamped to the end), so ordering is explicit and needs no id.
 * A datagram matching no rule takes the guest's default verdict for its
 * direction (FILTER_POLICY).
 */
struct cosmo_netctl_filter {
    uint16_t version;      /* COSMO_NETCTL_VERSION */
    uint16_t op;           /* COSMO_NETCTL_FILTER_* */
    uint32_t guest_addr;   /* network byte order: whose policy this is; HOST_ADDR (0) = the host's */
    uint8_t  direction;    /* COSMO_NETCTL_DIR_* (POLICY: a guest's TO_UPLINK/TO_GUEST/TO_HOST, the
                            * host's FROM_UPLINK) */
    uint8_t  proto;        /* COSMO_NETCTL_PROTO_* (0 = any); ignored by POLICY */
    uint8_t  dst_prefix;   /* 0..32 bits of dst_addr that must match; ignored by POLICY */
    uint8_t  verdict;      /* COSMO_NETCTL_VERDICT_* */
    uint32_t dst_addr;     /* network byte order; ignored by POLICY */
    uint16_t dst_port;     /* the transport selector, host byte order: for TCP/UDP/any a
                            * destination port (0 = any); for ICMP the ICMP type 0..255
                            * or COSMO_NETCTL_ICMP_TYPE_ANY; ignored by POLICY */
    uint16_t at_index;     /* ADD: insert position (>= count appends); else 0 */
    uint32_t src_addr;     /* version 4, network byte order: the source prefix; host rules only
                            * (a guest's rule must say 0/0 -- its source is the guest); ignored by POLICY */
    uint8_t  src_prefix;   /* 0..32 bits of src_addr that must match (0 = any source) */
    uint8_t  scope;        /* version 5: COSMO_NETCTL_SCOPE_* -- DIR_OUTPUT rules only, ANY otherwise */
    uint8_t  reserved[2];  /* must be 0 */
};

/* The read snapshot, version 2 and later: the port-forward list (struct
 * cosmo_netctl_list + its rules, unchanged) followed by this filter section --
 * a header, `guest_count` policy records (version 4: the host's record,
 * guest_addr HOST_ADDR, first, then the attached guests'), then `rule_count`
 * rules in evaluation order, each carrying its guest and its current index.
 * A reader that stops after the port-forward rules is unaffected. */
struct cosmo_netctl_filter_list {
    uint16_t version;      /* COSMO_NETCTL_VERSION */
    uint16_t rule_count;
    uint16_t guest_count;
    uint16_t reserved;
};

struct cosmo_netctl_filter_guest {
    uint32_t guest_addr;        /* network byte order; HOST_ADDR (0) is the host's record */
    uint8_t  policy_to_uplink;  /* COSMO_NETCTL_VERDICT_* */
    uint8_t  policy_to_guest;
    uint8_t  policy_to_host;    /* version 3: the INPUT chain's default */
    uint8_t  policy_from_uplink; /* version 4: the host chain's default -- meaningful in the host's
                                  * record alone (a guest's reads 0; its other three read 0 in the host's) */
    uint8_t  policy_output;      /* version 5: the OUTPUT chain's default, the host's record alone */
    uint8_t  reserved[3];        /* version 5: the record grew to 12 bytes when this byte was added */
};

struct cosmo_netctl_filter_rule {
    uint32_t guest_addr;   /* network byte order; HOST_ADDR (0) is a host rule */
    uint8_t  direction;
    uint8_t  proto;
    uint8_t  dst_prefix;
    uint8_t  verdict;
    uint32_t dst_addr;     /* network byte order */
    uint16_t dst_port;     /* host byte order */
    uint16_t index;        /* position in the guest's list (display) */
    uint32_t src_addr;     /* version 4, network byte order (0/0 = any source; always so for a guest's rule) */
    uint8_t  src_prefix;
    uint8_t  scope;        /* version 5: COSMO_NETCTL_SCOPE_* (ANY unless a DIR_OUTPUT rule says otherwise) */
    uint8_t  reserved[2];
};

/*
 * The flow section (version 6), after the filter section: a header, then
 * `count` flows. A flow is listed exactly when its table counts it against a
 * share -- in use and not yet expired at the read's one `now` (invariant
 * N24) -- and each table's flows are copied in one hold of its lock, so a
 * table's list is one instant; the NAT table's and the firewall's are two
 * instants, one after the other. Every flow is written as who opened it
 * (src) to whom (dst).
 */
#define COSMO_NETCTL_FLOW_NAT 1        /* table: NAT's conntrack */
#define COSMO_NETCTL_FLOW_FW  2        /* table: the firewall's flow table */

#define COSMO_NETCTL_FLOW_MASQ  1      /* NAT: a guest's outbound flow, masqueraded as nat_addr:nat_port */
#define COSMO_NETCTL_FLOW_DNAT  2      /* NAT: a client's inbound flow to nat_addr:nat_port, forwarded to dst */
#define COSMO_NETCTL_FLOW_GUEST 3      /* FW: a guest-to-guest flow the FORWARD chain accepted */
#define COSMO_NETCTL_FLOW_HOST  4      /* FW: a flow the host opened (guest_addr HOST_ADDR) */

#define COSMO_NETCTL_FLOW_ESTABLISHED 0x01   /* TCP: both directions have been seen */

/* Cumulative since boot and machine-wide -- the kernel does not count per
 * guest. Each has one cause. They are read beside the flows, not in the
 * same lock hold, so they may lag the flows by the refusals and creations
 * in flight (at most one per CPU): approximate to the instant, exact over
 * time. */
struct cosmo_netctl_flow_counters {
    uint64_t masq_new;
    uint64_t masq_drop_share;      /* refused: the guest's NAT share was full */
    uint64_t masq_drop_table;      /* refused: the whole NAT table was full */
    uint64_t masq_drop_noport;     /* refused: no NAT identifier was free */
    uint64_t dnat_drop_share;      /* refused: the target guest's NAT share was full */
    uint64_t dnat_drop_table;      /* refused: the whole NAT table was full */
    uint64_t dnat_drop_ambiguous;  /* refused: the reply's reverse key was already in use */
    uint64_t nat_expired;
    uint64_t fw_new;               /* guest-to-guest flows recorded */
    uint64_t fw_drop_share;        /* refused: the initiator's flow share was full */
    uint64_t fw_drop_table;        /* refused: no free slot in the flow table */
    uint64_t fw_host_new;          /* flows the host opened, recorded */
    uint64_t fw_host_unrecorded;   /* not a refusal: sent, but not recorded, so the reply takes the rules */
    uint64_t fw_expired;
};

struct cosmo_netctl_flow_list {
    uint16_t version;              /* COSMO_NETCTL_VERSION */
    uint16_t count;                /* struct cosmo_netctl_flow following */
    uint16_t nat_quota;            /* one guest's NAT share */
    uint16_t fw_quota;             /* one guest's flow share */
    uint16_t fw_host_quota;        /* the host's flow share */
    uint16_t reserved[3];
    struct cosmo_netctl_flow_counters counters;
};

struct cosmo_netctl_flow {
    uint8_t  table;                /* COSMO_NETCTL_FLOW_NAT / _FW */
    uint8_t  kind;                 /* COSMO_NETCTL_FLOW_MASQ / DNAT / GUEST / HOST */
    uint8_t  proto;                /* COSMO_NETCTL_PROTO_ICMP / TCP / UDP */
    uint8_t  flags;                /* COSMO_NETCTL_FLOW_ESTABLISHED */
    uint32_t guest_addr;           /* network byte order: whose share it counts against; HOST_ADDR = the host */
    uint32_t src_addr, dst_addr;   /* network byte order: the initiator and the responder */
    uint16_t src_port, dst_port;   /* host byte order; ICMP: the echo id, 0 */
    uint32_t nat_addr;             /* network byte order: MASQ the uplink identity, DNAT the dialled
                                    * host address; FW 0 */
    uint16_t nat_port;             /* host byte order: MASQ the lent port or echo id, DNAT the dialled port */
    uint16_t reserved;
    uint32_t expires_ms;           /* from the read, rounded up: never 0 for a listed flow */
};

/*
 * The lease section (version 7), after the flow section: a header, then
 * `count` leases, one per interface whose address the kernel's DHCP client
 * manages (an interface configured from fw_cfg has none). Each is copied
 * under its client's lock at one `now`. A client that holds no lease
 * (state INIT/SELECTING/REQUESTING) is listed with zero addresses and times.
 */
#define COSMO_NETCTL_DHCP_INIT       1
#define COSMO_NETCTL_DHCP_SELECTING  2
#define COSMO_NETCTL_DHCP_REQUESTING 3
#define COSMO_NETCTL_DHCP_BOUND      4   /* the lease is held, before T1 */
#define COSMO_NETCTL_DHCP_RENEWING   5   /* past T1: renewing with the server that granted it */
#define COSMO_NETCTL_DHCP_REBINDING  6   /* past T2: asking any server */

#define COSMO_NETCTL_MAX_LEASES 4         /* DHCP clients */
#define COSMO_NETCTL_MAX_DNS    3         /* DNS servers kept per lease */

struct cosmo_netctl_lease_list {
    uint16_t version;              /* COSMO_NETCTL_VERSION */
    uint16_t count;                /* struct cosmo_netctl_lease following */
    uint32_t reserved;
};

struct cosmo_netctl_lease {
    char     ifname[8];            /* NUL-terminated */
    uint8_t  state;                /* COSMO_NETCTL_DHCP_* */
    uint8_t  dns_count;            /* valid entries of dns[] */
    uint16_t reserved;
    uint32_t addr, mask, router;   /* network byte order; 0 without a lease */
    uint32_t server;               /* network byte order: the server that granted it */
    uint32_t dns[COSMO_NETCTL_MAX_DNS];   /* network byte order */
    uint32_t lease_s, t1_s, t2_s;  /* as granted, seconds; 0xffffffff lease: infinite */
    uint32_t remaining_ms;         /* to the lease's end from the read (0xffffffff: infinite;
                                    * a finite time over 0xfffffffe ms reads 0xfffffffe,
                                    * COSMO_NETCTL_LEASE_MS_MAX; likewise t1_ms, t2_ms) */
    uint32_t t1_ms, t2_ms;         /* to T1 and T2 from the read, 0 once passed */
    char     hostname[64];         /* version 8: the lease's option 12, NUL-terminated, or empty; only
                                    * letters, digits, '-' and '.' (the kernel drops any other name) */
};

/*
 * The interface section (version 8), after the lease section: a header,
 * then `count` interfaces in registration order (lo first), each copied
 * under the registry's lock (one instant for the list; the counters are
 * each a whole value, not one instant together).
 */
#define COSMO_NETCTL_IF_UP       0x1
#define COSMO_NETCTL_IF_LOOPBACK 0x2
#define COSMO_NETCTL_IF_NOARP    0x4
#define COSMO_NETCTL_IF_TAP      0x8   /* a guest's tap: never the default route */

#define COSMO_NETCTL_MAX_IFS     32    /* interfaces listed: the registry's capacity, so all of them */

struct cosmo_netctl_if_list {
    uint16_t version;              /* COSMO_NETCTL_VERSION */
    uint16_t count;                /* struct cosmo_netctl_if following */
    uint32_t reserved;
};

struct cosmo_netctl_if {
    char     name[8];              /* NUL-terminated */
    uint32_t index;                /* from 1, lo's */
    uint32_t flags;                /* COSMO_NETCTL_IF_* */
    uint32_t mtu;
    uint8_t  mac[6];               /* zero on lo */
    uint16_t reserved;
    uint32_t addr, mask;           /* network byte order; 0 without an IPv4 address */
    uint8_t  ip6_ll[16];           /* the IPv6 link-local address; lo's is ::1 */
    uint32_t reserved2;
    uint64_t rx_packets, rx_bytes, rx_dropped, rx_errors;
    uint64_t tx_packets, tx_bytes, tx_dropped, tx_errors;
};

/*
 * The route section (version 8), after the interface section: IPv4's
 * routes as the stack decides them -- there is no routing table: the
 * default through the first up, addressed interface that is not lo or a
 * tap, to its gateway, then each up, addressed interface's subnet on the
 * link (docs/kernel-services/network/design.md, "Inspecting the stack").
 */
#define COSMO_NETCTL_RT_GATEWAY 0x2    /* through `gateway` (Linux's RTF_GATEWAY) */
#define COSMO_NETCTL_MAX_ROUTES 33     /* a default and one subnet per interface */

struct cosmo_netctl_route_list {
    uint16_t version;              /* COSMO_NETCTL_VERSION */
    uint16_t count;                /* struct cosmo_netctl_route following */
    uint32_t reserved;
};

struct cosmo_netctl_route {
    char     ifname[8];            /* NUL-terminated */
    uint32_t dst, mask, gateway;   /* network byte order; the default is 0/0 */
    uint32_t src;                  /* network byte order: the source the stack picks on it */
    uint32_t flags;                /* COSMO_NETCTL_RT_* */
    uint32_t reserved;
};

/*
 * The neighbour section (version 8), after the route section: the ARP
 * table, copied under its lock. IPv6's neighbours are not listed.
 */
#define COSMO_NETCTL_NEIGH_INCOMPLETE 1   /* resolution in progress; mac zero */
#define COSMO_NETCTL_NEIGH_REACHABLE  2
#define COSMO_NETCTL_MAX_NEIGH        64  /* ARP_TABLE_SIZE */

struct cosmo_netctl_neigh_list {
    uint16_t version;              /* COSMO_NETCTL_VERSION */
    uint16_t count;                /* struct cosmo_netctl_neigh following */
    uint32_t reserved;
};

struct cosmo_netctl_neigh {
    char     ifname[8];            /* NUL-terminated */
    uint32_t addr;                 /* network byte order */
    uint8_t  mac[6];
    uint8_t  state;                /* COSMO_NETCTL_NEIGH_* */
    uint8_t  reserved;
};

/* The snapshot's bounds, so a reader can size its buffer for every valid
 * configuration rather than guess (the kernel refuses a short buffer with
 * -EMSGSIZE and never returns a partial snapshot). These mirror the kernel's
 * table limits and are checked against them at build time; the host object
 * is one more policy record with its own rule list. */
#define COSMO_NETCTL_MAX_FORWARDS        16   /* port-forward rules */
#define COSMO_NETCTL_MAX_GUESTS          8    /* concurrent guests */
#define COSMO_NETCTL_MAX_RULES_PER_GUEST 32   /* firewall rules per guest (and for the host) */
#define COSMO_NETCTL_MAX_POLICIES        (COSMO_NETCTL_MAX_GUESTS + 1)   /* the guests and the host */
#define COSMO_NETCTL_MAX_NAT_FLOWS       256  /* NAT's conntrack table */
#define COSMO_NETCTL_MAX_FW_FLOWS        320  /* the firewall's flow table: the guests' 256 and the host's 64 */
#define COSMO_NETCTL_SNAPSHOT_MAX \
    (sizeof(struct cosmo_netctl_list) + COSMO_NETCTL_MAX_FORWARDS * sizeof(struct cosmo_netctl_rule) + \
     sizeof(struct cosmo_netctl_filter_list) + COSMO_NETCTL_MAX_POLICIES * sizeof(struct cosmo_netctl_filter_guest) + \
     COSMO_NETCTL_MAX_POLICIES * COSMO_NETCTL_MAX_RULES_PER_GUEST * sizeof(struct cosmo_netctl_filter_rule) + \
     sizeof(struct cosmo_netctl_flow_list) + \
     (COSMO_NETCTL_MAX_NAT_FLOWS + COSMO_NETCTL_MAX_FW_FLOWS) * sizeof(struct cosmo_netctl_flow) + \
     sizeof(struct cosmo_netctl_lease_list) + COSMO_NETCTL_MAX_LEASES * sizeof(struct cosmo_netctl_lease) + \
     sizeof(struct cosmo_netctl_if_list) + COSMO_NETCTL_MAX_IFS * sizeof(struct cosmo_netctl_if) + \
     sizeof(struct cosmo_netctl_route_list) + COSMO_NETCTL_MAX_ROUTES * sizeof(struct cosmo_netctl_route) + \
     sizeof(struct cosmo_netctl_neigh_list) + COSMO_NETCTL_MAX_NEIGH * sizeof(struct cosmo_netctl_neigh))

#endif /* UAPI_COSMO_NETCTL_H */
