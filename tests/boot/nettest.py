"""Host side of the network harness test (docs/kernel-services/network/design.md).

QEMU user-mode networking forwards two host ports to the guest's echo
services on port 7, and the guest connects back to a port this module
listens on (passed through fw_cfg). When the serial log shows the guest
is ready, the harness exchanges TCP and UDP traffic and finally sends
QUIT.
"""

import os
import random
import select
import socket
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
# The guest makes exactly one back-connection attempt and never retries
# (kernel-services/network/nettest.c), so once a connection has arrived
# and died without the request, waiting out the rest of the run's budget
# cannot help -- and it actively hurts: it leaves no budget for the rest
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
            "QEMU_NET_HOSTFWD": f"tcp:127.0.0.1:{self.tcp_port}-:7,udp:127.0.0.1:{self.udp_port}-:7",
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
                               "delivered": False}
                        self.back_conns.append(rec)
                        opened.append(conn)
                        live.append({"sock": conn, "rec": rec, "buf": b"",
                                     "recv_deadline": now + BACK_RECV_S})
                        continue

                    c = next((x for x in live if x["sock"] is sock), None)
                    if c is None:
                        continue
                    try:
                        chunk = sock.recv(64)
                    except OSError:
                        chunk = b""
                    if not chunk:
                        # Connected, said nothing, went away. Not an
                        # exception, and recorded rather than dropped.
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
                        _close(c["sock"])
                        live.remove(c)

            if winner is not None:
                data = winner["buf"]
                self.results["back_accept_s"] = winner["rec"]["accept_s"]
                sock = winner["sock"]
                sock.setblocking(True)
                sock.settimeout(BACK_RECV_S)
                sock.sendall(BACK_REPLY)
                time.sleep(0.2)
            elif self.back_conns:
                # Nothing delivered the request. Report the first
                # connection's accept time, because "when did something
                # arrive" is still the question the timing line answers.
                self.results["back_accept_s"] = self.back_conns[0]["accept_s"]
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
            parts.append("%s accepted at %.1fs, %d byte(s)%s: %r"
                         % (c["peer"], c["accept_s"], c["bytes"],
                            " [the request]" if c["delivered"] else "",
                            c["preview"]))
        return f"{len(conns)} connection(s): " + "; ".join(parts)

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
        return f
