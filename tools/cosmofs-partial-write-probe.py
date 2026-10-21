#!/usr/bin/env python3
"""Prove a write-back that fails partway keeps the blocks it mapped.

    cosmofs-partial-write-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

cfs_writepage and cfs_writepages stored the inode only after a write
that succeeded. A record whose fifth block found no room had mapped the
four before it in the open inode -- and queued the blocks they replaced
free -- so a vnode released before the retry (its last write-back
failing too, the pages dropped) took that map with it: the new blocks
leaked and the old ones were freed under the committed inode that still
named them. test-pkgtx's full root found it, 3 to 7 leaked blocks.

The self-test cosmofs-extent-fail's `evict` case fails the fifth
set_extent of a rewrite through FI_COSMOFS_EXTENT_NOMEM/FBIG, releases
the file with every set_extent failing, commits, and runs the structural
check. With the fix it is clean and the first four pages read back new;
without it the check fails `rep.alloc_not_seen.count == 0 &&
rep.seen_not_alloc.count == 0`. --old reverse-applies the fix commit
(its cosmofs.c, the whole commit's change to it and nothing else) in a
throwaway worktree, keeping the self-test. The check runs in a debug
`make test` on either architecture.

Logs stay under out/cosmofs-partial-write-probe/<arch>-<mode>. Exit 0
when the mode shows what it should (fixed: cosmofs-extent-fail ok; old:
it fails at that check, with leaked and free-in-use blocks counted),
else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "cosmofs: a write-back that fails partway stores the inode it mapped"
SOURCES = ["kernel-services/filesystem/cosmofs/cosmofs.c"]
OK = re.compile(r"^SELFTEST: cosmofs-extent-fail\s+\.\.\. ok ", re.M)
FAILED = re.compile(r"^SELFTEST: cosmofs-extent-fail\s+\.\.\. FAIL: check failed: rep\.alloc_not_seen\.count == 0 "
                    r"&& rep\.seen_not_alloc\.count == 0 at line \d+", re.M)
COUNTS = re.compile(r"cosmofs-extent-fail: evict: release hits (\d+); (\d+) leaked, (\d+) free-in-use")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the commit that keeps the old checksum (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "cosmofs-partial-write-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"cpw-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            c = COUNTS.search(text)
            ok = FAILED.search(text) is not None and c is not None and int(c.group(2)) > 0 and int(c.group(3)) > 0
            seen = (f"{c.group(2)} leaked and {c.group(3)} free-in-use after the release (cosmofs-extent-fail FAIL)"
                    if ok else "no such failure")
        else:
            ok = OK.search(text) is not None
            seen = "the check clean after the release (cosmofs-extent-fail ok)" if ok else "cosmofs-extent-fail did not pass"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
