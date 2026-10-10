#!/usr/bin/env python3
"""Roadmap M2's acceptance test: install to a blank disk, reboot from it,
write a file, reboot, read it (docs/userland/testing.md, "test-install").

    install_test.py --image cosmoos.img --workdir DIR [--stages install,...]

Stage `install`: boot the live image with a blank 256 MiB virtio disk
attached, find that disk in `cosmo-install --list` by its size, install on
it, check that a second install without --force is refused, mount the new
root and look at it, and power off. Then, on the host, read the disk
image: a valid GPT, partition 1 the boot image's ESP byte for byte except
the command-line slot, the slot naming partition 2 by its PARTUUID, and
partition 2 of the CosmoOS root type.

Each boot is QEMU through scripts/qemu-run.sh with the serial console on
stdin/stdout; commands are typed at the shell's prompt as the shell test
does (tests/boot/shelltest.py). Every boot's serial log is kept in the
work directory. Exit 0 PASS, 1 FAIL.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPT = b"\ncosmo$ "
TARGET_MIB = 256
SS = 512
EXIT_SUCCESS = (0x10 << 1) | 1
MARKER = b"#cosmo-cmdline v1\n"
ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
ROOT_TYPE = uuid.UUID("c9b09224-e00d-418e-846a-9f1d9a61bfd5")


class Fail(Exception):
    pass


class Boot:
    """One QEMU boot: the serial log accumulates in memory and on disk."""

    def __init__(self, name, image, workdir, env_extra, timeout):
        self.name = name
        self.log_path = os.path.join(workdir, f"boot-{name}.log")
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.deadline = time.monotonic() + timeout
        env = dict(os.environ)
        env.update(env_extra)
        self.t0 = time.monotonic()
        self.proc = subprocess.Popen([os.path.join(ROOT, "scripts", "qemu-run.sh"), image], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        self.log = open(self.log_path, "wb")
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.prompts_seen = 0

    def _read(self):
        while True:
            b = self.proc.stdout.read1(4096) if hasattr(self.proc.stdout, "read1") else self.proc.stdout.read(1)
            if not b:
                break
            with self.lock:
                self.buf += b
            self.log.write(b)
            self.log.flush()

    def text(self):
        with self.lock:
            return self.buf.decode("utf-8", "replace").replace("\r", "")

    def wait_prompt(self, what):
        """Wait for the next prompt after the ones already consumed."""
        while time.monotonic() < self.deadline:
            with self.lock:
                n = self.buf.count(PROMPT)
            if n > self.prompts_seen:
                self.prompts_seen = n
                return
            if self.proc.poll() is not None:
                raise Fail(f"{self.name}: QEMU exited (code {self.proc.returncode}) waiting for {what}")
            time.sleep(0.1)
        raise Fail(f"{self.name}: no prompt after {what} before the deadline")

    def run(self, cmd):
        """Type `cmd`, wait for the next prompt, return what it printed."""
        before = len(self.text())
        self.proc.stdin.write(cmd.encode() + b"\n")
        self.proc.stdin.flush()
        self.wait_prompt(repr(cmd))
        out = self.text()[before:]
        return out

    def finish(self, cmd="exit"):
        """Type `cmd` (ending init), wait for QEMU to exit, return its code."""
        self.proc.stdin.write(cmd.encode() + b"\n")
        self.proc.stdin.flush()
        try:
            rc = self.proc.wait(timeout=max(1.0, self.deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise Fail(f"{self.name}: QEMU did not exit after {cmd!r}")
        self.reader.join(timeout=5)
        self.log.close()
        return rc

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        try:
            self.log.close()
        except Exception:
            pass

    def elapsed(self):
        return time.monotonic() - self.t0


def need(text, pattern, what):
    m = re.search(pattern, text, re.M)
    if not m:
        raise Fail(f"{what}: no line matching {pattern!r}")
    return m


# --- reading a GPT on the host ----------------------------------------------------

def parse_gpt(data):
    """(disk GUID, [(index, type, unique, first, last)]) of a valid table, or raise."""
    n = len(data) // SS
    if data[510:512] != b"\x55\xaa" or data[446 + 4] != 0xEE:
        raise Fail("target: no protective MBR")
    tables = []
    for lba in (1, n - 1):
        h = bytearray(data[lba * SS:lba * SS + 92])
        if h[:8] != b"EFI PART":
            raise Fail(f"target: no GPT header at LBA {lba}")
        crc = int.from_bytes(h[16:20], "little")
        h[16:20] = b"\0\0\0\0"
        if zlib.crc32(bytes(h)) & 0xFFFFFFFF != crc:
            raise Fail(f"target: GPT header CRC at LBA {lba}")
        my = int.from_bytes(h[24:32], "little")
        entries_lba = int.from_bytes(h[72:80], "little")
        count = int.from_bytes(h[80:84], "little")
        size = int.from_bytes(h[84:88], "little")
        ecrc = int.from_bytes(h[88:92], "little")
        arr = data[entries_lba * SS:entries_lba * SS + count * size]
        if my != lba or zlib.crc32(arr) & 0xFFFFFFFF != ecrc:
            raise Fail(f"target: GPT entries at LBA {lba} do not match their header")
        parts = []
        for i in range(count):
            e = arr[i * size:(i + 1) * size]
            if e[:16] == bytes(16):
                continue
            parts.append((i + 1, uuid.UUID(bytes_le=bytes(e[:16])), uuid.UUID(bytes_le=bytes(e[16:32])),
                          int.from_bytes(e[32:40], "little"), int.from_bytes(e[40:48], "little")))
        tables.append((bytes(h[56:72]), parts))
    if tables[0] != tables[1]:
        raise Fail("target: primary and backup GPT disagree")
    return tables[0]


def esp_of(image_bytes):
    """Partition 1's bytes of a GPT disk image."""
    _, parts = parse_gpt(image_bytes)
    for idx, typ, _, first, last in parts:
        if idx == 1 and typ == ESP_TYPE:
            return image_bytes[first * SS:(last + 1) * SS]
    raise Fail("boot image: no ESP as partition 1")


# --- stages -----------------------------------------------------------------------

def stage_install(args, results):
    work = args.workdir
    live = os.path.join(work, "live")
    os.makedirs(live, exist_ok=True)
    image = os.path.join(live, "cosmoos.img")
    shutil.copyfile(args.image, image)   # the scratch disks qemu-run.sh makes land beside it
    target = os.path.join(work, "target.img")
    with open(target, "wb") as f:
        f.truncate(TARGET_MIB << 20)
    env = {
        "QEMU_EXTRA": (os.environ.get("QEMU_EXTRA", "") +
                       f" -drive if=none,id=inst,format=raw,file={target} -device virtio-blk-pci,drive=inst").strip(),
    }
    b = Boot("install", image, work, env, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        out = b.run("cosmo-install --list")
        m = need(out, rf"^(\S+)\s+-\s+{(TARGET_MIB << 20) // SS}\s+{SS}\s", "cosmo-install --list")
        dev = m.group(1)
        t0 = time.monotonic()
        out = b.run(f"cosmo-install {dev}; echo install-status=$?")
        install_s = time.monotonic() - t0
        need(out, r"^install-status=0$", "cosmo-install")
        m = need(out, rf"^cosmo-install: installed on {dev}: esp ({dev}p?1), root ({dev}p?2), "
                      r"root=PARTUUID=([0-9a-f-]{36})$", "cosmo-install")
        root_dev, partuuid = m.group(2), m.group(3)
        out = b.run(f"cosmo-install {dev}; echo again-status=$?")
        need(out, r"^again-status=3$", "a second install without --force")
        need(out, r"already has a partition table; --force replaces it", "a second install without --force")
        out = b.run(f"mkdir /mnt/v && mount {root_dev} /mnt/v cosmofs && ls /mnt/v/sbin && ls /mnt/v && "
                    f"cat /mnt/v/etc/rc && cosmo-install --force {dev}; echo busy-status=$? && "
                    "umount /mnt/v && echo look-ok")
        need(out, r"^busy-status=3$", "an install over a mounted disk")
        need(out, rf"^cosmo-install: {dev} is mounted$", "an install over a mounted disk")
        need(out, r"^look-ok$", "the installed root")
        need(out, r"^cosmo-install$", "the installed /sbin")
        need(out, r"^svc boot$", "the installed /etc/rc")
        for d in ("bin", "sbin", "etc", "dev", "proc", "tmp", "mnt", "var"):
            need(out, rf"^{d}$", f"the installed root's /{d}")
        rc = b.finish()
        if rc != EXIT_SUCCESS:
            raise Fail(f"install boot: QEMU exit code {rc}, expected {EXIT_SUCCESS}")
    except Exception:
        b.kill()
        raise
    results.append(f"install: PASS in {b.elapsed():.1f}s (cosmo-install {install_s:.1f}s on {dev}, "
                   f"root {root_dev} partuuid {partuuid})")

    # The disk, read on the host.
    data = open(target, "rb").read()
    _, parts = parse_gpt(data)
    if len(parts) != 2:
        raise Fail(f"target: {len(parts)} partitions, expected 2")
    (i1, t1, u1, f1, l1), (i2, t2, u2, f2, l2) = parts
    if (i1, t1, i2, t2) != (1, ESP_TYPE, 2, ROOT_TYPE):
        raise Fail(f"target: partitions {parts}")
    if str(u2) != partuuid:
        raise Fail(f"target: partition 2 is {u2}, the installer said {partuuid}")
    esp_live = esp_of(open(args.image, "rb").read())
    esp_new = data[f1 * SS:(l1 + 1) * SS]
    if len(esp_new) != len(esp_live):
        raise Fail("target: partition 1 is not the size of the boot ESP")
    diff = [s for s in range(0, len(esp_live), SS) if esp_live[s:s + SS] != esp_new[s:s + SS]]
    if len(diff) != 1 or not esp_live[diff[0]:diff[0] + SS].startswith(MARKER):
        raise Fail(f"target: partition 1 differs from the boot ESP in {len(diff)} sectors, expected the slot alone")
    slot = esp_new[diff[0]:diff[0] + SS]
    want = MARKER + f"root=PARTUUID={partuuid}\n".encode()
    if slot != want + bytes(SS - len(want)):
        raise Fail(f"target: the slot reads {slot[:80]!r}")
    results.append(f"install: target disk: GPT valid, ESP = boot ESP but the slot, slot root=PARTUUID={partuuid}, "
                   f"root partition sectors {f2}-{l2}")
    return {"target": target, "partuuid": partuuid}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--stages", default="install")
    ap.add_argument("--timeout", type=float, default=240.0, help="per boot")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    results = []
    t0 = time.monotonic()
    try:
        stages = args.stages.split(",")
        if "install" in stages:
            stage_install(args, results)
    except Fail as e:
        for r in results:
            print(f"test-install: {r}")
        print(f"test-install: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-install: {r}")
    print(f"test-install: PASS in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
