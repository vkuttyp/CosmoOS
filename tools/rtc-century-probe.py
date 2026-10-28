#!/usr/bin/env python3
"""Prove net-login's RTC bound admits the firmware's century write.

    rtc-century-probe.py [--old] [--tree REV] [--fix REV]

test-net-login's stage j sets the clock two days ahead, writes it to the
RTC with hwclock -w, reboots and requires the guest to read the written
time plus the time since. CI run 40739284519 (main's merge of #480,
net-login test x86_64) read a second short: "after reboot the guest reads
1793382913, the RTC should read 1793382913..1793382917" (the bounds are
rounded; the lower one was above 1793382913).

The mechanism: at every boot edk2's PcRtcAcpiTableChangeCallback writes the
century register (CMOS 0x32) with the clock running. QEMU's MC146818 takes a
time register written while running as a new time (rtc_set_time) but keeps
the sub-second phase of the last set, so the RTC drops back by the time
since that set modulo a second. The probe drives QEMU's RTC through qtest,
no guest: it sets the time under SET as the kernel's hwclock -w does, waits
D, writes the century register, and times the next rollover against the
host, for D = 1.3, 1.6, 1.9 and 2.4 s. Each lag must be D mod 1 within
50 ms.

Then the bound: the reading the mechanism allows at its worst is the
largest measured rewind plus just under a second each for the boot's
whole-second RTC read and `date +%s`. The probe evaluates the stage's own
bound lines from the tree (x86-64) on that reading. --old reverse-applies
the fix commit's change to tests/boot/net_login_test.py in a throwaway
worktree: its 2 s bound rejects the reading. A boot cannot make this
deterministic (the two truncations are phases nothing controls), so the
probe proves the mechanism and the arithmetic, not a failing boot. x86-64
only: AArch64's PL031 has no century register and keeps its 2 s bound.

Exit 0 when the mode shows what it should (fixed: the rewinds match and the
bound admits the reading; old: they match and the bound rejects it).
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "test-net-login: the RTC bound counts the firmware's century write"
HARNESS = "tests/boot/net_login_test.py"
# Seconds from the set to the century write: after the first rollover (at
# most a second), rewinds of 0.3, 0.6, 0.9 and 0.4 s.
DELAYS = (1.3, 1.6, 1.9, 2.4)
TOLERANCE = 0.05
TRUNCATION = 0.999   # the most a whole-second reading loses


class Rtc:
    """QEMU's q35 RTC over qtest: no firmware, no guest."""

    def __init__(self):
        self.q = subprocess.Popen(["qemu-system-x86_64", "-M", "q35", "-accel", "qtest", "-qtest", "stdio",
                                   "-qtest-log", os.devnull, "-display", "none", "-nodefaults", "-S"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def cmd(self, c):
        self.q.stdin.write(c + "\n")
        while True:
            r = self.q.stdout.readline()
            if not r:
                sys.exit("qtest: QEMU exited")
            if r.startswith(("OK", "FAIL", "ERR")):
                return r.strip()

    def write(self, reg, v):
        self.cmd(f"outb 0x70 {reg:#x}")
        self.cmd(f"outb 0x71 {v:#x}")

    def read(self, reg):
        self.cmd(f"outb 0x70 {reg:#x}")
        return int(self.cmd("inb 0x71").split()[1], 16)

    def rollover(self):
        """The host time at which the seconds register next changes."""
        s0 = self.read(0x00)
        while self.read(0x00) == s0:
            pass
        return time.time()

    def close(self):
        self.q.kill()
        self.q.wait()


def rewind(delay):
    """Set the time, wait delay, write the century register; the lag."""
    rtc = Rtc()
    try:
        rtc.rollover()
        rtc.write(0x0b, 0x82)            # SET, 24-hour, BCD: as arch_rtc_write_epoch
        rtc.write(0x00, 0x00)
        rtc.write(0x0b, 0x02)            # SET off at t_set
        t_set = time.time()
        t1 = rtc.rollover()              # the clock's phase after the set
        time.sleep(max(0.0, delay - (time.time() - t_set)))
        t_c = time.time()
        rtc.write(0x32, 0x20)            # the century, the clock running: as edk2
        t2 = rtc.rollover()
        return (t_c - t_set) % 1.0, (t2 - t1) % 1.0
    finally:
        rtc.close()


def bound(tree):
    """The stage's lower bound on x86-64, from its own source, as an offset
    from the written time plus the elapsed time."""
    src = (tree / HARNESS).read_text()
    lines = [l.strip() for l in src.splitlines()
             if re.match(r"\s*(below = |lo, hi = target \+ \(h0 - t_set\))", l)]
    if not lines or not lines[-1].startswith("lo, hi = "):
        sys.exit(f"{HARNESS}: no 'lo, hi = target + (h0 - t_set) ...' line")
    env = {"os": os, "target": 0, "h0": 0.0, "h1": 0.0, "t_set": 0.0}
    saved = os.environ.get("QEMU_ARCH")
    os.environ["QEMU_ARCH"] = "x86_64"
    try:
        for l in lines:
            exec(l, env)
    finally:
        if saved is None:
            del os.environ["QEMU_ARCH"]
        else:
            os.environ["QEMU_ARCH"] = saved
    return env["lo"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"

    measured = []
    for d in DELAYS:
        want, lag = rewind(d)
        # 0.999 late is 0.001 early: compare on the circle.
        off = min(abs(lag - want), 1.0 - abs(lag - want))
        measured.append((d, want, want if off <= TOLERANCE else lag, off <= TOLERANCE))
        print(f"rtc-century: set, {d:.1f}s, century write: the RTC rolls over {lag:.3f}s late "
              f"(predicted {want:.3f}s)")
    mech = all(ok for *_, ok in measured)
    worst = max(lag for _, _, lag, _ in measured)
    reading = -(worst + 2 * TRUNCATION)

    tree = Path(tempfile.mkdtemp(prefix=f"rc-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            sha = a.fix or subprocess.check_output(
                ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={SUBJECT}", a.tree],
                text=True).strip()
            if not sha:
                sys.exit(f"no commit '{SUBJECT}' reachable from {a.tree}; name it with --fix")
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--", HARNESS])
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {HARNESS}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        lo = bound(tree)
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])

    admits = reading >= lo
    print(f"rtc-century: worst reading {reading:+.3f}s (rewind {worst:.3f}s + two truncations); "
          f"the {mode} bound is {lo:+.0f}s: {'admits' if admits else 'rejects'} it")
    ok = mech and (admits != a.old)
    seen = "rewinds match" if mech else "a rewind did not match its prediction"
    print(f"PROBE: {'PASS' if ok else 'FAIL'} mode={mode}: {seen}; the bound {'admits' if admits else 'rejects'} "
          f"the worst reading")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
