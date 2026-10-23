#!/usr/bin/env python3
"""Roadmap 1.2 B3's PCIe hotplug test (DRV-6, BLK-5;
docs/drivers/pci/testing.md, "test-hotplug").

    hotplug_test.py --image cosmoos.img --workdir DIR [--timeout S]

The live image is booted with three PCIe root ports (QEMU pcie-root-port):
rp1 holds a blank virtio disk, rp2 and rp3 are empty. QEMU's native PCIe
hotplug is driven through QMP, as an operator's device_del and device_add
are (on q35 its ACPI hotplug of root ports is turned off, so the slots'
own capability carries the events; virt has native hotplug only):

  1. every root port is watched: one `pci: ... hot-plug slot` line each,
     saying whether its events come by MSI or are polled;
  2. remove, idle: device_del of rp1's disk; QEMU's DEVICE_DELETED comes
     (which it sends only once the guest has powered the slot off), and
     the guest says the disk is removed;
  3. add: a disk with a GPT into rp2; the guest enumerates it, the virtio
     driver registers it and its two partitions are scanned without being
     asked (BLK-5);
  4. remove under I/O: a cosmofs on that disk's second partition, a shell
     loop writing to it, device_del; the device goes, the shell answers,
     no panic;
  5. NVMe: an NVMe controller added into rp3 is enumerated, its namespace
     registered, and removed again.

Each step's time from the QMP command to the guest's line is reported.
Exit 0 PASS, 1 FAIL.
"""

import argparse
import json
import os
import re
import shutil
import socket
import struct
import sys
import tempfile
import time
import uuid
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from install_test import Boot, Fail, need, EXIT_SUCCESS, ESP_TYPE, ROOT_TYPE  # noqa: E402

SS = 512
DISK_MIB = 64


def write_gpt(path, mib):
    """A disk image of `mib` MiB with a GPT of two partitions (a small one
    and the rest), both copies valid -- what the late scan must find."""
    nsect = (mib << 20) // SS
    entries = bytearray(128 * 128)
    first, mid, last = 2048, 2048 + 8192 - 1, nsect - 34
    for i, (ptype, a, b) in enumerate(((ESP_TYPE, first, mid), (ROOT_TYPE, mid + 1, last))):
        e = struct.pack("<16s16sQQQ72s", ptype.bytes_le, uuid.uuid4().bytes_le, a, b, 0, b"")
        entries[i * 128:(i + 1) * 128] = e
    ecrc = zlib.crc32(bytes(entries)) & 0xffffffff
    disk_guid = uuid.uuid4().bytes_le

    def header(my, alt, entries_lba):
        h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, my, alt, 34, nsect - 34,
                        disk_guid, entries_lba, 128, 128, ecrc)
        crc = zlib.crc32(h) & 0xffffffff
        h = h[:16] + struct.pack("<I", crc) + h[20:]
        return h + b"\0" * (SS - len(h))

    mbr = bytearray(SS)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xff, 0xff, 0xff, 1, min(nsect - 1, 0xffffffff))
    mbr[510:512] = b"\x55\xaa"
    with open(path, "wb") as f:
        f.truncate(mib << 20)
        f.seek(0)
        f.write(bytes(mbr))
        f.write(header(1, nsect - 1, 2))
        f.write(bytes(entries))
        f.seek((nsect - 33) * SS)
        f.write(bytes(entries))
        f.write(header(nsect - 1, 1, nsect - 33))


class Qmp:
    """A QMP client on a unix socket: commands answered in order, events kept."""

    def __init__(self, path, deadline):
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise Fail("QMP: no socket")
                time.sleep(0.2)
        self.buf = b""
        self.events = []
        self._msg()   # the greeting
        self.cmd("qmp_capabilities")

    def _msg(self, timeout=30.0):
        self.s.settimeout(timeout)
        while b"\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise Fail("QMP: socket closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **args):
        self.s.sendall(json.dumps({"execute": name, "arguments": args}).encode() + b"\n")
        while True:
            m = self._msg()
            if "event" in m:
                self.events.append(m)
                continue
            if "error" in m:
                raise Fail(f"QMP {name}: {m['error']}")
            return m.get("return")

    def wait_event(self, name, device, timeout):
        end = time.monotonic() + timeout
        while True:
            for e in self.events:
                if e["event"] == name and e.get("data", {}).get("device") == device:
                    self.events.remove(e)
                    return
            left = end - time.monotonic()
            if left <= 0:
                raise Fail(f"QMP: no {name} for {device} within {timeout:.0f}s")
            try:
                m = self._msg(timeout=left)
            except socket.timeout:
                continue
            if "event" in m:
                self.events.append(m)


def wait_line(b, pattern, start, timeout, what):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        m = re.search(pattern, b.text()[start:], re.M)
        if m:
            return m
        time.sleep(0.1)
    raise Fail(f"{what}: no line matching {pattern!r} within {timeout:.0f}s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args()
    arch = os.environ.get("QEMU_ARCH", "x86_64")
    work = args.workdir
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    image = os.path.join(work, "cosmoos.img")
    shutil.copyfile(args.image, image)
    hot0, hot1, hotn = (os.path.join(work, n) for n in ("hot0.img", "hot1.img", "hotn.img"))
    for p in (hot0, hotn):
        with open(p, "wb") as f:
            f.truncate(DISK_MIB << 20)
    write_gpt(hot1, DISK_MIB)
    sockdir = tempfile.mkdtemp(prefix="cosmo-hp-")
    qmp_path = os.path.join(sockdir, "qmp.sock")
    extra = (f"-device pcie-root-port,id=rp1,chassis=21,slot=1 -device pcie-root-port,id=rp2,chassis=22,slot=2 "
             f"-device pcie-root-port,id=rp3,chassis=23,slot=3 "
             f"-drive if=none,id=hot0,format=raw,file={hot0} -device virtio-blk-pci,id=hot0dev,drive=hot0,bus=rp1 "
             f"-drive if=none,id=hot1,format=raw,file={hot1} -drive if=none,id=hotn,format=raw,file={hotn} "
             f"-qmp unix:{qmp_path},server=on,wait=off")
    if arch == "x86_64":
        extra += " -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off"
    env = {"QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") + " " + extra).strip()}
    results = []
    b = Boot("hotplug", image, work, env, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        q = Qmp(qmp_path, time.monotonic() + 30)
        boot_log = b.text()

        # 1. The slots.
        slots = re.findall(r"^\[ INFO\] pci: (\S+): hot-plug slot \d+ \(bus ([0-9a-f]{2})\)[^,]*(?:, attention button)?, "
                           r"(events by MSI|no MSI: polled every 500 ms)$", boot_log, re.M)
        if len(slots) < 3:
            raise Fail(f"slots: {len(slots)} hot-plug slots announced, expected 3 root ports")
        mode = sorted(set(s[2] for s in slots))
        results.append(f"slots: {len(slots)} watched, {', '.join(mode)}")

        # The disk in rp1: the 64 MiB one.
        out = b.run("cosmo-install --list")
        m = need(out, rf"^(\S+)\s+-\s+{(DISK_MIB << 20) // SS}\s+{SS}\s", "rp1's disk in cosmo-install --list")
        d0 = m.group(1)

        # 2. Remove, idle.
        start = len(b.text())
        t0 = time.monotonic()
        q.cmd("device_del", id="hot0dev")
        q.wait_event("DEVICE_DELETED", "hot0dev", 15)
        t_del = time.monotonic() - t0
        wait_line(b, rf"^\[ INFO\] blk: {re.escape(d0)} removed$", start, 10, "remove, idle")
        need(b.text()[start:], r"attention button: removing the device behind it", "remove, idle")
        results.append(f"remove idle: {d0} gone, DEVICE_DELETED {t_del:.2f}s after device_del")

        # 3. Add a disk with a GPT: enumerated, registered, partitions scanned.
        start = len(b.text())
        t0 = time.monotonic()
        q.cmd("device_add", driver="virtio-blk-pci", id="hot1dev", drive="hot1", bus="rp2")
        m = wait_line(b, r"^\[ INFO\] part: (\S+) registered after boot: (\d+) partition\(s\)", start, 20, "add")
        t_add = time.monotonic() - t0
        d1, nparts = m.group(1), int(m.group(2))
        if nparts != 2:
            raise Fail(f"add: {d1} has {nparts} partitions scanned, expected 2")
        need(b.text()[start:], r"a device was added", "add")
        results.append(f"add: {d1} and its 2 partitions {t_add:.2f}s after device_add")
        p2 = d1 + ("p2" if d1[-1].isdigit() else "2")

        # 4. Remove under I/O.
        out = b.status(f"mkfs.cosmofs /dev/{p2}", "mkfs-status")
        need(out, r"^mkfs-status=0$", "mkfs on the hot-added partition")
        out = b.run(f"mkdir -p /mnt/h && mount -t cosmofs /dev/{p2} /mnt/h && echo mounted-h")
        need(out, r"^mounted-h$", "mount on the hot-added partition")
        b.run("(while true; do cat /bin/busybox > /mnt/h/f || break; sync; done) > /dev/null 2>&1 &")
        time.sleep(2)
        start = len(b.text())
        t0 = time.monotonic()
        q.cmd("device_del", id="hot1dev")
        q.wait_event("DEVICE_DELETED", "hot1dev", 20)
        t_io = time.monotonic() - t0
        wait_line(b, rf"^\[ INFO\] blk: {re.escape(d1)} removed$", start, 15, "remove under I/O")
        out = b.run("echo still-here")
        need(out, r"^still-here$", "the shell after a removal under I/O")
        b.run("kill %1 2>/dev/null; umount /mnt/h 2>/dev/null; true")
        results.append(f"remove under I/O: {d1} gone with a writer running, DEVICE_DELETED {t_io:.2f}s after "
                       f"device_del; the shell answers")

        # 5. NVMe in and out.
        start = len(b.text())
        q.cmd("device_add", driver="nvme", id="hotnvme", drive="hotn", serial="cosmo-hotnvme", bus="rp3")
        m = wait_line(b, r"^\[ INFO\] blk: (nvme\S+): \d+ sectors", start, 20, "NVMe add")
        dn = m.group(1)
        start = len(b.text())
        q.cmd("device_del", id="hotnvme")
        q.wait_event("DEVICE_DELETED", "hotnvme", 20)
        wait_line(b, rf"^\[ INFO\] blk: {re.escape(dn)} removed$", start, 15, "NVMe remove")
        results.append(f"nvme: {dn} added and removed")

        text = b.text()
        for bad in ("KERNEL PANIC", "KASSERT", "page fault in kernel"):
            if bad in text:
                raise Fail(f"the log has '{bad}'")
        rc = b.finish("poweroff")
        if rc != EXIT_SUCCESS:
            raise Fail(f"QEMU exit code {rc}, expected {EXIT_SUCCESS}")
    except Exception as e:
        b.kill()
        print(f"test-hotplug: FAIL: {e}\ntest-hotplug: logs in {work}")
        return 1
    finally:
        shutil.rmtree(sockdir, ignore_errors=True)
    for r in results:
        print(f"test-hotplug: {r}")
    print(f"test-hotplug: PASS ({arch}) in {b.elapsed():.1f}s; logs in {work}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
