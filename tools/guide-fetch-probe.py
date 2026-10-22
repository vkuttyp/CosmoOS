#!/usr/bin/env python3
"""Prove TEST-19's guide-runner change (docs/audit/2026-10-22-test19-guide-report.md).

    guide-fetch-probe.py [--old] [--base REV]

Runs tests/host/test_run_guide.py against tests/guide/run_guide.py. With
--old, against the runner as it is at REV (default: where this branch
left origin/main, before TEST-19), in a throwaway worktree: the test must fail at its first
check, a 503 answered and not typed again. Without --old, the test must
pass on this tree. Exit 0 when the mode shows what it should.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = "tests/host/test_run_guide.py"
FIRST = "FAIL a 503 is typed again and the retry passes"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--base", help="the tree whose runner --old runs (default: the merge base with origin/main)")
    a = ap.parse_args()
    if not a.old:
        r = subprocess.run([sys.executable, str(ROOT / TEST)], capture_output=True, text=True)
        ok = r.returncode == 0 and "test_run_guide: PASS" in r.stdout
        print(f"PROBE: {'PASS' if ok else 'FAIL'} mode=fixed: test_run_guide {'passed' if ok else 'failed'}")
        return 0 if ok else 1
    base = a.base or subprocess.check_output(
        ["git", "-C", str(ROOT), "merge-base", "HEAD", "origin/main"], text=True).strip()
    tree = Path(tempfile.mkdtemp(prefix="gfp-"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), base], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        (tree / TEST).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / TEST, tree / TEST)
        r = subprocess.run([sys.executable, str(tree / TEST)], capture_output=True, text=True)
        ok = r.returncode != 0 and FIRST in r.stdout
        print(f"PROBE: {'PASS' if ok else 'FAIL'} mode=old base={base[:12]}: "
              + (f"`{FIRST}`" if ok else f"not that failure (exit {r.returncode})"))
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
