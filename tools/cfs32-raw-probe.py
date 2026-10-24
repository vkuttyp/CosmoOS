#!/usr/bin/env python3
"""Prove a compressible file written in order is not stopped by the run
bound: past three quarters of it, new records are written plain and
merge (CFS-32's write policy; cosmofs design.md, "Plain past three
quarters of the bound").

    cfs32-raw-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

The probe runs the debug boot's self-test `cosmofs-extent-raw`: 200
compressible records in order, on a mount whose bound the test seam
lowers to 64 runs.

  fixed  the self-test passes: every record taken, inside the bound.
  --old  the policy commit reverse-applied whole in a throwaway worktree:
         a compressed record is a run of its own, so the write of the
         file is refused partway (CF33), and the self-test fails at that
         write.

The 1 GiB acceptance test on a real pool is `make test-large`
(tests/boot/large_test.py); this is its quick form. Logs stay under
out/cfs32-raw-probe/<arch>-<mode>. Exit 0 when the mode shows what it
should, else 1.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "cosmofs: past three quarters of the run bound, new records written plain so they merge (CFS-32)"
TEST = "cosmofs-extent-raw"
REFUSED = "check failed: file_pwrite("
LOST = re.compile(r"vfs: (\d+) dirty page\(s\) of inode \d+ on cosmofs lost: write-back failed \(-27\)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "cfs32-raw-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"c32r-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
        loss = LOST.search(text)
        if a.old:
            ok = line is not None and line.group(1) == "FAIL" and REFUSED in line.group(0)
            seen = line.group(0) if ok else f"no such failure (make exit {rc}; {line.group(0) if line else 'the test did not run'})"
        else:
            ok = line is not None and line.group(1) == "ok" and loss is None
            seen = (f"{info.group(0) if info else ''}; {line.group(0)}" if ok else
                    f"make exit {rc}; {line.group(0) if line else 'the test did not run'}; "
                    f"loss: {loss.group(0) if loss else 'none'}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
