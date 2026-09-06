# Networking: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| Target, loopback | The self-tests below, since unit 11 also `net-steer`, `net-csum-offload` and `net-bench`: `net-mbuf`, `net-cksum`, `net-arp`, `net-lo-udp`, `net-lo-tcp`, `net-lo-tcp-loss`, `net-tcp-mss` (the path MSS is decided outside the TCP lock: loopback and own addresses give `TCP_MSS_LO`, the gateway `TCP_MSS_V4`, and both ends of a loopback connection settle on `TCP_MSS_LO`), `net-netif-lifetime` (a synthetic interface: registry and lookup references, `netif_unregister` stops transmit and receive, the release runs once after the last put) and `net-accept-race` (64 accepts against a client that connects and drops at once; every child names its socket when accept returns) | `make test` |
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
the server sends; non-blocking sends fill the rings and end in
`-EAGAIN` with `WRITABLE` clear, the server drains exactly that many
bytes and `WRITABLE` returns; closing the server end makes the client
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
