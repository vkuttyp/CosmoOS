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

**N4. All protocol input runs on a `netrx/N` worker thread, one flow's
packets on one worker in order; timers never send.** `netif_rx` only
enqueues, on the queue of the CPU its flow hash selects (unit 11: the
same hash for the same flow whatever CPU injected the packet, so
per-flow order is the queue's FIFO order); `ether_input` and everything
it calls run on that worker; the ARP/ND ageing timer and the TCP
retransmit, delayed-ACK, TIME_WAIT and keepalive timers set a flag, take
a pcb reference and call `net_work_queue`, and the calling CPU's worker
runs the handler (`pcb_work`), which drops the reference; input and
`pcb_work` for one connection may run on different workers and are
serialised by the pcb lock (N14). Output runs on the caller's thread.
Check: `net-steer` (eight flows injected from every CPU: each flow seen
by one worker, in sequence; several workers used on 4 CPUs;
`netif_rx_on` lands where told; steering off puts everything on CPU 0);
review: the only callers of `ether_input`, `*_input` and `pcb_work` are
`worker_main`'s `input_one` and `run_work`. Gap: no runtime assertion
that the current thread is a worker in the input functions.

**N5. No spinlock is held across a driver transmit, a user-memory
copy, or a blocking wait.** TCP builds segments under its lock into a
`struct tcp_batch` and `batch_send` runs after unlock; UDP releases
the pcb lock before `ipv4/6_output`; ARP/ND release their table lock
before sending a request or the pending packet; the system calls copy
user memory into a kernel bounce buffer before calling `ksock_*`.
Lock order: `sock->lock` (mutex) → listener `pcb->lock` → child
`pcb->lock` (subclass 1) → `tcp-table` / UDP lock → ARP/ND lock →
`netif->lock` → driver locks → mbuf caches and `mbufq` locks
(leaves). A lookup never takes a pcb lock under the table lock: it
takes a reference, drops the table lock, then locks the pcb. The netif
registry lock is a spinlock never taken under a protocol lock; TCP's
path MSS is computed before the pcb lock (`tcp_path_mss`) and read
from `pcb->path_mss` under it. Check: the debug-build lock-order
checker on every boot (`docs/kernel/lockdep/`); `lo_transmit` calls
`netif_rx` synchronously, so a lock held across transmit over `lo`
would recurse into the receive queue lock and hang the loopback tests;
`mutex_lock` panics when entered with a spinlock held, and
`net-tcp-mss` checks the cached values.

**N6. Data the peer has acknowledged is gone from the send ring; data
we have acknowledged is in the receive ring.** `snd_una` advances only
when an ACK covers the bytes, and `netbuf_drop` frees exactly that
many; out-of-order segments are acknowledged with `rcv_nxt` (nothing
new) and held in the reassembly queue, never stored in the ring until
the bytes before them have arrived; `rcv_nxt` advances only over bytes
in the ring; a segment is stored before the ACK for it is built.
Retransmission always restarts from `snd_una`.
Check: `net-lo-tcp` (1 MiB v4 and 256 KiB v6 verified byte for byte),
`net-lo-tcp-loss` (every seventh data segment dropped; the transfer
still completes and `retransmits` grew), the harness's 256 KiB echo
through QEMU's user-mode stack, `net-tcp-reorder` (every fifth data
segment overtaken by the next; byte-exact, `ooo_queued` grew). Gap: no
duplication injection, no test with a peer that shrinks its window.

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

**N14. A TCP pcb is never freed while a socket points at it, and an
ended connection holds nothing.** The socket holds a reference that
`tcp_close` drops last. Every path that ends a connection (TIME_WAIT
expiry, the last ACK, a reset, the retransmit limit, keepalive
exhaustion, the FIN_WAIT_2 timeout) goes through `pcb_end_locked`: it
kills the pcb when `sock` is NULL (timers cancelled synchronously, out
of the table, reassembly queue freed, the state machine's reference
dropped by the caller after unlocking) and otherwise retires it
(CLOSED, timers off, out of the pcb table so the port is free and no
segment matches it) until `tcp_close`. The memory goes when the count
reaches zero, never under the pcb's own lock. Check: `net-lo-tcp` keeps
a shut-down socket 2.5 s past TIME_WAIT, uses it, and binds a new
socket to its former port; `net-tcp-rfc5961` resets a connection under
a live socket and reads the error; `net-tcp-keepalive` ends two
connections on the worker.

**N-L1. An interface is freed only by its release, after `netif_unregister`
and the last reference.** `netif_register` refuses an interface without
`ops->release`; the registry takes a reference; `netif_find`,
`netif_default`, `netif_loopback`, `ipv4_route` and `ipv6_route` return
referenced pointers that their callers put; `netif_unregister` leaves no
transmit, receive, queued packet, ARP or ND entry naming the interface
(`docs/kernel/quiesce/invariants.md` Q9–Q12). Check: `net-netif-lifetime`.
Gap: the virtio-net remove path is exercised only by unloading the module.

**N-L2. A TCP child never exists without an owner between accept and
attach.** `tcp_accept(pcb, owner)` attaches under the listener's and the
child's lock with the dequeue, and the queue's reference becomes the
socket's. Check: `net-accept-race`.

**N-L3. TCP pcb memory is freed only after its timers' callbacks have
returned.** `pcb_kill_locked` uses `timer_cancel_sync` on all four timers
before the state machine's reference is dropped; the callbacks take only
the network work lock and atomics on the pcb, and hold a reference of
their own across the work hand-off, so a callback in flight can never be
the last reference. Check: `timer-cancel-sync` for the mechanism;
`net-lo-tcp*` for the path. Gap: the callback/free race itself is not
driven by a test.

**N15. A SYN allocates nothing.** A listener answers a SYN from a
64-entry SYN cache or with a SYN cookie; a pcb (128 KiB of rings) is
allocated only for an ACK that matches a cache entry or a valid cookie,
and only while the accept queue is below the backlog. Check:
`net-tcp-syncache` (300 spoofed SYNs: `conns_passive` unchanged, cached
+ cookies = 300; a client then connects; an ACK matching nothing is
`syn_bad_ack`). Gap: no test of cookie expiry across the 8 s slot
boundary.

**N16. A segment resets a connection only at `rcv_nxt`, and never
because it carries SYN.** Elsewhere in the window a RST, any SYN, and an
ACK outside `[snd_una - 65535, snd_max]` earn a rate-limited challenge
ACK and change nothing (RFC 5961). TIME_WAIT ignores RST (RFC 1337) and
restarts only for a retransmitted FIN. Check: `net-tcp-rfc5961` (three
blind segments: `challenge_acks` +3, state ESTABLISHED, `rsts_in`
unchanged; the exact reset is accepted). Gap: the challenge-ACK rate
limit is not driven to its cap.

**N17. This host sends at most `ICMP_RATE_PER_SEC` ICMP replies a
second.** Unreachables and v4/v6 echo replies pass one token bucket;
the excess is counted (`icmp_ratelimited`), not sent. An unreachable
quotes exactly the received IP header and 8 bytes, copied from the
kernel's own copy of the header (options included). Check:
`net-icmp-limit` (300 echo requests in a burst: 100 replies, 200
suppressed). Gap: the quoting path with IP options is reviewed, not
tested.

**N18. A "fragmentation needed" message changes nothing unless it
quotes a segment in flight.** The quoted source must be ours, the quoted
transport TCP, and the quoted sequence number in `[snd_una, snd_max)`;
only then are the connection's MSS lowered and the destination's MTU
recorded (floor 576, 10 min). A forged quote therefore cannot lower the
MSS of future connections to a destination of the sender's choosing.
Check: `net-icmp-limit` (a message quoting a sequence never sent leaves
the MSS and the cache; one quoting `snd_una` lowers the MSS to 1460,
records 1500 and new connections start at 1460; `ipv4_pmtu_flush`
restores). Gap: no test through a real router.

**N19. An operation on a non-blocking object returns instead of
waiting, and `ready` reports exactly what would not block.** Sockets and
pipe ends check the mode before every wait (`-EAGAIN`, or
`-EINPROGRESS`/`-EALREADY` for a stream connect); `kobject_ready` is
computed from the same state the operation tests. Check: `net-nonblock`
(kernel API and object operations), `init --selftest` (system calls),
`lxtest` (Linux `SOCK_NONBLOCK`, `accept4`, `pipe2`, `fcntl`). Gap: no
wait primitive consumes readiness yet (`poll` is Linux stage 3).

**N20. A transport checksum is either verified or vouched for, never
skipped.** On receive TCP and UDP verify unless `M_CSUM_OK`, which only
an interface sets (`lo` for its own packets, virtio-net for frames the
device marked `DATA_VALID` or whose `NEEDS_CSUM` it finished); a driver
never sets it from a header flag it did not negotiate
(`NETIF_CAP_RXCSUM`). On transmit TCP leaves the partial form with
`NET_CSUM_TCP`, and `netif_transmit` finishes it in software unless the
interface has `NETIF_CAP_TXCSUM`, so no packet leaves an interface that
cannot finish it with an unfinished sum; the layers that prepend
headers advance `csum_start`. Check: `net-csum-offload` (the partial
form and its offsets after the IP header, software completion,
`-EINVAL` for bad offsets, a wrong checksum dropped and counted, the
same packet trusted with `M_CSUM_OK`), `net-lo-tcp` and every other
loopback test (offloaded both ways), the host harness (`eth0`: QEMU's
backend offers no offload, so software completion is what goes on the
wire). Gap: the virtio-net header writes and `NEEDS_CSUM` completion run
only where a backend offers the features (not QEMU user-mode), so they
are reviewed, not tested.

**N-L4. A socket woken outside a protocol lock is referenced with
`kobject_tryget` for the wake.** `sock_ref` (TCP) and `udp_input`. Check:
review; `net-lo-udp`, `net-lo-tcp` exercise both.
