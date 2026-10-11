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
`&`, `jobs`, `kill %1`, `^Z` and `bg`.

Exit 0 PASS, 1 FAIL.
"""

import argparse
import hashlib
import http.server
import os
import re
import shutil
import socketserver
import sys
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


def stage_net(args, results, inst):
    work = args.workdir
    www = os.path.join(work, "www")
    os.makedirs(www, exist_ok=True)
    blob = os.urandom(BLOB_SIZE)
    with open(os.path.join(www, "blob.bin"), "wb") as f:
        f.write(blob)
    want = hashlib.sha256(blob).hexdigest()
    srv, port = start_http(www)
    b = Boot("net", inst["target"], work, {"QEMU_DISKS": "boot"}, args.timeout)
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

        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"net boot: QEMU exit code {rc}")
    except Exception:
        b.kill()
        raise
    finally:
        srv.shutdown()
    results.append(f"net boot: PASS in {b.elapsed():.1f}s")


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
        stage_net(args, results, inst)
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
