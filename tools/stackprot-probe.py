#!/usr/bin/env python3
"""Prove the stack protector (BUILD-1; docs/kernel/security/design.md,
"The stack protector"): a stack buffer overrun panics as one.

    stackprot-probe.py [--old] [--arch x86_64|aarch64] [--tree REV]

The probe runs `make test-stackprot` (a CRASH_TEST=4 kernel that overruns a
16-byte stack buffer by 48 bytes on purpose; the harness requires the stack
protector's panic) in a throwaway worktree.

  fixed  boot-test PASS: "KERNEL PANIC: stack-protector: a stack canary was
         overwritten".
  --old  the fix commit reverse-applied, the test kept: no such panic -- the
         overrun goes unnoticed (x86-64) or ends in an unrelated fault
         (AArch64) -- and boot-test FAILs.

Logs stay under out/stackprot-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXES = [
    "build, kernel: the stack protector, with per-thread canaries (BUILD-1)",
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "stackprot-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"spp-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            subprocess.run([make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=debug", f"OUT={dbg}", "test-stackprot"],
                           stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=1800,
                           start_new_session=True)
        boot = Path(str(dbg) + "-stackprot") / "boot-test-stackprot.log"
        serial = boot.read_text(errors="replace") if boot.exists() else ""
        if boot.exists():
            subprocess.run(["cp", str(boot), str(work)], check=False)
        verdict = (work / "make-debug.log").read_text(errors="replace")
        passed = re.search(r"^boot-test: PASS", verdict, re.M) is not None
        caught = re.search(r"^KERNEL PANIC: stack-protector: a stack canary was overwritten", serial, re.M) is not None
        ok = (not passed and not caught) if a.old else (passed and caught)
        seen = [f"boot-test {'PASS' if passed else 'FAIL'}", "stack-protector panic" if caught else "no stack-protector panic"]
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: " + "; ".join(seen) + f" ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
