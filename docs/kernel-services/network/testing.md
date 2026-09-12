# Networking: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| Target, loopback | The self-tests below, since unit 11 also `net-steer`, `net-rxhook-grace`, `net-csum-offload` and `net-bench`: `net-mbuf`, `net-cksum`, `net-arp`, `net-lo-udp`, `net-lo-tcp`, `net-lo-tcp-loss`, `net-tcp-mss` (the path MSS is decided outside the TCP lock: loopback and own addresses give `TCP_MSS_LO`, the gateway `TCP_MSS_V4`, and both ends of a loopback connection settle on `TCP_MSS_LO`), `net-netif-lifetime` (a synthetic interface: registry and lookup references, `netif_unregister` stops transmit and receive, the release runs once after the last put) and `net-accept-race` (64 accepts against a client that connects and drops at once; every child names its socket when accept returns) | `make test` |
| Target, real NIC | `net-harness`: echo services on `eth0` driven by the host through QEMU user-mode networking (`tests/boot/nettest.py`), plus the guest connecting back to the host | `make test` |
| User mode | `init --selftest` runs `net_selftest()` over loopback through system calls 23–31 (`usertest: sockets ok`) | `make test` |
| Boot markers | `module: loaded virtio_net 1.0`, `net: eth0 registered`, and in self-test builds `NETTEST: client ok` and `NETTEST: done ... quit=1` | every `make test`, release included for the first two |

The boot test's total is `SELFTEST: PASS (61 tests)` since Phase 9 (58
after Phase 8). The seven network tests sit after the filesystem tests
and before the tty, pipe and process tests
(which run `init --selftest`), so init's socket test runs on a stack the
kernel tests have already exercised.

## Self-tests (`kernel-services/network/nettest.c`)

**`net-mbuf`**: `m_getcl` gives `M_PKTHDR | M_EXT` with `NET_HEADROOM`
of leading space; `m_append` of 3000 bytes spans two clusters and
`m_copydata` reads back the whole range, a tail slice, and fails one
byte past the end; `m_prepend` of 14 uses the headroom, `m_prepend` of
100 adds a leading buffer and `m_adj(114)` removes both; `m_pullup(m,
2000)` makes 2000 bytes contiguous, `m_pullup(m_get(), 2049)` is
refused; `m_adj(-1000)` trims the tail; `m_ref` shares a cluster and
sees the same bytes; `m_copypacket` linearises 1500 bytes; a 2-entry
`mbufq` accepts two packets and frees the third; the alive counters
return to their starting values.

**`net-cksum`**: the RFC 1071 example (`0x220d` in network order); a
checksum stored at an odd offset verifies to zero when folded in two
parts; a 1000-byte chain split 333/100/567 across three buffers gives
the same checksum as the flat buffer, from offset 0 and from offset 7.

**`net-arp`**: an unknown address is not in the table; `arp_resolve`
for `10.0.2.99` returns `-EINPROGRESS`, sends one request and adds
one entry; `arp_age` advanced by four seconds times out the entry,
drops the pending packet and removes it; a forged unsolicited reply
handed to `arp_input` creates no entry and bumps `unsolicited`, while
a request addressed to us from `10.99.0.9` is answered (`replies_sent`)
and records the asker's MAC (the test entries are flushed afterwards);
then the real gateway is
resolved (waiting up to a second) and its MAC is logged, or a warning
is logged when QEMU did not answer (the check is on the request
counters, not on the reply, so the test does not depend on the host
network). Without an Ethernet interface only the table logic runs.

**`net-lo-udp`**: for IPv4 (`127.0.0.1`) and IPv6 (`::1`): bind a
server, bind again (`-EINVAL`), a second socket on the same port
(`-EADDRINUSE`), `getsockname`; a client sends 5 bytes and a 1400-byte
message; the server receives both with the client's ephemeral port as
sender, replies `pong`, and the client reads it; a 100-byte datagram
read into a 10-byte buffer is truncated; a destination of family 0 is
`-EINVAL`; an unbound socket cannot receive (`-EINVAL`); a datagram to
a closed port is sent and counted as `rx_no_port`; a uid-1000 socket
cannot bind port 80 (`-EPERM`); `rx_bad_cksum` does not move; the
socket count returns to its starting value.

**`net-lo-tcp`**: a server thread accepts one connection and echoes;
the client connects (`getpeername`/`getsockname`, a second `connect`
is `-EISCONN`), streams 1 MiB (IPv4) or 256 KiB (IPv6) of a pattern
in varying chunk sizes while reading the echo back and verifying every
byte, shuts down for writing, drains to EOF (`recvfrom` returns 0),
and `sendto` afterwards is `-EPIPE`; the server saw the same byte
count. The IPv6 client then keeps its socket for 2.5 s, past the 2 s
TIME_WAIT, and `getsockname`, `recvfrom` (0) and `sendto` (`-EPIPE`)
still behave, and a new socket can bind the client's former port:
the pcb is not freed under a live socket and no longer holds the port. Then: `connect` to a closed port is `-ECONNREFUSED` and
`rsts_in` grew by one; a listener with backlog 2 accepts one of two
queued connections, exchanges `hi`, and closing the listener resets
the other (`c2` sees an error on read or write); `conns_established`
grew by at least four and `bad_cksum` did not move; segments and
retransmissions are logged (`selftest: net-lo-tcp: N segments, 0
retransmits`).

**`net-lo-tcp-loss`**: installs the loopback filter, which drops every
seventh TCP segment that carries data, and repeats the 1 MiB IPv4
transfer; the transfer completes byte-exact, `g_dropped > 0` and
`retransmits` grew (log: `dropped N data segments, N
retransmissions`). Exercises RTO retransmission, fast retransmit and
the acknowledge-and-drop handling of out-of-order data.

**`net-harness`**: skips with a log line unless fw_cfg carries
`opt/cosmo/nettest`. Otherwise requires `eth0` with an IPv4 address,
parses `tcp=<port>`, binds a TCP listener (backlog 4) and a UDP socket
on port 7 of any address, starts echo threads for both, prints
`NETTEST: ready tcp=7 udp=7`, connects to `<gateway>:<port>` (the host
behind QEMU's `10.0.2.2`), sends `cosmo hello\n`, expects
`cosmo world\n` and prints `NETTEST: client ok` (or `client failed
(rc)`); then serves echo for up to 60 s until a TCP connection whose
first bytes are `QUIT` arrives, prints `NETTEST: done tcp_conns=N
udp_pkts=N quit=1`, closes everything and checks `client_ok` and the
quit flag. The watchdog is kicked during the waits.

**`net-tcp-syncache`**: a listener on port 6020 (backlog 4) behind a
loopback filter that drops resets addressed to it (this host would
otherwise answer every SYN-ACK to a spoofed port with a RST and clear
the cache); 300 raw SYNs from 300 source ports are injected through
`ipv4_output`; afterwards `syn_cached` is between 1 and 64,
`syn_cookies_sent` makes up the rest of 300, `conns_passive` has not
moved and the listener has nothing to accept; a real client connects,
is accepted and exchanges `hello`; a raw ACK matching nothing is
counted as `syn_bad_ack` and creates nothing.

**`net-tcp-rfc5961`**: a connection to a holding server; three raw
segments from the server's port to the client's are injected: a RST
1000 bytes into the window, a SYN 10 bytes in, an ACK 100 000 bytes
beyond `snd_max`. After each the client is still ESTABLISHED and
`snd_una` unchanged; `challenge_acks` grew by three, `rsts_in` by
none. A RST at `rcv_nxt` then ends the connection: `recvfrom` is
`-ECONNRESET`, `rsts_in` +1.

**`net-tcp-reorder`**: the loopback filter holds a copy of every fifth
data segment to port 6022, drops the original, lets the next data
segment through and re-injects the held copy before the one after that;
a 512 KiB transfer completes byte-exact, `g_reordered > 0` and
`ooo_queued` grew (log: `7 segments delayed, 7 queued out of order, 0
retransmissions`).

**`net-tcp-keepalive`**: with `tcp_set_keepalive(150 ms, 50 ms, 3)`
set before the connection exists, a client connects to a holding
server and a filter black-holes every segment of that port in both
directions; within 3 s the client's pcb is CLOSED, `recvfrom` is
`-ETIMEDOUT`, `timeouts` grew and at least three probes were sent. Then
with `tcp_set_fin_wait2(100 ms)` a client connects and closes while the
server holds its end: the orphaned FIN_WAIT_2 is reaped
(`fin_wait2_timeouts` +1) within 2 s. Both hooks are restored.

**`net-icmp-limit`**: 300 echo requests to `127.0.0.1` in a burst: all
counted as received, at most 100 replied, at least 200
`icmp_ratelimited` (unreachables are never sent for 127/8, so the echo
path carries the test). Then a connection to a holding server over `lo`
(`mss` 16384, `ipv4_path_mtu(127.0.0.1)` 65535) with the black-hole
filter keeping 2000 sent bytes in flight; a crafted ICMP type 3 code 4
with MTU 1500 quoting the client's header with a sequence number 5000
below `snd_una` is ignored by TCP (`pmtu_updates` unchanged, `mss`
16384) but records the destination's MTU (`ipv4_path_mtu` 1500); the
same message quoting `snd_una` lowers `mss` and `path_mss` to 1460
and `tcp_path_mss` for new connections to 1460; the filter is removed,
the retransmission completes, `ipv4_pmtu_flush` restores 16384.

**`net-nonblock`**: a non-blocking listener answers `accept` with
`-EAGAIN` and no readiness; a non-blocking client's `connect` returns 0
or `-EINPROGRESS` (`-EALREADY` or `-EISCONN` on a repeat), becomes
`WRITABLE`, then `-EISCONN`; the listener turns `READABLE` and accepts;
`recvfrom` on the empty client is `-EAGAIN` and not `READABLE` until
the server sends; non-blocking sends fill the rings until readiness
reports no room — the test sends while `WRITABLE` is set and stops the
moment it clears, so the observation is the loop's exit condition and
not a sample taken after a pause; an ACK processed on another worker
frees space again at any time, which is what made the older form flaky
under `QEMU_SMP=1`, and `WRITABLE` is a low-water predicate, so a
refused 4 KiB send while it is still set means nothing — the server
then drains exactly that many bytes and `WRITABLE` returns; closing the server end makes the client
`HANGUP` and reads 0. A non-blocking datagram socket is `-EAGAIN` and
`WRITABLE` only, then `READABLE` after a datagram to itself. Pipe ends
through `kobject_io_of`, `kobject_set_nonblock` (-1 asks, returns the
previous mode) and `kobject_ready`: an empty read is `-EAGAIN`, writes
fill exactly `PIPE_SIZE` then `-EAGAIN` with `WRITABLE` clear, the
read end is `READABLE`, and after the write end is released `HANGUP`
with the rest of the bytes then 0; the console is `WRITABLE` and
refuses `set_nonblock` with `-EOPNOTSUPP`.

`tcp_transfer` ends by waiting, bounded, until the port it used binds
again: the server's child leaves `LAST_ACK` only when the `netrx` worker
processes the client's final ACK, and the test thread (higher priority)
can run ahead of the worker now that `quiesce_read_unlock` at the end of
a transmit is a prompt preemption point.

## Forwarding and NAT

These create their own taps on private subnets (`10.9.x`, `10.77.x`) chosen
to collide with neither the QEMU user-net NIC (`10.0.2.0/24`) nor `tap0`
(`10.0.3.0/24`), so routing reaches the test taps and not a live interface.
Each seeds the ARP cache for its neighbours (a request from the neighbour to
the tap's own IP, which the stack answers and records) so a forwarded packet
transmits at once, and reads frames back skipping the ARP the stack queued.

**`net-route`**: two taps with overlapping subnets -- a `/16` registered
first, a `/24` second. An address in both routes to the `/24` (longest
prefix, not registration order); an address only in the `/16` routes to it;
loopback is unchanged. Proved by reintroducing a first-match walk, which
lets the `/16` capture the `/24`'s address.

**`net-forward`**: two taps, one marked `NETIF_FORWARD`. A guest datagram to
the other subnet is read back on the uplink with the TTL down one, addresses
and payload intact, IP checksum valid; a TTL-1 datagram draws an ICMP
time-exceeded back to the guest and nothing on the uplink; a datagram
arriving on the *non-forwarding* uplink is dropped as not-for-us, never
forwarded (real-NIC ingress stays a non-router). Proved by reintroducing a
missing TTL decrement (the forwarded TTL is then 64) and an ignored ingress
gate (the uplink packet is then forwarded to the guest).

**`net-nat`**: two taps, the guest side marked `NETIF_FORWARD` and
`NETIF_MASQUERADE`. UDP, TCP-SYN and ICMP-echo round trips -- the source
masqueraded out (checksum valid under the new pseudo-header, the lent
port/id in range), the reply restored to the guest (original port/id,
checksum valid, payload intact); an ICMP dest-unreach quoting a NAT'd packet
translated back with its inner source and port un-NAT'd (and a corrupt-
checksum ICMP error is *not* translated); a guest frame forged with an
uplink-subnet source is dropped by the reverse-path check, never emitted;
the lent port is disjoint from the host ephemeral range; the table bounded
(a flood of distinct flows fills it, further ones dropped, `entries` never
exceeding `NAT_TABLE_SIZE`); and the entries reclaimed by `nat_age`. Proved
by reintroducing a missing pseudo-header checksum fixup (the uplink reads an
invalid checksum), a table that clobbers instead of dropping when full
(`out_drop_full` never rises), and an age that reclaims nothing (the entries
never expire).

**`net-dnat`**: two taps and a static port-forward rule (`tcp:8080 →
10.77.5.15:80`). A client SYN to the host's uplink address port 8080 is read
back on the guest tap rewritten to the guest's `:80` (source intact, checksum
valid); the guest's SYN-ACK is read back on the uplink with its source
rewritten to `host:8080` (checksum valid), so the client sees a reply from
what it dialed; a UDP round trip through a second rule; a connection to an
unruled port is delivered to the host, not forwarded to the guest; a flood of
distinct client flows against one guest's rule settles at its share of the
table (`NAT_QUOTA_PER_GUEST`, not the whole `NAT_TABLE_SIZE`) with the rest
dropped — a busy forward cannot starve a peer — and the flows are reclaimed by
`nat_age`. Proved by reintroducing no reply un-rewrite (the reply's source
port is then the guest's, not the dialed port), a rule that matches every port
(an unruled port is then forwarded to the guest), and — the finding that
prompted the quota — a `nat_dnat_create` that ignores the per-guest cap (the
flood then fills far past `NAT_QUOTA_PER_GUEST` and would exhaust a peer's
slots).

**`net-tapctl`**: drives the control device through the VFS
(`/dev/net/tapctl`). A `FORWARD_ADD` command written to it installs a rule (a
subsequent client SYN is DNAT'd to the guest), the `read` snapshot lists
exactly that rule, a `FORWARD_DEL` removes it and reaps its conntrack entry
(the live flow count drops) so a re-sent SYN stays local and the listing is
empty; a duplicate `(proto, host_port)` `ADD` is `-EEXIST`, an off-guest-tap
target is refused, a short write is `-EINVAL`, and a wrong version is
`-ENOTSUP` -- each changing nothing. Proved by reintroducing a delete that
does not reap conntrack (the flow count does not drop), an add that accepts
any connected subnet (the off-tap target is then installed), and an add that
allows a duplicate binding (the duplicate is then accepted).

## Autoconfiguration (DHCP and DNS)

**`tap-filter`**: the tap input filter that the DHCP server rides. A frame
of a private ethertype is claimed and answered out the tap (the stack never
sees it); an ARP request is *not* claimed and the stack answers it; clearing
the filter lets a formerly-claimed frame reach the stack. Proved by making
`tap_inject` never consult the filter -- the claimed frame is then not
answered.

**`net-dhcp`**: a synthetic guest on a tap injects a DISCOVER with the
broadcast flag set; the reply read back off the tap is an OFFER sent as the
limited broadcast at both layers (IP `255.255.255.255`, Ethernet
`ff:ff:ff:ff:ff:ff`) carrying the guest address, mask, router/DNS and lease;
REQUEST → ACK; a REQUEST for a wrong address → NAK; a flag-clear DISCOVER →
a `chaddr` link-unicast with IP `yiaddr`; a second hardware address is
offered nothing. Proved by reintroducing an IP destination of `yiaddr` under
the broadcast flag (the guest would drop it) and an unconditional ACK (a
wrong address is then not NAK'd).

**`net-dns`**: driven through a loopback upstream responder. A guest query
is relayed with a rewritten id and its answer returned with the guest's
original id and the upstream's A record; two queries sharing an id come back
to the right ports; an unconfigured upstream yields SERVFAIL; a flood to a
black-hole upstream fills the pending table (it never exceeds the bound,
further queries drop) and `tapsvc_dns_age` reclaims it. Proved by
reintroducing no id restoration (the answer carries the wrong id), a table
that clobbers instead of dropping when full (`dns_drop_full` never rises),
and an age that reclaims nothing.

## Many guests

**`net-multiguest`**: eight opens of `/dev/net/tap` (through the VFS, as
`vmctl` does) yield eight taps on eight distinct subnets (`10.0.3.1` …
`10.0.10.1`), each forwarding; a ninth open is `-ENOSPC`. A frame written to
one file is answered only on that file's tap (an ARP for `tap0`'s address
replied on file 0, nothing on file 1). A datagram from `tap0`'s guest to
`tap1`'s guest is read back on `tap1` with its source intact — guest-to-guest
is routed, not masqueraded (with A's to-guest firewall policy opened first,
since the firewall unit defaults inter-guest traffic to drop — this step
asserts routing, not policy). A frame from `tap0`'s guest sourced as a forged
same-subnet address (`10.0.3.50`, not its assigned `.15`) is dropped as
spoofed (`fwd_spoofed` rises) and never reaches `tap1`. With a port-forward
rule per guest, closing `tap0`'s file destroys only its tap, purges only its
guest's rule (`tap1`'s remains), and frees its slot for reuse. Proved by
reintroducing a `release` that skips the purge (the departed guest's rule
lingers), a masquerade that fires between taps (the guest-to-guest source is
then rewritten), and — the finding that prompted the tightening — the loose
"any source on the subnet" reverse-path check (the forged `10.0.3.50` then
reaches the peer). The per-open lifecycle itself is `vfs-chrdev-open` (VFS
tests).

## The forwarding firewall

**`net-firewall`**: two guests opened through `/dev/net/tap` as `vmctl` opens
them (so each attaches), A on `tap0` and B on `tap1`, driven by frame
injection through their files. (1) With no rule, A→B UDP is dropped by the
default policy (never read back on B's tap; `fw_stats.drop_default` and
`ip_stats.fwd_filtered` rise). (2) A→the world is accepted by the to-uplink
default (`accept_default` rises). (3) One rule — A→B udp/7001 ACCEPT — lets
exactly that flow through (read back on B with A's real source; `accept_rule`
and `flow_new` rise) while A→B udp/7002 still drops. (4) Stateful return in
the guest→guest direction: B's reply to the accepted flow reaches A with no
rule for B (`accept_established` rises); an unsolicited B→A datagram does
not. (4b) TCP: on an accepted A→B connection, B's SYN-ACK and ACK are
admitted as replies, but a **bare SYN from B on the reversed ports is a new
connection**, not a reply — it takes B's default drop whatever the tuple says
(a guest injects arbitrary flags, so the reverse-tuple shortcut must not
honour a SYN without ACK). (5) ICMP echo state keyed on the identifier: an accepted A→B echo
request admits B's echo *reply* with the same id, while a B→A echo *request*
is dropped (a reverse request is not a reply) and a reply with a different id
is dropped (no flow). (6) Ordering and identity: a DROP inserted at index 0
for the same traffic wins first-match for a new flow (`drop_rule` rises), the
listing shows the five rules in order (the two `TO_HOST` seeds among them,
since the INPUT-chain unit), deleting the DROP by tuple restores
the ACCEPT, a second delete is `-ENOENT`, and a duplicate add is `-EEXIST`.
(7) Binding by address, not handle: a rule for B written through a
`/dev/net/tapctl` handle that is then closed still admits B→A; closing B's
tap purges it (`fw_rule_add` for the departed B is `-ENOENT`, its list is
empty), and a fresh tap reusing B's address starts with no rules and no
flows (the same B→A datagram now drops). (8) The control round trip: an ADD
appears in the snapshot's filter section with its guest, fields and index,
the snapshot's length equals the sum of its sections, `FILTER_POLICY`
flipping A's to-guest default to ACCEPT lets an unruled A→B flow through and
flipping it back drops the next; a short write is `-EINVAL`, a wrong version
`-ENOTSUP`, an unattached `guest_addr` `-ENOENT`, a `FILTER_DEL` of an
uninstalled tuple `-ENOENT`, and an ICMP rule whose selector is above 255
(not a type, not the wildcard) `-EINVAL` — each changing nothing.

Proved by reintroducing a verdict that always ACCEPTs (the default-drop test
then reads the datagram back on B), a flow table that records nothing (B's
reply is then dropped as unsolicited), an ICMP match that ignores the echo
id (the wrong-id reply is then admitted), an `fw_rule_add` that skips
the attached-guest check (an add for the departed B then succeeds, and the
reused address inherits the stale rule), and an ESTABLISHED shortcut that
honours a reverse bare SYN (B's SYN on the reversed ports is then admitted).

**`net-input`** (the INPUT chain): two guests opened through `/dev/net/tap`,
A on `tap0` and B on `tap1`, driven by frame injection; a verdict is taken on
the network worker, so counters are awaited. (1) Attach seeded exactly the
tap's two services as `TO_HOST` rules on the gateway — `udp gateway/32 :53`
and `icmp gateway/32 type 8` — and the `TO_HOST` default is DROP. (2) The
seeds reach the host: a DNS query to the gateway is accepted by rule
(`in_accept_rule`), and a checksummed echo request draws an echo *reply*
back on A's tap. (3) Everything else is closed by default: UDP to
`gateway:7000`, a TCP SYN to `gateway:2222`, and a datagram to the host's
uplink address (when the NIC is present) are dropped (`in_drop_default`,
`ip_stats.in_filtered`). (4) A rule opens a service per datagram, statelessly:
a SYN and then a bare ACK to the ruled port are both admitted by the same
rule. (4c) `ANY` keeps its forwarding-only meaning: an `ANY udp gateway/32
:7003 ACCEPT` rule does *not* open host port 7003 (`in_drop_default` rises),
while the same tuple as `TO_HOST` does (`in_accept_rule`) — no wildcard
written before the INPUT chain existed silently opens the host. (5) The seeds are real rules: deleting the echo seed by tuple makes the
next echo request drop with no reply; re-adding it reopens echo. (6) The ICMP
selector is a type: a guest echo *reply* (type 0) and a guest
Need-Fragmentation (type 3/4 quoting a host→guest datagram) are dropped by
default and `pmtu_updates` does not move; a `type 0` rule admits the reply
alone while need-frag still drops; `FW_ICMP_TYPE_ANY` admits need-frag too.
(7) Anti-spoof on the local path: with B first given an any-destination DNS
rule that would admit exactly this datagram under *B's* policy, sources from
A's tap that are not A — a stray `10.0.3.50`, the neighbour B, a world
address — toward the gateway's DNS are dropped as spoofed (`in_spoofed`,
`in_filtered`) before any rule is read, so a guest cannot borrow its
neighbour's permissions by forging its source; a datagram forged as the gateway *itself* never
reaches the firewall, because `ipv4_input` already drops one of our own
addresses arriving from a link as a martian (`rx_bad_header`) — a stronger
drop, counted upstream. (8) Per guest: A's TCP rule does not open B's path;
B's release purges its seeds (its list is empty); a reopened B re-seeds
exactly two `TO_HOST` rules. (9) Policy: `TO_HOST` ACCEPT admits an unruled
port (`in_accept_default`), DROP closes it again, and `ANY` is not a policy
direction (`-EINVAL`). (10) The control channel: a `TO_HOST` rule written
through `/dev/net/tapctl` is listed with its direction beside a guest record
carrying `policy_to_host`; an ICMP rule with selector `0` (echo-reply) and
one with `ICMP_TYPE_ANY` are both writable and deletable (type 0 is not
mistaken for the wildcard), `256` is `-EINVAL`, and `FILTER_POLICY` with
`DIR_ANY` is `-EINVAL`.

Proved by reintroducing a verdict that always accepts (the closed port then
counts an accept, not a drop), an `ANY` direction that also matches the host
(the wildcard rule then opens port 7003), a missing anti-spoof (`in_spoofed` never
rises; the datagram forged as B would then be admitted under B's own rule),
echo as a hard-coded hole instead of a seeded rule (deleting the echo seed
then changes nothing), and an ICMP match that ignores the type (a guest echo
*reply* is then admitted by the echo-request seed). The report's fifth proof
— the verdict placed *before* `nat_in` — is not runnable as stated: with the
verdict gated on guest-tap ingress, nothing `nat_in` claims arrives on a
guest tap (a masqueraded reply arrives on the uplink), so the ordering was
unobservable then; the host chain's `net-hostinput` makes it observable
(below).

**`net-hostinput`** (the host chain): an "uplink" tap `u` built as `net-dnat`
builds one (a real, non-guest link) with five world addresses ARP-seeded on
its far side, plus guest A through `/dev/net/tap`; host services as ksock
sockets — TCP listeners on `:2222`/`:2223`, a UDP listener on `:7000`, a UDP
socket bound `:7001` and *connected* to a world peer, a loopback-bound UDP
listener `127.0.0.1:7002`; verdicts awaited on the worker; the world drives
TCP by hand (`hin_mk_tcp` with sequence, acknowledgment, flags, window and
payload) and reads the host's answers back from the tap by protocol and
world port. (1) The host object ships with default ACCEPT and no rules; a
world SYN to `:2222` draws a SYN-ACK (`hin_accept_default`) and a UDP
datagram reaches the `:7000` listener. (2) A DROP rule with a source
(`FROM_UPLINK tcp from 10.77.8.0/24 :2222`) drops a SYN from inside the prefix
(`hin_drop_rule`, `hin_quiet`, `tcp quiet_dropped`; nothing read back) while
a SYN from `10.0.9.9` is still accepted by the default. (3) Quiet delivery:
C1's handshake completes under ACCEPT, then a port-2222 DROP rule is added —
C1's data segment is delivered and acknowledged (`hin_quiet` rises,
`quiet_dropped` does not); an ACK-only probe and a SYN+ACK from a source with
no connection are freed (`quiet_dropped`) with no RST read back; a new SYN
makes no SYN-cache entry or cookie (`syn_cached`, `syn_cookies_sent`
unchanged) and draws no SYN-ACK; under a `udp any` DROP rule the connected
socket's peer is delivered, the listener and an unbound port get nothing back
(`udp quiet_dropped`, no port-unreachable) — and without the rule the unbound
port draws its port-unreachable, so the negative is meaningful. (4) TCP's own
validation, silenced: on C1 an out-of-window segment draws no window ACK, a
reset not at `rcv_nxt`, an in-window SYN and an out-of-range ACK draw no
challenge (`challenge_acks` unchanged) and the connection lives; a burst of
`TCP_CHALLENGE_PER_SEC + 20` in-window SYNs under the rule is freed and then
an in-window SYN on C2 (`:2223`, no rule) still draws its challenge ACK — the
budget was not spent on the rejected ones; on C4 with a 150 ms keepalive idle
armed at establishment and out-of-window probes arriving every 40 ms under a
rule covering the peer, the keepalive probe still fires (`keepalive_probes`
rises); the host `connect`s out to a world peer under a rule covering it — its
SYN is read back, the peer's SYN+ACK is delivered quiet and the host's
completing ACK is read back, `connect` returns 0 — while a bare ACK from that
peer to a tuple with no connection is freed silently; the host then closes
first, the peer's ACK and FIN are accepted (the FIN's ACK read back,
`TIME_WAIT`), a bare ACK there draws nothing, and a *retransmitted* FIN is
acknowledged (the fifth acceptance point). Accepted segments that do not
advance `snd_una` keep their output on C1: three duplicate ACKs make the 100
bytes in flight retransmit (`retransmits` rises); an ACK with window 0 blocks
a 50-byte send (nothing of that size read back) and a pure window update
releases it; two data segments with an unchanged ACK are delivered and
acknowledged together; the peer's FIN is acknowledged and the accepted socket
reads EOF. A valid reset (`seq == rcv_nxt`) from a peer under a DROP rule
tears C2 down (`-ECONNRESET`, `rsts_in`) and emits nothing. (5) ICMP by type:
an `icmp type 8` DROP drops an echo request (since the host-state unit,
delivered quiet and freed by `icmp_input`: `icmp_quiet_dropped`; no reply), a
type-0 datagram passes the default, and without the rule echo is answered.
(6) Off-link, with no rule installed: the world's UDP to guest A's gateway
`:53` and to `127.0.0.1:7002` are dropped `rx_offlink` (the loopback listener
hears nothing, yet a loopback sender still reaches it), as are guest A's
datagrams to `127.0.0.1` and to the uplink's address (`in_filtered`
unchanged — no longer INPUT's default drop); the host chain's counters do not
move for any of them, nor for A's DNS query to its gateway, which INPUT's seed
still admits. (7) Ordering after `nat_in`, now provable: with a port-forward
`tcp:8080 → A:80` and the host default DROP, the world's SYN to `host:8080`
is read back on A's tap, DNAT'd, and no `hin_*` counter moves; loopback is
untouched by the default; under DROP an unruled SYN drops
(`hin_drop_default`) and an explicit ACCEPT rule readmits it. (8) Scope: a
host rule naming `TO_HOST` or `ANY`, a guest rule naming `FROM_UPLINK` or a
source, a host rule with `src_prefix 0` and a non-zero address, and a policy
in the other scope's direction are all `-EINVAL`. (9) The control channel: a
host rule with a source written through `/dev/net/tapctl` (`guest_addr 0`)
is listed in the host's record beside `policy_from_uplink`; a 20-byte
(version-3) write, a host rule naming `TO_HOST` and a guest rule naming
`FROM_UPLINK` are refused.

Proved by reintroducing: a host verdict that ignores its rules (the sourced
DROP then delivers the SYN and a SYN-ACK is read back); a source match that
ignores the prefix (the out-of-prefix SYN then drops); `batch_send` not
honouring the quiet bit (the ACK-only probe then draws a RST, the
out-of-window segment a window ACK, the in-window SYN a challenge — one
revert, three observed responses); the gate placed at the final flush only
instead of inside `batch_send` (the listener-rejection early flush then
escapes it: the sourced DROP's SYN is not even counted `quiet_dropped`, and
behind it the no-pcb flush sends the RST); the quiet bit cleared at the window test
instead of after the last rejection (the in-window SYN and the mis-positioned
reset then draw challenge ACKs); the bit cleared only inside the advancing-ACK
branch (the window update then releases nothing, the third duplicate ACK
builds no retransmission and the peer's FIN draws no ACK); the `SYN_SENT`
block not clearing it (the host's completing ACK is discarded and `connect`
never returns); `challenge_ack` consulting the budget before the quiet bit
(the burst then starves C2's legitimate challenge); the SYN-cache allocation
not gated (the new SYN under the rule then draws a SYN-ACK); `last_rx_ns` left
before the acceptability test (the keepalive probe then never fires while the
rejected probes arrive); a DROP that frees TCP at the IP layer (C1's data then
never arrives); `udp_input` ignoring the flag (the unbound port then draws a
port-unreachable under the rule); the off-link invariant removed (`rx_offlink`
never rises and the world's datagram reaches the loopback-bound listener); and
the host verdict moved before `nat_in` (the DNAT'd SYN then drops under the
host default DROP — the proof the INPUT unit could not run).

**`net-hoststate`** (the host's own flows): an "uplink" tap as
`net-hostinput` builds one with two world addresses ARP-seeded, a guest tap
with its own `tapsvc` for the DNS leg, and two guests through `/dev/net/tap`
for the guest pool; host sockets bound but never connected. The world's
traffic is closed by a **sourced DROP rule** covering the test's subnet
rather than by the machine-wide default, so that a failing assertion cannot
harden the real uplink for every test that follows; the hardened *default*
is exercised at the end, with its verdicts taken and the default restored
*before* anything is asserted. (1) An unconnected client's reply survives:
the host's `sendto` to `world:5300` is read back on the tap and recorded
(`hin_flow_new` rises by exactly one), and the reply on that tuple is
delivered to the socket with no rule anywhere (`hin_accept_established`),
while a datagram from another port at that peer, from another peer, or to
another local port is freed (`udp quiet_dropped`) and no socket sees it.
(1b) A refused send opens nothing: an oversized datagram, rejected with
`-EMSGSIZE` after its tuple was read, records no flow and leaves its reverse
tuple closed. (2) One tuple, and no notion of intent: a *second* unsolicited datagram on
the open tuple is admitted too (the socket's validation is the second line),
while the world initiating to a port the host never sent from is freed; and
a datagram carrying the host's *own* address as its source — the only thing
a forward-direction match could be on a real link — is dropped as a martian
(`rx_bad_header`) before any chain, no `hin_*` counter moving, which is why
the state step admits on the reverse match alone.
(3) The host can ping: its `icmp_send_echo` is recorded by identifier, the
reply reaches the echo hook with that identifier and source, and a reply
carrying another identifier is freed (`icmp_quiet_dropped`, no hook).
(4) An echo *request* is a request: quiet delivery hands it to `icmp_input`,
which answers nothing (`icmp_quiet_dropped`, nothing read back) and leaves
`icmp_echo_rcvd`/`icmp_echo_replied` untouched — the host-wide echo-reply
budget is not spent on a refused probe. (5) Path-MTU discovery survives: the
host connects outbound (PR #107's `SYN_SENT` acceptance point admits the
SYN+ACK) — and `hin_flow_new` does *not* move, because TCP is not recorded —
sends 1200 bytes, and the router's Need-Fragmentation quoting that segment,
delivered quiet, is consumed (`tcp`/`ip pmtu_updates` rise) and the segment
comes back inside the new MTU (≤ 536 bytes, same sequence); one quoting a
tuple with no connection still *reaches* the consumer
(`icmp_needfrag_rcvd` rises — the firewall admitted it to the layer that can
tell) and is refused there (`pmtu_updates` unmoved); a port-unreachable
quoting the connection is freed (`icmp_quiet_dropped`, no consumer).
(6) The DNS proxy end to end: a guest's query is relayed out the uplink from
the proxy's **unconnected** socket (recorded, `hin_flow_new`), the
upstream's answer to that port is admitted by state
(`hin_accept_established`), and the guest reads the answer with its own id
and the A record — through a DROP that would otherwise free it, with the
proxy unchanged. (7) A send on a live flow refreshes rather than re-records
(`hin_flow_new` and the live-flow count unmoved); `fw_age` past the idle
timeout closes the tuple (the next reply is freed) and a fresh send opens it
again. (8) Neither a forwarded guest flow nor a loopback send is the host's:
a masqueraded guest→world datagram (read back masqueraded on the uplink) and
a loopback send both leave `hin_flow_new` unmoved. (9) The share is the
host's own: `FW_FLOW_QUOTA_HOST` distinct flows record and the live count
equals the share, the next one does not (`hin_flow_drop_full`) yet its
datagram still leaves (read back), and a guest-to-guest flow still records
(`flow_new`) with the host's share full. (10) State beats the hardened
default too, not only a rule.

Proved by reintroducing: the state step removed from `fw_host_verdict` (the
unconnected socket's reply then drops — and with it the proxy's answer);
a peer match loosened to the address alone (the same-peer-other-port
datagram is then admitted); the local-port match dropped (the datagram to
another local port is then admitted); the echo identifier ignored (the
wrong-identifier reply then fires the hook); the flow recorded before `output_on` accepts the
datagram (the oversized send then opens a tuple); the record moved from
`ipv4_output` into `output_on`, which shows at that same assertion and
behind it puts the masqueraded guest flow in the host's share; the real-link egress test dropped (a loopback send then
records); TCP recorded as well (the outbound connection then records a flow
on the uplink's hottest send path); refresh treated as creation
(`hin_flow_new` rises on every send and one client fills the share); the
one-entry cache used without validating the tuple (64 distinct flows then
collapse into one refreshed entry); the host's share unbounded (the flow
past it records and the guests' pool shrinks); ICMP freed at the IP layer
instead of delivered quiet (the Need-Fragmentation then never reaches TCP
and the path MTU never moves); and `icmp_input` ignoring the flag (the
refused echo request is then answered).

**`net-input`** (adjusted): its "guest → the host's uplink address" case is
now dropped by the off-link invariant (`rx_offlink`) before any chain, not by
INPUT's default (`in_drop_default`).

**`net-output`** (the OUTPUT chain): an "uplink" tap as `net-hoststate`
builds one and a guest through `/dev/net/tap` — so the guest's tap
masquerades, which is what makes the host's sends to it `FW_SCOPE_GUEST`,
and its INPUT seeds let its DNS query through — plus a host UDP socket bound
but never connected. (1) The default is ACCEPT and changes nothing: the
host's datagram to the world leaves as before (`out_accept_default`).
(2) A rule refuses a send and the sender is told: `ksock_sendto` returns
**`-EPERM`**, `tx_filtered` and `out_drop_rule` rise by one, nothing reaches
the link, and no reply state is opened — with the host chain closed for that
world, the reply that a recorded flow would have admitted is dropped
instead; delete the rule and the same send records its flow and the same
reply is delivered, so the ordering is asserted from both sides. The step
uses a destination port of its own, because step (1)'s successful send left a
live flow that would have admitted the reply regardless. (3) An ICMP rule
refuses an echo request and `icmp_send_echo` returns `-EPERM`; without the
rule the request leaves; and a rule naming **this link's own address as the
source** still matches a request whose source the sender left to the route,
which is the resolve-once property. (4) The scope separates the egresses:
the same datagram to a guest and to the world, under a `guest` rule, then a
`world` rule, then `any`; a rule naming a destination prefix instead still
works, so the scope adds a dimension rather than replacing one; and two
rules alike but for the scope are two rules (the second `add` is not
`-EEXIST`, both drop their own egress, each deletes by its own tuple).
(5) The host's reply to a guest is filterable — the case the INPUT and host
chains both named and neither could express: the guest's DNS query reaches
the proxy through INPUT's seed and the proxy answers it, and with a
`scope guest` rule on the guest's own port that answer never reaches the
guest's tap (`tx_filtered` rises). (6) A failure that is not a verdict is not
counted as one: an oversized datagram is accepted by the chain and refused by
`output_on` (`-EMSGSIZE`), leaving `tx_filtered` and the flow count alone. A
true no-route send is not reachable from a socket here, because the NIC
carries a default route; the oversized case makes the same point
deterministically. (7) Loopback passes no chain: a rule matching by every
other field does not touch a `127.0.0.1` send. (8) TCP stalls rather than
failing, the documented limit: a nonblocking `connect` to a refused port
returns `-EINPROGRESS`, not `-EPERM`, while `out_drop_rule` rises and no SYN
reaches the link. (9) `nat_in`'s delivery to a guest is this chain's traffic
too: a DNAT'd SYN reaches the guest without a rule and is stopped by a
`scope guest` one (`tx_filtered`). (10) Scope discipline: a scope on a
`TO_HOST` or `FROM_UPLINK` rule is `-EINVAL`, an `OUTPUT` rule on a guest is
`-EINVAL`, a scope value out of range is `-EINVAL`, and `policy <guest> out`
is `-EINVAL` while the host's `policy_output` reads ACCEPT. (11) The control
channel: an `OUTPUT` rule with a scope written through `/dev/net/tapctl`
round-trips with its scope beside `policy_output` in the host's record; a
version-4 writer is refused **by version**, since the command's size did not
change; a guest naming `OUTPUT` is refused there too; and the policy record
is the version-5 one, asserted by a static assert on its size.

Proved by reintroducing: the gate counting the verdict but not stopping the
datagram (the refused `sendto` then returns success and the datagram reaches
the link); the loopback exemption removed (the `127.0.0.1` send then fails);
the scope ignored in the match (the guest-scoped rule then drops the host's
world traffic too); the scope left out of a rule's identity (the second of
two rules alike but for it is refused `-EEXIST`); `-EPERM` replaced by a
silent success (the failed `sendto` then returns the byte count and the
caller cannot tell); the source resolved *after* the verdict (the
source-prefix rule then judges `0` and misses the echo it should refuse —
and `net-hoststate` fails with it, because the flow key takes the same
unresolved value); `OUTPUT` allowed on a guest's object (the guest's add then
succeeds); and the two that keep the send path's fast path honest — a new
rule not invalidating it (the first rule then never binds, so the refused
send succeeds) and a policy flip not invalidating it (the hardened default
then lets everything out). Nine in all.

One proof named in the report is **not observable, and the reason is worth
keeping**: moving the verdict to *after* the host chain's flow read changes
nothing the suite can see. `fw_host_record` is already conditional on
`output_on` succeeding, so a datagram the verdict drops can never be
recorded whichever side of the read the verdict sits on. The ordering is
kept for clarity and for the work it saves; the property it was thought to
guarantee holds for a stronger reason, which the host-state unit's own
proof 13 covers.

**`net-dnat` and `net-tapctl`** (races fixed with the host-state unit): two
assertions in these tests were written without a barrier against the network
worker, and the host-state unit's timing perturbation turned both into
intermittent CI failures — on the GIC-variant boot, where the same tree
passed the default one. They are fixed rather than retried. `net-dnat` aged
the NAT table while the flood it had just injected could still be draining
(`dnat_drop_full` rising says *some* packet was refused, not that all were
processed), so an entry created behind `nat_age` survived it; it now waits
for translations plus refusals to account for every injected datagram, and
asserts that sum. `net-tapctl` asserted that a client SYN is *not* forwarded
to the guest without first draining the guest tap, so any frame an earlier
step left queued failed it immediately (30 ms, not the 500 ms timeout); it
now drains before injecting, which leaves the assertion's own meaning
untouched. The host-chain and host-state tests' *positive* waits also became
patient (`HIN_TRIES`): those loops return as soon as what they wait for
arrives, so a generous budget costs nothing when the stack works and stops a
loaded runner from failing an assertion that would have passed. Negative
waits keep their short budgets, since they must not wait for something that
should never come.

**`net-tapctl`, `net-firewall`, `net-input`** (adjusted for version 4): the
snapshot buffers and lengths count one more policy record (the host's) and
the grown rule record; `fw_policy_get` reports a fourth slot.

**`net-firewall`** (adjusted for version 3): its ICMP rule now carries
`FW_ICMP_TYPE_ANY` (version 2's `0` meant "any"; version 3's `0` is echo
reply), the "ICMP with a port" rejection became "a selector above 255", and
its rule counts include the two `TO_HOST` seeds each guest attaches with (the
step-6 list is five rules with the seeds at indices 2–3; a reopened guest
lists exactly its two seeds; the snapshot carries at least five rules).

**`net-tapctl`** (extended): the snapshot's expected length is now computed
from the filter section the read appends (ABI version 2 and later: its
header, the attached guests' policies and rules), rather than assumed to end
at the port-forward rules.

## The host harness (`tests/boot/nettest.py`, `run_boot_test.py`)

`run_boot_test.py` creates a `NetTest` for normal runs (not
`--expect-panic`, not `--expect-selftest no`). It picks three free
host ports and exports `QEMU_NET_HOSTFWD=tcp:127.0.0.1:P1-:7,udp:127.0.0.1:P2-:7`
and `QEMU_FWCFG_NETTEST=tcp=P3` for `scripts/qemu-run.sh`, which turns
them into `-netdev user,id=n0,ipv4=on,ipv6=on,hostfwd=...` and
`-fw_cfg name=opt/cosmo/nettest,string=tcp=P3`. A thread listens on
P3 and answers the guest's `cosmo hello\n` with `cosmo world\n`;
another polls the serial log for `NETTEST: ready`, then: opens a TCP
connection to P1, writes 256 KiB of seeded random bytes in chunks of 1
to 9000 bytes while a reader collects the echo and compares it;
sends 20 UDP datagrams to P2 and counts echoes (18 or more pass, QEMU's
user-mode backend may lose one); opens a second TCP connection and
sends `QUIT`. When self-tests are enabled the run fails on any of:
no ready line, TCP mismatch, fewer than 18 UDP echoes, the
guest-initiated connection not received, QUIT not sent, or the
`NETTEST: client ok` / `NETTEST: done .*quit=1` markers missing. The
default timeout is 180 s (the harness gets timeout minus 30 s).

Release builds (`make BUILD=release test`) have no self-tests, so the
harness is created but its results are not evaluated; the two boot
markers (`virtio_net` loaded, `eth0` registered) are still required.

## User-mode test (`userland/init/init.c`, `net_selftest`)

Run by `process-user` as `init --selftest`, after `fs_selftest`: a UDP
socket binds `127.0.0.1:40000` (a second bind is `-EINVAL`), sends
itself a datagram, receives it with the sender's address and port,
`getsockname` reports the port; a connected `sendto`/`recvfrom` pair
with NULL addresses works; a TCP `connect` to a closed loopback port is
`-ECONNREFUSED`; a TCP socket binds port 80 (init is uid 0); `listen`
on a fresh unbound socket is `-EINVAL`; `socket(99, ...)` is
`-EAFNOSUPPORT`, `socket(AF_INET, 7)` is `-EINVAL`, `bind` on the
console handle is `-EBADF`. Prints `usertest: sockets ok`, which the
boot test requires.

## Running

```sh
make test                                   # 58 self-tests, harness, USERTEST
QEMU_SMP=1 make test                        # single CPU (worker and callers share one CPU)
make BUILD=release test                     # boot markers only
QEMU_PCAP=/tmp/guest.pcap make run          # record every frame on eth0 (filter-dump)
QEMU_NET_HOSTFWD=tcp:127.0.0.1:2007-:7 QEMU_FWCFG_NETTEST=tcp=1 make run
                                            # run the echo services by hand (the back-connection fails, echo works)
```

`make run` boots with the same NIC and QEMU user-mode networking;
`eth0` gets `10.0.2.15/24` with gateway `10.0.2.2` unless
`opt/cosmo/ipv4` is passed with `QEMU_EXTRA='-fw_cfg
name=opt/cosmo/ipv4,string=10.0.2.20/24,10.0.2.2'`. A pcap recorded
with `QEMU_PCAP` opens in Wireshark or `tcpdump -r`.

## Debugging notes

- QEMU's user-mode backend drops Ethernet frames shorter than 60
  bytes and, when started with `ipv6=on` alone, disables IPv4
  entirely; `scripts/qemu-run.sh` therefore pads frames in
  `ether_output` and passes `ipv4=on,ipv6=on`. Both were found with a
  `filter-dump` capture: the guest's ARP requests were visible, no
  reply ever came.
- The 8 s hang watchdog fires during long blocking waits in kernel
  tests; the network tests kick it (`sched_watchdog_kick`) in their
  wait loops. A new long-running test must do the same.
- `netif_dump()` logs every interface's counters; the `*_get_stats`
  functions are what the tests compare before and after.

## Receive scaling (unit 11)

**`net-steer`**: a fake interface `steer0` (10.9.0.1); `net_flow_hash`
of two frames of one flow is equal and non-zero, of another flow
different; every CPU in turn injects 16 frames of each of 8 TCP flows
(pinned threads) and the worker-side hook records the CPU and sequence
per flow: no flow seen on two workers, no sequence out of order, and
on 4 CPUs at least two workers used; `netif_rx_on(m, 1)` is seen on CPU
1; with steering off the eight flows all arrive on CPU 0; CPU 0's
queue counters are non-zero; unregister releases the interface.

**`net-rxhook-grace`**: the receive hook announces itself and then
lingers 10 ms inside the worker; the test, seeing the announcement,
clears the hook while it is lingering and checks on return that the
hook has finished. On two or more CPUs the frame is queued to another
CPU's worker (`netif_rx_on`), so the clear really does overlap the hook;
without the grace period in `netif_set_rx_hook` the check fails there.
This is what lets `net-nicbench` keep one hook context per round on its
stack.

**`net-csum-offload`**: a fake interface `csum0` with both capabilities
transmits a hand-built IPv4/TCP packet in the partial form (flags,
`csum_start` 20, `csum_offset` 16, not yet valid; `m_csum_complete`
makes it valid and clears the flags); without the capability
`netif_transmit` finishes it; out-of-range offsets are `-EINVAL` with
nothing transmitted; a received packet with a corrupted payload is
dropped and counted in `bad_cksum`, and the same packet marked
`M_CSUM_OK` passes the checksum and reaches the pcb lookup; `lo` has both
capabilities.

**`net-bench`** (reports only): loopback TCP with one and two concurrent
4 MiB flows, and 10 000 64-byte UDP sends, steering off then on.
Measured 2026-09-06 on the boot test (QEMU TCG, 4 CPUs, Apple Silicon
host; noisy, indicative):

| Mode | TCP 1 flow | TCP 2 flows (total) | UDP sends/s (delivered of 10 000) |
|---|---|---|---|
| steering off (one queue, CPU 0) | 31–38 MiB/s | 49–63 MiB/s | 43 000–53 000 (460–480) |
| steering on (per-CPU queues) | 34–51 MiB/s | 68–71 MiB/s | ~42 000 (470–9 900) |

**`net-nicbench`** (reports; fails only if fewer than half the ARP
replies return): per non-loopback interface, 2 000 ARP round trips
through the driver's rings to the gateway with at most 64 in flight,
10 000 UDP sends of 1 KiB through the whole stack and out the NIC, and
the software checksum's share of a send. The results table and the
offload decision they gate are in `docs/drivers/e1000e/design.md`
("Offloads"): 12–14 k round trips/s and 20–23 k sends/s on x86_64,
about 60 % of that on aarch64, a checksum share of 1–2 %, and the two
drivers within noise of each other.

Two things it found on its first run. An open-loop sender lost
three quarters of its replies in the receive queue — the driver had
received every one — which is why the ARP loop is windowed: a round
trip is only a round trip if the reply is waited for. And the e1000e
driver was double-counting the interface statistics that `netif` already
keeps, which looked plausible alone and was obvious beside virtio-net's
figures in the same boot.

Two concurrent flows gain 30–40 %; a single flow gains too, because
its two directions hash to different workers. The UDP send rate is the
sender's system-call rate in both modes; how many datagrams the
receiver keeps depends on whether its thread gets to drain the 64-entry
socket queue between bursts (with steering the receiver's worker is
usually another CPU, and most runs deliver nearly all of them), so the
delivered count varies from run to run and is reported, not compared.
The reorder test (`net-tcp-reorder`) now delays every fifth data
segment (13 per run) since `m_copypacket` copies segments longer than
a cluster. On one CPU (`QEMU_SMP=1`) both modes are the same path and
the numbers agree.

## Gaps

- The virtio-net checksum offload paths (`NEEDS_CSUM` on transmit,
  `DATA_VALID`/`NEEDS_CSUM` on receive) run only against a backend that
  offers `CSUM`/`GUEST_CSUM`; QEMU's user-mode network does not, so they
  are reviewed, not tested. Multi-queue negotiation (`VIRTIO_NET_F_MQ`)
  is not implemented for the same reason (`design.md`, "virtio-net
  offloads").
- No host tests yet: the checksum and header parsers should get a
  `tests/host/test_net.c` with a bit-flip fuzz loop (constitution
  section 60); today only well-formed packets and QEMU's stack
  exercise them.
- No duplication or window-shrink injection (reordering is injected by
  `net-tcp-reorder`).
- No IPv6 traffic through `eth0` (QEMU's user-mode IPv6 is enabled but
  the harness uses IPv4); IPv6 is tested over `lo` only, and ND
  against a real peer is untested.
- `ETIMEDOUT` is reached through keepalive (`net-tcp-keepalive`); the
  retransmit limit itself (8 retransmissions, about 2 minutes with the
  RTO doubling) is not driven.
- Concurrency stress is two concurrent TCP flows and eight steered
  flows from every CPU (`net-bench`, `net-steer`); lock order is checked
  by lockdep on every boot.
- No timed socket operations exist; non-blocking mode and readiness
  are tested, a wait primitive over readiness (`poll`) does not exist
  yet.
- No test of the SYN cookie slot boundary (a cookie from the previous
  8 s slot), of the challenge-ACK cap, or of a path MTU message from a
  real router.
