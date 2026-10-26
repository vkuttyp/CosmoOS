#!/usr/bin/env python3
"""Prove CPU time is measured and split (LX-14; process design.md, "CPU
time"; roadmap 1.2 A5).

    cputime-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

The probe boots a debug build and judges lxtest's CPU-time block
(tests/linux/lxtest.c, test_cputime) by its own lines.

  fixed  `lxtest: cputime ok` and `LINUXTEST: PASS`: the CPU-time clocks
         stand still through a sleep, an exited thread's time stays its
         process's in /proc/self/stat, getrusage and times split user from
         system time, and ITIMER_VIRTUAL and ITIMER_PROF fire.
  --old  the fix commit reverse-applied whole in a throwaway worktree:
         the two defects fail their checks -- CLOCK_PROCESS_CPUTIME_ID
         advances through a 200 ms sleep (it read the monotonic clock,
         `p1 - p0 < 50000000ull`), and a thread's 300 ms is gone from
         /proc/self/stat once it exits (`s1 - s0 >= 15`) -- and the
         missing features fail theirs: getrusage, times and both timers.

Logs stay under out/cputime-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = ("kernel/process: CPU time split into user and system time, and ITIMER_VIRTUAL and ITIMER_PROF, "
           "in Nova (roadmap 1.2 A5, LX-14)")
# The checks --old must see fail: the two defects, then one per missing feature.
OLD_FAILS = [
    r"p1 > 0 && p1 - p0 < 50000000ull",
    r"s0 >= 0 && s1 - s0 >= 15",
    r"sc2\(LX_getrusage, LX_RUSAGE_SELF, &r0\) == 0",
    r"k0 > 0",
    r"sc3\(LX_setitimer, which, &nv, 0\) == 0",
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "cputime-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"cpt-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            # The whole commit, not chosen hunks, three-way; a conflict only
            # in docs/ keeps the tree's text, one in code stops the probe.
            if subprocess.run(["git", "-C", str(tree), "revert", "--no-commit", sha],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                clash = subprocess.check_output(["git", "-C", str(tree), "diff", "--name-only", "--diff-filter=U"],
                                                text=True).split()
                code = [p for p in clash if not p.startswith("docs/")]
                if code:
                    sys.exit(f"the revert of {sha} conflicts in code: {' '.join(code)}")
                subprocess.run(["git", "-C", str(tree), "checkout", "HEAD", "--", *clash], check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        dbg = tree / "out" / "d"
        with (work / "make-debug.log").open("w") as log:
            subprocess.run([make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={dbg}", "test"],
                           stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=1800,
                           start_new_session=True)
        boot = dbg / "boot-test.log"
        serial = boot.read_text(errors="replace") if boot.exists() else ""
        if boot.exists():
            subprocess.run(["cp", str(boot), str(work)], check=False)
        fails = [ln for ln in serial.splitlines() if ln.startswith("LINUXTEST: FAIL ")]
        done = re.search(r"^lxtest: cputime ok$", serial, re.M) is not None
        passed = re.search(r"^LINUXTEST: PASS$", serial, re.M) is not None
        if a.old:
            missing = [p for p in OLD_FAILS if not any(re.search(p, ln) for ln in fails)]
            ok = done and not missing
            seen = f"{len(fails)} failed checks" + (f"; did not fail: {missing}" if missing else "; every expected one")
        else:
            ok = done and passed and not fails
            seen = "lxtest: cputime ok, LINUXTEST: PASS" if ok else f"block ran: {done}; {len(fails)} failed checks"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
