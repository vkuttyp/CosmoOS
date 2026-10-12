#!/usr/bin/env python3
"""acpi-discovery-probe.py -- the three defects the platform-discovery unit
fixed, each shown failing with the old line and passing without it
(docs/audit/2026-10-13-acpi-discovery-report.md, "Defects found and fixed").

    acpi-discovery-probe.py bar  --arch x86_64|aarch64 [--old]
    acpi-discovery-probe.py gtdt [--old]
    acpi-discovery-probe.py isb  [--old] --vm NAME --hds FILE --socket PATH [--boots N]

Each check works in a throwaway worktree of --tree (default HEAD) and keeps
its logs under out/acpi-discovery-probe/<check>-<arch>-<mode>. The fixes
were part of one commit with the rest of the unit, so --old does not
reverse a commit (that would remove the discovery the checks run on): it
puts back exactly the defect's own expression, named below, and refuses
to run if that expression is not found where the fix left it.

  bar   PCI 64-bit BAR sizing. --old: pci_bar_mem_size returns ~mask + 1.
        Runs the host test test_pci_bar, which fails at "a 64-bit BAR whose
        upper half reads zero" (Parallels' NVMe controller). Both
        architectures' host builds.
  gtdt  The GTDT's timer GSIVs. --old: read at 80 and 88 (CntReadBase and
        the platform timer count) instead of 56 and 64. Builds the AArch64
        release image and runs test-platforms' gicv3 variant, which fails:
        the platform line's timer PPIs are not 30/27 and the kernel says
        "timer: GTDT unusable". AArch64 only: x86-64 reads no GTDT.
  isb   The ISB after arch_mmu_map's table stores. --old: dsb only. QEMU's
        TCG never shows this, so the check boots a Parallels Desktop VM on
        Apple Silicon --boots times with the AArch64 release image (copied
        over --hds, the VM's boot disk, which must be the image's size) and
        counts panics against shell prompts on the serial socket. Measured
        for the report: 4 of 6 panicked with --old, 0 of 6 without.
        AArch64 only, and not deterministic: a rate, not a single boot.

Exit 0 when the check gave the expected result for the mode (PASS fixed,
FAIL old; for isb, any panic with --old and none without), else 1.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MAKE = "gmake" if sys.platform == "darwin" else "make"

# (file, the fixed text, the old text): the defect's own expression.
OLD = {
    "bar": [("kernel/include/kernel/pci_bar.h", "    return mask & ~(mask - 1);\n", "    return ~mask + 1;\n")],
    "gtdt": [("kernel/arch/aarch64/timer.c", "        memcpy(&phys, b + 56, 4);\n        memcpy(&virt, b + 64, 4);\n",
              "        memcpy(&phys, b + 80, 4);\n        memcpy(&virt, b + 88, 4);\n")],
    "isb": [("kernel/arch/aarch64/mmu.c", " * report.md). Linux pairs the two the same way for kernel mappings. */\n    dsb_ishst();\n    isb();\n",
             " * report.md). Linux pairs the two the same way for kernel mappings. */\n    dsb_ishst();\n")],
}


def worktree(work, rev):
    tree = Path(tempfile.mkdtemp(prefix="worktree-", dir=work))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), rev], check=True,
                   stdout=subprocess.DEVNULL)
    return tree


def apply_old(tree, check):
    for rel, fixed, old in OLD[check]:
        p = tree / rel
        s = p.read_text()
        if s.count(fixed) != 1:
            raise SystemExit(f"{check}: the fixed text is not found exactly once in {rel}; the probe is stale")
        p.write_text(s.replace(fixed, old))


def run_logged(cmd, log, cwd=None, env=None):
    with open(log, "w") as out:
        return subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, cwd=cwd, env=env).returncode


def check_bar(tree, work, arch):
    log = work / "host-test.log"
    rc = run_logged([MAKE, "-C", str(tree), f"ARCH={arch}", "host-test"], log)
    text = log.read_text(errors="replace")
    # The harness prints "  FAIL file:line: expr" under the test's name,
    # then the name's verdict.
    failed = re.search(r"test_pci_bar\.c:\d+: pci_bar_mem_size\(mask64\(0x[0-9a-f]+u, 0x00000000u\)\)", text)
    print(f"bar: host-test rc={rc}{'; the read-only upper half FAILS' if failed else ''} ({log})")
    return rc == 0, bool(failed)


def build_release_image(tree, work):
    log = work / "build.log"
    rc = run_logged([MAKE, "-C", str(tree), "ARCH=aarch64", "BUILD=release", "-j6", "image"], log)
    if rc:
        raise SystemExit(f"build failed ({log})")
    return tree / "out" / "aarch64-release" / "cosmoos.img"


def check_gtdt(tree, work):
    img = build_release_image(tree, work)
    # The key the build signed with, as its signing stamp records it: the
    # gicv3 variant does not re-sign, but platform_test.py requires --key.
    stamp = (tree / "out" / "aarch64-release" / "gen" / "signing.stamp").read_text()
    key = re.search(r"^MODSIGN_KEY=(.*)$", stamp, re.M).group(1)
    log = work / "platforms.log"
    rc = run_logged([sys.executable, str(tree / "tests" / "boot" / "platform_test.py"), "--image", str(img),
                     "--workdir", str(work / "platforms"), "--variants", "gicv3", "--key", key], log)
    text = log.read_text(errors="replace")
    print(f"gtdt: test-platforms gicv3 rc={rc}: " + " | ".join(l for l in text.splitlines() if "gicv3" in l))
    return rc == 0, rc != 0 and re.search(r"timer PPIs|GTDT was not used", text) is not None


def parallels_boot(vm, hds, sock, img, log, timeout):
    subprocess.run(["prlctl", "stop", vm, "--kill"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if os.path.getsize(img) != os.path.getsize(hds):
        raise SystemExit(f"isb: {hds} is not the image's size ({os.path.getsize(img)} bytes)")
    shutil.copyfile(img, hds)
    subprocess.run(["prlctl", "start", vm], check=True, stdout=subprocess.DEVNULL)
    end = time.monotonic() + timeout
    s = None
    while time.monotonic() < end:
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(sock)
            break
        except OSError:
            s = None
            time.sleep(0.05)
    data = b""
    if s is not None:
        s.settimeout(0.5)
        # Read to the shell prompt, or past a panic to its end (the reason
        # follows the "KERNEL PANIC" line), or to the deadline.
        while time.monotonic() < end and b"\ncosmo$ " not in data:
            if b"KERNEL PANIC" in data:
                end = min(end, time.monotonic() + 3)
                if b"halting" in data:
                    break
            try:
                d = s.recv(65536)
                if not d:
                    break
                data += d
            except socket.timeout:
                pass
        s.close()
    subprocess.run(["prlctl", "stop", vm, "--kill"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    Path(log).write_bytes(data)
    return "panic" if b"KERNEL PANIC" in data else "shell" if b"\ncosmo$ " in data else "neither"


def check_isb(tree, work, a):
    img = build_release_image(tree, work)
    seen = []
    for i in range(a.boots):
        r = parallels_boot(a.vm, a.hds, a.socket, img, work / f"boot{i + 1}.serial", a.boot_timeout)
        seen.append(r)
        print(f"isb: boot {i + 1}: {r}", flush=True)
    panics = seen.count("panic")
    print(f"isb: {panics} of {a.boots} panicked, {seen.count('shell')} reached the shell")
    for i, r in enumerate(seen):
        if r == "panic":
            text = (work / f"boot{i + 1}.serial").read_text(errors="replace")
            why = re.search(r"KERNEL PANIC:?\s*(.*)", text[text.index("KERNEL PANIC"):])
            print(f"isb: boot {i + 1}: {why.group(1).strip()[:160] if why else '?'}")
    return panics == 0 and seen.count("shell") == a.boots, panics > 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("check", choices=sorted(OLD))
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="aarch64")
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--vm")
    ap.add_argument("--hds")
    ap.add_argument("--socket")
    ap.add_argument("--boots", type=int, default=6)
    ap.add_argument("--boot-timeout", type=float, default=90.0)
    a = ap.parse_args()
    if a.check in ("gtdt", "isb") and a.arch != "aarch64":
        ap.error(f"{a.check} is AArch64 only")
    if a.check == "isb" and not (a.vm and a.hds and a.socket):
        ap.error("isb needs --vm, --hds and --socket (a Parallels VM with its serial port on that socket)")
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "acpi-discovery-probe" / f"{a.check}-{a.arch}-{mode}"
    if work.exists() and any(work.iterdir()):
        raise SystemExit(f"refusing to overwrite nonempty probe output {work}")
    work.mkdir(parents=True, exist_ok=True)
    tree = worktree(work, a.tree)
    try:
        if a.old:
            apply_old(tree, a.check)
        if a.check == "bar":
            passed, old_seen = check_bar(tree, work, a.arch)
        elif a.check == "gtdt":
            passed, old_seen = check_gtdt(tree, work)
        else:
            passed, old_seen = check_isb(tree, work, a)
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])
    ok = old_seen if a.old else passed
    print(f"PROBE: {'PASS' if ok else 'FAIL'} ({a.check}, {mode}: "
          f"{'the defect shows' if old_seen else 'no defect seen'})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
