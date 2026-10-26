#!/usr/bin/env python3
"""Prove one AIO PWRITE stays under the dirty bound (MEM-9; vfs
design.md, "Dirty pages, bounded"; invariant V40).

    mem9-aio-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

The probe boots a debug build for the self-test aio-pwrite-dirty: the
cache limit lowered to 256 pages (the bound is 128), write-back off, one
COSMO_AIO_PWRITE of 256 pages through the ring's own body
(aio_test_pwrite).

  fixed  aio-pwrite-dirty ok: the request asks about the bound every
         64 KiB and leaves at most the bound and 16 pages dirty.
  --old  the fix commit reverse-applied whole in a throwaway worktree:
         the request is throttled only before its loop, every page it
         wrote is left dirty, and the test fails `left <= bound + 16`.

The self-test is judged by its own line, not the boot's verdict. Logs
stay under out/mem9-aio-probe/<arch>-<mode>. Exit 0 when the mode shows
what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "aio: an AIO PWRITE asks about the dirty bound every 64 KiB and lets f->lock go to be throttled (MEM-9)"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "mem9-aio-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"m9a-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
        aw = re.search(r"^SELFTEST: aio-pwrite-dirty\s+\.\.\. (ok|FAIL)\b.*$", serial, re.M)
        info = re.search(r"^.*selftest: aio-pwrite-dirty: (.*)$", serial, re.M)
        seen = f"aio-pwrite-dirty: {aw.group(0).split('... ', 1)[1].strip() if aw else 'no line'}" + (
            f" ({info.group(1).strip()})" if info else "")
        if a.old:
            ok = aw is not None and aw.group(1) == "FAIL" and "left <= bound + 16" in aw.group(0)
        else:
            ok = aw is not None and aw.group(1) == "ok"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
