#!/usr/bin/env python3
"""Prove new files are stamped with the wall clock, not the time since boot.

    file-times-probe.py --arch x86_64|aarch64 [--old] [--tree REV]

Over ssh on Parallels Desktop, `touch now.txt; ls -l now.txt` showed Jan 1
1970 while `date` showed 2026: vfs_now_ns returned the monotonic clock. The
fix commit makes it the wall clock, and lxtest (the Linux ABI user test,
every debug boot) checks a new file's mtime against CLOCK_REALTIME. --old
reverse-applies the fix commit's kernel-services/vfs/vfs.c in a throwaway
worktree: the boot's Linux ABI test then fails on that check, the mtime
being the uptime in seconds. Logs stay under out/file-times-probe/<arch>-<mode>.
Exit 0 when the mode shows what it should (fixed: LINUXTEST: PASS; old: the
check's FAIL line), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "vfs: stamp files with the wall clock, not the time since boot"
SOURCE = "kernel-services/vfs/vfs.c"
OLD_LINE = "LINUXTEST: FAIL later.st_mtime >= rt0.tv_sec - 1"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "file-times-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    tree = Path(tempfile.mkdtemp(prefix="worktree-", dir=work))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            sha = subprocess.check_output(["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings",
                                           f"--grep={SUBJECT}", a.tree], text=True).strip()
            if not sha:
                sys.exit(f"no commit '{SUBJECT}' reachable from {a.tree}")
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--", SOURCE])
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        serial = work / "boot.serial"
        with (work / "boot.result").open("w") as out:
            rc = subprocess.run([make, "-C", str(tree), f"ARCH={a.arch}", f"BOOT_LOG={serial}", "test"],
                                stdout=out, stderr=subprocess.STDOUT).returncode
        log = serial.read_text(errors="replace") if serial.exists() else ""
        if a.old:
            ok = rc != 0 and OLD_LINE in log
            m = re.search(re.escape(OLD_LINE) + r".*\((\d+)\)", log)
            seen = f"the check fails, mtime {m.group(1)} s" if m else "the check did not fail"
        else:
            ok = rc == 0 and "LINUXTEST: PASS" in log
            seen = "LINUXTEST: PASS" if ok else "no LINUXTEST: PASS"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
