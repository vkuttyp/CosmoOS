#!/usr/bin/env python3
"""Prove that nothing frees a packet after a failed mbufq_enqueue.

    mbuf-double-free-probe.py --arch x86_64|aarch64 [--old] [--tree REV]

mbufq_enqueue frees the packet when the queue is full (N2). tap_transmit,
rxq_purge's two re-queues and the ping unit's raw-socket delivery freed it
again. The second free usually did nothing -- the reference count was
already 0 -- and harmed only when another CPU had reused the mbuf in
between, freeing a live packet: net-raw-icmp failed one 2-CPU boot in
eight. m_free now panics, in debug builds, on an mbuf with no reference
left. --old reverse-applies the fix commit (tap.c, netif.c, raw.c) in a
throwaway worktree, keeping that check: the boot then panics with it
(the tap self-test fills the tap's queue every boot). Logs stay under
out/mbuf-double-free-probe/<arch>-<mode>. Exit 0 when the mode shows what
it should (fixed: the boot passes, no such panic; old: the panic), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "network: a failed mbufq_enqueue has already freed the packet; tap_transmit, rxq_purge and raw delivery freed it again"
SOURCES = ["kernel-services/network/tap.c", "kernel-services/network/netif.c", "kernel-services/network/raw.c"]
OLD_LINE = "KERNEL PANIC: m_free: mbuf"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "mbuf-double-free-probe" / f"{a.arch}-{mode}"
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
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--", *SOURCES])
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        serial = work / "boot.serial"
        with (work / "boot.result").open("w") as out:
            rc = subprocess.run([make, "-C", str(tree), f"ARCH={a.arch}", f"BOOT_LOG={serial}", "test"],
                                stdout=out, stderr=subprocess.STDOUT).returncode
        log = serial.read_text(errors="replace") if serial.exists() else ""
        if a.old:
            ok = rc != 0 and OLD_LINE in log
            seen = "it panics on the double free" if OLD_LINE in log else "no double-free panic"
        else:
            ok = rc == 0 and OLD_LINE not in log
            seen = "the boot passes" if ok else ("a double-free panic" if OLD_LINE in log else "the boot fails")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
