"""Host side of the network harness test (docs/kernel-services/network/design.md).

QEMU user-mode networking forwards two host ports to the guest's echo
services on port 7, and the guest connects back to a port this module
listens on (passed through fw_cfg). When the serial log shows the guest
is ready, the harness exchanges TCP and UDP traffic and finally sends
QUIT.
"""

import os
import random
import re
import select
import socket
import sys
import threading
import time


# How long the accepted connection has to deliver the guest's twelve
# bytes. Separate from the accept deadline, and named because the two
# are different questions: whether the guest connected, and whether what
# it sent arrived (docs/audit/next-subsystem-nettest-deadline.md).
BACK_RECV_S = 10

# The guest's request, and the only thing that identifies its connection.
# Nothing else does: every connection to this port arrives from 127.0.0.1
# through QEMU's own socket, so the peer address cannot tell the guest
# from anything else that reaches the port
# (docs/audit/next-subsystem-nettest-accept.md).
BACK_REQUEST = b"cosmo hello\n"
BACK_REPLY = b"cosmo world\n"

# How much of a connection's payload is kept for the failure roster. The
# full byte count is always recorded; this bounds only the preview,
# because a foreign connection may send megabytes and a failure line is
# not a place to put them. Thirty-two matches what the harness already
# kept for the guest's own connection.
BACK_PREVIEW = 32

# How deep the accept queue is. One meant that a connection the harness
# had not yet accepted made the *next* connect stall silently -- the SYN
# dropped, no refusal -- so a stale connection both won the accept and
# blocked the guest's. Measured, not guessed
# (docs/audit/next-subsystem-nettest-accept.md).
BACK_BACKLOG = 8

# How long to keep accepting after every connection that arrived has
# resolved without delivering the request.
#
# The guest makes at most HARNESS_ATTEMPTS back-connection attempts
# (kernel-services/network/nettest.c), each starting once the previous
# one has failed, so once a connection has arrived and died without the
# request and no further attempt arrives within the grace below, waiting
# out the rest of the run's budget cannot help -- and it actively hurts: it leaves no budget for the rest
# of the boot, turning one harness failure into a run-wide timeout with
# every later marker missing. Seen on PR #177's own CI, which gave up at
# 157.0s where the previous harness gave up at 100.9s.
#
# The grace still allows a second connection to arrive after a first one
# closed instantly, which is the case a bare "stop when nothing is live"
# would lose. While *no* connection has ever arrived the full budget
# still applies -- a guest that connects late must still be found, which
# is what the deadline unit established
# (docs/audit/next-subsystem-nettest-deadline.md).
BACK_GRACE_S = BACK_RECV_S

# The write-write-read exchange, both ways (docs/kernel-services/network/testing.md,
# "The host harness"; docs/audit/2026-10-07-delack-nagle-report.md). A request
# is two WWR_HALF-byte halves written as two sends; the answer is the request,
# sent once both halves are in. Host to guest: this module connects to the
# guest's service on port 8 and runs WWR_ROUNDS rounds with the default socket
# (Nagle on) and WWR_ROUNDS more on a second connection with TCP_NODELAY.
# Guest to host: on the back-connection, after the hello, the guest runs the
# same two batches and the host answers in two writes, Nagle on for the first
# batch and TCP_NODELAY for the second. The guest's kernel carries the same
# two constants (kernel-services/network/nettest.c, WWR_HALF/WWR_ROUNDS).
#
# What the figures mean: QEMU's user-mode backend terminates the guest's TCP
# in its own stack, and that stack never holds a small segment for an
# acknowledgement (libslirp tcp_output.c: `(1 || idle || TF_NODELAY)`), so a
# Nagle shape here exercises the HOST kernel's Nagle against the host
# kernel's own delayed acknowledgement on the loopback leg to QEMU -- Linux
# shows its 40 ms there -- and never the guest's. The TCP_NODELAY shapes are
# the path's latency through QEMU and the guest; those are bounded. The
# guest-side interaction is measured where the peer can be built:
# `net-tcp-nagle-peer`.
WWR_HALF = 16
WWR_ROUNDS = 50
# The bound on the TCP_NODELAY shapes' median round trip. A delayed
# acknowledgement in the guest's path would cost 40 ms on every round; a
# median of fifty rounds through QEMU runs a few milliseconds on a loaded CI
# runner.
WWR_BUDGET_S = 0.025

# How long the liveness probe gets. Generous: a slow answer is a reading,
# not a failure, and the exchange has already failed by the time this runs
# (docs/audit/next-subsystem-nettest-probe.md).
PROBE_TIMEOUT_S = 5.0

# Linux TCP states, for byte 0 of TCP_INFO. The distinction the probe is
# for is ESTABLISHED versus CLOSE_WAIT: slirp holding an open socket and
# never forwarding, against slirp having closed its end without sending.
TCP_STATES = {
    1: "ESTABLISHED", 2: "SYN_SENT", 3: "SYN_RECV", 4: "FIN_WAIT1",
    5: "FIN_WAIT2", 6: "TIME_WAIT", 7: "CLOSE", 8: "CLOSE_WAIT",
    9: "LAST_ACK", 10: "LISTEN", 11: "CLOSING",
}


def _tcp_state(sock):
    """This connection's TCP state, or None where it cannot be read.

    Linux only, and only byte 0 of TCP_INFO -- `tcpi_state`. The rest of
    that struct is kernel-specific and reading it would be borrowing
    trouble for no gain. macOS is where this is developed and Linux is
    where CI runs, so None here is ordinary and the four end-classes
    carry the diagnosis on their own.
    """
    # Linux only, and enforced rather than assumed. macOS defines a
    # TCP_INFO whose struct is not Linux's, so byte 0 is not tcpi_state
    # there -- it read FIN_WAIT1 for a plainly established connection,
    # which is the kind of confident wrong answer this unit exists to
    # avoid producing.
    if not sys.platform.startswith("linux"):
        return None
    try:
        raw = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 1)
    except (AttributeError, OSError):
        return None
    return TCP_STATES.get(raw[0], f"state {raw[0]}") if raw else None


def _so_error(sock):
    try:
        return sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
    except OSError:
        return None


def _close(sock):
    """Close a socket and never raise. Called on every exit path."""
    try:
        sock.close()
    except OSError:
        pass


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class NetTest:
    def __init__(self):
        # When the harness started waiting. Every deadline below is
        # reported against this, because the question two weeks of
        # `net-harness` sightings could not answer was *when* the guest
        # connected relative to when the host began listening
        # (docs/audit/next-subsystem-nettest-deadline.md).
        self.t0 = time.monotonic()
        self.tcp_port = free_port()
        self.udp_port = free_port()
        self.back_port = free_port()
        self.wwr_port = free_port()
        self.results = {}
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", self.back_port))
        self.listener.listen(BACK_BACKLOG)
        # Bound and listening now, because the port number goes to QEMU
        # and must be held before QEMU is told about it. *Not* accepting
        # now, and no deadline yet: the guest does not exist. This used
        # to start a thread here with a hard-coded 120-second timeout,
        # which meant the clock ran through the whole boot and the
        # listener closed underneath a guest that connected late
        # (docs/audit/next-subsystem-nettest-deadline.md). The backlog
        # holds the connection until run_when_ready accepts it.

    def env(self):
        return {
            "QEMU_NET_HOSTFWD": f"tcp:127.0.0.1:{self.tcp_port}-:7,udp:127.0.0.1:{self.udp_port}-:7,"
                                f"tcp:127.0.0.1:{self.wwr_port}-:8",
            "QEMU_FWCFG_NETTEST": f"tcp={self.back_port}",
        }

    def _back_server(self, deadline):
        """Find the guest's connection among whatever reaches the port.

        The harness used to listen with a backlog of one and accept
        exactly once, blindly, treating whatever it dequeued first as
        the guest's. It never checked, and when that assumption was
        false it reported `TimeoutError` -- which names nothing. That is
        the whole of what `net-harness` said for three weeks, on both
        architectures, on CI and locally, several times on branches that
        change no code. The tally is kept in `docs/testing/flakes.md`,
        *The count*, and deliberately not repeated here
        (docs/audit/next-subsystem-nettest-accept.md).

        The rule here: **the guest's connection is the one that delivers
        `cosmo hello\n`. Every other connection is evidence, and
        evidence is reported rather than discarded silently.**

        Two deadlines, and they are different questions. The accept
        budget is what is left of the run's; each connection's receive
        budget is BACK_RECV_S measured from *its own* accept, so a guest
        that connects late is not charged for time an earlier intruder
        burned.
        """
        remaining = max(1.0, deadline - time.monotonic())
        self.results["back_wait_s"] = remaining
        accept_deadline = time.monotonic() + remaining

        # Every connection that reaches the port, in arrival order. The
        # roster is the point: a failure where one silent connection
        # arrived is a different defect from one where two arrived and
        # the second carried the request, and the old line could not
        # tell them apart.
        self.back_conns = []
        live = []          # [{sock, rec, buf, recv_deadline}]
        # Every socket this loop accepts, so that a connection dropped
        # from `live` -- by its own deadline, by closing, or by sending
        # the wrong thing -- is still closed. The loop may accept many
        # foreign connections inside its budget, and leaking a
        # descriptor for each would eventually break the `select` that
        # collects the roster.
        opened = []
        winner = None
        data = b""
        # When every connection that had arrived last became resolved.
        # None while something is still live, or while nothing has
        # arrived at all.
        settled_at = None

        try:
            self.listener.setblocking(False)
            while winner is None:
                now = time.monotonic()
                accepting = now < accept_deadline
                for c in live:
                    if now >= c["recv_deadline"]:
                        # Probed before closing: once it is closed there
                        # is nothing left to ask.
                        self._probe_conn(c, "deadline")
                        _close(c["sock"])
                live = [c for c in live if now < c["recv_deadline"]]
                # Keep going while there is still something to wait for:
                # room in the accept budget, or a connection whose own
                # receive budget has not run out.
                if not accepting and not live:
                    break
                # Bounded once the guest has had its one attempt.
                if self.back_conns and not live:
                    if settled_at is None:
                        settled_at = now
                    elif now - settled_at >= BACK_GRACE_S:
                        break
                else:
                    settled_at = None

                watch = [c["sock"] for c in live]
                wake = accept_deadline if accepting else now + 3600.0
                for c in live:
                    wake = min(wake, c["recv_deadline"])
                if settled_at is not None:
                    wake = min(wake, settled_at + BACK_GRACE_S)
                if accepting:
                    watch.append(self.listener)
                try:
                    ready, _, _ = select.select(watch, [], [],
                                                max(0.0, wake - now))
                except (OSError, ValueError):
                    break
                now = time.monotonic()

                for sock in ready:
                    if sock is self.listener:
                        try:
                            conn, peer = self.listener.accept()
                        except OSError:
                            continue
                        conn.setblocking(False)
                        rec = {"peer": f"{peer[0]}:{peer[1]}",
                               "accept_s": now - self.t0,
                               "bytes": 0, "preview": b"",
                               "delivered": False,
                               "ended": None, "errno": None,
                               "state": None, "so_error": None}
                        self.back_conns.append(rec)
                        opened.append(conn)
                        live.append({"sock": conn, "rec": rec, "buf": b"",
                                     "recv_deadline": now + BACK_RECV_S})
                        continue

                    c = next((x for x in live if x["sock"] is sock), None)
                    if c is None:
                        continue
                    # A reset raises here and an orderly FIN does not.
                    # Flattening both into b"" -- which this loop used to
                    # do -- records a reset as a graceful close, and the
                    # failure under investigation involves a reset
                    # (docs/audit/next-subsystem-nettest-probe.md).
                    err = None
                    try:
                        chunk = sock.recv(64)
                    except OSError as e:
                        chunk = b""
                        err = e.errno
                    if not chunk:
                        self._probe_conn(c, "error" if err is not None else "closed",
                                         errno=err)
                        _close(c["sock"])
                        live.remove(c)
                        continue
                    c["buf"] += chunk
                    c["rec"]["bytes"] = len(c["buf"])
                    c["rec"]["preview"] = c["buf"][:BACK_PREVIEW]
                    if c["buf"].endswith(b"\n") or len(c["buf"]) >= 64:
                        if c["buf"] == BACK_REQUEST:
                            c["rec"]["delivered"] = True
                            winner = c
                            break
                        # Answered, but not with the request. Keep the
                        # record, stop reading it, and let it go.
                        self._probe_conn(c, "wrong-data")
                        _close(c["sock"])
                        live.remove(c)

            if winner is not None:
                data = winner["buf"]
                self.results["back_accept_s"] = winner["rec"]["accept_s"]
                sock = winner["sock"]
                sock.setblocking(True)
                sock.settimeout(BACK_RECV_S)
                sock.sendall(BACK_REPLY)
                # The reverse write-write-read exchange rides on this
                # connection: the guest starts it as soon as it has the
                # reply, and the host closes only after serving it.
                self._wwr_host_reply(sock)
                time.sleep(0.2)
            elif self.back_conns:
                # Nothing delivered the request. Report the first
                # connection's accept time, because "when did something
                # arrive" is still the question the timing line answers.
                self.results["back_accept_s"] = self.back_conns[0]["accept_s"]
                # Failure path only: the exchange is already lost, so a
                # probe cannot cost a passing run anything.
                self._probe_slirp()
        except Exception as e:  # noqa: BLE001
            self.results["back_error"] = repr(e)
        finally:
            for sock in opened:
                _close(sock)
            # On every path, including the one that ends without an
            # exception: a peer that connects and closes without sending
            # leaves no exception behind, and an instrument that records
            # nothing there cannot tell silence from a wrong answer.
            self.results["back_request"] = data == BACK_REQUEST
            self.results["back_done_s"] = time.monotonic() - self.t0
            if winner is not None:
                self.results["back_bytes"] = winner["rec"]["bytes"]
                self.results["back_data"] = repr(winner["rec"]["preview"])
            elif self.back_conns:
                self.results["back_bytes"] = self.back_conns[0]["bytes"]
                self.results["back_data"] = repr(self.back_conns[0]["preview"])
            else:
                self.results["back_bytes"] = 0
                self.results["back_data"] = repr(b"")
            self.results["back_roster"] = self.roster()
            try:
                self.listener.close()
            except OSError:
                pass
            self.results["back_closed_s"] = time.monotonic() - self.t0

    @staticmethod
    def _recv_exactly(sock, n):
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise EOFError(f"peer closed after {len(buf)} of {n} bytes")
            buf += chunk
        return buf

    def _wwr_host_reply(self, sock):
        """Answer the guest's write-write-read rounds on the back-connection.

        Two batches of WWR_ROUNDS: the reply goes out as two writes, the
        first batch on the socket as accepted (Nagle on), the second with
        TCP_NODELAY. The guest times each round and prints the two
        distributions (`NETTEST: wwr guest-client ...`); this side only
        counts what it served, so a short exchange names the round.
        """
        served = 0
        try:
            for batch in range(2):
                if batch == 1:
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                for _ in range(WWR_ROUNDS):
                    req = self._recv_exactly(sock, 2 * WWR_HALF)
                    sock.sendall(req[:WWR_HALF])
                    sock.sendall(req[WWR_HALF:])
                    served += 1
        except Exception as e:  # noqa: BLE001
            self.results["wwr_reverse_error"] = repr(e)
        self.results["wwr_reverse_served"] = served

    def _wwr_client(self):
        """Host-driven write-write-read against the guest's port-8 service.

        One connection per mode: the default socket (Nagle on) and one with
        TCP_NODELAY. Each runs WWR_ROUNDS rounds -- two sends of WWR_HALF
        bytes, then the 2*WWR_HALF-byte answer -- and keeps every round's
        time; the distributions are printed by latency() and the NODELAY
        median is bounded by failures().
        """
        half = bytes([0x77]) * WWR_HALF
        for mode in ("nagle", "nodelay"):
            rounds = []
            try:
                s = socket.create_connection(("127.0.0.1", self.wwr_port), timeout=20)
                s.settimeout(20)
                if mode == "nodelay":
                    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                for _ in range(WWR_ROUNDS):
                    t0 = time.monotonic()
                    s.sendall(half)
                    s.sendall(half)
                    reply = self._recv_exactly(s, 2 * WWR_HALF)
                    if reply != half + half:
                        raise ValueError(f"reply {reply!r}")
                    rounds.append(time.monotonic() - t0)
                s.close()
            except Exception as e:  # noqa: BLE001
                self.results[f"wwr_h2g_{mode}_error"] = repr(e)
            self.results[f"wwr_h2g_{mode}"] = sorted(rounds)

    def latency(self):
        """One line with the host-driven write-write-read distributions."""
        parts = []
        for mode in ("nagle", "nodelay"):
            r = self.results.get(f"wwr_h2g_{mode}")
            if not r:
                parts.append(f"{mode}: no rounds ({self.results.get(f'wwr_h2g_{mode}_error', 'not run')})")
                continue
            n = len(r)
            parts.append("%s: %d rounds min %.1f ms, p50 %.1f ms, p90 %.1f ms, max %.1f ms"
                         % (mode, n, r[0] * 1e3, r[n // 2] * 1e3, r[n * 9 // 10] * 1e3, r[-1] * 1e3))
        served = self.results.get("wwr_reverse_served")
        return ("network harness: write-write-read host->guest, " + "; ".join(parts) +
                f"; guest->host rounds served {served if served is not None else '-'} of {2 * WWR_ROUNDS}")

    @staticmethod
    def guest_failures(lines):
        """The guest's half of the exchange, judged from its serial lines.

        The guest prints one distribution per batch (`NETTEST: wwr
        guest-client host-nagle=on|off ...`, microseconds). Both lines are
        required, and the `TCP_NODELAY` batch's median is bounded like the
        host-driven one: it is the same path the other way, and a delayed
        acknowledgement anywhere on it would cost every round 40 ms. The
        Nagle batch is the host kernel's own behaviour and is reported only.
        """
        f = []
        for mode in ("on", "off"):
            pat = re.compile(r"^NETTEST: wwr guest-client host-nagle=%s rounds=(\d+) min=(\d+) p50=(\d+) p90=(\d+) max=(\d+) us"
                             % mode)
            hits = [pat.match(ln) for ln in lines]
            hits = [m for m in hits if m]
            if not hits:
                f.append(f"missing marker /{pat.pattern}/ (network harness: the guest's write-write-read batch)")
                continue
            rounds, _mn, p50, _p90, _mx = (int(g) for g in hits[-1].groups())
            if rounds != WWR_ROUNDS:
                f.append(f"network harness: write-write-read guest->host (host-nagle={mode}) ran {rounds} of {WWR_ROUNDS} rounds")
            if mode == "off" and p50 > WWR_BUDGET_S * 1e6:
                f.append("network harness: write-write-read guest->host (host TCP_NODELAY) median %.1f ms over the %.0f ms budget"
                         % (p50 / 1e3, WWR_BUDGET_S * 1e3))
        return f

    def _probe_conn(self, c, ended, errno=None):
        """Record how one connection ended, and its state while it can be read.

        Called from every path that stops watching a connection, and
        always *before* the socket is closed. The end class is the
        portable half: `closed` is an orderly FIN, `error` is a reset or
        another receive failure with its errno kept, `deadline` is the
        budget expiring with the connection open and silent, and
        `wrong-data` is a peer that answered with something else. Those
        were all `0 byte(s)` in the roster before this unit, so no
        sighting could say which had happened
        (docs/audit/next-subsystem-nettest-probe.md).

        Nothing is written to the socket. A one-byte write was the first
        design and it cannot discriminate: a write into `CLOSE_WAIT`
        succeeds, so an open peer and a closed one both look alive.
        """
        rec = c["rec"]
        if rec.get("ended") is not None:
            return                      # first reason wins
        rec["ended"] = ended
        rec["errno"] = errno
        rec["state"] = _tcp_state(c["sock"])
        rec["so_error"] = _so_error(c["sock"])

    def _probe_slirp(self):
        """Was the path through slirp answering at all, just now?

        Connect to the guest's echo service through the same slirp
        instance and time the connect and a one-byte round trip,
        separately.

        **The reading is asymmetric.** This traverses slirp *and* the
        guest -- which must accept the forwarded connection, schedule its
        echo thread and reply -- so a slow answer does not single out
        QEMU's main loop; guest-side delay looks identical. A *fast*
        answer is the informative one: it rules out the whole path having
        stalled, which is the only mechanism still standing after the
        foreign-connection and retransmission theories died.
        """
        t0 = time.monotonic()
        try:
            s = socket.create_connection(("127.0.0.1", self.tcp_port),
                                         timeout=PROBE_TIMEOUT_S)
        except OSError as e:
            self.results["probe_slirp"] = f"connect failed after {time.monotonic()-t0:.2f}s: {e!r}"
            return
        connect_s = time.monotonic() - t0
        try:
            s.settimeout(PROBE_TIMEOUT_S)
            t1 = time.monotonic()
            s.sendall(b"p")
            echoed = s.recv(4)
            echo_s = time.monotonic() - t1
            self.results["probe_slirp"] = (
                f"connect {connect_s*1000:.0f} ms, echo {echo_s*1000:.0f} ms"
                f"{'' if echoed == b'p' else f' (echoed {echoed!r})'}")
        except OSError as e:
            self.results["probe_slirp"] = (
                f"connect {connect_s*1000:.0f} ms, then echo failed: {e!r}")
        finally:
            _close(s)

    def roster(self):
        """Every connection that reached the port, as one line.

        This is the sentence every sighting needed and none had: the
        old failure said "connection accepted at 90.9s" -- a time
        without an identity. How many there were is
        `docs/testing/flakes.md`, *The count*.
        """
        conns = getattr(self, "back_conns", [])
        if not conns:
            return "no connection arrived"
        parts = []
        for c in conns:
            how = c.get("ended") or "still open"
            if c.get("errno") is not None:
                how += f" errno {c['errno']}"
            if c.get("state"):
                how += f", {c['state']}"
            if c.get("so_error"):
                how += f", SO_ERROR {c['so_error']}"
            parts.append("%s accepted at %.1fs, %d byte(s)%s: %r [%s]"
                         % (c["peer"], c["accept_s"], c["bytes"],
                            " [the request]" if c["delivered"] else "",
                            c["preview"], how))
        line = f"{len(conns)} connection(s): " + "; ".join(parts)
        probe = self.results.get("probe_slirp")
        return line + (f"; slirp probe: {probe}" if probe else "")

    def run_when_ready(self, log_path, proc, timeout):
        """Wait for the guest's ready line, then run the exchange."""
        self.results["budget_s"] = timeout
        deadline = time.monotonic() + timeout
        ready = False
        while time.monotonic() < deadline and proc.poll() is None:
            try:
                with open(log_path, "rb") as f:
                    if b"NETTEST: ready" in f.read():
                        ready = True
                        break
            except OSError:
                pass
            time.sleep(0.2)
        self.results["ready"] = ready
        self.results["ready_s"] = time.monotonic() - self.t0
        if not ready:
            return
        # The guest prints readiness and *then* connects back, waiting
        # for the reply before it serves anything, so this must come
        # before the echo exchanges below.
        self._back_server(deadline)
        time.sleep(0.3)
        self._tcp_echo()
        self._udp_echo()
        self._wwr_client()
        self._quit()

    def _tcp_echo(self):
        try:
            s = socket.create_connection(("127.0.0.1", self.tcp_port), timeout=20)
            s.settimeout(20)
            rng = random.Random(1234)
            payload = bytes(rng.getrandbits(8) for _ in range(256 * 1024))
            received = bytearray()
            sent = 0

            def reader():
                nonlocal received
                while len(received) < len(payload):
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    received += chunk

            t = threading.Thread(target=reader, daemon=True)
            t.start()
            while sent < len(payload):
                n = min(rng.randint(1, 9000), len(payload) - sent)
                s.sendall(payload[sent:sent + n])
                sent += n
            t.join(60)
            self.results["tcp_echo"] = bytes(received) == payload
            s.close()
        except Exception as e:  # noqa: BLE001
            self.results["tcp_echo"] = False
            self.results["tcp_error"] = repr(e)

    def _udp_echo(self):
        ok = 0
        try:
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u.settimeout(2)
            for i in range(20):
                msg = f"cosmo udp {i} ".encode() + bytes(range(i * 7 % 256, i * 7 % 256 + 40))
                u.sendto(msg, ("127.0.0.1", self.udp_port))
                try:
                    data, _ = u.recvfrom(4096)
                    if data == msg:
                        ok += 1
                except socket.timeout:
                    pass
            u.close()
        except Exception as e:  # noqa: BLE001
            self.results["udp_error"] = repr(e)
        self.results["udp_echo"] = ok
        self.results["udp_ok"] = ok >= 18   # QEMU user-mode may lose a datagram or two

    def _quit(self):
        try:
            s = socket.create_connection(("127.0.0.1", self.tcp_port), timeout=10)
            s.sendall(b"QUIT")
            time.sleep(0.2)
            s.close()
            self.results["quit_sent"] = True
        except Exception as e:  # noqa: BLE001
            self.results["quit_sent"] = False
            self.results["quit_error"] = repr(e)

    def timing(self):
        """One line about the deadlines, printed whether or not it failed.

        The margin is the subject: `net-harness` has been re-run for a
        fortnight without anyone being able to say how close the guest's
        back-connection came to the deadline, because nothing recorded
        it (docs/audit/next-subsystem-nettest-deadline.md).
        """
        r = self.results
        def t(k):
            v = r.get(k)
            return f"{v:.1f}s" if isinstance(v, float) else "-"
        return ("network harness: ready at %s, back-connection accepted at %s, "
                "budget %s, listener closed at %s"
                % (t("ready_s"), t("back_accept_s"), t("budget_s"), t("back_closed_s")))

    def failures(self):
        f = []
        r = self.results
        if not r.get("ready"):
            f.append("network harness: guest never reported NETTEST: ready")
            return f
        if not r.get("tcp_echo"):
            f.append(f"network harness: TCP echo mismatch ({r.get('tcp_error', 'data differs')})")
        if not r.get("udp_ok"):
            f.append(f"network harness: UDP echo returned {r.get('udp_echo', 0)}/20 ({r.get('udp_error', '')})")
        if not r.get("back_request"):
            def t(k):
                v = r.get(k)
                return f"{v:.1f}s" if isinstance(v, float) else "never"
            # The roster, not a bare accept time. "connection accepted
            # at 90.9s" was a time without an identity, and answering
            # *which* connection that was is the whole of this unit
            # (docs/audit/next-subsystem-nettest-accept.md).
            where = r.get("back_roster") or self.roster()
            f.append(
                "network harness: guest-initiated connection failed "
                f"({r.get('back_error', 'no error, the request was wrong or absent')}) — "
                f"listening on 127.0.0.1:{self.back_port}; {where}; "
                f"gave up at {t('back_done_s')}, guest reported ready at {t('ready_s')}, "
                f"accept budget {t('back_wait_s')}, "
                f"recv budget {BACK_RECV_S}.0s per connection from its own accept"
            )
        if not r.get("quit_sent"):
            f.append("network harness: could not send QUIT")
        for mode in ("nagle", "nodelay"):
            rounds = r.get(f"wwr_h2g_{mode}") or []
            if len(rounds) != WWR_ROUNDS:
                f.append(f"network harness: write-write-read host->guest ({mode}) completed "
                         f"{len(rounds)} of {WWR_ROUNDS} rounds ({r.get(f'wwr_h2g_{mode}_error', '')})")
        nodelay = r.get("wwr_h2g_nodelay") or []
        if len(nodelay) == WWR_ROUNDS and nodelay[WWR_ROUNDS // 2] > WWR_BUDGET_S:
            f.append("network harness: write-write-read host->guest (nodelay) median %.1f ms over the %.0f ms budget"
                     % (nodelay[WWR_ROUNDS // 2] * 1e3, WWR_BUDGET_S * 1e3))
        if r.get("wwr_reverse_served") != 2 * WWR_ROUNDS:
            f.append(f"network harness: write-write-read guest->host served {r.get('wwr_reverse_served')} "
                     f"of {2 * WWR_ROUNDS} rounds ({r.get('wwr_reverse_error', '')})")
        return f
