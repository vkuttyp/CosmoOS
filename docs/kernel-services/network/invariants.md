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
runs the handler (`pcb_work`), which drops the reference (an item is
claimed atomically, so two timers of one pcb firing on two CPUs link it
once and the second caller's reference is dropped at once); input and
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

**N21. A socket's pending error is never lost, never invented, and
cleared exactly once — by the reader it reached.** Two of those are
absolute; the third is what "once" means here, and it is worth stating
plainly because the two accessors differ. `ksock_error`, which every
in-kernel caller uses, hands the verdict to **exactly one** caller: the
clear is part of the read. The syscall pair is deliberately *at least*
once — a delivery that can fail must not clear before it has succeeded,
so two callers racing may both be told the same true verdict and only
the one whose token matches clears it. Being told the truth twice is not
a failure mode worth excluding at the price of the two that are: losing
a verdict nobody was told, and telling a caller there is no error while
one is pending. `struct socket::error` is written only by
`sock_set_error` and read only by `ksock_error`, both with atomic
operations and **neither holding `s->lock`**: the writer runs in
packet-receive context, where that mutex cannot be taken, and of the five
readers three run inside `mutex_lock(&s->lock)` (`ksock_connect`'s
completion paths, `socket.c:267`, `:282`, `:299`) while two do not
(`ksock_accept`, `:224`; UDP's `ksock_recvfrom`, `:372`) — so the mutex
cannot be the field's rule, and it is not. `ksock_error`'s read clears by
compare-exchange, which is what makes "once" true against two readers
rather than merely likely; `ksock_ready`'s `COSMO_IO_ERROR` and
the wait conditions *test* the field without clearing it. A stream
socket's `pcb->error` is reported without clearing, because a dead
connection must keep failing — the two halves have different rules on
purpose and a reader should not assume one. That sticky half has a rule
of its own, for the same reason: two of its readers run without
`pcb->lock` (`ksock_error`, and `output_result`'s fast path), so every
write to it is an `__atomic_store_n` under that lock and every unlocked
read an atomic load (`tcp.h`, `error`). A reader that *holds* the lock
may read it plainly.

A verdict that could not be delivered was not delivered. Both ABI entry
points settle every refusal — the length word, the size, the user range —
before reading, and then do **not** clear until the value has reached the
caller: `ksock_error_peek` reports without clearing and hands back an opaque
token, and `ksock_error_delivered` commits afterwards only if that token
still names what is there — because a range check is not a promise that
a page is writable or that it stays mapped. The field is therefore one
64-bit word: the low half an errno, the high half a generation every
write bumps. A commit that compared the errno alone would clear a
*second* verdict of the same value that arrived during the copy and had
been told to nobody, and two ICMP messages about one flow carry the same
errno readily. Taking the
value and putting it back on failure would be wrong in a way that is
worth writing down, since it is what this unit did first: between the
take and the restore, a concurrent asker is told **0** while a verdict is
pending and undelivered, which is a worse answer than any this pair can
give. Two askers racing here are both told the truth and one of them
clears it, which is the *at least once* half of the rule above. The commit
is a compare-exchange on the delivered value, so a newer verdict that
arrived during the copy is not destroyed by it. An ICMP message sets an error
only when its quoted four-tuple belongs to a **connected** socket of this
host's: an unconnected socket has no flow for a message to be about, and
admitting one would let anything on the path kill a socket by quoting a
plausible port (RFC 5927 — the bar N16 sets for a reset and N18 for a
path-MTU message). Check: `net-sockerr-udp` (`ECONNREFUSED` and
`EHOSTUNREACH` arrive, each delivered exactly once through `ksock_error`,
`COSMO_IO_ERROR` raised and then cleared), `net-sockerr-spoof` (six messages differing from the
delivering one in a single field of the quoted four-tuple change
nothing — counted, so a frame that was never injected fails the test
rather than quietly reducing six cases to five), `net-sockerr-accept` (a pending error reaches `accept` as an
errno, and `accept` never reports success without a socket),
`net-sockerr-locking` (the same answer with the mutex held and without
it), `lxtest` and `usertest` (`SO_ERROR` through both ABI doors, positive
as POSIX asks). Gap: TCP takes no ICMP hard error at all, by decision —
RFC 1122 §4.2.3.9 forbids aborting a connection on a soft one and
`pcb->error` already carries the verdict the segments themselves give —
so an errno only ICMP could supply never reaches a stream socket.

**N23. A tap's reader is woken by every frame `tap_transmit` queues,
and a reader blocked in the tap's read holds the file and therefore the
tap, so the tap's release never runs under one.** `tap_transmit` wakes
`rx_wait` after `mbufq_enqueue` succeeds (and only then: a dropped frame
wakes nobody, there is nothing to read); `tap_recv_wait` re-checks the
queue's length under the wait's own discipline and dequeues again after
a wake, so two readers racing for one frame leave one waiting rather
than one holding NULL. The release path (`tap_chr_release`) runs from
`file_release`, which runs when the file's last reference drops, and a
read in progress holds a reference the system call took from
`handle_lookup` -- so the only ways out of a blocked read are a frame
or the kill that returns `-EINTR`, and closing the handle from another
thread of the process drops a reference the read does not hold.
**Checked by** `tap-ready`: a thread's blocking read waits 30 ms with
nothing queued and returns the frame a `netif_transmit` queues; `io_poll`
on the file times out at 20 ms and returns `READABLE` on a transmit; a
process blocked in its own tap's read is killed, exits 137, and `tap0`
exists until after it exits and not after that. Its mutations: the wake
removed (the reader and the poll both time out), the read never waiting
(the thread's read returns before the transmit), the wait made unkillable
(the killed process never exits).

**N22. A `struct netif *` that outlives the lock that found it holds a
reference.** ARP and ND entries store the interface a resolution is for
(`arp.c`, `ipv6.c`). Their ageing passes copy that pointer into a retry
array under the table lock, release the lock, and then dereference it —
`send_arp` reads `nif->mac` and `nif->ip4.addr`, `nd_send` the same. Both
now take `netif_get` as they copy and `netif_put` after the send.

**Taking the reference under the table lock is what makes it sound**, and
the order matters: `arp_flush`/`nd_flush` take the same lock, so an entry
present under it means `netif_unregister` step 5 has not run for that
interface — and step 6's `kobject_put` is after step 5. The registry's
reference is therefore still held, so the get cannot resurrect a dying
object.

**What the reference replaces is not the flush.** Before it, the safety
of the retry rested on nothing at all: step 4's per-CPU barrier drains an
`age_work` already queued, but `age_work` re-arms every `ARP_RETRY_NS`,
so a retry could begin *after* the barrier, copy the pointer, and read it
after step 6 freed the interface. Its own comment says the barrier is
for `input_one` — the receive path — and it is. The retry survived by the
timer not having fired.

**Why entries do not hold a reference each.** The flush already clears
them, so a per-entry reference would keep an interface alive until the
next ageing pass rather than fixing anything, and would turn a missing
flush from a dangling pointer into a leak. The dangling pointer is the
defect.

**The other holders, from the sweep this invariant is the result of.**
`tapsvc` keeps a `struct netif *` for its service's lifetime and takes no
reference; it is safe by explicit ordering — `tapsvc_stop` runs
immediately before `tap_destroy` in the one teardown path
(`tap.c`) — and that ordering, not a reference, is what a second teardown
path would have to preserve. `nettest.c`'s two test fixtures hold one
within the scope of a test that owns the interface.

**Asserted by** `net-arp-retry-unregister` and `net-nd-retry-unregister`,
which park a retry in that one-unlock window and run `netif_unregister`
to completion against it: the driver's release must not have run, and the
reference count must show the retry's hold. Without the `netif_get` the
count assertion fails immediately, which is the reference's absence
stated directly rather than a crash hoped for.

**N24. The operator's flow listing shows exactly what the shares count.**
A NAT entry or a firewall flow is listed (`nat_flow_list`,
`fw_flow_list`, and through them the `/dev/net/tapctl` snapshot's flow
section) exactly when it is in use and not expired at the read's one
`now`. That is the test `nat_guest_count` and `flow_slot` apply when they
decide a share is full. An expired entry that has not been reaped is not
listed: it no longer holds a share, and listing it would contradict the
refusal the operator is reading the listing to explain. Each table is
copied in one hold of its own lock, so a table's list is one instant. The
refusal counters are read beside it, not under it, so they are exact over
time and approximate to the instant. **Checked by** `net-flows-nat` and
`net-flows-fw`:
- a guest flooded past its share is listed at exactly the share;
- a spent firewall share's flows are listed at exactly the share when the
  next one is refused;
- a listing taken past the timeout, with nothing aged, is empty while the
  entries are still in use.

Its mutations: either listing ignoring `expires_ns`.
