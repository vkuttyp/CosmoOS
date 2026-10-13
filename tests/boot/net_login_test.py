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
     -- up to three attempts, since this needs the runner's internet.

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
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"reboot boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    results.append(f"e. reboot: the host key was kept (StrictHostKeyChecking=yes passed), PASS in {b.elapsed():.1f}s")


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
             {"QEMU_DISKS": "boot", "QEMU_NET_HOSTFWD": f"tcp:127.0.0.1:{ssh_port}-:22"}, args.timeout)
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
                      r"dns 10\.0\.2\.3$", "eth0's lease (netcfg show)")
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
            if ns_ok and body_ok:
                addr = re.search(r"^Address: (\d+\.\d+\.\d+\.\d+)$", out, re.M).group(1)
                results.append(f"c. by name: nslookup example.com -> {addr}; wget http://example.com/ has "
                               f"'Example Domain' (attempt {attempt})")
                break
            last = (out + out2).strip()[-400:]
            time.sleep(2)
        else:
            raise Fail(f"by name: three attempts failed; the last said:\n{last}")

        console_jobs(b, results)
        stage_ssh(args, results, b, ssh_port)

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
