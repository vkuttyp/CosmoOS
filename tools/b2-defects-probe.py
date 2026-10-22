#!/usr/bin/env python3
"""Prove roadmap 1.2 B2's cosmofs defects, one fix at a time.

    b2-defects-probe.py --case NAME [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

Each case is one fix commit and the self-test that proves it
(docs/audit/2026-10-22-b2-fsck-report.md):

  ro           CFS-1   a read-only mount writes nothing          cosmofs-ro-replay
  compress     CFS-2   no records below format 6                 cosmofs-version-compress
  nomem        CFS-6   -ENOMEM returned, the transaction kept    cosmofs-nomem
  scrub-counts CFS-11  scrub's counts survive -EIO                cosmofs-scrub-counts
  root-type    a root that is not a directory is refused         cosmofs-root-type
  scrub-open   the scrub leaves the open transaction alone       cosmofs-scrub-open

--old reverts the case's fix commit -- the whole commit, every file it
changed (tests and seams live in their own commits), three-way -- in a
throwaway worktree and runs a debug `make test`. Old mode passes when the
case's self-test fails at its stated check (root-type: the kernel panics
on the VFS assertion instead, the self-test never reports), and nothing
fails but that test and the ones named as its consequences. Fixed mode
passes when `make test` passes.

Logs stay under out/b2-defects-probe/<case>-<arch>-<mode>. Exit 0 when
the mode shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# subject of the fix, self-test, the check its old run fails, other
# self-tests the old code fails as a consequence (allowed, not required).
CASES = {
    "ro": ("cosmofs: a read-only mount writes nothing (CFS-1)", "cosmofs-ro-replay",
           r"changed == 0", ()),
    "compress": ("cosmofs: no compressed records below format version 6 (CFS-2)", "cosmofs-version-compress",
                 r"v5 == 0", ()),
    "nomem": ("cosmofs: a failure a retry could survive no longer abandons the transaction (CFS-6)",
              "cosmofs-nomem", r"retry == 0", ()),
    "scrub-counts": ("cosmofs: a scrub that met an unrecoverable block still reports (CFS-11)",
                     "cosmofs-scrub-counts", r"direct == 0 && sc\.unrecoverable == 1", ()),
    "root-type": ("cosmofs: a root that is not a directory is refused at the mount", "cosmofs-root-type",
                  None, ()),
    # cosmofs-compress scrubs with a transaction open too, and with CFS-6's
    # allocations its luck runs out: it fails the same way.
    "scrub-open": ("cosmofs: the scrub leaves the open transaction's blocks to the commit", "cosmofs-scrub-open",
                   r"sc\.unrecoverable == 0", ("cosmofs-compress",)),
}
PANIC = re.compile(r"KASSERT|PANIC|panic", re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", choices=sorted(CASES), required=True)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    subject, test, check, also = CASES[a.case]
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "b2-defects-probe" / f"{a.case}-{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"b2p-{a.arch[:3]}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            sha = a.fix or subprocess.check_output(
                ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={subject}", a.tree],
                text=True).strip()
            if not sha:
                sys.exit(f"no commit '{subject}' reachable from {a.tree}; name it with --fix")
            # A three-way revert of the whole commit: a later fix may have
            # touched the lines around it (root-type's check sits beside
            # CFS-1's), which a plain reverse patch refuses.
            subprocess.run(["git", "-C", str(tree), "revert", "--no-commit", sha], check=True,
                           stdout=subprocess.DEVNULL)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "dbg"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={out}",
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args + ["test"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=2400).returncode
        logs = list(out.glob("boot-test.log"))
        text = logs[0].read_text(errors="replace") if logs else ""
        (work / "boot-test.log").write_text(text)
        failed = re.findall(r"^SELFTEST: (\S+)\s+\.\.\. FAIL", text, re.M)
        mine = re.search(rf"^SELFTEST: {re.escape(test)}\s+\.\.\. (ok|FAIL: (.*))$", text, re.M)
        if a.old:
            if check is None:
                ok = mine is None and PANIC.search(text) is not None and not failed
                seen = ("the kernel stopped on the assertion before the self-test reported" if ok else
                        f"no panic alone ({test}: {mine.group(0) if mine else 'no line'}; failed: {failed})")
            else:
                at = mine is not None and mine.group(2) is not None and \
                    re.match(rf"check failed: {check} at line \d+", mine.group(2)) is not None
                ok = at and test in failed and set(failed) <= {test, *also}
                seen = (f"{test} FAIL at `{check}`" + (f" (also {sorted(set(failed) - {test})})"
                                                         if set(failed) - {test} else "")
                        if ok else f"not that failure ({mine.group(0) if mine else 'no line'}; failed: {failed})")
        else:
            ok = rc == 0 and mine is not None and mine.group(1) == "ok" and not failed
            seen = (f"{test} ok, make test passed" if ok else
                    f"make test exited {rc}; {mine.group(0) if mine else 'no line'}; failed: {failed}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} case={a.case} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
