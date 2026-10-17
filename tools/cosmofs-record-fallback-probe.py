#!/usr/bin/env python3
"""Prove a compressed record the plain fallback replaces goes first, whole.

    cosmofs-record-fallback-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

When a page written inside a compressed record leaves the record unable to
compress (cosmofs.c, record_write), the record is written again as plain
blocks. Before the fix the first of them was meant to displace the record,
but set_extent refuses a one-block range that would cut a record (-EIO),
and data_write_block had stored that block's checksum over the record's
already: the write-back failed and the record stayed mapped with a
checksum that no longer matched it, unreadable ("no copy matches its
checksum"). wget hit it on an installed system about once in twenty 12 MB
downloads (found in shell completeness S7, whose test fetches release sets).

The self-test cosmofs-compress writes eight incompressible pages over a
compressed record and syncs: with the fix the sync succeeds and the file
reads back; without it the sync fails. --old reverse-applies the fix
commit's kernel-services/filesystem/cosmofs/cosmofs.c (the fix, and
nothing else) in a throwaway worktree, keeping the self-test. The check
runs in a debug `make test` on either architecture.

Logs stay under out/cosmofs-record-fallback-probe/<arch>-<mode>. Exit 0
when the mode shows what it should (fixed: cosmofs-compress ok; old: it
fails at the sync), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "cosmofs: a record the plain fallback replaces goes first, whole"
SOURCES = ["kernel-services/filesystem/cosmofs/cosmofs.c"]
OK = re.compile(r"^SELFTEST: cosmofs-compress\s+\.\.\. ok ", re.M)
FAILED = re.compile(r"^SELFTEST: cosmofs-compress\s+\.\.\. FAIL: check failed: file_sync\(f\) == 0 at line \d+", re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the commit that made the fallback drop the record (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "cosmofs-record-fallback-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"crf-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--"] + SOURCES)
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {SOURCES}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "dbg"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={out}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            subprocess.run(args + ["test"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                           timeout=2400)
        logs = list(out.glob("boot-test.log"))
        text = logs[0].read_text(errors="replace") if logs else ""
        (work / "boot-test.log").write_text(text)
        if a.old:
            ok = FAILED.search(text) is not None
            seen = "the sync over the record failed (cosmofs-compress FAIL)" if ok else "no such failure"
        else:
            ok = OK.search(text) is not None
            seen = "the record was replaced and reads back (cosmofs-compress ok)" if ok else "cosmofs-compress did not pass"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
