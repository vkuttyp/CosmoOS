#!/usr/bin/env python3
"""platform_test.py -- the AArch64 kernel discovers its platform from the
firmware tables (docs/kernel/arch/aarch64/design.md, "Platform discovery";
docs/kernel/arch/aarch64/testing.md, "make test-platforms").

    platform_test.py --image cosmoos.img --workdir DIR [--arch A] [--variants a,b,...]

Boots the image under QEMU configured to differ from virt's defaults in
every way QEMU allows (its memory layout is fixed): each variant must reach
the shell and print the platform line with what that configuration has.
The memory variants run on both architectures (x86-64 q35, AArch64 virt
with highmem on, its default); the rest are AArch64's.

  gicv2      gic-version=2: the platform line names a GICv2
  gicv3      gic-version=3: a GICv3 with its redistributors
  lowmem     highmem=off: no device or RAM above 4 GiB
  rndr       a CPU with RNDR (neoverse-v1): the pool seeds from the CPU
  nornd      no virtio-rng and a CPU without RNDR: unseeded, still boots
  psci-exit  no semihosting: `exit` ends the boot with PSCI SYSTEM_OFF,
             and QEMU exits 0 by itself
  psci-poweroff  the same, ended by BusyBox `poweroff` (init's SIGUSR2)
  noserial   -serial none and `serial=none` on the command line (the image
             signed again with the build's key, --key): QEMU's SPCR is
             always there, so the command line is what makes this the
             machine without a serial console. The framebuffer must be the
             console from the first line; the virtio console's replay of
             the log ring is where the test reads it
  mem1g, mem4g, mem8g  1, 4 and 8 GiB of RAM: the loader's direct map
             reaches the top of RAM (it was a fixed 4 GiB, and with 4 GiB
             or more the firmware loads the loader itself above it, so the
             AArch64 loader lost its own PC at the table switch: Parallels
             at its default 4 GB, docs/boot/design.md "The direct map"),
             and `free` reports at least 95% of the RAM

No variant passes fw_cfg options (opt/cosmo/*): absent configuration is the
normal case. A serial variant ends by typing `exit` (or `poweroff`) and
requires QEMU's clean-exit status -- the semihosting one, or 0 from a PSCI
power-off without semihosting; noserial is stopped once the virtio
console shows the userland. Exit 0 PASS, 1 FAIL; every boot's logs stay in DIR.
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import firmware_stall  # noqa: E402
import mkmanifest  # noqa: E402
import modsign  # noqa: E402

EXIT_SUCCESS = (0x10 << 1) | 1
PLATFORM = re.compile(r"platform: firmware OEM '([^']*)'( \(QEMU\))?; console ([^;]*); ([^;]*); "
                      r"timer PPIs (\d+) \(physical\) (\d+) \(virtual\); PSCI (\w+)")

VARIANTS = {
    "gicv2": dict(env={"QEMU_GIC": "2"}, want={"gic": r"^GICv2 GICD 0x8000000 GICC 0x8010000"}),
    "gicv3": dict(env={"QEMU_GIC": "3"}, want={"gic": r"^GICv3 GICD 0x8000000 GICR 0x80a0000"}),
    "lowmem": dict(env={"QEMU_VIRT_OPTS": ",highmem=off"}, want={}),
    "rndr": dict(env={"QEMU_CPU": "neoverse-v1", "QEMU_RNG": "0"}, want={"log": r"random: cpu source: rndr"}),
    "nornd": dict(env={"QEMU_CPU": "cortex-a72", "QEMU_RNG": "0"},
                  want={"log": r"random: cpu source: none"}),
    "psci-exit": dict(env={"QEMU_SEMIHOSTING": "0"}, rc=0,
                      want={"log": r"shutdown: exit status 0[\s\S]*shutdown: power off through PSCI SYSTEM_OFF \((HVC|SMC)\)"}),
    "psci-poweroff": dict(env={"QEMU_SEMIHOSTING": "0"}, rc=0, end=b"poweroff\n",
                          want={"log": r"init: power off requested[\s\S]*shutdown: exit status 0[\s\S]*"
                                       r"shutdown: power off through PSCI SYSTEM_OFF \((HVC|SMC)\)"}),
    "noserial": dict(env={"QEMU_SERIAL": "none"}, serial=False, cmdline=b"root=boot-disk serial=none\n",
                     want={"log": r"fbcon: no serial console: the framebuffer is the console from the first line",
                           "console": r"^framebuffer only$"}),
}
for _gib in (1, 4, 8):
    VARIANTS[f"mem{_gib}g"] = dict(env={"QEMU_MEM": f"{_gib}G"}, cmds=[b"free\n"], mem_gib=_gib,
                                   arches=("aarch64", "x86_64"), want={})
# The loader's line naming the direct map it built (boot/uefi/main.c direct_map_size).
DIRECT_MAP = re.compile(r"direct map: 0x0-0x([0-9a-f]+) \((\d+) GiB\), to the end of ([^\n]*) at 0x([0-9a-f]+)")
FREE_MEM = re.compile(r"^Mem:\s+(\d+)\s", re.M)


class Fail(Exception):
    pass


def boot(name, image, work, spec, timeout, key, arch):
    vdir = os.path.join(work, name)
    os.makedirs(vdir)
    img = os.path.join(vdir, "cosmoos.img")
    shutil.copyfile(image, img)
    if "cmdline" in spec:
        # The command line is signed (verified boot): sign the copy again.
        mkmanifest.esp_put(img, "::/cosmo/cmdline", spec["cmdline"])
        mkmanifest.resign_image(img, modsign.read_hex_key(key, "signing key"))
    vcon = os.path.join(vdir, "vcon.log")
    env = dict(os.environ)
    env.update({"QEMU_ARCH": arch, "QEMU_VCON": vcon, "QEMU_SMP": "2"})
    env.update(spec["env"])
    serial = spec.get("serial", True)
    log_path = os.path.join(vdir, "serial.log")
    buf = bytearray()
    lock = threading.Lock()
    state = {}

    def launch(attempt):
        with lock:
            del buf[:]
        state["log"] = open(log_path, "wb")
        p = subprocess.Popen([os.path.join(ROOT, "scripts", "qemu-run.sh"), img], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)

        def reader(proc=p, log=state["log"]):
            while True:
                b = proc.stdout.read1(4096)
                if not b:
                    break
                with lock:
                    buf.extend(b)
                log.write(b)
                log.flush()
        state["reader"] = threading.Thread(target=reader, daemon=True)
        state["reader"].start()
        return p

    def read_log(attempt):
        # With no serial the loader's banner never reaches stdout; the
        # virtio console carries the kernel's log instead.
        if serial:
            with lock:
                return bytes(buf)
        # Handed over once the kernel has written anything to the virtio
        # console (qemu-run.sh creates it empty): a firmware that never
        # hands over leaves it empty and is retried like any other boot.
        try:
            data = open(vcon, "rb").read()
        except OSError:
            return b""
        return data + firmware_stall.BANNER if data else b""

    def kill(p):
        p.kill()
        p.wait()

    proc, _ = firmware_stall.launch_with_retry(launch, read_log, kill, lambda: None,
                                               handover_s=min(firmware_stall.HANDOVER_S, timeout))
    deadline = time.monotonic() + timeout
    ready = b"CosmoOS userland ready"

    def text():
        if serial:
            with lock:
                return bytes(buf)
        try:
            return open(vcon, "rb").read()
        except OSError:
            return b""

    try:
        while time.monotonic() < deadline:
            t = text()
            if b"KERNEL PANIC" in t:
                raise Fail(f"{name}: kernel panic")
            if ready in t and (not serial or b"\ncosmo$ " in t):
                break
            if proc.poll() is not None:
                raise Fail(f"{name}: QEMU exited ({proc.returncode}) before the userland")
            time.sleep(0.2)
        else:
            raise Fail(f"{name}: no userland within {timeout:.0f}s")
        if serial:
            # One command per prompt: the shell reads a line at a time
            # and drops what arrived with it (a "free\nexit\n" ran free only).
            for cmd in spec.get("cmds", []):
                prompts = text().count(b"\ncosmo$ ")
                proc.stdin.write(cmd)
                proc.stdin.flush()
                cmd_deadline = time.monotonic() + 60
                while text().count(b"\ncosmo$ ") <= prompts:
                    if time.monotonic() > cmd_deadline or proc.poll() is not None:
                        raise Fail(f"{name}: no prompt after {cmd.strip().decode()!r}")
                    time.sleep(0.2)
            proc.stdin.write(spec.get("end", b"exit\n"))
            proc.stdin.flush()
            try:
                rc = proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                raise Fail(f"{name}: QEMU still running 60 s after {spec.get('end', b'exit').strip().decode()!r}")
            want_rc = spec.get("rc", EXIT_SUCCESS)
            if rc != want_rc:
                raise Fail(f"{name}: QEMU exit status {rc}, expected {want_rc}")
            state["reader"].join(timeout=5)
            # A panic after the command can still end in the expected
            # status (semihosting's failure exit is 35, a PSCI power-off 0).
            if b"KERNEL PANIC" in text():
                raise Fail(f"{name}: kernel panic after {spec.get('end', b'exit').strip().decode()!r}")
        else:
            proc.kill()
            proc.wait()
    finally:
        if proc.poll() is None:
            proc.kill()
        state["reader"].join(timeout=5)
        state["log"].close()
    # The kernel's log: the serial stream, or with no serial port the
    # virtio console, which carries the log from its driver's start on.
    with lock:
        klog = bytes(buf).decode("utf-8", "replace") if serial else ""
    if not serial and os.path.exists(vcon):
        klog = open(vcon, "rb").read().decode("utf-8", "replace")
    if "mem_gib" in spec:
        return check_memory(name, spec["mem_gib"], klog)
    # First, so an unusable GTDT is named as that rather than as a platform
    # line whose timer part ("timer: no usable GTDT (...)") does not parse.
    if re.search(r"timer: GTDT (absent|unusable)", klog):
        raise Fail(f"{name}: the GTDT was not used")
    m = PLATFORM.search(klog)
    if not m:
        raise Fail(f"{name}: no platform line in the kernel log")
    oem, qemu, console, gic, tphys, tvirt, psci = m.groups()
    if not qemu or oem != "BOCHS ":
        raise Fail(f"{name}: firmware not identified as QEMU's: OEM {oem!r}")
    if (tphys, tvirt) != ("30", "27"):
        raise Fail(f"{name}: timer PPIs {tphys}/{tvirt}, expected 30/27 from the GTDT")
    if psci not in ("HVC", "SMC"):
        raise Fail(f"{name}: no PSCI conduit, though QEMU's FADT declares one")
    if serial and not console.startswith("PL011 at 0x9000000 (SPCR, IRQ 33)"):
        raise Fail(f"{name}: console {console!r}, expected the SPCR's PL011")
    for what, pat in spec["want"].items():
        hay = {"gic": gic, "console": console}.get(what, klog)
        if not re.search(pat, hay, re.M):
            raise Fail(f"{name}: expected {pat!r} in the {what if what in ('gic', 'console') else 'kernel log'}")
    return f"{name}: PASS ({m.group(0)})"


def check_memory(name, gib, klog):
    """The loader's direct map reaches the top of RAM, and the kernel uses
    the RAM: `free`'s total is at least 95% of it (the rest is firmware's,
    the kernel image and its page array)."""
    m = DIRECT_MAP.search(klog)
    if not m:
        raise Fail(f"{name}: no 'direct map:' line from the loader")
    size, what, end = int(m.group(1), 16), m.group(3), int(m.group(4), 16)
    if size < end or size % (1 << 30):
        raise Fail(f"{name}: direct map 0x{size:x} does not cover {what} at 0x{end:x}")
    f = FREE_MEM.search(klog)
    if not f:
        raise Fail(f"{name}: no 'Mem:' line from free")
    total_kib = int(f.group(1))
    if total_kib < gib * (1 << 20) * 95 // 100:
        raise Fail(f"{name}: free reports {total_kib} KiB of {gib} GiB")
    return f"{name}: PASS (direct map {size >> 30} GiB to the end of {what}; free: {total_kib} KiB total)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--arch", default="aarch64", choices=("aarch64", "x86_64"))
    ap.add_argument("--variants", default=None, help="default: every variant of --arch")
    ap.add_argument("--timeout", type=float, default=240.0)
    ap.add_argument("--key", required=True, help="the build's signing key (MODSIGN_KEY), to sign noserial's command line")
    a = ap.parse_args()
    if not (math.isfinite(a.timeout) and 0 < a.timeout <= 3600):
        ap.error("--timeout must be a number of seconds in (0, 3600]")
    shutil.rmtree(a.workdir, ignore_errors=True)
    os.makedirs(a.workdir)
    names = (a.variants.split(",") if a.variants else
             [n for n, v in VARIANTS.items() if a.arch in v.get("arches", ("aarch64",))])
    bad = [n for n in names if n not in VARIANTS or a.arch not in VARIANTS[n].get("arches", ("aarch64",))]
    if bad:
        ap.error(f"unknown variants on {a.arch}: {', '.join(bad)}")
    failed = False
    for n in names:
        t0 = time.monotonic()
        try:
            print(f"test-platforms: {boot(n, a.image, a.workdir, VARIANTS[n], a.timeout, a.key, a.arch)} in "
                  f"{time.monotonic() - t0:.1f}s", flush=True)
        except (Fail, firmware_stall.FirmwareStall) as e:
            print(f"test-platforms: FAIL: {e} (logs in {a.workdir}/{n})", flush=True)
            failed = True
    print(f"test-platforms: {'FAIL' if failed else 'PASS'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
