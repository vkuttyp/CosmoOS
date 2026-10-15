#!/usr/bin/env python3
"""Roadmap M4's acceptance test: from the installed system, through QEMU
user networking, DHCP configures the interface, wget fetches by address
and by hostname (docs/userland/testing.md, "test-net-login").

    net_login_test.py --image cosmoos.img --workdir DIR

The live image is installed on a blank disk as test-install does
(install_test.stage_install), then the installed disk is booted alone with
QEMU user networking and no fw_cfg address, so the kernel's DHCP client
configures it:

  a. DHCP: eth0 is bound to 10.0.2.15/24 with gateway 10.0.2.2 and DNS
     10.0.2.3 (`netcfg show`), and /etc/resolv.conf names 10.0.2.3.
  b. By address: wget fetches a 1 MiB file from this harness's HTTP server
     at 10.0.2.2:<port> (slirp's alias for the host), and its SHA-256 on
     the guest is the file's.
  c. By name: `nslookup example.com` answers through slirp's resolver, and
     `wget -O - http://example.com/` returns a body with "Example Domain"
     -- up to three attempts, since this needs the runner's internet;
     and `wget -O - https://example.com/` the same, its certificate
     checked against the installed CA bundle.
  f. https (the https unit): wget fetches the 1 MiB file over https from
     this harness, its certificate from a test CA made for the run, and
     refuses an untrusted, an expired and a wrongly named certificate,
     ssl_client naming why; --no-check-certificate fetches anyway.

And on the same boot, job control in ash on the console (PR 2 of M4):
`&`, `jobs`, `kill %1`, `^Z` and `bg`. Then ssh (PR 3), with the guest's
port 22 forwarded to a host port:

  d. The host's OpenSSH client logs in as root with a key made for this
     run (its public half typed into /root/.ssh/authorized_keys at the
     console), runs `uname -a; echo ok`, and logs in with an RSA key
     (3072 bits) put there beside it; then a session with a PTY
     (ssh -tt, under a host pty of a known size) where `tty` names
     /dev/pts/N, `stty size` is the client's window, and job control works
     (sleep 100 &, jobs, kill %1, wait).
  e. The installed system reboots; the second login passes the client's
     known_hosts check (StrictHostKeyChecking=yes): the host key was kept.
     It also keeps the host name (g), from /etc/hostname: this boot's
     DHCP server names nothing.
  g. Network inspection (shell completeness S1), on the net boot, whose
     DHCP server names the machine (option 12, QEMU_NET_HOSTNAME): `ip a`
     shows eth0 with its DHCP address, `ip r` the default and the subnet,
     `ip neigh` the gateway; BusyBox `ifconfig`, `route -n`, `netstat
     -tuln` (dropbear on :22), `arp -a` (the gateway) and `hostname` print
     the same machine; `ifconfig eth0 down` fails with EPERM with PRIV_NET
     dropped, then works as root, and `up` brings the link back (a ping of
     the gateway answers).

Exit 0 PASS, 1 FAIL.
"""

import argparse
import datetime
import fcntl
import hashlib
import http.server
import os
import pty
import re
import select
import shutil
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import termios
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_test  # noqa: E402  (Boot, Fail, need, stage_install)

from install_test import Boot, Fail, need  # noqa: E402

BLOB_SIZE = 1 << 20
HOSTNAME = "cosmo-s1"   # what slirp's DHCP server names the machine on the net boot (option 12)
MAC = "52:54:00:c0:5f:05"   # scripts/qemu-run.sh's virtio-net


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass


def start_http(directory):
    """An HTTP server on 127.0.0.1 (the guest's 10.0.2.2) serving `directory`."""
    handler = lambda *a, **k: QuietHandler(*a, directory=directory, **k)  # noqa: E731
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


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
    raise Fail("the https step needs OpenSSL 3.4 or later (x509 -not_after); set OPENSSL to one")


def make_test_certs(d):
    """A test CA and three server certificates it signs, made for this run
    in `d` (no key is kept in the tree): `server` for 10.0.2.2 and
    cosmo-test.invalid, valid from yesterday for 30 days; `expired`, the same names, valid in
    2020 only; `wrongname`, for other.invalid only. Each NAME.pem with
    NAME-key.pem; the CA is ca.pem."""
    o = find_openssl()
    os.makedirs(d, exist_ok=True)

    def run(*argv):
        r = subprocess.run([o, *argv], cwd=d, capture_output=True, text=True)
        if r.returncode != 0:
            raise Fail(f"openssl {argv[0]}: {r.stderr.strip()[-400:]}")

    ec = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes"]
    # Valid from a day ago, as CAs backdate: the guest's clock, read from
    # the RTC at boot, can be a little behind this host's, and a
    # certificate made a moment ago would then start in its future.
    now = datetime.datetime.now(datetime.timezone.utc)
    valid = ["-not_before", (now - datetime.timedelta(days=1)).strftime("%Y%m%d%H%M%SZ"),
             "-not_after", (now + datetime.timedelta(days=30)).strftime("%Y%m%d%H%M%SZ")]
    run("req", "-x509", *ec, "-keyout", "ca-key.pem", "-out", "ca.pem", *valid,
        "-subj", "/CN=CosmoOS test CA (this run only)",
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    for serial, (name, cn, san, when) in enumerate((
            ("server", "cosmo-test.invalid", "IP:10.0.2.2,DNS:cosmo-test.invalid", valid),
            ("expired", "cosmo-test.invalid", "IP:10.0.2.2,DNS:cosmo-test.invalid",
             ["-not_before", "20200101000000Z", "-not_after", "20210101000000Z"]),
            ("wrongname", "other.invalid", "DNS:other.invalid", valid)), start=1):
        with open(os.path.join(d, name + ".ext"), "w") as f:
            f.write(f"subjectAltName={san}\nbasicConstraints=critical,CA:FALSE\nextendedKeyUsage=serverAuth\n")
        run("req", "-new", *ec, "-keyout", name + "-key.pem", "-out", name + ".csr", "-subj", "/CN=" + cn)
        run("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca-key.pem",
            "-set_serial", str(serial), "-extfile", name + ".ext", "-out", name + ".pem", *when)


def start_https(directory, tls, name):
    """An HTTPS server on 127.0.0.1 serving `directory` with the test
    certificate `name` from make_test_certs' directory `tls`. A client
    that refuses the certificate fails the handshake in accept, which
    socketserver drops, as an OSError, before serving."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(tls, name + ".pem"), os.path.join(tls, name + "-key.pem"))
    handler = lambda *a, **k: QuietHandler(*a, directory=directory, **k)  # noqa: E731
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler)
    srv.daemon_threads = True
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def stage_https(b, results, port, www, want, work):
    """wget over https (the https unit): /bin/ssl_client checks the server's
    certificate. The harness serves three test certificates, all for
    10.0.2.2 but one, from a test CA the guest fetches over http and names
    in SSL_CERT_FILE: the good one passes; with the installed bundle
    (/etc/ssl/certs/ca-certificates.crt, no test CA), the expired one, and
    the one for another name, wget fails and ssl_client names the reason;
    --no-check-certificate passes the expired one, saying it did not
    check."""
    tls = os.path.join(work, "tls")
    make_test_certs(tls)
    shutil.copy(os.path.join(tls, "ca.pem"), os.path.join(www, "testca.pem"))
    servers = {n: start_https(www, tls, n) for n in ("server", "expired", "wrongname")}
    try:
        out = b.status(f"wget -q -O /tmp/testca.pem http://10.0.2.2:{port}/testca.pem", "wget-ca")
        need(out, r"^wget-ca=0$", "fetching the test CA over http")
        ca = "env SSL_CERT_FILE=/tmp/testca.pem"

        sport = servers["server"][1]
        out = b.status(f"{ca} wget -q -O /tmp/blob-s.bin https://10.0.2.2:{sport}/blob.bin", "https-ok")
        need(out, r"^https-ok=0$", "wget https with the test CA")
        out = b.run("sha256sum /tmp/blob-s.bin")
        got = need(out, r"^([0-9a-f]{64})\s+/tmp/blob-s\.bin$", "sha256sum of the https file").group(1)
        if got != want:
            raise Fail(f"the file fetched over https has SHA-256 {got}, the served file's {want}")

        refused = []
        for what, env, name, reason in (
                ("the installed bundle", "", "server", r"not correctly signed by the trusted CA"),
                ("an expired certificate", ca + " ", "expired", r"validity has expired"),
                ("a certificate for another name", ca + " ", "wrongname", r"Common Name \(CN\) does not match")):
            sp = servers[name][1]
            out = b.status(f"{env}wget -q -O /tmp/refused https://10.0.2.2:{sp}/blob.bin", "https-refused")
            if re.search(r"^https-refused=0$", out, re.M):
                raise Fail(f"wget https accepted {what}:\n{out.strip()[-600:]}")
            need(out, r"^ssl_client: 10\.0\.2\.2: the server's certificate is not accepted:$",
                 f"ssl_client's refusal of {what}")
            need(out, reason, f"the reason for refusing {what}")
            refused.append(what)

        sp = servers["expired"][1]
        out = b.status(f"wget -q --no-check-certificate -O /tmp/blob-k.bin https://10.0.2.2:{sp}/blob.bin",
                       "https-k")
        need(out, r"^https-k=0$", "wget --no-check-certificate")
        need(out, r"^ssl_client: 10\.0\.2\.2: the server's certificate was not checked \(-k\)$",
             "ssl_client's notice that it did not check")
    finally:
        for srv, _ in servers.values():
            srv.shutdown()
    results.append(f"f. https: 1 MiB from 10.0.2.2:{sport} with the test CA, SHA-256 matches; refused "
                   f"{', '.join(refused)}; --no-check-certificate fetches past an expired certificate")


def console_jobs(b, results):
    """Job control in BusyBox ash on the console (roadmap M4, decision 4):
    init started it as a session leader with the console its controlling
    terminal, so `&`, `jobs`, `kill %1`, `^Z`, `bg` and `fg` work."""
    b.run("sleep 100 &")
    out = b.run("jobs")
    need(out, r"^\[1\]\+\s+Running\s+sleep 100$", "jobs after sleep 100 &")
    b.run("kill %1")
    out = b.run("wait; echo waited")
    need(out, r"^waited$", "wait after kill %1")
    out = b.run("jobs; echo jobs-listed")
    if re.search(r"Running\s+sleep 100", out):
        raise Fail(f"the killed job is still running: {out!r}")
    # ^Z: a foreground job stopped from the keyboard, then resumed in the background.
    before = len(b.text())
    b.proc.stdin.write(b"sleep 30\n")
    b.proc.stdin.flush()
    time.sleep(1.0)                      # ash has made sleep the foreground group
    b.proc.stdin.write(b"\x1a")
    b.proc.stdin.flush()
    b.wait_prompt("^Z")
    out = b.text()[before:]
    need(out, r"\[1\]\+\s+Stopped\s+sleep 30", "^Z on a foreground job")
    out = b.run("jobs")
    need(out, r"^\[1\]\+\s+Stopped\s+sleep 30$", "jobs after ^Z")
    b.run("bg")
    out = b.run("jobs")
    need(out, r"^\[1\]\+\s+Running\s+sleep 30$", "jobs after bg")
    b.run("kill %1")
    out = b.run("wait; echo waited2")
    need(out, r"^waited2$", "wait after the resumed job's kill")
    out = b.run("readlink /proc/self/fd/0")
    need(out, r"^/dev/console$", "/proc/self/fd/0 on the console")
    results.append("console: ash job control -- &, jobs, kill %1, ^Z (Stopped), bg (Running); "
                   "/proc/self/fd/0 is /dev/console")


WIN_ROWS, WIN_COLS = 37, 101


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def ssh_base(work, port, strict, key="id_ed25519"):
    return ["ssh", "-i", os.path.join(work, key), "-p", str(port), "-o", "IdentitiesOnly=yes",
            "-o", "BatchMode=yes", "-o", "UserKnownHostsFile=" + os.path.join(work, "known_hosts"),
            "-o", "StrictHostKeyChecking=" + ("yes" if strict else "accept-new"), "-o", "ConnectTimeout=20",
            "-o", "LogLevel=ERROR", "root@127.0.0.1"]


def xfer_base(prog, work, port, key="id_ed25519"):
    """scp or sftp with ssh_base's options: -P names the port for both."""
    return [prog, "-i", os.path.join(work, key), "-P", str(port), "-o", "IdentitiesOnly=yes",
            "-o", "BatchMode=yes", "-o", "UserKnownHostsFile=" + os.path.join(work, "known_hosts"),
            "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=20", "-o", "LogLevel=ERROR"]


def xfer(argv, timeout=120):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def ssh_run(work, port, strict, command, timeout=60, key="id_ed25519"):
    """`command` over ssh; (exit status, output)."""
    p = subprocess.run(ssh_base(work, port, strict, key) + [command], capture_output=True, text=True,
                       timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def ssh_wait(work, port, strict, deadline_s=120):
    """Until the server answers a login: it makes its host key at the first
    boot, from a pool that may still be seeding."""
    t0 = time.monotonic()
    last = ""
    while time.monotonic() - t0 < deadline_s:
        try:
            rc, out = ssh_run(work, port, strict, "echo ready", timeout=40)
        except subprocess.TimeoutExpired:
            rc, out = -1, "timeout"
        if rc == 0 and "ready" in out:
            return time.monotonic() - t0
        last = out.strip()[-300:]
        time.sleep(2)
    raise Fail(f"ssh: no login within {deadline_s} s; the last attempt said: {last}")


def ssh_pty_session(work, port, lines, marker, timeout=90):
    """ssh -tt under a host pty of WIN_ROWS x WIN_COLS, typing `lines`;
    the session's whole output once `marker` has been seen."""
    master, slave = pty.openpty()
    p = None
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", WIN_ROWS, WIN_COLS, 0, 0))
        p = subprocess.Popen(ssh_base(work, port, True)[:-1] + ["-tt", "root@127.0.0.1"], stdin=slave,
                             stdout=slave, stderr=slave, close_fds=True, start_new_session=True)
    finally:
        os.close(slave)
    try:
        return _pty_session(p, master, lines, marker, timeout)
    finally:
        # Whatever happened, the client is gone and reaped and the pty closed.
        if p is not None and p.poll() is None:
            p.kill()
            p.wait()
        os.close(master)


def _pty_session(p, master, lines, marker, timeout):
    out = b""
    t0 = time.monotonic()

    def pump(until, secs):
        nonlocal out
        end = time.monotonic() + secs
        while time.monotonic() < end and (until is None or until not in out):
            r, _, _ = select.select([master], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                out += chunk

    pump(b"# ", 30)   # the remote shell's prompt (root's)
    for ln in lines:
        os.write(master, ln.encode() + b"\n")
        pump(None, 1.0)
    pump(("\n" + marker).encode(), max(1.0, timeout - (time.monotonic() - t0)))   # its output, not its echo
    os.write(master, b"exit\n")
    try:
        p.wait(timeout=20)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    pump(None, 0.5)
    return out.decode("utf-8", "replace").replace("\r", "")


def stage_ssh(args, results, b, port):
    work = args.workdir
    key = os.path.join(work, "id_ed25519")
    rsa = os.path.join(work, "id_rsa")
    for f in (key, key + ".pub", rsa, rsa + ".pub", os.path.join(work, "known_hosts")):
        if os.path.exists(f):
            os.unlink(f)
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "net-login-test", "-f", key], check=True)
    # An RSA key too, as most people already have one (id_rsa.pub): the
    # server takes rsa-sha2-256 signatures for logins (dropbear has no rsa-sha2-512), not SHA-1.
    subprocess.run(["ssh-keygen", "-q", "-t", "rsa", "-b", "3072", "-N", "", "-C", "net-login-test-rsa", "-f", rsa],
                   check=True)
    pub = open(key + ".pub").read().strip()
    rsa_pub = open(rsa + ".pub").read().strip()
    out = b.run(f"echo '{pub}' > /root/.ssh/authorized_keys && echo '{rsa_pub}' >> /root/.ssh/authorized_keys && "
                "cat /root/.ssh/authorized_keys | wc -l")
    need(out, r"^2$", "root's authorized_keys")
    waited = ssh_wait(work, port, strict=False)
    rc, out = ssh_run(work, port, True, "uname -a; echo ok")
    if rc != 0 or not re.search(r"^ok$", out, re.M) or not re.search(r"^Linux \S+ ", out, re.M):
        raise Fail(f"ssh: uname -a; echo ok: status {rc}, output {out!r}")
    uname = re.search(r"^Linux .*$", out, re.M).group(0)
    results.append(f"d. ssh: key login as root ({waited:.1f}s to the first login): {uname!r}, ok")
    rc, out = ssh_run(work, port, True, "echo rsa-ok", key="id_rsa")
    if rc != 0 or not re.search(r"^rsa-ok$", out, re.M):
        raise Fail(f"ssh with an RSA key: status {rc}, output {out!r}")
    results.append("d. ssh: an RSA key (3072 bits) logs in too")
    # Roadmap M5: the server's service holds net and cred alone, so a root
    # session over ssh has those two and cannot mount.
    rc, out = ssh_run(work, port, True, "/sbin/priv; /sbin/mount none /tmp ramfs; echo mount=$?")
    if not re.search(r"^net cred$", out, re.M) or not re.search(r"^mount=1$", out, re.M) \
            or "Operation not permitted" not in out:
        raise Fail(f"ssh: the session's privileges: status {rc}, output {out!r}")
    results.append("d. ssh: the session holds 'net cred' (the dropbear service's privileges); mount is EPERM")
    marker = "pty-session-done"
    out = ssh_pty_session(work, port, ["tty", "stty size", "sleep 100 &", "jobs", "kill %1",
                                       f"wait; echo {marker} wait=$?"], marker)
    m = re.search(r"^(/dev/pts/\d+)$", out, re.M)
    if not m:
        raise Fail(f"ssh -tt: tty named no /dev/pts/N: {out[-600:]!r}")
    if not re.search(rf"^{WIN_ROWS} {WIN_COLS}$", out, re.M):
        raise Fail(f"ssh -tt: stty size is not the client's {WIN_ROWS} {WIN_COLS}: {out[-600:]!r}")
    if not re.search(r"\[1\]\+\s+Running\s+sleep 100", out):
        raise Fail(f"ssh -tt: jobs did not list sleep 100: {out[-600:]!r}")
    if not re.search(rf"^{marker} wait=0$", out.split("echo " + marker, 1)[-1], re.M):
        raise Fail(f"ssh -tt: the session's wait did not succeed: {out[-600:]!r}")
    results.append(f"d. ssh -tt: tty {m.group(1)}, stty size {WIN_ROWS} {WIN_COLS}, sleep 100 & / jobs / "
                   "kill %1 / wait")
    stage_files(work, port, results)


def stage_files(work, port, results):
    """scp both ways, in both protocols, and an sftp session: the scp-sftp
    unit. `scp -O` is the classic protocol, which runs /bin/scp (dropbear's)
    through the session's shell; plain scp (OpenSSH 9.0 and later) and sftp
    use the sftp subsystem, which runs /usr/libexec/sftp-server (OpenSSH's).
    Each file is compared by SHA-256 on both sides."""
    data = os.urandom(256 * 1024)
    want = hashlib.sha256(data).hexdigest()
    src = os.path.join(work, "xfer-src.bin")
    with open(src, "wb") as f:
        f.write(data)

    def remote_sum(path):
        rc, out = ssh_run(work, port, True, f"sha256sum {path}")
        m = re.search(r"^([0-9a-f]{64})\s", out, re.M)
        if rc != 0 or not m:
            raise Fail(f"sha256sum {path}: status {rc}, output {out!r}")
        return m.group(1)

    def local_sum(path):
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()

    for mode, extra, name in (("classic", ["-O"], "up-classic.bin"), ("sftp", [], "up-sftp.bin")):
        rc, out = xfer(xfer_base("scp", work, port) + extra + [src, f"root@127.0.0.1:/root/{name}"])
        if rc != 0:
            raise Fail(f"scp ({mode}) to the guest: status {rc}, output {out!r}")
        if remote_sum(f"/root/{name}") != want:
            raise Fail(f"scp ({mode}) to the guest: /root/{name} differs")
        back = os.path.join(work, f"down-{mode}.bin")
        if os.path.exists(back):
            os.unlink(back)
        rc, out = xfer(xfer_base("scp", work, port) + extra + [f"root@127.0.0.1:/root/{name}", back])
        if rc != 0 or local_sum(back) != want:
            raise Fail(f"scp ({mode}) from the guest: status {rc}, output {out!r}")
    results.append(f"d. scp: 256 KiB to the guest and back, classic (scp -O, /bin/scp) and sftp protocol, "
                   f"SHA-256 {want[:16]}... on both sides")

    got = os.path.join(work, "down-sftp-session.bin")
    if os.path.exists(got):
        os.unlink(got)
    batch = os.path.join(work, "sftp.batch")
    with open(batch, "w") as f:
        f.write(f"mkdir /root/sftp-dir\ncd /root/sftp-dir\nput {src} s.bin\nls -l\n"
                f"get s.bin {got}\nrename s.bin t.bin\nls\nrm t.bin\ncd /root\nrmdir /root/sftp-dir\n")
    rc, out = xfer(xfer_base("sftp", work, port) + ["-b", batch, "root@127.0.0.1"])
    if rc != 0 or "t.bin" not in out or local_sum(got) != want:
        raise Fail(f"sftp -b: status {rc}, output {out[-800:]!r}")
    rc, out = ssh_run(work, port, True, "ls /root/sftp-dir 2>&1; echo gone=$?")
    if not re.search(r"^gone=1$", out, re.M):
        raise Fail(f"sftp: rmdir left /root/sftp-dir: {out!r}")
    results.append("d. sftp: mkdir, put, ls -l, get (SHA-256 matches), rename, rm, rmdir in one batch")


def stage_inspect(b, results):
    """g: what the machine's networking looks like, through the native ip
    (netctl's version-8 sections) and BusyBox's ifconfig, route, netstat,
    arp and hostname (the interface ioctls and /proc/net)."""
    out = b.run("ip a")
    need(out, r"^1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65535 state UNKNOWN$", "ip a: lo")
    need(out, r"^    inet 127\.0\.0\.1/8 scope host lo$", "ip a: lo's address")
    need(out, r"^2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP$", "ip a: eth0")
    need(out, rf"^    link/ether {MAC} brd ff:ff:ff:ff:ff:ff$", "ip a: eth0's MAC")
    need(out, r"^    inet 10\.0\.2\.15/24 brd 10\.0\.2\.255 scope global dynamic eth0$", "ip a: eth0's DHCP address")
    need(out, r"^       valid_lft \d+sec preferred_lft \d+sec$", "ip a: the lease's lifetime")
    need(out, r"^    inet6 fe80::5054:ff:fec0:5f05/64 scope link$", "ip a: eth0's link-local address")
    out = b.run("ip -4 a")
    need(out, r"^    inet 10\.0\.2\.15/24 ", "ip -4 a: eth0's address")
    if re.search(r"link/|inet6", out):
        raise Fail(f"ip -4 a printed more than IPv4: {out!r}")
    out = b.run("ip r")
    need(out, r"^default via 10\.0\.2\.2 dev eth0 proto dhcp src 10\.0\.2\.15$", "ip r: the default route")
    need(out, r"^10\.0\.2\.0/24 dev eth0 proto kernel scope link src 10\.0\.2\.15$", "ip r: the subnet")
    out = b.run("ip neigh")
    gw_mac = need(out, r"^10\.0\.2\.2 dev eth0 lladdr ([0-9a-f:]{17}) REACHABLE$", "ip neigh: the gateway").group(1)
    out = b.status("ip addr add 10.0.2.99/24 dev eth0", "ip-add")
    need(out, r"this ip only shows", "ip refusing a change")
    need(out, r"^ip-add=2$", "ip addr add's status")
    results.append(f"g. ip: eth0 10.0.2.15/24 dynamic, {MAC}, fe80::5054:ff:fec0:5f05; default via 10.0.2.2 "
                   f"proto dhcp; neigh 10.0.2.2 {gw_mac}; ip addr add refused")

    out = b.run("ifconfig")
    need(out, rf"^eth0\s+Link encap:Ethernet\s+HWaddr {MAC.upper()}\s*$", "ifconfig: eth0")
    need(out, r"^\s+inet addr:10\.0\.2\.15\s+Bcast:10\.0\.2\.255\s+Mask:255\.255\.255\.0$", "ifconfig: eth0's address")
    need(out, r"^\s+UP BROADCAST RUNNING MULTICAST\s+MTU:1500\s+Metric:1$", "ifconfig: eth0's flags")
    need(out, r"^\s+RX packets:[1-9]\d* ", "ifconfig: eth0's counters (/proc/net/dev)")
    need(out, r"^lo\s+Link encap:Local Loopback\s*$", "ifconfig: lo")
    out = b.run("route -n")
    need(out, r"^0\.0\.0\.0\s+10\.0\.2\.2\s+0\.0\.0\.0\s+UG\s+0\s+0\s+0\s+eth0$", "route -n: the default")
    need(out, r"^10\.0\.2\.0\s+0\.0\.0\.0\s+255\.255\.255\.0\s+U\s+0\s+0\s+0\s+eth0$", "route -n: the subnet")
    out = b.run("netstat -tuln")
    # A listener's Send-Q is its backlog, as Linux shows it (16, TCP_MAX_BACKLOG).
    need(out, r"^tcp\s+0\s+\d+\s+0\.0\.0\.0:22\s+0\.0\.0\.0:\*\s+LISTEN\s*$", "netstat -tuln: dropbear on :22")
    need(out, r"^tcp\s+0\s+\d+\s+:::22\s+:::\*\s+LISTEN\s*$", "netstat -tuln: dropbear on [::]:22 (/proc/net/tcp6)")
    if "bogus data" in out or "can't open" in out:
        raise Fail(f"netstat -tuln could not read /proc/net: {out!r}")
    out = b.run("arp -a")
    need(out, rf"^\? \(10\.0\.2\.2\) at {gw_mac} \[ether\]\s+on eth0$", "arp -a: the gateway")
    out = b.run("hostname; hostname -s; cat /etc/hostname")
    if out.count(HOSTNAME) < 3:
        raise Fail(f"hostname, hostname -s and /etc/hostname should each say {HOSTNAME}: {out!r}")
    results.append(f"g. busybox: ifconfig eth0 10.0.2.15 Bcast 10.0.2.255 Mask 255.255.255.0; route -n default "
                   f"UG and subnet U; netstat -tuln dropbear on 0.0.0.0:22 and :::22; arp -a 10.0.2.2 {gw_mac}; hostname {HOSTNAME} "
                   f"(DHCP option 12, /etc/hostname)")

    out = b.status("priv -d net -- ifconfig eth0 down", "down-noprv")
    need(out, r"SIOCSIFFLAGS: Operation not permitted", "ifconfig eth0 down without PRIV_NET")
    if re.search(r"^down-noprv=0$", out, re.M):
        raise Fail(f"ifconfig eth0 down succeeded without PRIV_NET: {out!r}")
    out = b.run("ip link show eth0")
    need(out, r"^2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP$", "eth0 after the refused down")
    out = b.status("ifconfig eth0 down", "down")
    need(out, r"^down=0$", "ifconfig eth0 down as root")
    out = b.run("ip link show eth0; ifconfig eth0")
    need(out, r"^2: eth0: <BROADCAST,MULTICAST> mtu 1500 state DOWN$", "ip link after ifconfig eth0 down")
    if re.search(r"^\s+UP ", out, re.M):
        raise Fail(f"ifconfig still shows eth0 up after the down: {out!r}")
    out = b.status("ifconfig eth0 up", "up")
    need(out, r"^up=0$", "ifconfig eth0 up as root")
    out = b.status("ping -c 1 -W 5 10.0.2.2", "ping")
    need(out, r"^ping=0$", "a ping of the gateway after ifconfig eth0 up")
    out = b.run("ip link show eth0")
    need(out, r"^2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP$", "ip link after ifconfig eth0 up")
    results.append("g. ifconfig eth0 down: EPERM with PRIV_NET dropped (eth0 stays UP); as root DOWN, then up and "
                   "the gateway answers a ping")


def stage_reboot_ssh(args, results, inst, port):
    """e: the second boot; the client's known_hosts must still match."""
    b = Boot("reboot", inst["target"], args.workdir,
             {"QEMU_DISKS": "boot", "QEMU_NET_HOSTFWD": f"tcp:127.0.0.1:{port}-:22"}, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        ssh_wait(args.workdir, port, strict=True)
        rc, out = ssh_run(args.workdir, port, True, "echo again")
        if rc != 0 or not re.search(r"^again$", out, re.M):
            raise Fail(f"ssh after the reboot: status {rc}, output {out!r}")
        out = b.run("hostname")
        need(out, rf"^{HOSTNAME}$", "the host name after a boot whose DHCP server names none (/etc/hostname)")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"reboot boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"e. reboot: the host key was kept (StrictHostKeyChecking=yes passed), the host name "
                   f"{HOSTNAME} too (/etc/hostname), PASS in {b.elapsed():.1f}s")


def stage_net(args, results, inst):
    work = args.workdir
    www = os.path.join(work, "www")
    os.makedirs(www, exist_ok=True)
    blob = os.urandom(BLOB_SIZE)
    with open(os.path.join(www, "blob.bin"), "wb") as f:
        f.write(blob)
    want = hashlib.sha256(blob).hexdigest()
    srv, port = start_http(www)
    ssh_port = free_port()   # after the HTTP server has its port, so the two cannot be the same
    b = Boot("net", inst["target"], work,
             {"QEMU_DISKS": "boot", "QEMU_NET_HOSTFWD": f"tcp:127.0.0.1:{ssh_port}-:22",
              "QEMU_NET_HOSTNAME": HOSTNAME}, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        log = b.text()
        need(log, r"^init: switched to the disk root \S+ ", "the installed boot")
        need(log, r"^\[ INFO\] dhcp: eth0: bound to 10\.0\.2\.15 for \d+ s, \d+ DNS server\(s\), first 10\.0\.2\.3$",
             "the kernel's DHCP client")
        if re.search(r"opt/cosmo/ipv4", log):
            raise Fail("the boot configured an interface from fw_cfg; this test needs DHCP")

        # a. The lease, and resolv.conf from it.
        out = b.run("netcfg show; cat /etc/resolv.conf; echo a-done")
        need(out, r"^a-done$", "netcfg show")
        m = need(out, r"^eth0: bound 10\.0\.2\.15/24 gateway 10\.0\.2\.2 server 10\.0\.2\.2 lease (\d+) s .* "
                      rf"dns 10\.0\.2\.3 hostname {HOSTNAME}$", "eth0's lease (netcfg show)")
        need(out, r"^nameserver 10\.0\.2\.3$", "/etc/resolv.conf")
        results.append(f"a. DHCP: eth0 10.0.2.15/24 gateway 10.0.2.2 dns 10.0.2.3, lease {m.group(1)} s; "
                       "/etc/resolv.conf names 10.0.2.3")

        # b. A 1 MiB file by address.
        t0 = time.monotonic()
        out = b.status(f"wget -q -O /tmp/blob.bin http://10.0.2.2:{port}/blob.bin", "wget-ip")
        secs = time.monotonic() - t0
        need(out, r"^wget-ip=0$", "wget by address")
        out = b.run("sha256sum /tmp/blob.bin")
        got = need(out, r"^([0-9a-f]{64})\s+/tmp/blob\.bin$", "sha256sum of the fetched file").group(1)
        if got != want:
            raise Fail(f"the fetched file's SHA-256 is {got}, the served file's {want}")
        results.append(f"b. by address: 1 MiB from 10.0.2.2:{port} in {secs:.1f}s, SHA-256 {want[:16]}... matches")

        # c. By name, through slirp's resolver; the runner's internet.
        last = ""
        for attempt in range(1, 4):
            out = b.status("nslookup example.com", "nslookup")
            ns_ok = re.search(r"^nslookup=0$", out, re.M) and re.search(r"^Address: \d+\.\d+\.\d+\.\d+$", out, re.M)
            out2 = b.status("wget -q -O - http://example.com/ > /tmp/ex.html", "wget-name")
            out2 += b.run("grep -c 'Example Domain' /tmp/ex.html")
            body_ok = re.search(r"^wget-name=0$", out2, re.M) and re.search(r"^[1-9]\d*$", out2, re.M)
            # The same over https, checked against the installed bundle.
            out3 = b.status("wget -q -O - https://example.com/ > /tmp/exs.html", "wget-https")
            out3 += b.run("grep -c 'Example Domain' /tmp/exs.html")
            tls_ok = re.search(r"^wget-https=0$", out3, re.M) and re.search(r"^[1-9]\d*$", out3, re.M)
            if ns_ok and body_ok and tls_ok:
                addr = re.search(r"^Address: (\d+\.\d+\.\d+\.\d+)$", out, re.M).group(1)
                results.append(f"c. by name: nslookup example.com -> {addr}; wget http://example.com/ and "
                               f"https://example.com/ have 'Example Domain' (attempt {attempt})")
                break
            last = (out + out2 + out3).strip()[-600:]
            time.sleep(2)
        else:
            raise Fail(f"by name: three attempts failed; the last said:\n{last}")

        stage_https(b, results, port, www, want, work)
        console_jobs(b, results)
        stage_ssh(args, results, b, ssh_port)
        stage_inspect(b, results)

        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"net boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    finally:
        srv.shutdown()
    results.append(f"net boot: PASS in {b.elapsed():.1f}s")
    return ssh_port


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    results = []
    t0 = time.monotonic()
    try:
        inst = install_test.stage_install(args, results)
        ssh_port = stage_net(args, results, inst)
        stage_reboot_ssh(args, results, inst, ssh_port)
    except Fail as e:
        for r in results:
            print(f"test-net-login: {r}")
        print(f"test-net-login: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-net-login: {r}")
    print(f"test-net-login: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
