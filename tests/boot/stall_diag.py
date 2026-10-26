"""What a boot that missed its deadline was doing, asked of QEMU before it
is killed (docs/testing/flakes.md, "The kernel stopped in the firmware's RNG
call": x86-64 release boots that print the loader's `paging:` line and then
nothing, inside OVMF's GetRNG).

`capture(sock, path)` sends HMP commands over the boot's QMP socket and
writes their answers to `path`: the CPUs' registers twice, a second apart
(a CPU spinning moves its RIP, one halted keeps it), the instructions at
each PC, and the virtio-rng device's queue (a request the guest made and
QEMU never answered shows as `inuse` above 0, `last_avail_idx` past
`used_idx`). Not `info virtio-status`: on QEMU 11.1 it ends the QMP
connection, and every command after it is lost. Nothing here judges
the boot; the failure is the caller's, and this only adds evidence beside
the serial log. Shared by tests/boot/install_test.Boot's boots.
"""

import json
import socket
import time


def _hmp(f, line):
    f.write(json.dumps({"execute": "human-monitor-command", "arguments": {"command-line": line}}) + "\n")
    f.flush()
    while True:
        reply = json.loads(f.readline())
        if "return" in reply:
            return reply["return"]
        if "error" in reply:
            return f"(error: {reply['error'].get('desc', reply['error'])})\n"


def capture(sock_path, out_path, timeout=20):
    """Best effort: any failure is written to `out_path` and swallowed."""
    with open(out_path, "w") as out:
        try:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(timeout)
            s.connect(sock_path)
            f = s.makefile("rw")
            json.loads(f.readline())   # the greeting
            f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
            f.flush()
            json.loads(f.readline())
            lines = ["info status", "info cpus", "info registers -a"]
            for line in lines:
                out.write(f"=== {line}\n{_hmp(f, line)}\n")
            time.sleep(1.0)
            out.write(f"=== info registers -a (1 s later)\n{_hmp(f, 'info registers -a')}\n")
            cpus = _hmp(f, "info cpus")
            for n in range(cpus.count("CPU #")):
                out.write(f"=== cpu {n}; x/16i $pc\n{_hmp(f, f'cpu {n}')}{_hmp(f, 'x/16i $pc')}\n")
            virtio = _hmp(f, "info virtio")
            out.write(f"=== info virtio\n{virtio}\n")
            for row in virtio.splitlines():
                if "rng" not in row:
                    continue
                line = f"info virtio-queue-status {row.split()[0]} 0"
                out.write(f"=== {line}\n{_hmp(f, line)}\n")
            s.close()
        except (OSError, ValueError) as e:
            out.write(f"capture failed: {e!r}\n")
