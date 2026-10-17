#!/usr/bin/env python3
"""Prove a block device is one mount's at a time (vfs invariant V38).

    mount-claim-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

make test-busybox's check mount-twice mounts the NVMe scratch disk's
cosmofs, mounts it again at a second directory, and requires the second
mount refused ("Resource busy") with the first still there. Before the fix
nothing claimed a device: both mounts succeeded, two cosmofs instances
writing one disk (found in shell completeness S6's review).

--old reverse-applies the fix commit's kernel-services/vfs/vfs.c and
kernel/include/kernel/vfs.h (the claim, and nothing else) in a throwaway
worktree, so the second mount succeeds and mount-twice fails. The fix
commit is found by its subject, or named with --fix. The check runs in a
release test-busybox on either architecture.

Logs stay under out/mount-claim-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should (fixed: mount-twice ok; old: mount-twice failed with
the second mount made), else 1.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "vfs: a block device is one mount's at a time"
SOURCES = ["kernel-services/vfs/vfs.c", "kernel/include/kernel/vfs.h"]
OK = "BBTEST: ok mount-twice"
FAILED = "BBTEST: FAIL mount-twice: want [1 0 1] got [0 1 1]"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the commit that added the claim (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "mount-claim-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"mcp-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
        out = tree / "out" / "rel"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            subprocess.run(args + ["test-busybox"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                           timeout=1800)
        logs = list((out / "test-busybox").glob("boot-busybox.log"))
        text = logs[0].read_text(errors="replace") if logs else ""
        (work / "boot-busybox.log").write_text(text)
        if a.old:
            ok = FAILED in text
            seen = "the second mount was made (mount-twice failed)" if ok else "no such failure"
        else:
            ok = OK in text
            seen = "the second mount was refused (mount-twice ok)" if ok else "mount-twice did not pass"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
