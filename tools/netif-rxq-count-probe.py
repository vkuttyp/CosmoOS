#!/usr/bin/env python3
"""Prove netif_rxq_count counts only its own interface's packets.

    netif-rxq-count-probe.py --arch x86_64|aarch64 [--old] [--tree REV]

netif_rxq_count documented "the number of queued receive packets from
`nif`" and summed every CPU's receive queue, every interface's packets:
net-netif-lifetime asserted on it around a packet to a gone interface and
failed in CI when other traffic arrived or drained between its two reads
(PR #384, x86-64 entropy boots). netif-rxq-count (every debug boot) parks
CPU 0's worker on a decoy interface's first packet, so its second stays
queued, and requires a second interface to count 0. --old reverse-applies
the fix commit's kernel-services/network/netif.c in a throwaway worktree:
the second interface counts the decoy's packet, NETIF-RXQ-COUNT: FAIL,
every time. One CPU. Logs stay under
out/netif-rxq-count-probe/<arch>-<mode>. Exit 0 when the mode shows what it
should (fixed: the boot passes; old: NETIF-RXQ-COUNT: FAIL), else 1.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "netif: netif_rxq_count counts only its interface's packets"
SOURCE = 'kernel-services/network/netif.c'
OLD_LINES = ['NETIF-RXQ-COUNT: FAIL']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / 'netif-rxq-count-probe' / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    tree = Path(tempfile.mkdtemp(prefix="worktree-", dir=work))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            sha = subprocess.check_output(["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings",
                                           f"--grep={SUBJECT}", a.tree], text=True).strip()
            if not sha:
                sys.exit(f"no commit '{SUBJECT}' reachable from {a.tree}")
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--", SOURCE])
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        serial = work / "boot.serial"
        with (work / "boot.result").open("w") as out:
            rc = subprocess.run([make, "-C", str(tree), f"ARCH={a.arch}", f"BOOT_LOG={serial}", "test"], env={**os.environ, "QEMU_SMP": "1"},
                                stdout=out, stderr=subprocess.STDOUT).returncode
        log = serial.read_text(errors="replace") if serial.exists() else ""
        hit = next((p for p in OLD_LINES if p in log), None)
        if a.old:
            ok = rc != 0 and hit is not None
            seen = f"it fails: {hit}" if hit else "no failure of the kind named"
        else:
            ok = rc == 0 and hit is None
            seen = "the boot passes" if ok else (f"it fails: {hit}" if hit else "the boot fails")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
