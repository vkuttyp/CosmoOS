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

/* Version 2 adds the forwarding firewall (docs/audit/next-subsystem-firewall.md):
 * the FILTER_* commands below and a filter section appended to the read
 * snapshot after the port-forward list. */
#define COSMO_NETCTL_VERSION 2

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

/* Filter directions: which way a forwarded datagram is leaving. */
#define COSMO_NETCTL_DIR_ANY       0   /* a rule matching either direction */
#define COSMO_NETCTL_DIR_TO_UPLINK 1   /* egress is not a guest tap (the world) */
#define COSMO_NETCTL_DIR_TO_GUEST  2   /* egress is another guest's tap */

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
    uint32_t guest_addr;   /* network byte order: whose policy this is */
    uint8_t  direction;    /* COSMO_NETCTL_DIR_* (POLICY: TO_UPLINK or TO_GUEST only) */
    uint8_t  proto;        /* COSMO_NETCTL_PROTO_* (0 = any); ignored by POLICY */
    uint8_t  dst_prefix;   /* 0..32 bits of dst_addr that must match; ignored by POLICY */
    uint8_t  verdict;      /* COSMO_NETCTL_VERDICT_* */
    uint32_t dst_addr;     /* network byte order; ignored by POLICY */
    uint16_t dst_port;     /* host byte order, 0 = any; ignored by POLICY and for ICMP */
    uint16_t at_index;     /* ADD: insert position (>= count appends); else 0 */
};

/* The read snapshot, version 2: the port-forward list (struct
 * cosmo_netctl_list + its rules, unchanged) followed by this filter section --
 * a header, `guest_count` per-guest policy records, then `rule_count` rules
 * in evaluation order, each carrying its guest and its current index. A
 * reader that stops after the port-forward rules is unaffected. */
struct cosmo_netctl_filter_list {
    uint16_t version;      /* COSMO_NETCTL_VERSION */
    uint16_t rule_count;
    uint16_t guest_count;
    uint16_t reserved;
};

struct cosmo_netctl_filter_guest {
    uint32_t guest_addr;        /* network byte order */
    uint8_t  policy_to_uplink;  /* COSMO_NETCTL_VERDICT_* */
    uint8_t  policy_to_guest;
    uint16_t reserved;
};

struct cosmo_netctl_filter_rule {
    uint32_t guest_addr;   /* network byte order */
    uint8_t  direction;
    uint8_t  proto;
    uint8_t  dst_prefix;
    uint8_t  verdict;
    uint32_t dst_addr;     /* network byte order */
    uint16_t dst_port;     /* host byte order */
    uint16_t index;        /* position in the guest's list (display) */
};

#endif /* UAPI_COSMO_NETCTL_H */
