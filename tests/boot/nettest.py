"""Host side of the network harness test (docs/kernel-services/network/design.md).

QEMU user-mode networking forwards two host ports to the guest's echo
services on port 7, and the guest connects back to a port this module
listens on (passed through fw_cfg). When the serial log shows the guest
is ready, the harness exchanges TCP and UDP traffic and finally sends
QUIT.
"""

import os
import random
import socket
import threading
import time


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
        self.listener.listen(1)
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
        # Whatever is left of the run's budget, which is the same budget
        # the readiness wait drew on: one deadline for the exchange,
        # derived from --timeout, started when the run started.
        remaining = max(1.0, deadline - time.monotonic())
        self.listener.settimeout(remaining)
        self.results["back_wait_s"] = remaining
        try:
            conn, _ = self.listener.accept()
            self.results["back_accept_s"] = time.monotonic() - self.t0
            conn.settimeout(10)
            data = b""
            while not data.endswith(b"\n") and len(data) < 64:
                chunk = conn.recv(64)
                if not chunk:
                    break
                data += chunk
            self.results["back_request"] = data == b"cosmo hello\n"
            conn.sendall(b"cosmo world\n")
            time.sleep(0.2)
            conn.close()
        except Exception as e:  # noqa: BLE001
            self.results["back_error"] = repr(e)
            self.results["back_gaveup_s"] = time.monotonic() - self.t0
        finally:
            self.listener.close()
            self.results["back_closed_s"] = time.monotonic() - self.t0

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
            f.append(
                "network harness: guest-initiated connection failed "
                f"({r.get('back_error', 'bad request')}) — "
                f"listening on 127.0.0.1:{self.back_port}, gave up "
                f"{r.get('back_gaveup_s', float('nan')):.1f}s after the harness started, "
                f"guest reported ready at {r.get('ready_s', float('nan')):.1f}s"
            )
        if not r.get("quit_sent"):
            f.append("network harness: could not send QUIT")
        return f
