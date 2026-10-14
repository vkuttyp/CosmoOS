#!/usr/bin/env python3
"""Prove the Linux door reads socket()'s protocol argument.

    sock-proto-probe.py --arch x86_64|aarch64 [--old] [--tree REV]

Found while scoping ping (2026-10-14): lx_socket never read its protocol
argument, so socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP) -- a Linux "ping
socket", which CosmoOS does not build -- quietly made a UDP socket, and
so did any other protocol with either type. The fix commit refuses a
protocol other than 0 or the type's own with -EPROTONOSUPPORT, and lxtest
(the Linux ABI user test, every debug boot) checks it. --old
reverse-applies the fix commit's compat/linux/syscalls.c in a throwaway
worktree: lxtest then fails its first protocol check, the ping socket
being a descriptor. Logs stay under out/sock-proto-probe/<arch>-<mode>.
Exit 0 when the mode shows what it should (fixed: LINUXTEST: PASS; old:
the check's FAIL line), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "compat: socket() reads its protocol: 0 or the type's own, else -EPROTONOSUPPORT"
SOURCE = "compat/linux/syscalls.c"
OLD_LINE = "LINUXTEST: FAIL sc3(LX_socket, LX_AF_INET, LX_SOCK_DGRAM, LX_IPPROTO_ICMP) == -93"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "sock-proto-probe" / f"{a.arch}-{mode}"
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
            seen = f"the check fails, the ping socket is descriptor {m.group(1)}" if m else "the check did not fail"
        else:
            ok = rc == 0 and "LINUXTEST: PASS" in log
            seen = "LINUXTEST: PASS" if ok else "no LINUXTEST: PASS"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
