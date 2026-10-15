#!/usr/bin/env python3
"""Prove the AArch64 kernel's exit survives a QEMU without semihosting.

    semihost-undef-probe.py [--old] [--tree REV]

Before the power-off unit (roadmap-1.1 P1), every AArch64 boot under a QEMU
started without -semihosting-config panicked at its end: the semihosting
exit's `hlt #0xF000` is an undefined instruction there, and the trap
handler panicked (`unhandled exception 1027 (undefined instruction)`).
The fix lists the HLT in the exception table and has handle_sync offer an
EL1 undefined instruction the fixup first; the exit returns and the kernel
powers off through PSCI. --old reverse-applies the fix commit's
kernel/arch/aarch64/trap.c in a throwaway worktree: `make test-platforms`'
psci-exit variant then fails with that panic. AArch64 only: x86-64's exit
is an I/O port write, which no machine takes as undefined.

Logs stay under out/semihost-undef-probe/aarch64-<mode>. Exit 0 when the
mode shows what it should (fixed: psci-exit PASS; old: the panic and
psci-exit FAIL), else 1.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "AArch64 powers off: PSCI SYSTEM_OFF after the emulator exit"
SOURCE = "kernel/arch/aarch64/trap.c"
PANIC = "KERNEL PANIC: unhandled exception 1027 (undefined instruction)"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "semihost-undef-probe" / f"aarch64-{mode}"
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
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {SOURCE}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "aarch64-release"
        with (work / "build.log").open("w") as log:
            if subprocess.run([make, "-C", str(tree), "-j8", "ARCH=aarch64", "BUILD=release", "image"],
                              stdout=log, stderr=subprocess.STDOUT).returncode != 0:
                sys.exit(f"the build failed ({work / 'build.log'})")
        pwork = work / "platforms"
        with (work / "test.log").open("w") as log:
            rc = subprocess.run([sys.executable, str(tree / "tests" / "boot" / "platform_test.py"),
                                 "--image", str(out / "cosmoos.img"), "--workdir", str(pwork),
                                 "--key", "unused", "--variants", "psci-exit"],
                                stdout=log, stderr=subprocess.STDOUT).returncode
        serial = pwork / "psci-exit" / "serial.log"
        text = serial.read_text(errors="replace") if serial.exists() else ""
        if a.old:
            ok = rc != 0 and PANIC in text
            seen = "psci-exit fails with the undefined-instruction panic" if ok else "no such panic"
        else:
            ok = rc == 0 and "shutdown: power off through PSCI SYSTEM_OFF" in text
            seen = "psci-exit PASS, powered off through PSCI" if ok else "psci-exit did not pass"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch=aarch64 mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
