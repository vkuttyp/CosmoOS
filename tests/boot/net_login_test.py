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
     cosmo-fetch (roadmap 1.1 P6) fetches the same file and refuses the
     expired certificate.

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
  h. Process tools on /proc (shell completeness S3), on the same boot:
     BusyBox `ps` lists init, the ssh server and the console shell with
     their pids and states, each pid known without /proc -- the shell's
     $$, init as its parent ($PPID), and dropbear as the child of the
     supervisor `svc status` names -- and ps itself running.
  i. Ownership on cosmofs (shell completeness S4), on the same boot: a
     file chowned to 1000:1000 and a link to it chowned with -h to
     1001:1001 (the target keeps its owner), then synced; on the reboot
     (e) both owners are still there.
  j. Power and clock (shell completeness S5), on the reboot boot (e), which
     QEMU runs with QEMU_REBOOT=1 so a guest reset restarts the machine:
     with sys_admin dropped, `reboot` (a signal to init), `reboot -f`
     (reboot(2)), `date -s` and `hwclock -w` are each refused, and
     `date -s` too for uid 1000; `ntpd -q` against this harness's NTP
     server, which answers a day ahead, sets the clock to within 1 s of
     it; then `date -s` two days ahead, `hwclock -w` and `reboot`, and
     the machine restarts in the same QEMU, and after it the wall clock
     is the RTC's (to 2 s, the read keeps whole seconds).
  k. Mounting (shell completeness S6), on the net boot, which has a second,
     blank disk: `lsblk` names the installed disk's GPT partitions (the
     root at /) and the blank disk; `blkid` names the root's PARTUUID (the
     installer's) and its label, cosmo-root, and the disk's GPT; `mkfs.cosmofs -L s6data` on
     the blank disk, a line for it in /etc/fstab and `mount -a` mount it
     at /data, where a file is written and synced; `umount` of it is
     refused with mount dropped. On the reboot (e), /etc/rc's `mount -a`
     has mounted it again, and the file is there.

Exit 0 PASS, 1 FAIL.
"""

import argparse
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
import tlstest  # noqa: E402

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


def make_test_certs(d):
    """The test CA and server certificates (tests/boot/tlstest.py): `server`
    for 10.0.2.2 and cosmo-test.invalid, `expired`, `wrongname` (other.invalid
    only); the CA is ca.pem."""
    try:
        tlstest.make_test_certs(d, ip="10.0.2.2")
    except tlstest.CertError as e:
        raise Fail(str(e))


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
    check. cosmo-fetch, on the same checks, fetches the file and refuses the
    expired certificate."""
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
            # Roadmap 1.1 P3: the refusal ends on mbedTLS's reason; wget adds
            # nothing (it said "error getting response", with a stale errno
            # after a name lookup: "Resource temporarily unavailable").
            need(out, r"^https-refused=1$", f"wget's exit status refusing {what}")
            extra = re.search(r"^wget: .*$", out, re.M)
            if extra:
                raise Fail(f"wget refusing {what} added '{extra.group(0)}' after ssl_client's reason")
            refused.append(what)

        # cosmo-fetch (pkg's and cosmo-upgrade's client, the same checks):
        # the file whole with the test CA, the expired certificate refused
        # (exit 3); its full cases run on the host (make host-test-fetch).
        out = b.status(f"{ca} cosmo-fetch https://10.0.2.2:{sport}/blob.bin /tmp/blob-f.bin", "fetch-ok")
        need(out, r"^fetch-ok=0$", "cosmo-fetch with the test CA")
        out = b.run("sha256sum /tmp/blob-f.bin")
        got = need(out, r"^([0-9a-f]{64})\s+/tmp/blob-f\.bin$", "sha256sum of cosmo-fetch's file").group(1)
        if got != want:
            raise Fail(f"cosmo-fetch's file has SHA-256 {got}, the served file's {want}")
        sp = servers["expired"][1]
        out = b.status(f"{ca} cosmo-fetch https://10.0.2.2:{sp}/blob.bin /tmp/refused", "fetch-refused")
        need(out, r"^fetch-refused=3$", "cosmo-fetch refusing an expired certificate (exit 3)")
        need(out, r"^cosmo-fetch: 10\.0\.2\.2: the server's certificate is not accepted:$",
             "cosmo-fetch's refusal of the expired certificate")

        out = b.status(f"wget -q --no-check-certificate -O /tmp/blob-k.bin https://10.0.2.2:{sp}/blob.bin",
                       "https-k")
        need(out, r"^https-k=0$", "wget --no-check-certificate")
        need(out, r"^ssl_client: 10\.0\.2\.2: the server's certificate was not checked \(-k\)$",
             "ssl_client's notice that it did not check")
    finally:
        for srv, _ in servers.values():
            srv.shutdown()
    results.append(f"f. https: 1 MiB from 10.0.2.2:{sport} with the test CA, SHA-256 matches; refused "
                   f"{', '.join(refused)}; cosmo-fetch the same file, and refused the expired one (3); "
                   f"--no-check-certificate fetches past an expired certificate")


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
                f"get s.bin {got}\nrename s.bin t.bin\nls\n")
    rc, out = xfer(xfer_base("sftp", work, port) + ["-b", batch, "root@127.0.0.1"])
    if rc != 0 or "t.bin" not in out or local_sum(got) != want:
        raise Fail(f"sftp -b: status {rc}, output {out[-800:]!r}")
    # The long listing. An OpenSSH 9.1+ client renders it itself when the
    # server offers users-groups-by-id@openssh.com (OpenSSH's sftp-server
    # does, on Linux too), and SFTP v3 carries no link count, so it prints
    # "?" there (sftp-common.c ls_file, remote=1); an older client prints
    # the server's long name, whose count is lstat's (roadmap 1.1 P3).
    m = re.search(r"^-rw\S*\s+(\?|\d+)\s+\S+\s+\S+\s+%d\s.*\ss\.bin$" % os.path.getsize(src), out, re.M)
    if not m:
        raise Fail(f"sftp ls -l: no long line for s.bin in {out[-800:]!r}")
    links = m.group(1)
    if links not in ("?", "1"):
        raise Fail(f"sftp ls -l: s.bin's link count is {links}, not 1 (or the client's '?')")
    # The count the server's long name is made from: lstat's st_nlink, of
    # the file sftp wrote (s.bin, renamed t.bin: the same inode).
    rc, nl = ssh_run(work, port, True, "stat -c 'nlink=%h' /root/sftp-dir/t.bin")
    if not re.search(r"^nlink=1$", nl, re.M):
        raise Fail(f"stat -c %h over ssh of the file sftp wrote: {nl!r}")
    with open(batch, "w") as f:
        f.write("cd /root/sftp-dir\nrm t.bin\ncd /root\nrmdir /root/sftp-dir\n")
    rc, out = xfer(xfer_base("sftp", work, port) + ["-b", batch, "root@127.0.0.1"])
    if rc != 0:
        raise Fail(f"sftp -b (rm, rmdir): status {rc}, output {out[-800:]!r}")
    rc, out = ssh_run(work, port, True, "ls /root/sftp-dir 2>&1; echo gone=$?")
    if not re.search(r"^gone=1$", out, re.M):
        raise Fail(f"sftp: rmdir left /root/sftp-dir: {out!r}")
    results.append(f"d. sftp: mkdir, put, ls -l (link count shown as '{links}'; the server's stat says 1), "
                   "get (SHA-256 matches), rename, then rm and rmdir in a second batch")


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
    out = b.run("ip -6 r")
    # IPv6 routes the link through the default interface alone (ipv6_route);
    # eth1 is up too, and must not be listed.
    need(out, r"^fe80::/64 dev eth0 proto kernel metric 256 pref medium$", "ip -6 r: eth0's link-local route")
    if "dev eth1" in out:
        raise Fail(f"ip -6 r lists eth1, which IPv6 never routes through: {out!r}")
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

    # Every other setter family, on eth1 (the harness's second NIC, which
    # nothing else here uses): once with PRIV_NET dropped, which must be
    # EPERM and change nothing, then as root.
    for args, ioctl in (("10.0.3.15", "SIOCSIFADDR"), ("netmask 255.255.0.0", "SIOCSIFNETMASK"),
                        ("broadcast 10.0.2.255", "SIOCSIFBRDADDR"), ("mtu 1500", "SIOCSIFMTU"),
                        ("hw ether 52:54:00:c0:5f:06", "SIOCSIFHWADDR")):
        out = b.status(f"priv -d net -- ifconfig eth1 {args}", "set-noprv")
        need(out, rf"{ioctl}: Operation not permitted", f"ifconfig eth1 {args} without PRIV_NET")
        if re.search(r"^set-noprv=0$", out, re.M):
            raise Fail(f"ifconfig eth1 {args} succeeded without PRIV_NET: {out!r}")
    out = b.run("ip -4 a show eth1")
    need(out, r"^    inet 10\.0\.2\.15/24 brd 10\.0\.2\.255 ", "eth1 untouched by the refused setters")
    out = b.status("ifconfig eth1 10.0.3.15 netmask 255.255.255.0", "set-addr")
    need(out, r"^set-addr=0$", "ifconfig eth1 10.0.3.15 netmask 255.255.255.0 as root")
    out = b.run("ip -4 a show eth1; ip r; ifconfig eth1")
    # Set by hand, so not the lease's: no "dynamic", no lease lifetime.
    need(out, r"^    inet 10\.0\.3\.15/24 brd 10\.0\.3\.255 scope global eth1$", "ip a: eth1's new address, static")
    need(out, r"^10\.0\.3\.0/24 dev eth1 proto kernel scope link src 10\.0\.3\.15$", "ip r: eth1's new subnet")
    need(out, r"^default via 10\.0\.2\.2 dev eth0 ", "ip r: the default still through eth0")
    need(out, r"^\s+inet addr:10\.0\.3\.15\s+Bcast:10\.0\.3\.255\s+Mask:255\.255\.255\.0$", "ifconfig eth1")
    for args, ok, ioctl in (("broadcast 10.0.3.255", True, None), ("broadcast 10.0.3.1", False, "SIOCSIFBRDADDR"),
                            ("mtu 1500", True, None), ("mtu 1400", False, "SIOCSIFMTU"),
                            ("hw ether 52:54:00:c0:5f:06", True, None),
                            ("hw ether 52:54:00:00:00:01", False, "SIOCSIFHWADDR")):
        out = b.status(f"ifconfig eth1 {args}", "set")
        if ok:
            need(out, r"^set=0$", f"ifconfig eth1 {args} (the value it has)")
        else:
            need(out, rf"{ioctl}: (Invalid argument|Not supported)", f"ifconfig eth1 {args} refused")
            if re.search(r"^set=0$", out, re.M):
                raise Fail(f"ifconfig eth1 {args} succeeded: {out!r}")
    out = b.run("ifconfig eth1")
    need(out, r"^eth1\s+Link encap:Ethernet\s+HWaddr 52:54:00:C0:5F:06\s*$", "eth1's MAC unchanged")
    need(out, r"^\s+UP BROADCAST RUNNING MULTICAST\s+MTU:1500\s+Metric:1$", "eth1's MTU unchanged")
    results.append("g. setters: address, netmask, broadcast, MTU and MAC each EPERM without PRIV_NET (eth1 "
                   "unchanged); as root eth1 10.0.3.15/24 (ip a, ip r, ifconfig); the broadcast and MTU and MAC it "
                   "has accepted, others refused (EINVAL, EOPNOTSUPP)")


def stage_processes(b, results):
    """h: BusyBox ps over /proc/<pid>/stat (shell completeness S3)."""
    out = b.run("echo shell=$$ parent=$PPID; svc status dropbear")
    m = need(out, r"^shell=(\d+) parent=(\d+)$", "the console shell's pid and its parent's")
    sh_pid, init_pid = m.group(1), m.group(2)
    sup = need(out, r"^dropbear\s+running \(supervisor (\d+)\)$", "svc status dropbear").group(1)
    out = b.run("ps -o pid,ppid,stat,comm")
    need(out, rf"^\s*{init_pid}\s+0\s+S\s+init$", "ps: init, sleeping, no parent")
    sshd = need(out, rf"^\s*(\d+)\s+{sup}\s+S\s+dropbear$", "ps: the ssh server, its supervisor's child").group(1)
    need(out, rf"^\s*{sh_pid}\s+{init_pid}\s+S\s+sh$", "ps: this shell, waiting for ps")
    need(out, rf"^\s*\d+\s+{sh_pid}\s+R\s+ps$", "ps: ps itself, running")
    results.append(f"h. ps: init {init_pid} S, dropbear {sshd} S (supervisor {sup}), the shell {sh_pid} S, ps R")


OWN_CHECK = "stat -c '%n %u %g' /root/s4own; stat -c '%n %u %g' /root/s4lnk"


def own_lines(out, what):
    """i: the owners chown gave the file and the link, by the stat lines."""
    need(out, r"^/root/s4own 1000 1000$", f"{what}: /root/s4own owned by 1000:1000")
    need(out, r"^/root/s4lnk 1001 1001$", f"{what}: the link /root/s4lnk owned by 1001:1001 (chown -h)")


def stage_ownership(b, results):
    """i: chown and chown -h on the installed system's cosmofs root; the
    reboot (e) reads them back."""
    b.run("rm -f /root/s4own /root/s4lnk; touch /root/s4own; ln -s s4own /root/s4lnk")
    out = b.status("chown 1000:1000 /root/s4own && chown -h 1001:1001 /root/s4lnk && sync", "chown")
    need(out, r"^chown=0$", "chown and chown -h as root")
    own_lines(b.run(OWN_CHECK), "before the reboot")
    results.append("i. chown 1000:1000 on a cosmofs file, chown -h 1001:1001 on a link to it; synced")


NTP_EPOCH = 2208988800   # 1900 to 1970


def start_ntp(offset_s):
    """j: an NTP server on 127.0.0.1 (10.0.2.2 to the guest) whose clock is
    the host's plus offset_s: stratum 1, version 4, mode 4, each reply's
    originate the request's transmit time (ntpd drops anything else)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(0.5)
    stop = threading.Event()

    def ts(t):
        return (int(t) + NTP_EPOCH) << 32 | int((t % 1) * (1 << 32))

    def serve():
        while not stop.is_set():
            try:
                req, peer = s.recvfrom(512)
            except socket.timeout:
                continue
            if len(req) < 48:
                continue
            now = time.time() + offset_s
            reply = struct.pack("!BBbbII4sQ", 0x24, 1, req[2], -20, 0, 1 << 10, b"LOCL", ts(now)) + req[40:48] \
                + struct.pack("!QQ", ts(now), ts(time.time() + offset_s))
            s.sendto(reply, peer)
        s.close()

    threading.Thread(target=serve, daemon=True).start()
    return s.getsockname()[1], stop


def wait_text(b, pattern, start, what):
    """The first match of `pattern` in the serial log after `start`."""
    while time.monotonic() < b.deadline:
        m = re.compile(pattern, re.M).search(b.text(), start)
        if m:
            return m
        if b.proc.poll() is not None:
            raise Fail(f"{b.name}: QEMU exited (code {b.proc.returncode}) waiting for {what}")
        time.sleep(0.1)
    raise Fail(f"{b.name}: no {what} before the deadline")


def guest_epoch(b, what):
    """`date +%s` on the guest, and the host's time around the reading."""
    h0 = time.time()
    out = b.run("date +%s")
    h1 = time.time()
    return int(need(out, r"^(\d{9,})$", what).group(1)), h0, h1


def stage_power_clock(args, results, b):
    """j: refusals, ntpd -q a day ahead, date -s + hwclock -w across a
    reboot inside this QEMU."""
    out = b.status("priv -d sys_admin -- reboot", "rb")
    need(out, r"^rb=[1-9]\d*$", "reboot refused with sys_admin dropped (init's signal)")
    out = b.status("priv -d sys_admin -- reboot -f", "rbf")
    need(out, r"^rbf=[1-9]\d*$", "reboot -f refused with sys_admin dropped (reboot(2))")
    need(out, r"Operation not permitted", "reboot -f: EPERM")
    future = int(time.time()) + 3 * 86400
    # BusyBox's date reports a refused set and exits 0, as on Linux: the
    # message, and the clock below, are the check.
    out = b.run(f"priv -d sys_admin -- date -s @{future}")
    need(out, r"can't set date: Operation not permitted", "date -s refused with sys_admin dropped (EPERM)")
    out = b.run(f"priv -u 1000 -- date -s @{future}")
    need(out, r"can't set date: Operation not permitted", "date -s refused to uid 1000 (EPERM)")
    out = b.status("priv -d sys_admin -- hwclock -w", "hw")
    need(out, r"^hw=[1-9]\d*$", "hwclock -w refused with sys_admin dropped")
    need(out, r"Permission denied", "hwclock -w: EACCES, as Linux's RTC_SET_TIME")
    g, h0, h1 = guest_epoch(b, "the clock after the refusals")
    if g > h1 + 5 or g >= future - 86400:
        raise Fail(f"a refused date -s moved the clock: guest {g}, host {h1:.0f}")

    port, stop = start_ntp(86400)
    try:
        out = b.status(f"ntpd -q -n -p 10.0.2.2:{port}", "ntp")
    finally:
        stop.set()
    need(out, r"^ntp=0$", "ntpd -q against the harness's server")
    g, h0, h1 = guest_epoch(b, "the clock after ntpd -q")
    if not (h0 + 86400 - 1 <= g <= h1 + 86400 + 1):
        raise Fail(f"ntpd -q: the guest reads {g}, the server's clock was {h0 + 86400:.1f}..{h1 + 86400:.1f}")
    ntp_off = g - (h0 + h1) / 2

    target = int(time.time()) + 2 * 86400
    out = b.status(f"date -s @{target} >/dev/null && hwclock -w", "hw")
    t_set = time.time()
    need(out, r"^hw=0$", "date -s and hwclock -w as root")
    out = b.run("hwclock -u")
    need(out, r"\d\d:\d\d:\d\d", "hwclock -u reads the RTC back")
    start = len(b.text())
    with b.lock:
        raw_start = len(b.buf)
    b.proc.stdin.write(b"reboot\n")
    b.proc.stdin.flush()
    wait_text(b, r"reboot: restarting system", start, "the kernel's restart line")
    how = wait_text(b, r"reboot: restart through (.*)$", start, "the restart's method").group(1)
    b.deadline = time.monotonic() + args.timeout   # a second boot inside the same QEMU
    banner = wait_text(b, r"CosmoOS kernel", start, "a second kernel banner after reboot")
    # Prompts before the second banner are the old boot's (its shell may
    # print one after `reboot` returns); the new boot's first prompt may
    # already be in, so count only up to the banner (found in review).
    with b.lock:
        pos = b.buf.find(b"CosmoOS kernel", raw_start)
        b.prompts_seen = b.buf[:pos].count(install_test.PROMPT)
    b.wait_prompt("the first prompt after reboot")
    after = b.text()[banner.start():]
    need(after, r"switched to the disk root", "the installed system again after reboot")
    g, h0, h1 = guest_epoch(b, "the clock after reboot")
    # Keep (docs/testing/flakes.md): the bound is the RTC's resolution, not a
    # time budget. Below, the reading may lose under a second each to the
    # boot's whole-second RTC read and to `date +%s`; on x86-64 also the
    # firmware's century write (edk2's PcRtcAcpiTableChangeCallback, every
    # boot, the clock running), which QEMU's MC146818 takes as a new time
    # with the old sub-second phase: the RTC drops back by the time since
    # hwclock -w modulo a second (tools/rtc-century-probe.py). Above, QEMU
    # keeps the old phase at hwclock -w's write too.
    below = 3 if os.environ.get("QEMU_ARCH", "x86_64") == "x86_64" else 2
    lo, hi = target + (h0 - t_set) - below, target + (h1 - t_set) + 2
    if not (lo <= g <= hi):
        raise Fail(f"after reboot the guest reads {g}, the RTC should read {lo:.0f}..{hi:.0f}")
    results.append(f"j. reboot, reboot -f, date -s and hwclock -w refused without sys_admin (date -s for uid "
                   f"1000 too); ntpd -q a day ahead: {ntp_off - 86400:+.1f}s from the server; date -s + "
                   f"hwclock -w + reboot ({how}): the restarted system reads the RTC's time")


BLANK_MIB = 16


def blank_disk_env(work):
    """k: a blank disk beside the installed one, the same file on both boots."""
    path = os.path.join(work, "blank.img")
    if not os.path.exists(path):
        with open(path, "wb") as f:
            f.truncate(BLANK_MIB << 20)
    return (os.environ.get("QEMU_EXTRA", "") +
            f" -drive if=none,id=blank,format=raw,file={path} -device virtio-blk-pci,drive=blank").strip()


def lsblk_rows(out):
    """k: lsblk's rows as (tree prefix, name, size, ro, type, mountpoint)."""
    rows = []
    for line in out.splitlines():
        m = re.match(r"^(\|-|`-)?(\S+)\s+(\S+)\s+([01])\s+(disk|part)\s*(\S*)$", line)
        if m:
            rows.append(m.groups(""))
    return rows


def stage_mounting(b, results, inst):
    """k: lsblk, blkid, and a blank disk made a cosmofs and mounted by
    /etc/fstab; the reboot (e) checks it came back."""
    out = b.run("awk '$2 == \"/\" { print $1 }' /proc/mounts")
    root = need(out, r"^/dev/(\S+)$", "the root's device in /proc/mounts").group(1)
    out = b.run("lsblk")
    need(out, r"^NAME\s+SIZE\s+RO\s+TYPE\s+MOUNTPOINTS$", "lsblk's header")
    rows = lsblk_rows(out)
    disks = [r for r in rows if r[4] == "disk"]
    parts = [r for r in rows if r[4] == "part"]
    rootrow = [r for r in parts if r[1] == root]
    if len(rootrow) != 1 or rootrow[0][0] != "`-" or rootrow[0][5] != "/":
        raise Fail(f"lsblk: the root {root} is not the last partition, mounted at /:\n{out}")
    if not [r for r in parts if r[0] == "|-" and r[1] != root]:
        raise Fail(f"lsblk: no ESP row before the root's:\n{out}")
    blank = [r for r in disks if r[2] == f"{BLANK_MIB}M" and not root.startswith(r[1])]
    if len(blank) != 1:
        raise Fail(f"lsblk: want one {BLANK_MIB}M disk besides the installed one:\n{out}")
    blank = blank[0][1]
    rootdisk = max((r[1] for r in disks if root.startswith(r[1])), key=len)
    out = b.run(f"blkid /dev/{root} /dev/{rootdisk}")
    need(out, rf'^/dev/{root}: LABEL="cosmo-root" UUID="[0-9a-f-]{{36}}" TYPE="cosmofs" '
              rf'PARTUUID="{inst["partuuid"]}"$', "blkid: the root's label, type and PARTUUID")
    need(out, rf'^/dev/{rootdisk}: PTUUID="[0-9a-f-]{{36}}" PTTYPE="gpt"$', "blkid: the installed disk's GPT")
    out = b.status(f"mkfs.cosmofs -L s6data /dev/{blank}", "mkfs")
    need(out, r"^mkfs=0$", f"mkfs.cosmofs on {blank}")
    out = b.status(f"mkdir -p /data && echo '/dev/{blank} /data cosmofs defaults 0 0' >> /etc/fstab && mount -a",
                   "mnta")
    need(out, r"^mnta=0$", "mount -a with the new /etc/fstab line")
    out = b.run("grep ' /data ' /proc/mounts; lsblk; blkid")
    need(out, rf"^/dev/{blank} /data cosmofs rw 0 0$", "/proc/mounts: the blank disk at /data")
    if not [r for r in lsblk_rows(out) if r[1] == blank and r[5] == "/data"]:
        raise Fail(f"lsblk: {blank} is not shown at /data:\n{out}")
    need(out, rf'^/dev/{blank}: LABEL="s6data" UUID="[0-9a-f-]{{36}}" TYPE="cosmofs"$', "blkid: the new label")
    out = b.status("priv -d mount -- umount /data", "umnt")
    need(out, r"^umnt=[1-9]\d*$", "umount refused with mount dropped")
    out = b.status(f"echo {inst['nonce']} > /data/s6 && sync && grep -c ' /data ' /proc/mounts", "wr")
    need(out, r"^1$", "/data still mounted after the refused umount")
    need(out, r"^wr=0$", "a file written to /data and synced")
    results.append(f"k. lsblk: {root} under its disk at /, the blank {blank}; blkid: {root} LABEL cosmo-root, "
                   f"PARTUUID {inst['partuuid']}; mkfs.cosmofs -L s6data {blank}, /etc/fstab + mount -a at /data; "
                   f"umount refused with mount dropped")
    return blank


def mounted_again(b, inst, results):
    """k, after the reboot: /etc/rc's mount -a mounted the blank disk again."""
    out = b.run("grep ' /data ' /proc/mounts; cat /data/s6")
    need(out, rf"^/dev/{inst['blank']} /data cosmofs rw 0 0$", "/data mounted by /etc/fstab after the reboot")
    need(out, rf"^{inst['nonce']}$", "the file written to /data before the reboot")
    results.append(f"k. after the reboot /etc/rc's mount -a mounted {inst['blank']} at /data again, the file there")


def stage_reboot_ssh(args, results, inst, port):
    """e: the second boot; the client's known_hosts must still match."""
    b = Boot("reboot", inst["target"], args.workdir,
             {"QEMU_DISKS": "boot", "QEMU_NET_HOSTFWD": f"tcp:127.0.0.1:{port}-:22", "QEMU_REBOOT": "1",
              "QEMU_EXTRA": blank_disk_env(args.workdir)},
             args.timeout)
    try:
        b.wait_prompt("the first prompt")
        mounted_again(b, inst, results)
        ssh_wait(args.workdir, port, strict=True)
        rc, out = ssh_run(args.workdir, port, True, "echo again")
        if rc != 0 or not re.search(r"^again$", out, re.M):
            raise Fail(f"ssh after the reboot: status {rc}, output {out!r}")
        out = b.run("hostname")
        need(out, rf"^{HOSTNAME}$", "the host name after a boot whose DHCP server names none (/etc/hostname)")
        own_lines(b.run(OWN_CHECK), "after the reboot")
        stage_power_clock(args, results, b)
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"reboot boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"e. reboot: the host key was kept (StrictHostKeyChecking=yes passed), the host name "
                   f"{HOSTNAME} too (/etc/hostname), and the owners of i (1000:1000, the link's 1001:1001); "
                   f"PASS in {b.elapsed():.1f}s")


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
              "QEMU_NET_HOSTNAME": HOSTNAME, "QEMU_EXTRA": blank_disk_env(work)}, args.timeout)
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
        stage_processes(b, results)
        stage_ownership(b, results)
        inst["nonce"] = os.urandom(8).hex()
        inst["blank"] = stage_mounting(b, results, inst)

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
