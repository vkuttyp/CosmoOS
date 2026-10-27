#!/usr/bin/env python3
"""Prove the two security defects of roadmap 1.2's first unit's third PR:
NET-6 (the SYN-cookie secret kept from an unseeded pool) and SEC-4 (the
Linux signal trampoline at a fixed executable address), each with a test
that fails on every boot without its fix.

    always-security-probe.py [--old] [--arch x86_64|aarch64] [--tree REV]

The probe boots a debug build and judges:

  NET-6  the self-test net-syncookie-seeded: the secret marked as drawn
         unseeded, a cookie made on the seeded pool must use a secret drawn
         from it.
  SEC-4  lxtest's trampoline checks: on x86-64 nothing at 0x7FFFFFFF1000;
         on AArch64 the handler's return page is not that address.

  fixed  net-syncookie-seeded ok, LINUXTEST: PASS.
  --old  the two fix commits reverse-applied in a throwaway worktree, the
         tests kept: net-syncookie-seeded FAILs, and lxtest fails the
         trampoline check (x86-64: SEGV_MAPERR expected, ACCERR seen;
         AArch64: tpage != LX_SIGTRAMP).

Logs stay under out/always-security-probe/<arch>-<mode>. Exit 0 when the
mode shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXES = [
    "kernel-services/network: the SYN-cookie secret is drawn again once the pool seeds (NET-6)",
    "compat/linux: the signal trampoline at a random page, and none on x86-64 (SEC-4)",
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "always-security-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"asp-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            for subject in FIXES:
                sha = subprocess.check_output(
                    ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={subject}",
                     a.tree], text=True).strip()
                if not sha:
                    sys.exit(f"no commit '{subject}' reachable from {a.tree}")
                # The whole commit, three-way; a conflict only in docs/ keeps
                # the tree's text, one in code stops the probe.
                if subprocess.run(["git", "-C", str(tree), "revert", "--no-commit", sha],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                    clash = subprocess.check_output(["git", "-C", str(tree), "diff", "--name-only",
                                                     "--diff-filter=U"], text=True).split()
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
        def selftest(name):
            m = re.search(rf"^SELFTEST: {re.escape(name)}\s+\.\.\. (ok|FAIL)\b", serial, re.M)
            return m.group(1) if m else "no line"
        cookie = selftest("net-syncookie-seeded")
        passed = re.search(r"^LINUXTEST: PASS$", serial, re.M) is not None
        tramp_check = "g_sig.code == LX_SEGV_MAPERR" if a.arch == "x86_64" else "tpage != LX_SIGTRAMP"
        tramp_failed = any(tramp_check in ln for ln in serial.splitlines() if ln.startswith("LINUXTEST: FAIL "))
        if a.old:
            ok = cookie == "FAIL" and tramp_failed
        else:
            ok = cookie == "ok" and passed and not tramp_failed
        seen = [f"net-syncookie-seeded: {cookie}",
                "lxtest: " + ("the trampoline check failed" if tramp_failed else ("PASS" if passed else "no PASS"))]
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: " + "; ".join(seen) + f" ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
