#!/usr/bin/env python3
"""Prove a block device is one mount's at a time (vfs invariant V38).

    mount-claim-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

make test-busybox's check mount-twice mounts the NVMe scratch disk's
cosmofs, mounts it again at a second directory, and requires the second
mount refused ("Resource busy") with the first still there. Before the fix
nothing claimed a device: both mounts succeeded, two cosmofs instances
writing one disk (found in shell completeness S6's review).

--old takes the claim out of a mount in a throwaway worktree: the VFS is
CeeSafe (vfs_mount.cee, roadmap 2.0 SA7), so where the fix commit's diff
of vfs.c no longer applies, do_mount claims nothing, as before the fix.
The second mount then succeeds and mount-twice fails. The check runs in a
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
# do_mount's claim, and the mount without one.
MOUNT = "kernel-services/vfs/vfs_mount.cee"
CLAIM = "    var rc = if (bdev.isNull()) 0 else claim(mnt);\n"
NO_CLAIM = "    var rc: i32 = 0; // MOUNT CLAIM PROBE: no claim, as before the fix (not for merge)\n"
OK = "BBTEST: ok mount-twice"
FAILED = "BBTEST: FAIL mount-twice: want [1 0 1] got [0 1 1]"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
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
            src = tree / MOUNT
            body = src.read_text()
            if body.count(CLAIM) != 1:
                sys.exit(f"{MOUNT}: the claim is not there exactly once; the probe is stale")
            src.write_text(body.replace(CLAIM, NO_CLAIM))
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
