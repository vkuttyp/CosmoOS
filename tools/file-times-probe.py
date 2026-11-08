#!/usr/bin/env python3
"""Prove new files are stamped with the wall clock, not the time since boot.

    file-times-probe.py --arch x86_64|aarch64 [--old] [--tree REV]

Over ssh on Parallels Desktop, `touch now.txt; ls -l now.txt` showed Jan 1
1970 while `date` showed 2026: vfs_now_ns returned the monotonic clock. The
fix commit makes it the wall clock, and lxtest (the Linux ABI user test,
every debug boot) checks a new file's mtime against CLOCK_REALTIME. --old
makes vfs_now_ns the monotonic clock again in a throwaway worktree (it is
vfs_vnode.cee's since roadmap 2.0 SA7, where the fix's diff of vfs.c no
longer applies): the boot's Linux ABI test then fails on that check, the
mtime being the uptime in seconds. Logs stay under out/file-times-probe/<arch>-<mode>.
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
SOURCE = "kernel-services/vfs/vfs_vnode.cee"
FIXED = "fn nowNs() -> u64 => unsafe { clock_realtime_ns() };"
OLD = "fn nowNs() -> u64 => unsafe { clock_now_ns() }; // FILE TIMES PROBE: the monotonic clock (not for merge)"
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
            src = tree / SOURCE
            body = src.read_text()
            if body.count(FIXED) != 1:
                sys.exit(f"{SOURCE}: vfs_now_ns is not there exactly once; the probe is stale")
            src.write_text(body.replace(FIXED, OLD))
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
