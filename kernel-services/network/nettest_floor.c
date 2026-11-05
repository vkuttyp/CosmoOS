/*
 * nettest_floor.c - What the network self-tests' CeeSafe ports cannot hold
 * (roadmap 2.0 SA6). The tests are the nettest_*.cee files and their release
 * framework nettest_frame.cee, which skip where a build lacks their seams;
 * this file keeps the asserts of every port's copied constants and rooms
 * against the headers, as fifo_floor.c holds fifo.cee's.
 */

#include <kernel/compiler.h>
#include <kernel/errno.h>
#include <kernel/completion.h>
#include <kernel/faultinject.h>
#include <kernel/fwcfg.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/fw.h>
#include <kernel/net/nat.h>
#include <kernel/net/raw.h>
#include <kernel/net/tapsvc.h>
#include <kernel/net/dhcp.h>
#include <uapi/cosmo/netctl.h>
#include <kernel/vfs.h>
#include <kernel/net/tap.h>
#include <kernel/net/tcp.h>
#include <kernel/random.h>
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

#include "nettest.h"

/* The release framework (nettest_frame.cee): the values it writes as its
 * own constants, and the census's interface list as it reads it. */
_Static_assert(ENOMEM == 12 && COSMO_AF_INET == 2 && COSMO_SOCK_STREAM == 1 && COSMO_SHUT_RD == 0 &&
                   SCHED_PRIO_DEFAULT == 32 && sizeof(((struct nettest_census *)0)->netifs) == 512,
               "nettest_frame.cee's errno.h, syscall.h and thread.h values, and the census's list");

/* net-mbuf, net-mbufq-double, net-cksum and net-arp are CeeSafe
 * (nettest_base.cee); the values it writes as its own constants: */
_Static_assert(M_PKTHDR == 1 && M_EXT == 2 && M_QUEUED == (1u << 6) && NET_HEADROOM == 128 && EINPROGRESS == 115 &&
               NET_HEADROOM >= 12 + 14 + 40 + 60, "nettest_base.cee's mbuf.h and errno.h values");

/* --- UDP and TCP over loopback ------------------------------------------
 *
 * net-lo-udp, net-lo-tcp, net-tcp-delack, net-tcp-nagle-peer, net-tcp-mss
 * and net-lo-tcp-loss are CeeSafe (nettest_lo.cee), with the bulk
 * transfer they share with net-rx-dup and net-tcp-reorder
 * (nettest_tcp_transfer). Their servers' state, struct tcp_server, is in
 * nettest.h, and the release framework they call is nettest_frame.cee's.
 * The values the batch writes as its own constants, and struct netaddr's
 * layout as its NettestAddr has it: */
_Static_assert(EIO == 5 && EPIPE == 32 && EINVAL == 22 && EMSGSIZE == 90 && EADDRINUSE == 98 && EISCONN == 106 &&
               ECONNREFUSED == 111, "nettest_lo.cee's errno.h values");
_Static_assert(COSMO_AF_INET == 2 && COSMO_AF_INET6 == 10 && COSMO_SOCK_STREAM == 1 && COSMO_SOCK_DGRAM == 2 &&
               COSMO_SHUT_RD == 0 && COSMO_SHUT_WR == 1, "nettest_lo.cee's socket values");
_Static_assert(TCP_SNDBUF == 65536 && TCP_MSS_V4 == 1460 && TCP_MSS_V6 == 1440 && TCP_MSS_LO == 16384 &&
               TCP_DELACK_NS == 40ull * 1000000 && NET_EPHEMERAL_LO == 49152 && ETH_P_IP == 0x0800 &&
               IPPROTO_TCP == 6, "nettest_lo.cee's tcp.h, udp.h, ether.h and ip.h values");
_Static_assert(sizeof(struct netaddr) == 20 && __builtin_offsetof(struct netaddr, port) == 2 &&
               __builtin_offsetof(struct netaddr, v4) == 4 && __builtin_offsetof(struct netaddr, v6) == 4 &&
               _Alignof(struct netaddr) == 4, "nettest_lo.cee's NettestAddr is struct netaddr");
_Static_assert(sizeof(struct tcp_server) <= 512 && _Alignof(struct tcp_server) <= 8,
               "nettest_lo.cee's ServerRoom holds a struct tcp_server");

/* --- the pcb and its timers, and a duplicated received frame -----------------
 *
 * tcp-pcb-timer-free and net-rx-dup are CeeSafe (nettest_hooks.cee), with
 * their skips where a build lacks the hooks; the pinning wrapper is
 * nettest_frame.cee's.
 */

/* The values nettest_hooks.cee writes as its own constants, and its room: */
_Static_assert(SCHED_PRIO_DEFAULT == 32 && TICK_NS == 4000000 && FI_NET_RX_DUP == 12 && COSMO_AF_INET == 2 &&
                   ETH_HLEN == 14 && sizeof(cpumask_t) == 8,
               "nettest_hooks.cee's thread.h, timer.h, faultinject.h and ether.h values");
_Static_assert(sizeof(struct fi_stats) <= 512 && sizeof(struct arp_stats) <= 512, "nettest_hooks.cee's rooms");

/* --- the host harness's guest side: CeeSafe (nettest_harness.cee) ------------
 *
 * net-harness, with its echo services and the back-connection's retried
 * exchange; its NETTEST: lines are kprintf's, as they were. The values it
 * writes as its own constants: */
_Static_assert(COSMO_AF_INET == 2 && COSMO_SOCK_STREAM == 1 && COSMO_SOCK_DGRAM == 2 && COSMO_SHUT_RD == 0 &&
                   COSMO_SHUT_RDWR == 2 && sizeof(struct tcp_stats) <= 1024,
               "nettest_harness.cee's syscall.h values, and its room");

/* --- the interface's lifetime against the neighbour tables -------------- *
 *
 * net-arp-retry-unregister, net-nd-retry-unregister, net-arp-flush-counts,
 * net-nd-flush-counts and net-netif-lifetime are CeeSafe
 * (nettest_lifetime.cee), with fake interfaces of their own. The two retry
 * tests need the debug build's park hooks and skip without them. The values
 * the port writes as its own constants, and its rooms:
 */
_Static_assert(EINVAL == 22 && EEXIST == 17 && ENODEV == 19 && EINPROGRESS == 115 && NETIF_UP == 1 &&
                   NETIF_LOOPBACK == 2 && NETIF_NOARP == 4 && NETIF_GONE == 8 && NETIF_NODEFAULT == 0x10 &&
                   SCHED_PRIO_DEFAULT == 32 && NS_PER_SEC == 1000000000ull,
               "nettest_lifetime.cee's errno.h, netif.h, thread.h and time values");
_Static_assert(__builtin_offsetof(struct netif, obj) == 0 && sizeof(struct arp_stats) <= 512 && sizeof(struct ip_stats) <= 512,
               "nettest_lifetime.cee's object and rooms");

/* --- a neighbour entry is (interface, address) ---------------------------- *
 *
 * net-arp-per-interface, net-nd-per-interface and net-neigh-down-race are
 * CeeSafe (nettest_neigh.cee), with fake interfaces of their own. The down
 * race needs the debug build's park hooks and skips without them. The values
 * the port writes as its own constants, its rooms and the IPv6 header it
 * builds:
 */
_Static_assert(EINPROGRESS == 115 && ENETUNREACH == 101 && ENOMEM == 12 && NETIF_UP == 1 && NETIF_NODEFAULT == 0x10 &&
                   ETH_HLEN == 14 && ETH_P_IP == 0x0800 && ETH_P_ARP == 0x0806 && ETH_P_IPV6 == 0x86DD &&
                   ICMPV6_NS == 135 && ICMPV6_NA == 136 && SCHED_PRIO_DEFAULT == 32 && NS_PER_SEC == 1000000000ull,
               "nettest_neigh.cee's errno.h, netif.h, ether.h, ip.h, thread.h and time values");
_Static_assert(sizeof(struct ipv6_hdr) == 40 && __builtin_offsetof(struct ipv6_hdr, src) == 8 &&
                   __builtin_offsetof(struct ipv6_hdr, dst) == 24 && sizeof(struct in6_addr) == 16 &&
                   sizeof(struct arp_stats) <= 512 && sizeof(struct ip_stats) <= 512,
               "nettest_neigh.cee's IPv6 header and rooms");

/* --- accept against its races, the census against a wake reference -------
 *
 * net-accept-race, net-census-wake-ref and net-accept-order are CeeSafe
 * (nettest_accept.cee), with the listen-backlog step net-lo-tcp shares and
 * the wait they used. The latter two need the debug build's seams and skip
 * without them. The values the port writes as its own constants:
 */
_Static_assert(EINVAL == 22 && COSMO_AF_INET == 2 && COSMO_SOCK_STREAM == 1 && COSMO_IO_READABLE == 1 &&
                   NET_EPHEMERAL_LO == 49152 && SCHED_PRIO_DEFAULT == 32,
               "nettest_accept.cee's errno.h, syscall.h, inet.h and thread.h values");

/* --- milestone 8: hardening -------------------------------------------------------- *
 *
 * net-tcp-syncache, net-tcp-rfc5961, net-tcp-reorder, net-tcp-keepalive,
 * net-icmp-limit, net-raw-icmp and net-nonblock are CeeSafe
 * (nettest_tcp.cee), with their injected segment, filters, holding server
 * and predicates; nettest_tcp.cee and nettest_accept.cee each have
 * nettest.c's old wait_until under its contract. The values it writes as
 * its own constants: */
_Static_assert(EAGAIN == 11 && EDESTADDRREQ == 89 && EPROTONOSUPPORT == 93 && EOPNOTSUPP == 95 && ECONNRESET == 104 &&
               EISCONN == 106 && ETIMEDOUT == 110 && EALREADY == 114 && EINPROGRESS == 115,
               "nettest_tcp.cee's errno.h values");
_Static_assert(COSMO_IO_READABLE == 1 && COSMO_IO_WRITABLE == 2 && COSMO_IO_HANGUP == 4 && TH_SYN == 0x02 &&
               TH_RST == 0x04 && TH_ACK == 0x10 && TCP_CLOSED == 0 && TCP_ESTABLISHED == 4 &&
               TCP_SYNCACHE_SIZE == 64 && sizeof(struct tcp_hdr) == 20 && sizeof(struct icmp_hdr) == 8,
               "nettest_tcp.cee's syscall.h and tcp.h values");
_Static_assert(IPPROTO_ICMP == 1 && IPPROTO_UDP == 17 && IP_DEFAULT_TTL == 64 && ICMP_ECHO_REPLY == 0 &&
               ICMP_ECHO == 8 && ICMP_DEST_UNREACH == 3 && ICMP_UNREACH_NEEDFRAG == 4 && ICMP_RATE_PER_SEC == 100 &&
               RAW_RXQ_MAX == 64 && PIPE_SIZE == 16384 && SCHED_PRIO_DEFAULT == 32,
               "nettest_tcp.cee's ip.h, raw.h, pipe.h and thread.h values");

/* --- receive steering and checksum offload (unit 11) ------------------------
 *
 * net-steer and net-csum-offload are CeeSafe (nettest_nif.cee). The
 * values it writes as its own constants: */
_Static_assert(NETIF_LOOPBACK == 2 && NETIF_CAP_TXCSUM == 1 && NETIF_CAP_RXCSUM == 2 && M_CSUM_OK == 16 &&
               NET_CSUM_TCP == 1 && NET_CSUM_TX == 3 && CONFIG_MAX_CPUS == 64 && sizeof(((struct netif *)0)->name) == 8,
               "nettest_nif.cee's netif.h, mbuf.h and percpu.h values");

/* --- the loopback benchmark: CeeSafe (nettest_bench.cee) ----------------------
 *
 * net-bench. The values it writes as its own constants, and its rooms: */
_Static_assert(COSMO_AF_INET == 2 && COSMO_SOCK_STREAM == 1 && COSMO_SOCK_DGRAM == 2 && COSMO_SHUT_RD == 0 &&
                   COSMO_SHUT_WR == 1 && CONFIG_MAX_CPUS == 64 && SCHED_PRIO_DEFAULT == 32 &&
                   sizeof(struct tcp_stats) <= 1024 && sizeof(struct net_cpu_stats) <= 128,
               "nettest_bench.cee's syscall.h, percpu.h and thread.h values, and its rooms");

/* --- the NIC-path benchmark: CeeSafe (nettest_nicbench.cee) -----------------
 *
 * net-nicbench, with its own copy of net-second-nic's search for another
 * interface. The values it writes as its own constants, and its rooms: */
_Static_assert(COSMO_AF_INET == 2 && COSMO_SOCK_DGRAM == 2 && ENOBUFS == 105 && ETH_HLEN == 14 && ETH_P_IP == 0x0800 &&
                   ETH_P_ARP == 0x0806 && NETIF_LOOPBACK == 2 && sizeof(struct net_cpu_stats) <= 128 &&
                   sizeof(struct netif_tx_probe) <= 64 && sizeof(struct arp_stats) <= 256,
               "nettest_nicbench.cee's syscall.h, errno.h, ether.h and netif.h values, and its rooms");

/* --- the socket's pending error (invariant N21), the receive hook's grace ----
 *
 * net-sockerr-udp, net-sockerr-spoof, net-sockerr-accept,
 * net-sockerr-locking and net-rxhook-grace are CeeSafe
 * (nettest_sockerr.cee). The values it writes as its own constants: */
_Static_assert(EAGAIN == 11 && ENETUNREACH == 101 && ECONNABORTED == 103 && ECONNREFUSED == 111 &&
               EHOSTUNREACH == 113 && COSMO_IO_ERROR == 8, "nettest_sockerr.cee's errno.h and syscall.h values");
_Static_assert(IPPROTO_ICMP == 1 && IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && ICMP_DEST_UNREACH == 3 &&
               ICMP_UNREACH_PORT == 3 && ICMP_UNREACH_HOST == 1 && ETH_P_IP == 0x0800 && sizeof(struct ipv4_hdr) == 20,
               "nettest_sockerr.cee's ip.h and ether.h values");

/* --- an interface against the receive worker ------------------------------
 *
 * netif-remove-worker and netif-rxq-count are CeeSafe (nettest_worker.cee),
 * with fake interfaces of their own. Both need the debug build's worker
 * hooks; without them they pass silently, as the C did. The values the port
 * writes as its own constants:
 */
_Static_assert(NETIF_UP == 1 && NETIF_NODEFAULT == 0x10 && ETH_HLEN == 14 && SCHED_PRIO_DEFAULT == 32 &&
                   NS_PER_SEC == 1000000000ull,
               "nettest_worker.cee's netif.h, ether.h, thread.h and time values");

/* --- the tap tests' frames; tap, route, forwarding ----------------------
 *
 * selftest_tap, net-route and net-forward are CeeSafe (nettest_tap.cee),
 * with the frame builders every tap test uses (nettest_wrap,
 * nettest_mk_udp, nettest_mk_tcp, nettest_mk_icmp, nettest_l4cksum,
 * nettest_l4_ok, nettest_seed_arp, nettest_build_udp, nettest_recv_ip,
 * nettest_mk_dns; nettest.h), which the tests below still call. The
 * values it writes as its own constants: */
_Static_assert(ETH_HLEN == 14 && ICMP_TIME_EXCEEDED == 11 && ICMP_TIMXCEED_INTRANS == 0 && TAP_TXQ_MAX == 64 &&
               DHCP_DISCOVER == 1 && DHCP_OFFER == 2 && DHCP_REQUEST == 3 && DHCP_ACK == 5 && DHCP_NAK == 6 &&
               INADDR_BROADCAST_N == 0xffffffffu && NS_PER_SEC == 1000000000ull && sizeof(struct udp_hdr) == 8 &&
               sizeof(struct eth_hdr) == 14, "nettest_tap.cee's ether.h, ip.h, tap.h and dhcp.h values");

/* --- net-nat and net-second-nic: CeeSafe (nettest_nat.cee) -------------
 *
 * The values it writes as its own constants: */
_Static_assert(IPPROTO_ICMP == 1 && IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && ICMP_ECHO_REPLY == 0 &&
                   ICMP_DEST_UNREACH == 3 && ICMP_UNREACH_PORT == 3 && ICMP_ECHO == 8 && TH_SYN == 0x02 &&
                   TH_ACK == 0x10 && ETH_HLEN == 14 && sizeof(struct udp_hdr) == 8 && sizeof(struct tcp_hdr) == 20 &&
                   sizeof(struct icmp_hdr) == 8 && sizeof(struct ipv4_hdr) == 20,
               "nettest_nat.cee's ip.h, tcp.h and ether.h values");
_Static_assert(NAT_PORT_MIN == 40000 && NAT_PORT_MAX == 49151 && NAT_TABLE_SIZE == 256 && NAT_QUOTA_PER_GUEST == 32 &&
                   NAT_TIMEOUT_UDP_NS == 30000000000ull && NETIF_LOOPBACK == 2 && EINPROGRESS == 115 &&
                   sizeof(struct nat_stats) <= 512 && sizeof(struct ip_stats) <= 512,
               "nettest_nat.cee's nat.h, netif.h and errno.h values, and its rooms");

/* --- tap-filter, net-dhcp, net-dns: CeeSafe (nettest_tap.cee) ------------ */

/* --- inbound port forwarding, many guests, the forwarding firewall ----------
 *
 * net-pf-clear, net-dnat, net-multiguest and net-firewall are CeeSafe
 * (nettest_fwd.cee), with the firewall tests' guest helpers
 * (nettest_fwt_send, nettest_fwt_recv, nettest_fwt_mk_icmp; nettest.h) the
 * chain tests below call. The values it writes as its own constants: */
_Static_assert(ENOENT == 2 && EEXIST == 17 && ENOSPC == 28 && ENOTSUP == 95 && NAT_TABLE_SIZE == 256 &&
               NAT_PF_MAX == 16 && NAT_QUOTA_PER_GUEST == 32 && NAT_TIMEOUT_TCP_NS == 30ull * 1000000000ull &&
               NAT_KIND_DNAT == 1, "nettest_fwd.cee's errno.h and nat.h values");
_Static_assert(FW_DROP == 0 && FW_ACCEPT == 1 && FW_DIR_TO_GUEST == 2 && FW_DIR_TO_HOST == 3 &&
               FW_ICMP_TYPE_ANY == 0xffff && FW_MAX_GUESTS == 8 && FW_RULES_PER_GUEST == 32 &&
               NETIF_FORWARD == (1u << 5) && COSMO_O_RDWR == 0x0002 && COSMO_O_NONBLOCK == 0x0800,
               "nettest_fwd.cee's fw.h, netif.h and syscall.h values");
_Static_assert(COSMO_NETCTL_VERSION == 8 && COSMO_NETCTL_FILTER_ADD == 3 && COSMO_NETCTL_FILTER_DEL == 4 &&
               COSMO_NETCTL_FILTER_POLICY == 5 && COSMO_NETCTL_PROTO_ICMP == 1 && COSMO_NETCTL_PROTO_TCP == 6 &&
               COSMO_NETCTL_PROTO_UDP == 17 && COSMO_NETCTL_DIR_TO_GUEST == 2 && COSMO_NETCTL_VERDICT_DROP == 0 &&
               COSMO_NETCTL_VERDICT_ACCEPT == 1 && COSMO_NETCTL_SNAPSHOT_MAX <= 65536,
               "nettest_fwd.cee's netctl.h values, and its snapshot room");

/* --- the runtime network control channel (/dev/net/tapctl) ----------------
 *
 * net-tapctl is CeeSafe (nettest_ctl.cee), with the walk of a snapshot's
 * sections (nettest_netctl_flows_off, nettest_netctl_snapshot_len;
 * nettest.h) the flow-listing tests below use. The values it writes as
 * its own constants: */
_Static_assert(EMSGSIZE == 90 && COSMO_NETCTL_FORWARD_ADD == 1 && COSMO_NETCTL_FORWARD_DEL == 2 &&
               COSMO_NETCTL_IF_UP == 0x1 && COSMO_NETCTL_IF_LOOPBACK == 0x2 && COSMO_NETCTL_FLOW_NAT == 1 &&
               COSMO_NETCTL_FLOW_DNAT == 2 && COSMO_NETCTL_FLOW_ESTABLISHED == 0x01 &&
               NAT_TIMEOUT_TCP_NS / 1000000u == 30000 && sizeof(((struct cosmo_netctl_if *)0)->name) == 8,
               "nettest_ctl.cee's errno.h, netctl.h and nat.h values");

/* --- the INPUT chain (docs/audit/next-subsystem-input-chain.md) ----------
 *
 * net-input is CeeSafe (nettest_input.cee), with the checksummed ICMP
 * messages the host chains test with (nettest_fwt_ones_sum,
 * nettest_fwt_mk_icmp_ck, nettest_fwt_mk_needfrag,
 * nettest_fwt_recv_echo_reply; nettest.h). The values it writes as its own
 * constants: */
_Static_assert(ICMP_DEST_UNREACH == 3 && FW_DIR_ANY == 0 && COSMO_NETCTL_DIR_ANY == 0 &&
               COSMO_NETCTL_DIR_TO_HOST == 3 && COSMO_NETCTL_ICMP_TYPE_ANY == 0xffff,
               "nettest_input.cee's ip.h, fw.h and netctl.h values");

/* --- the host chain (docs/audit/next-subsystem-host-input.md) ------------- */

/* The values nettest_hostin.cee writes as its own constants, and struct
 * hin_seg's payload where it writes it: */
_Static_assert(ENOENT == 2 && EAGAIN == 11 && EINVAL == 22 && ECONNRESET == 104 && ETH_HLEN == 14 &&
               IPPROTO_ICMP == 1 && IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && ICMP_ECHO_REPLY == 0 &&
               ICMP_DEST_UNREACH == 3 && ICMP_ECHO == 8 && TH_FIN == 0x01 && TH_SYN == 0x02 && TH_RST == 0x04 &&
               TH_PSH == 0x08 && TH_ACK == 0x10 && TCP_CHALLENGE_PER_SEC == 100,
               "nettest_hostin.cee's errno.h, ip.h and tcp.h values");
_Static_assert(FW_DROP == 0 && FW_ACCEPT == 1 && FW_DIR_ANY == 0 && FW_DIR_TO_HOST == 3 && FW_DIR_FROM_UPLINK == 4 &&
               FW_HOST_GUEST_IP == 0 && COSMO_NETCTL_VERSION == 8 && COSMO_NETCTL_FILTER_ADD == 3 &&
               COSMO_NETCTL_FILTER_DEL == 4 && COSMO_NETCTL_PROTO_TCP == 6 && COSMO_NETCTL_DIR_TO_HOST == 3 &&
               COSMO_NETCTL_DIR_FROM_UPLINK == 4 && COSMO_NETCTL_VERDICT_DROP == 0 &&
               COSMO_NETCTL_VERDICT_ACCEPT == 1 && COSMO_NETCTL_HOST_ADDR == 0 && COSMO_O_RDWR == 0x0002 &&
               COSMO_O_NONBLOCK == 0x0800 && COSMO_AF_INET == 2 && COSMO_SOCK_STREAM == 1 &&
               COSMO_SOCK_DGRAM == 2,
               "nettest_hostin.cee's fw.h, netctl.h and socket values");
_Static_assert(__builtin_offsetof(struct hin_seg, pay) == 28 && sizeof(struct hin_seg) <= 128 &&
                   _Alignof(struct hin_seg) <= 8,
               "nettest_hostin.cee writes hin_seg's payload at 28, in a 128-byte room");

/* --- the host's own state (docs/audit/next-subsystem-host-state.md) ------- */

/* net-zero-window-probe, net-tcp-sws, net-fin-acks-last-data and
 * net-hoststate are CeeSafe (nettest_hoststate.cee). */
/* The values nettest_hoststate.cee writes as its own constants (net-dns's
 * upstream answers with the 93.184.216.34 it checks): */
_Static_assert(EMSGSIZE == 90 && ICMP_UNREACH_PORT == 3 && ICMP_UNREACH_NEEDFRAG == 4 && FW_DIR_TO_GUEST == 2 &&
                   FW_FLOW_QUOTA_HOST == 64,
               "nettest_hoststate.cee's errno.h, ip.h and fw.h values");

/* --- the operator's flow listing (docs/audit/next-subsystem-net-flows.md) --- */

/*
 * net-flows-nat and net-flows-fw are CeeSafe (nettest_flows.cee), with the
 * read of the whole snapshot. The values it writes as its own constants, and
 * the room it reads the snapshot and the listings into:
 */
_Static_assert(IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && TH_SYN == 0x02 && TH_ACK == 0x10 && NAT_TABLE_SIZE == 256 &&
                   NAT_QUOTA_PER_GUEST == 32 && NAT_PORT_MIN == 40000 && NAT_PORT_MAX == 49151 &&
                   NAT_TIMEOUT_UDP_NS == 30000000000ull && NAT_KIND_MASQ == 0 && FW_ACCEPT == 1 &&
                   FW_DIR_TO_GUEST == 2 && FW_HOST_GUEST_IP == 0 && FW_FLOW_QUOTA_PER_GUEST == 32 &&
                   FW_FLOW_QUOTA_HOST == 64 && FW_FLOW_MAX == 320,
               "nettest_flows.cee's ip.h, tcp.h, nat.h and fw.h values");
_Static_assert(COSMO_NETCTL_VERSION == 8 && COSMO_NETCTL_FLOW_NAT == 1 && COSMO_NETCTL_FLOW_FW == 2 &&
                   COSMO_NETCTL_FLOW_MASQ == 1 && COSMO_NETCTL_FLOW_GUEST == 3 && COSMO_NETCTL_FLOW_HOST == 4 &&
                   COSMO_NETCTL_FLOW_ESTABLISHED == 0x01 && COSMO_NETCTL_PROTO_TCP == 6 &&
                   COSMO_NETCTL_PROTO_UDP == 17 && COSMO_NETCTL_HOST_ADDR == 0 && COSMO_O_RDWR == 0x0002 &&
                   COSMO_O_NONBLOCK == 0x0800 && COSMO_AF_INET == 2,
               "nettest_flows.cee's netctl.h and socket values");
_Static_assert(COSMO_NETCTL_SNAPSHOT_MAX <= 65536 && NAT_TABLE_SIZE * sizeof(struct nat_flow) <= 10240 &&
                   FW_FLOW_MAX * sizeof(struct fw_flow_info) <= 10240 &&
                   sizeof(struct cosmo_netctl_flow_list) <= 192 && sizeof(struct cosmo_netctl_flow) <= 192 &&
                   sizeof(struct nat_stats) <= 1024 && sizeof(struct fw_stats) <= 1024 && sizeof(struct fw_rule) <= 64,
               "nettest_flows.cee's rooms");

/* --- the OUTPUT chain (docs/audit/next-subsystem-output-chain.md) --------- */

/*
 * net-output and net-tcpverdict are CeeSafe (nettest_output.cee), with the
 * OUTPUT rule they build and the drain of a guest's tap file. The values it
 * writes as its own constants, and the rooms it builds and reads into:
 */
_Static_assert(EPERM == 1 && EAGAIN == 11 && EINVAL == 22 && EMSGSIZE == 90 && ENOTSUP == 95 && EINPROGRESS == 115 &&
                   ETH_HLEN == 14 && IPPROTO_ICMP == 1 && IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && ICMP_ECHO == 8 &&
                   TH_SYN == 0x02 && TH_PSH == 0x08 && TH_ACK == 0x10 && TCP_ESTABLISHED == 4,
               "nettest_output.cee's errno.h, ip.h and tcp.h values");
_Static_assert(FW_DROP == 0 && FW_ACCEPT == 1 && FW_DIR_TO_HOST == 3 && FW_DIR_FROM_UPLINK == 4 && FW_DIR_OUTPUT == 5 &&
                   FW_SCOPE_ANY == 0 && FW_SCOPE_WORLD == 1 && FW_SCOPE_GUEST == 2 && FW_HOST_GUEST_IP == 0,
               "nettest_output.cee's fw.h values");
_Static_assert(COSMO_NETCTL_VERSION == 8 && COSMO_NETCTL_FILTER_ADD == 3 && COSMO_NETCTL_FILTER_DEL == 4 &&
                   COSMO_NETCTL_PROTO_UDP == 17 && COSMO_NETCTL_DIR_OUTPUT == 5 && COSMO_NETCTL_VERDICT_DROP == 0 &&
                   COSMO_NETCTL_VERDICT_ACCEPT == 1 && COSMO_NETCTL_SCOPE_GUEST == 2 && COSMO_NETCTL_HOST_ADDR == 0 &&
                   COSMO_O_RDWR == 0x0002 && COSMO_O_NONBLOCK == 0x0800 && COSMO_AF_INET == 2 &&
                   COSMO_SOCK_STREAM == 1 && COSMO_SOCK_DGRAM == 2 && COSMO_IO_ERROR == 8,
               "nettest_output.cee's netctl.h, socket and poll values");
_Static_assert(sizeof(struct cosmo_netctl_filter_guest) == 12, "the v5 policy record is 12 bytes");
_Static_assert(sizeof(struct fw_rule) <= 64 && sizeof(struct cosmo_netctl_filter) <= 64 &&
                   sizeof(struct cosmo_netctl_list) <= 64 && sizeof(struct cosmo_netctl_filter_list) <= 64 &&
                   sizeof(struct cosmo_netctl_filter_guest) <= 64 && sizeof(struct cosmo_netctl_filter_rule) <= 64 &&
                   sizeof(struct fw_stats) <= 1024 && sizeof(struct ip_stats) <= 1024 &&
                   sizeof(struct tcp_stats) <= 1024 && COSMO_NETCTL_SNAPSHOT_MAX <= 65536,
               "nettest_output.cee's rooms");

/* --- tap-ready and net-dhcp-client --------------------------------------- *
 *
 * CeeSafe (nettest_tapdev.cee): the tap's file waited on, and the kernel's
 * DHCP client against a fixture interface. The latter needs the client's
 * test seams; without them, the C's skip stands. The values the port writes
 * as its own constants, and the rooms it reads into:
 */
_Static_assert(COSMO_O_RDWR == 0x0002 && COSMO_O_NONBLOCK == 0x0800 && COSMO_IO_READABLE == 1 &&
                   COSMO_IO_WRITABLE == 2 && COSMO_SIGKILL == 9 && ETH_HLEN == 14 && ETH_P_IP == 0x0800 &&
                   ETH_P_ARP == 0x0806 && IPPROTO_UDP == 17 && INADDR_BROADCAST_N == 0xffffffffu &&
                   NETIF_UP == 1 && NETIF_NODEFAULT == 0x10,
               "nettest_tapdev.cee's syscall.h, ether.h, ip.h and netif.h values");
#if CONFIG_SELFTEST
_Static_assert(BOOTP_REQUEST == 1 && BOOTP_REPLY == 2 && DHCP_HTYPE_ETHER == 1 && DHCP_DISCOVER == 1 &&
                   DHCP_OFFER == 2 && DHCP_REQUEST == 3 && DHCP_ACK == 5 && DHCP_NAK == 6 && OPT_SUBNET == 1 &&
                   OPT_ROUTER == 3 && OPT_DNS == 6 && OPT_REQIP == 50 && OPT_LEASE == 51 && OPT_MSGTYPE == 53 &&
                   OPT_SERVERID == 54 && OPT_END == 255 && DHCP_SPORT == 67 && DHCP_CPORT == 68 && BOOTP_LEN == 236 &&
                   DHCP_FIXED == 240 && BOOTP_OFF_OP == 0 && BOOTP_OFF_HTYPE == 1 && BOOTP_OFF_HLEN == 2 &&
                   BOOTP_OFF_XID == 4 && BOOTP_OFF_CIADDR == 12 && BOOTP_OFF_YIADDR == 16 && BOOTP_OFF_CHADDR == 28 &&
                   DHCP_MAGIC == 0x63825363u && DHCP_MAX_CLIENTS == 4 && DHCP_S_SELECTING == 2 &&
                   DHCP_S_REQUESTING == 3 && DHCP_S_BOUND == 4 && DHCP_S_RENEWING == 5 && DHCP_S_REBINDING == 6 &&
                   DHCP_TEST_RETX == 0 && DHCP_TEST_T1 == 1 && DHCP_TEST_T2 == 2 && DHCP_TEST_EXPIRE == 3,
               "nettest_tapdev.cee's dhcp.h values");
_Static_assert(DHCP_MAX_CLIENTS * sizeof(struct dhcp_lease_info) <= 1024 && sizeof(struct dhcp_stats) <= 256,
               "nettest_tapdev.cee's rooms");
#endif

/* --- three tests held at a seam ---------------------------------------------
 *
 * net-netif-flags, net-tap-release-order and net-syncookie-seeded are
 * CeeSafe (nettest_seam.cee). The first two need the debug build's park and
 * release seams, the third the self-test build's cookie seam; each skips
 * without them. tap.h's after-purge seam is tap.cee's. The values the port
 * writes as its own constants:
 */
_Static_assert(COSMO_O_RDWR == 0x0002 && COSMO_O_NONBLOCK == 0x0800 && IPPROTO_UDP == 17 && ETH_HLEN == 14 &&
                   sizeof(struct ipv4_hdr) == 20 && __builtin_offsetof(struct ipv4_hdr, src) == 12 &&
                   NAT_TABLE_SIZE == 256 && FW_FLOW_MAX == 320 && SCHED_PRIO_DEFAULT == 32,
               "nettest_seam.cee's syscall.h, ip.h, ether.h, nat.h, fw.h and thread.h values");
_Static_assert(sizeof(struct nat_stats) <= 512 && sizeof(struct ip_stats) <= 512 &&
                   NAT_TABLE_SIZE * sizeof(struct nat_flow) <= 8192 && FW_FLOW_MAX * sizeof(struct fw_flow_info) <= 12288,
               "nettest_seam.cee's rooms");
