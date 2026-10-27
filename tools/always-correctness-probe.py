#!/usr/bin/env python3
"""Prove the two correctness defects of roadmap 1.2's first unit's second
PR: LX-1 (signal-stop's check 9) and PKG-1 (pkg's walks over 128 installed
records), each with a test that makes its failure certain.

    always-correctness-probe.py [--old] [--arch x86_64|aarch64] [--tree REV]

The probe boots a debug build and judges:

  LX-1   the self-test signal-stop, whose parent waits 100 ms after SIGCONT
         on purpose, and signal-stop-exit (an exited child reports its
         exit, the kernel's order, unchanged by the fix).
  PKG-1  rc.test's 130 hand-made installed records: its line
         "pkg-many: N of 130 listed".

  fixed  signal-stop and signal-stop-exit ok, "pkg-many: 130 of 130".
  --old  the two fix commits reverse-applied in a throwaway worktree, the
         tests kept: signal-stop FAILs with check 9, and pkg list shows
         128 of the 130.

Logs stay under out/always-correctness-probe/<arch>-<mode>. Exit 0 when the
mode shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXES = [
    "userland/init: signal-stop's child exits only after its continue is seen (LX-1)",
    "pkg: every whole-database walk reads every installed record (PKG-1)",
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "always-correctness-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"acp-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            for subject in FIXES:
                sha = subprocess.check_output(
                    ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={subject}",
                     a.tree], text=True).strip()
                if not sha:
                    sys.exit(f"no commit '{subject}' reachable from {a.tree}")
                # The whole commit, three-way; a conflict only in docs/ keeps
                # the tree's text, one in code stops the probe.
                if subprocess.run(["git", "-C", str(tree), "revert", "--no-commit", sha],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                    clash = subprocess.check_output(["git", "-C", str(tree), "diff", "--name-only",
                                                     "--diff-filter=U"], text=True).split()
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
        def selftest(name):
            m = re.search(rf"^SELFTEST: {re.escape(name)}\s+\.\.\. (ok|FAIL)\b", serial, re.M)
            return m.group(1) if m else "no line"
        stop, stop_exit = selftest("signal-stop"), selftest("signal-stop-exit")
        check9 = re.search(r"signal probe 'signal-stop' failed check 9", serial) is not None
        many = re.search(r"^pkg-many: (\d+) of 130 listed$", serial, re.M)
        listed = int(many.group(1)) if many else -1
        if a.old:
            ok = stop == "FAIL" and check9 and stop_exit == "ok" and 0 <= listed < 130
        else:
            ok = stop == "ok" and stop_exit == "ok" and listed == 130
        seen = [f"signal-stop: {stop}{' (check 9)' if check9 else ''}", f"signal-stop-exit: {stop_exit}",
                f"pkg-many: {listed if listed >= 0 else 'no line'} of 130"]
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: " + "; ".join(seen) + f" ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
