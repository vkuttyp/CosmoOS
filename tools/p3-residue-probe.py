#!/usr/bin/env python3
"""Prove roadmap 1.1 P3's three defect fixes, one at a time.

    p3-residue-probe.py --defect zero|wget|freecount [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

  zero       A read of a regular file or /dev/zero larger than the 64 KiB
             bounce returned 64 KiB: `dd if=/dev/zero bs=1M count=4` copied
             four partial records. make test-busybox's dd-zero-1m requires
             "4+0 4+0 4194304".
  wget       wget refusing a server's certificate printed "wget: error
             getting response" after ssl_client's reason (with a stale
             errno after a name lookup, "Resource temporarily
             unavailable"). make test-net-login requires no wget line.
  freecount  Every mount of a cleanly unmounted cosmofs warned that the
             superblock's free count differed from the bitmap's by the
             last commit's frees. make test-install requires no warning at
             the installed root's first mount.

--old reverse-applies the fix commit's sources (and nothing else) in a
throwaway worktree, so the check fails; the commit is found by its subject,
or named with --fix. Each check runs in a release build on either
architecture. (sftp's "?" link count, P3's fourth item, is OpenSSH's own
rendering, not a defect: docs/audit/2026-10-19-p3-residue-report.md.)

Logs stay under out/p3-residue-probe/<defect>-<arch>-<mode>. Exit 0 when the
mode shows what it should (fixed: the target PASSes; old: the named check
fails), else 1.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFECTS = {
    "zero": dict(
        subject="io: a large read of a file or /dev/zero is filled, not cut at the bounce",
        sources=["kernel/syscall/native.c", "kernel/include/kernel/object.h", "kernel/include/kernel/vfs.h",
                 "kernel-services/vfs/vfs.c", "kernel-services/vfs/memdev.c"],
        target="test-busybox", ok="BBTEST: ok dd-zero-1m", failed="BBTEST: FAIL dd-zero-1m: want [4+0 4+0 4194304]"),
    "wget": dict(
        subject="wget: a refused certificate ends on ssl_client's reason",
        sources=["ports/busybox/wget-ssl-client.patch"],
        target="test-net-login", ok="test-net-login: PASS",
        failed="added 'wget: error getting response' after ssl_client's reason"),
    "freecount": dict(
        subject="cosmofs: compare the free count with the bitmap before the free-log replay",
        sources=["kernel-services/filesystem/cosmofs/cosmofs_core.c"],
        target="test-install", ok="test-install: PASS",
        failed="the installed root's first mount: the mount warned 'cosmofs: free block count"),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--defect", choices=sorted(DEFECTS), required=True)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    d = DEFECTS[a.defect]
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "p3-residue-probe" / f"{a.defect}-{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"p3{a.defect[0]}-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            sha = a.fix or subprocess.check_output(
                ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={d['subject']}",
                 a.tree], text=True).strip()
            if not sha:
                sys.exit(f"no commit '{d['subject']}' reachable from {a.tree}; name it with --fix")
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--"] + d["sources"])
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {d['sources']}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "rel"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}",
                f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        if a.defect != "wget":
            # BusyBox and the pinned sources are the same in either mode;
            # the wget defect is in BusyBox, which must be built from the tree.
            args.append(f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}")
        log_path = work / "make.log"
        with log_path.open("w") as log:
            subprocess.run(args + [d["target"]], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                           timeout=3600)
        text = log_path.read_text(errors="replace")
        for f in (out / d["target"]).glob("boot-*.log"):
            (work / f.name).write_text(f.read_text(errors="replace"))
            text += f.read_text(errors="replace")
        ok = (d["failed"] if a.old else d["ok"]) in text
        seen = (f"the check failed as the defect predicts" if a.old else f"{d['target']} passed") if ok else \
            "not what the mode predicts"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} defect={a.defect} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
