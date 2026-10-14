#!/usr/bin/env python3
"""Prove io_poll waits out its timeout's callback before it returns.

    poll-timer-probe.py --arch x86_64|aarch64 [--old] [--tree REV]

io_poll's timer and the alarm it writes live in io_poll's frame; the
callback runs on the CPU that armed it, and io_poll cancelled with
timer_cancel, which does not wait for a running callback: a poller moved to
another CPU returned while it ran, and the callback wrote into a dead frame
and woke a thread through a pointer read from it. poll-timer-outlives (every
debug boot with three CPUs) holds the callback and moves the poller. --old
reverse-applies the fix commit's kernel/io/poll.c in a throwaway worktree
and boots four CPUs: the test fails !early, or -- the race itself, as on
AArch64 -- alarm_fired faults on the dead frame's pointer first. Logs stay
under out/poll-timer-probe/<arch>-<mode>. Exit 0 when the mode shows what it
should (fixed: the boot passes; old: either failure), else 1.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "poll: wait out the timeout's callback before io_poll returns"
SOURCE = 'kernel/io/poll.c'
OLD_LINES = ['SELFTEST: poll-timer-outlives ... FAIL', 'KERNEL PANIC: page fault: kernel read at 0x00000000000000a0']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / 'poll-timer-probe' / f"{a.arch}-{mode}"
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
            rc = subprocess.run([make, "-C", str(tree), f"ARCH={a.arch}", f"BOOT_LOG={serial}", "test"], env={**os.environ, "QEMU_SMP": "4"},
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
