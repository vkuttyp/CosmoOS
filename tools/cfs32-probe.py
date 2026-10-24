#!/usr/bin/env python3
"""Prove a write past cosmofs's run bound is refused at the write, with
nothing lost (CFS-32, invariants CF33 and V39).

    cfs32-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

A compressed record is one run, and an inode may have CFS_MAX_EXTENTS
runs; a compressible file reached that at about 128 MiB, and the bound
was found at write-back, not at the write: every write-back failed
-EFBIG, and the pages were lost with the vnode (`vfs: N dirty page(s)
... lost`) while the file's size still counted them.

The probe runs the debug boot's self-test `cosmofs-extent-bound` (a
sparse compressible file, one record at every other record's place, on a
mount whose bound the test seam lowers to 64 runs):

  fixed  the self-test passes: the write is refused -EFBIG near the
         bound, and no page of that file is lost.
  --old  the fix commit reverse-applied whole in a throwaway worktree:
         the self-test fails with every record accepted, and the boot
         says its pages were lost to write-back failing -EFBIG (-27).

Logs stay under out/cfs32-probe/<arch>-<mode>. Exit 0 when the mode shows
what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "cosmofs, vfs: a write past the run bound refused at the write, nothing dirtied that cannot be written (CFS-32)"
TEST = "cosmofs-extent-bound"
LOST = re.compile(r"vfs: (\d+) dirty page\(s\) of inode \d+ on cosmofs lost: write-back failed \(-27\)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "cfs32-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"c32-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha])
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "o"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={out}",
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}", "test"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=3600, start_new_session=True).returncode
        serial = out / "boot-test.log"
        if serial.exists():
            shutil.copy(serial, work / "boot-test.log")
        text = serial.read_text(errors="replace") if serial.exists() else ""
        line = re.search(rf"^SELFTEST: {re.escape(TEST)} +\.\.\. (ok|FAIL).*$", text, re.M)
        info = re.search(rf"^\[ INFO\] {re.escape(TEST)}: .*$", text, re.M)
        # The loss attributed to this test: between its own line and its verdict.
        start = info.start() if info else 0
        end = line.end() if line else len(text)
        lost = LOST.search(text, start, end)
        if a.old:
            ok = line is not None and line.group(1) == "FAIL" and lost is not None
            seen = (f"{line.group(0)}; {lost.group(0)}" if ok else
                    f"no such failure (make exit {rc}; {line.group(0) if line else 'the test did not run'}; "
                    f"loss: {lost.group(0) if lost else 'none'})")
        else:
            ok = line is not None and line.group(1) == "ok" and lost is None
            seen = (f"{info.group(0) if info else ''}; {line.group(0)}" if ok else
                    f"make exit {rc}; {line.group(0) if line else 'the test did not run'}; "
                    f"loss: {lost.group(0) if lost else 'none'}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
