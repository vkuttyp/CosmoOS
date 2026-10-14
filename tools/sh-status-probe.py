#!/usr/bin/env python3
"""Prove cosmo-sh expands $? and $X after the commands before them on a line.

    sh-status-probe.py --arch x86_64|aarch64 [--old] [--tree REV]

`false; echo $?` printed 0 in the console shell: run_line lexed, and so
expanded, the whole line before running any of it. The fix commit lexes
each pipeline just before it runs, and /etc/rc.test (every debug boot)
checks `false; cosmo-sh -c "exit $?"` and `Z=7; cosmo-sh -c "exit $Z"`.
--old reverse-applies the fix commit's userland/shell/sh.c in a throwaway
worktree, keeping the new checks: the boot then prints `status 0` and
SHTEST: FAIL. Logs stay under out/sh-status-probe/<arch>-<mode>. Exit 0
when the mode shows what it should (fixed: status 1 and SHTEST: PASS;
old: status 0 and SHTEST: FAIL), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "cosmo-sh: lex each pipeline just before it runs"
SOURCE = "userland/shell/sh.c"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "sh-status-probe" / f"{a.arch}-{mode}"
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
        status = re.search(r"^status (\d+)\r?$", log, re.M)
        status = status.group(1) if status else "none"
        if a.old:
            ok = rc != 0 and status == "0" and re.search(r"^SHTEST: FAIL", log, re.M) is not None
        else:
            ok = rc == 0 and status == "1" and re.search(r"^SHTEST: PASS", log, re.M) is not None
        seen = f"status {status}, " + ("SHTEST: PASS" if "SHTEST: PASS" in log else "no SHTEST: PASS")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
