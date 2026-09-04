# Networking: invariants

Rules that must not be broken without changing this document and the
code together. Each names how it is checked today and what is not yet
covered.

**N1. The stack depends on no NIC driver** (constitution invariant 5).
Nothing under `kernel-services/network/` includes a driver header or
names a driver; a driver reaches the stack only through `struct
netif_ops`, `netif_register`, `netif_rx` and the mbuf API, all of which
are the exported symbols listed in `api.md`. `virtio_net` is a module
outside the kernel image, and every protocol test runs over `lo`, which
is a `netif` like any other. Check: review of includes; `make test`
passes `net-mbuf` .. `net-lo-tcp-loss` before `virtio_net` matters, and
`QEMU_EXTRA` can remove the NIC without breaking them. Gap: no build
barrier forbids the include.

**N2. An mbuf pointer is exactly one reference, and handing a packet
to a function that "takes" it ends the caller's reference.** `m_free`
drops one reference on one buffer, `m_freem` on a chain; `netif_rx`,
`netif_transmit`, `ops->transmit`, `ether_output`, `ipv4/6_output`,
`udp_input`, `tcp_input`, `mbufq_enqueue` (even when it fails),
`arp_resolve` returning `-EINPROGRESS` or an error, and `m_prepend` /
`m_pullup` returning NULL all consume the argument. A shared cluster
(`m_ref`) is freed with its last referencing mbuf. Check: `net-mbuf`
compares `mbufs_alive` and `clusters_alive` before and after; every
network self-test leaves `socket_count()` where it started, which
frees every queued datagram; the loopback loss test drops packets
through the filter (freed by the caller) and the counters stay
balanced. Gap: no leak check across the harness test or across a
boot, and no debug-mode poisoning of freed mbufs.

**N3. No header is read before it is known to be inside the buffer.**
Every layer calls `m_pullup(m, sizeof header)` before casting, then
checks the header's own length fields (`IPV4_HDR_LEN`, IPv6 payload
length, `TCP_HDR_LEN`, UDP length, ARP hardware/protocol sizes) against
`pkt.len` before using them, and options are walked by length with
bounds checks (`parse_mss`). Check: review of `ether_input`,
`arp_input`, `ipv4_input`, `ipv6_input`, `icmp_input`, `icmpv6_input`,
`udp_input`, `tcp_input`; malformed inputs in `net-cksum` and
`net-lo-udp`. Gap: the host bit-flip fuzzer over the parsers that
`design.md` asks for is not written yet; parsers are exercised only by
well-formed traffic and QEMU's user-mode stack.

**N4. All protocol input runs on the `netrx` thread; timers never send.**
`netif_rx` only enqueues; `ether_input` and everything it calls run on
the worker; the ARP/ND ageing timer and the TCP retransmit, delayed-ACK
and TIME_WAIT timers set a flag and call `net_work_queue`, and the
worker runs the handler. Output runs on the caller's thread. Check:
`KASSERT`-free today, by review: the only callers of `ether_input`,
`*_input` and `pcb_work` are `worker_main`'s `input_one` and
`run_work`. Gap: no runtime assertion that the current thread is the
worker in the input functions.

**N5. No spinlock is held across a driver transmit, a user-memory
copy, or a blocking wait.** TCP builds segments under its lock into a
`struct tcp_batch` and `batch_send` runs after unlock; UDP releases
the pcb lock before `ipv4/6_output`; ARP/ND release their table lock
before sending a request or the pending packet; the system calls copy
user memory into a kernel bounce buffer before calling `ksock_*`.
Lock order: `sock->lock` (mutex) → TCP/UDP lock → ARP/ND lock →
`netif->lock` → driver locks → mbuf caches and `mbufq` locks
(leaves). Check: review; `lo_transmit` calls `netif_rx` synchronously,
so a lock held across transmit over `lo` would recurse into the
receive queue lock and hang the loopback tests. Gap: no lock-order
checker.

**N6. Data the peer has acknowledged is gone from the send ring; data
we have acknowledged is in the receive ring.** `snd_una` advances only
when an ACK covers the bytes, and `netbuf_drop` frees exactly that
many; out-of-order segments are acknowledged with `rcv_nxt` (nothing
new) and dropped rather than partly stored; a segment is stored before
the ACK for it is built. Retransmission always restarts from `snd_una`.
Check: `net-lo-tcp` (1 MiB v4 and 256 KiB v6 verified byte for byte),
`net-lo-tcp-loss` (every seventh data segment dropped; the transfer
still completes and `retransmits` grew), the harness's 256 KiB echo
through QEMU's user-mode stack. Gap: no reordering or duplication
injection, no test with a peer that shrinks its window.

**N7. Ports below 1024 need uid 0; a bound (address, port) is unique.**
`ksock_bind` refuses `1..1023` for `uid != 0`; `udp_bind` and
`tcp_bind` refuse a port already bound with the same or a wildcard
address (a pcb in TIME_WAIT does not block reuse); ephemeral ports
come from `49152..65535` and are checked the same way. Check:
`net-lo-udp` (`-EADDRINUSE`, `-EPERM` for uid 1000), `init --selftest`
binds port 80 as uid 0. Gap: no test that the ephemeral counter wraps
correctly under exhaustion.

**N8. Nothing from a real interface impersonates a local address.**
`ipv4_input` drops packets whose source is `127/8` or one of our own
addresses when they arrive on an interface without `NETIF_LOOPBACK`
(`rx_not_for_us`), and packets whose destination is not ours and not
broadcast; `ipv6_input` applies the same source rule and accepts as destination
only our own addresses, the all-nodes group and our solicited-node
group. Check: review; the harness traffic
confirms legitimate packets pass. Gap: no negative test injects a
martian.

**N9. Malformed or unwanted traffic is counted, never logged above
debug level.** Every drop increments a `struct *_stats` counter
(`netif->stats`, `ip_stats`, `udp_stats`, `tcp_stats`, `arp_stats`);
no input path calls `kinfo`/`kwarn`. Check: review; the boot log of a
`make test` run contains no per-packet lines. Gap: none.

**N10. UDP over IPv6 always carries a checksum; IPv4 UDP checksums are
always generated.** `udp_sendto` computes the checksum for both
families (0 becomes 0xffff); `udp_input` drops an IPv6 datagram with
checksum 0 (`rx_bad_cksum`) and verifies every non-zero checksum.
Check: `net-lo-udp` runs the v4 and v6 paths and asserts
`rx_bad_cksum` did not move; the harness's UDP echo crosses QEMU's
stack, which verifies checksums. Gap: TCP has no equivalent negative
test (a corrupted segment is only reviewed).

**N11. The socket UAPI is stable.** System-call numbers 23–31, the
`COSMO_AF_*`, `COSMO_SOCK_*`, `COSMO_SHUT_*` values, the 28-byte
`struct cosmo_sockaddr` (host-order `port`, network-order `addr`) and
the `COSMO_E*` numbers added in this phase are never renumbered or
reshaped; `addr_from_user` rejects a shorter length with `-EINVAL` and
reads exactly 28 bytes of a longer one, so a future extension must use
a new call or a flag. Check: `init
--selftest` (`net_selftest`) uses the header verbatim; the boot test
requires `usertest: sockets ok`. Gap: no ABI snapshot test on the host.

**N12. A socket handle honours its rights and its object type.**
`accept`, `recvfrom` and `read` require READ; `sendto` and `write`
require WRITE; every socket call resolves the handle with
`socket_from_kobject`, so a file or console handle is `-EBADF`.
Check: `net_selftest` (`bind` on the console handle is `-EBADF`).
Gap: no test drops a right from a socket handle (no `handle_dup` with
reduced rights exists yet).

**N13. Received ARP and ND traffic learns only what the protocols
require, and never at the cost of state in use.** A request or
neighbour solicitation addressed to us records the asker; a reply or
advertisement completes only an entry we are resolving; unsolicited
replies and requests for other hosts change nothing (`arp_stats.
unsolicited`). Learning from received traffic never evicts an entry; a
full table learns nothing. Only our own resolution evicts, preferring
the least recently updated reachable entry over one with a resolution
in flight. Check: `net-arp` (forged reply ignored, request to us
learned). Gap: an on-link host that lies in a request addressed to us
still poisons its own IP's mapping; ARP offers no defence and none is
attempted.

**N14. A TCP pcb is never freed while a socket points at it.** Every
free path checks `pcb->sock`; when TIME_WAIT ends under a live socket
the pcb becomes CLOSED and waits for `tcp_close`. Check: `net-lo-tcp`
keeps a shut-down socket 2.5 s past TIME_WAIT and uses it.
