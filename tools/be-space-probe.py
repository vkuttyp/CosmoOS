#!/usr/bin/env python3
"""Prove be-space fails only on a command that does not answer, not on a
slow runner (docs/testing/flakes.md, "be-space ... 2026-10-24").

    be-space-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

test-be's be-space boot is about forty commands on an encrypted root,
twenty-two of them `cosmo-snap list`. It missed its 240 s per-boot
deadline twice on CI's slower x86-64 runners with every command
answered: its length there was about 1.35 times the 187 s of a faster
runner, and the deadline fell inside its last list.

The probe puts a local host where those runners were. A release `make
test-encrypt-b1b4 ENCRYPT_B1B4_TESTS=be BE_ONLY=space` runs the be-space
boot alone after an install, with BOOT_TIMEOUT at DEADLINE_S, under the
length the boot takes locally and far over any one command:

  fixed  test-be passes: each command has its own budget
         (Boot.per_command), and the boot outlives its deadline.
  --old  the fix commit reverse-applied in a throwaway worktree: test-be
         fails `be-space: no prompt after ... before the deadline`.

Logs stay under out/be-space-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "tests/boot: be-space's commands each have a budget; the boot's length is not a property (flakes.md, be-space)"
# Under be-space's length run alone after an install (70 s here on both
# architectures: no environments yet, so each list walks less), and well
# over any one command (8.2 s at most), the install's boots (16 s) and a
# boot to its first prompt (10 s).
DEADLINE_S = {"x86_64": 45, "aarch64": 45}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "be-space-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"bsp-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            # The whole commit, not chosen hunks.
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha])
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "o"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}",
                f"BOOT_TIMEOUT={DEADLINE_S[a.arch]}", "ENCRYPT_B1B4_TESTS=be", "BE_ONLY=space",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}",
                "test-encrypt-b1b4"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=3600, start_new_session=True).returncode
        text = (work / "make.log").read_text(errors="replace")
        verdict = re.search(r"^test-be: (PASS|FAIL).*$", text, re.M)
        times = out / "test-be-encrypted" / "boot-be-space-times.log"
        last = times.read_text(errors="replace").strip().splitlines()[-1] if times.exists() else ""
        length = float(last.split()[0]) if last else 0.0
        subprocess.run(["cp", "-R", str(out / "test-be-encrypted"), str(work)], check=False)
        if a.old:
            ok = (rc != 0 and verdict is not None and verdict.group(1) == "FAIL" and "be-space: no prompt after" in
                  verdict.group(0) and "before the deadline" in verdict.group(0))
            seen = (f"the deadline missed: {verdict.group(0)}" if ok else
                    f"no such failure (make exit {rc}; {verdict.group(0) if verdict else 'no verdict'})")
        else:
            ok = rc == 0 and verdict is not None and verdict.group(1) == "PASS" and length > DEADLINE_S[a.arch]
            seen = (f"test-be passes; be-space took {length:.1f} s against a {DEADLINE_S[a.arch]} s boot deadline" if ok else
                    f"make exit {rc}; {verdict.group(0) if verdict else 'no verdict'}; be-space {length:.1f} s")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
