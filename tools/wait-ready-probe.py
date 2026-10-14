#!/usr/bin/env python3
"""Prove waitqueue_prepare never turns a READY thread back to BLOCKED (S34).

    wait-ready-probe.py --arch x86_64|aarch64 [--old] [--tree REV]

A wait that arms several queues (poll) prepares each in turn; a wake from
one armed earlier makes the thread READY and queues it, and the next
prepare used to write BLOCKED over that, leaving a BLOCKED thread on a run
queue (found under scp and sftp traffic: rr_enqueue's assertion). The
wait-multi-prepare self-test (every debug boot) arms two queues with a wake
between. --old reverse-applies the fix commit's kernel/scheduler/wait.c in a
throwaway worktree: the test then fails !breached. Logs stay under
out/wait-ready-probe/<arch>-<mode>. Exit 0 when the mode shows what it should
(fixed: the boot passes; old: that failure), else 1.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = 'sched: waitqueue_prepare never turns a READY thread back to BLOCKED (S34)'
SOURCE = 'kernel/scheduler/wait.c'
OLD_LINES = ['SELFTEST: wait-multi-prepare ... FAIL']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / 'wait-ready-probe' / f"{a.arch}-{mode}"
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
            rc = subprocess.run([make, "-C", str(tree), f"ARCH={a.arch}", f"BOOT_LOG={serial}", "test"], env={**os.environ, "QEMU_SMP": "2"},
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
