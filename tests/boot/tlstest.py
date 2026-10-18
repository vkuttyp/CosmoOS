"""TLS fixtures shared by the tests that serve HTTPS: test certificates made
for the run, and a file server that records what it is asked for.

make_test_certs(d, ip) writes, in `d` (no key is kept in the tree):
  ca.pem          a test CA (this run only)
  server.pem      for IP `ip` and cosmo-test.invalid (and localhost), valid
                  from a day ago for 30 days, signed by the CA
  expired.pem     the same names, valid in 2020 only
  wrongname.pem   for other.invalid only
  untrusted.pem   the server's names, signed by other-ca.pem, a second CA
                  the clients are not given
each NAME.pem with NAME-key.pem.

Server(directory, tls=None, name=None) serves `directory` on 127.0.0.1,
over HTTPS with certificate `name` from make_test_certs' directory `tls`,
else plain HTTP. It records each connection it accepted and each request
(path and Range header) in .connections and .requests, answers ranges
(`bytes=N-`), and takes per-path behaviour: .redirect[path] = location,
.truncate[path] = bytes to send of the body before closing (the full
Content-Length is announced), .chunked (a set of paths), .ignore_range (a
set of paths answered 200 whole). A client that refuses the certificate
fails the handshake in accept, which socketserver drops before any request.
"""
import datetime
import http.server
import os
import shutil
import socketserver
import ssl
import subprocess
import threading


class CertError(Exception):
    pass


def find_openssl():
    """An openssl that can write a certificate valid only in the past
    (`x509 -not_after`, OpenSSL 3.4 and later): $OPENSSL, the one on PATH,
    or Homebrew's (macOS's own is LibreSSL)."""
    for o in (os.environ.get("OPENSSL"), shutil.which("openssl"), "/opt/homebrew/opt/openssl@3/bin/openssl",
              "/usr/local/opt/openssl@3/bin/openssl"):
        if not o or not os.path.exists(o):
            continue
        r = subprocess.run([o, "x509", "-help"], capture_output=True, text=True)
        if "-not_after" in r.stdout + r.stderr:
            return o
    raise CertError("the https step needs OpenSSL 3.4 or later (x509 -not_after); set OPENSSL to one")


def make_test_certs(d, ip="10.0.2.2"):
    o = find_openssl()
    os.makedirs(d, exist_ok=True)

    def run(*argv):
        r = subprocess.run([o, *argv], cwd=d, capture_output=True, text=True)
        if r.returncode != 0:
            raise CertError(f"openssl {argv[0]}: {r.stderr.strip()[-400:]}")

    ec = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes"]
    # Valid from a day ago, as CAs backdate: the guest's clock, read from
    # the RTC at boot, can be a little behind this host's, and a
    # certificate made a moment ago would then start in its future.
    now = datetime.datetime.now(datetime.timezone.utc)
    valid = ["-not_before", (now - datetime.timedelta(days=1)).strftime("%Y%m%d%H%M%SZ"),
             "-not_after", (now + datetime.timedelta(days=30)).strftime("%Y%m%d%H%M%SZ")]
    for ca, cn in (("ca", "CosmoOS test CA (this run only)"), ("other-ca", "CosmoOS untrusted test CA")):
        run("req", "-x509", *ec, "-keyout", ca + "-key.pem", "-out", ca + ".pem", *valid, "-subj", "/CN=" + cn,
            "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    names = f"IP:{ip},DNS:cosmo-test.invalid,DNS:localhost"
    for serial, (name, cn, san, when, ca) in enumerate((
            ("server", "cosmo-test.invalid", names, valid, "ca"),
            ("expired", "cosmo-test.invalid", names, ["-not_before", "20200101000000Z", "-not_after", "20210101000000Z"],
             "ca"),
            ("wrongname", "other.invalid", "DNS:other.invalid", valid, "ca"),
            ("untrusted", "cosmo-test.invalid", names, valid, "other-ca")), start=1):
        with open(os.path.join(d, name + ".ext"), "w") as f:
            f.write(f"subjectAltName={san}\nbasicConstraints=critical,CA:FALSE\nextendedKeyUsage=serverAuth\n")
        run("req", "-new", *ec, "-keyout", name + "-key.pem", "-out", name + ".csr", "-subj", "/CN=" + cn)
        run("x509", "-req", "-in", name + ".csr", "-CA", ca + ".pem", "-CAkey", ca + "-key.pem",
            "-set_serial", str(serial), "-extfile", name + ".ext", "-out", name + ".pem", *when)


def key_pin(tls, name):
    """sha256:HEX of certificate `name`'s public key (DER
    SubjectPublicKeyInfo), the form repos.conf's `pin` and cosmo-fetch -p
    take."""
    o = find_openssl()
    pub = subprocess.run([o, "x509", "-in", os.path.join(tls, name + ".pem"), "-pubkey", "-noout"],
                         capture_output=True, check=True).stdout
    der = subprocess.run([o, "pkey", "-pubin", "-outform", "der"], input=pub, capture_output=True, check=True).stdout
    import hashlib
    return "sha256:" + hashlib.sha256(der).hexdigest()


class _Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):  # noqa: N802 (http.server's name)
        srv = self.server
        path = self.path.split("?", 1)[0]
        with srv.lock:
            srv.requests.append((path, self.headers.get("Range")))
        if path in srv.redirect:
            self.send_response(302)
            self.send_header("Location", srv.redirect[path])
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        local = self.translate_path(path)
        if not os.path.isfile(local):
            self.send_error(404)
            return
        data = open(local, "rb").read()
        start = 0
        rng = self.headers.get("Range")
        if rng and path not in srv.ignore_range and rng.startswith("bytes=") and rng.endswith("-"):
            start = int(rng[6:-1])
            if start >= len(data):
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{len(data)}")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
        else:
            self.send_response(200)
        body = data[start:]
        if path in srv.chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if path in srv.chunked:
            for i in range(0, len(body), 7000):
                piece = body[i:i + 7000]
                self.wfile.write(b"%x\r\n" % len(piece) + piece + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif path in srv.truncate:
            self.wfile.write(body[:srv.truncate[path]])
        else:
            self.wfile.write(body)
        self.close_connection = True


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, directory, tls=None, name=None):
        self.requests, self.connections, self.lock = [], 0, threading.Lock()
        self.redirect, self.truncate, self.chunked, self.ignore_range = {}, {}, set(), set()
        handler = lambda *a, **k: _Handler(*a, directory=directory, **k)  # noqa: E731
        super().__init__(("127.0.0.1", 0), handler)
        if tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(os.path.join(tls, name + ".pem"), os.path.join(tls, name + "-key.pem"))
            self.socket = ctx.wrap_socket(self.socket, server_side=True)
        self.port = self.server_address[1]
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def get_request(self):
        req = super().get_request()   # for TLS, after the handshake
        with self.lock:
            self.connections += 1
        return req

    def handle_error(self, request, client_address):
        # A client that refuses the server after the handshake (a pin)
        # hangs up before its request: that is the case under test.
        import sys
        if not isinstance(sys.exc_info()[1], (ConnectionError, ssl.SSLError)):
            super().handle_error(request, client_address)

    def paths(self):
        with self.lock:
            return [p for p, _ in self.requests]
