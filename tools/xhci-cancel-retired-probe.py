#!/usr/bin/env python3
"""Show xhci-cancel-retired detects a cancel that does not wait for a callback.

The suspicion (Unit 1 audit: xHCI cancel racing a running callback) is
refuted on the current tree: xhci_gone waits for the interrupt handler and
the irqpoll worker before answering -ENOENT. --old removes those two waits
in a throwaway worktree, so the test is shown to fail when the property it
claims is absent (not vacuous). Needs two CPUs: boots run at QEMU_SMP=2.
Run once per architecture. Logs stay under out/.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DRIVER = "drivers/usb/xhci.c"
OLD_MARKER = "XHCI-CANCEL-RETIRED: FAIL rc=-2 exited_at_return=0"
FIXED_MARKER = "XHCI-CANCEL-RETIRED: PASS rc=-2 exited_at_return=1"
EXPECTED_FAILURES = {
    "kernel reported failure via debug-exit",
    "no 'SELFTEST: PASS' line",
    "missing successful xHCI retired-callback cancel proof",
}
WAITS = '''    if (x->vector >= 0)
        synchronize_irq((unsigned)x->vector);
    irq_poll_synchronize(&x->poll);   /* the events may be the irqpoll worker's: its pass too */
    return -ENOENT;'''
NO_WAITS = '''    (void)x;   /* tools/xhci-cancel-retired-probe.py --old: answer without waiting */
    return -ENOENT;'''


def restore_old(tree):
    path = tree / DRIVER
    source = path.read_text()
    if source.count(WAITS) != 1:
        raise RuntimeError("xhci_gone's waits anchor changed")
    path.write_text(source.replace(WAITS, NO_WAITS))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD", help="commit with the test")
    args = parser.parse_args()

    mode = "old" if args.old else "fixed"
    work = ROOT / "out" / "xhci-cancel-retired-probe" / f"{args.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        raise SystemExit(f"refusing to overwrite nonempty probe output {work}")
    tree = Path(tempfile.mkdtemp(prefix="worktree-", dir=work))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), args.tree], check=True)
    try:
        if args.old:
            restore_old(tree)
        make = "gmake" if sys.platform == "darwin" else "make"
        command = [make, "-C", str(tree), f"ARCH={args.arch}", "QEMU_SMP=2"]
        build_log = work / "build.log"
        with build_log.open("w") as output:
            rc = subprocess.run(command + ["-j6", "image"], stdout=output,
                                stderr=subprocess.STDOUT).returncode
        if rc:
            print(f"PROBE: FAIL (build; {build_log})")
            return 1

        serial = work / "boot.serial"
        result_path = work / "boot.result"
        with result_path.open("w") as output:
            rc = subprocess.run(command + [f"BOOT_LOG={serial}", "test"], stdout=output,
                                stderr=subprocess.STDOUT).returncode
        log = serial.read_text(errors="replace") if serial.exists() else ""
        result = result_path.read_text(errors="replace")
        failures = set(re.findall(r"^  - (.+)$", result, re.M))
        marker = OLD_MARKER if args.old else FIXED_MARKER
        proof = marker in log and "SNAPTEST: PASS" in log and "NETTEST: done " in log and \
            "served 100 of 100" in result
        if args.old:
            forbidden = [f for f in failures if re.fullmatch(
                r"forbidden marker /SELFTEST: FAIL/: SELFTEST: FAIL \(\d+ of \d+\)", f)]
            proof = proof and rc != 0 and len(forbidden) == 1 and failures - set(forbidden) == EXPECTED_FAILURES
            proof = proof and re.search(r"SELFTEST: xhci-cancel-retired\s+\.\.\. FAIL:", log) is not None
            proof = proof and re.search(r"SELFTEST: FAIL \(1 of \d+\)", log) is not None
        else:
            proof = proof and rc == 0 and not failures
            proof = proof and re.search(r"SELFTEST: PASS \(\d+ tests\)$", log, re.M) is not None
            proof = proof and re.search(r"SELFTEST: xhci-cancel-retired\s+\.\.\. ok", log) is not None
        print(f"PROBE: {'PASS' if proof else 'FAIL'} arch={args.arch} mode={mode} ({work})")
        if not proof:
            print(f"  make exit={rc}; harness failures={sorted(failures)}; logs={result_path},{serial}")
        return 0 if proof else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
