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
        self.tcp_port = free_port()
        self.udp_port = free_port()
        self.back_port = free_port()
        self.results = {}
        self.back_thread = threading.Thread(target=self._back_server, daemon=True)
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", self.back_port))
        self.listener.listen(1)
        self.listener.settimeout(120)
        self.back_thread.start()

    def env(self):
        return {
            "QEMU_NET_HOSTFWD": f"tcp:127.0.0.1:{self.tcp_port}-:7,udp:127.0.0.1:{self.udp_port}-:7",
            "QEMU_FWCFG_NETTEST": f"tcp={self.back_port}",
        }

    def _back_server(self):
        try:
            conn, _ = self.listener.accept()
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
        finally:
            self.listener.close()

    def run_when_ready(self, log_path, proc, timeout):
        """Wait for the guest's ready line, then run the exchange."""
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
        if not ready:
            return
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
            f.append(f"network harness: guest-initiated connection failed ({r.get('back_error', 'bad request')})")
        if not r.get("quit_sent"):
            f.append("network harness: could not send QUIT")
        return f
