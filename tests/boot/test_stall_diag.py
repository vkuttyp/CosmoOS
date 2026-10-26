#!/usr/bin/env python3
"""stall_diag.capture against a fake QMP server (docs/testing/flakes.md,
"The kernel stopped in the firmware's RNG call"): the HMP commands it
sends, the virtio-rng device it finds in `info virtio` and asks about, and
that a missing socket is written down rather than raised. No QEMU runs.
"""

import json
import os
import socket
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stall_diag  # noqa: E402

FAILURES = []
CHECKS = 0


def check(cond, what):
    global CHECKS
    CHECKS += 1
    print(f"{'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        FAILURES.append(what)


RNG = "/machine/peripheral-anon/device[2]/virtio-backend"
ANSWERS = {
    "info status": "VM status: running\n",
    "info cpus": "* CPU #0: thread_id=1\n  CPU #1: thread_id=2\n",
    "info virtio": f"/machine/peripheral-anon/device[1]/virtio-backend [virtio-serial]\n{RNG} [virtio-rng]\n",
}


def serve(listener, seen):
    conn, _ = listener.accept()
    f = conn.makefile("rw")
    f.write(json.dumps({"QMP": {"version": {}}}) + "\n")
    f.flush()
    for line in f:
        req = json.loads(line)
        if req["execute"] == "qmp_capabilities":
            reply = {"return": {}}
        else:
            cmd = req["arguments"]["command-line"]
            seen.append(cmd)
            reply = {"return": ANSWERS.get(cmd, f"answer to {cmd}\n")}
        f.write(json.dumps(reply) + "\n")
        f.flush()
    conn.close()


def main():
    d = tempfile.mkdtemp(prefix="sd")
    sock = os.path.join(d, "qmp")
    out = os.path.join(d, "diag.log")
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(sock)
    listener.listen(1)
    seen = []
    t = threading.Thread(target=serve, args=(listener, seen), daemon=True)
    t.start()
    stall_diag.capture(sock, out)
    t.join(timeout=5)
    text = open(out).read()
    check("info registers -a" in seen and seen.count("info registers -a") == 2, "registers asked for twice")
    check(seen.count("x/16i $pc") == 2 and "cpu 0" in seen and "cpu 1" in seen, "each CPU's instructions at its PC")
    check(f"info virtio-queue-status {RNG} 0" in seen,
          "the virtio-rng device found in `info virtio` and its queue asked about")
    check(not any(c.startswith("info virtio-status") for c in seen),
          "not info virtio-status, which ends the connection")
    check(not any("virtio-serial" in c for c in seen if c.startswith("info virtio-")),
          "no other virtio device asked about")
    check(f"=== info virtio-queue-status {RNG} 0\nanswer to info virtio-queue-status {RNG} 0" in text,
          "each answer written under its command")
    gone = os.path.join(d, "gone")
    stall_diag.capture(gone, out)
    check(open(out).read().startswith("capture failed:"), "no socket: written down, not raised")
    print(f"test_stall_diag: {CHECKS - len(FAILURES)} of {CHECKS} checks passed")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
