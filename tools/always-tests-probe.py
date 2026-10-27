#!/usr/bin/env python3
"""Prove the three self-test defects of roadmap 1.2's first unit
("Always, whichever direction"): TEST-1, TEST-2 and TEST-3, each fixed in
its test and each with a control that makes its failure certain.

    always-tests-probe.py [--old] [--arch x86_64|aarch64] [--tree REV]

The probe boots a debug build and judges three self-tests by their own
lines:

  wait-multi-prepare (TEST-1)   a schedule() right after the wake picks the
                                thread again with no switch; the run must
                                not be judged.
  module-unload-busy (TEST-2)   module_unload's 48 ms wait replayed with
                                this CPU's per-CPU clock injected 40 ms back
                                (the deadline clock never reads it); the
                                wait is judged on the deadline's own clock.
  net-neigh-down-race (TEST-3)  armed for ndown0, a decoy interface's
                                resolve must pass the park hook.

  fixed  all three `ok`.
  --old  the three fix commits reverse-applied in a throwaway worktree,
         the controls kept: each test FAILs, on its control's own check.

Logs stay under out/always-tests-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# Each defect: the fix commit's subject, the self-test, and the text of its FAIL line under --old.
DEFECTS = [
    ("kernel/scheduler: wait-multi-prepare decides on picks, not switches (TEST-1)",
     "wait-multi-prepare", "the control's re-pick was not seen"),
    ("kernel/module: module-unload-busy judges the wait on the deadline clock (TEST-2)",
     "module-unload-busy", "control_passed"),
    ("kernel-services/network: the neighbour park hooks are armed for one interface (TEST-3)",
     "net-neigh-down-race", "!arp_decoy"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "always-tests-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"atp-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            for subject, _, _ in DEFECTS:
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
        ok, seen = True, []
        for _, test, old_text in DEFECTS:
            line = re.search(rf"^SELFTEST: {re.escape(test)}\s+\.\.\. (ok|FAIL)\b(.*)$", serial, re.M)
            if line is None:
                ok = False
                seen.append(f"{test}: no line")
                continue
            verdict, rest = line.group(1), line.group(2)
            if a.old:
                good = verdict == "FAIL" and old_text in rest
            else:
                good = verdict == "ok"
            ok = ok and good
            seen.append(f"{test}: {verdict}{rest.strip()[:90] and ' ' + rest.strip()[:90]}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: " + "; ".join(seen) + f" ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
