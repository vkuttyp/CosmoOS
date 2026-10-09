#!/usr/bin/env python3
"""Prove xHCI retains controller DMA when HCH does not acknowledge halt.

Run once per architecture. --old restores the ignored HCH wait result in a
throwaway worktree and requires the deterministic halt fixture to fail.
Logs stay under out/.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BASELINE = "2661385c14c388190d8becfe95388f80c23995d3"
DRIVER = "drivers/usb/xhci.c"
OLD_MARKER = "XHCI-HALT-ACK-SWEEP: FAIL"
FIXED_MARKER = "XHCI-HALT-ACK-SWEEP: PASS"
EXPECTED_FAILURES = {
    "kernel reported failure via debug-exit",
    "no 'SELFTEST: PASS' line",
    "missing successful xHCI halt acknowledgement proof",
}


OLD_HALT = '''static bool xhci_halt_controller(struct xhci *x)
{
    uint32_t cmd = rd32(x->op + XHCI_USBCMD);
    if (cmd & USBCMD_RS) {
#if CONFIG_FAULTINJECT
        if (!faultinject_should_fail(FI_XHCI_HALT_ACK)) {
            wr32(x->op + XHCI_USBCMD, cmd & ~USBCMD_RS);
#if CONFIG_SELFTEST
            if (x->test_synthetic)
                wr32(x->op + XHCI_USBSTS, rd32(x->op + XHCI_USBSTS) | USBSTS_HCH);
#endif
        }
#else
        wr32(x->op + XHCI_USBCMD, cmd & ~USBCMD_RS);
#endif
        (void)wait_bits(x->op + XHCI_USBSTS, USBSTS_HCH, USBSTS_HCH, 100);
    }
    return true;   /* the old remove path discarded the HCH wait result */
}

'''


def restore_old(tree):
    path = tree / DRIVER
    source = path.read_text()
    start = source.index("static bool xhci_halt_controller(struct xhci *x)")
    end = source.index("#if CONFIG_SELFTEST\nbool xhci_test_halt_ack", start)
    if source[start:end].count("static bool xhci_halt_controller(") != 1:
        raise RuntimeError("xHCI halt helper anchor changed")
    path.write_text(source[:start] + OLD_HALT + source[end:])


def verify_baseline():
    source = subprocess.check_output(["git", "-C", str(ROOT), "show", f"{BASELINE}:{DRIVER}"], text=True)
    if "FI_XHCI_HALT_ACK" in source or "xhci_test_halt_ack" in source:
        raise RuntimeError("pinned main unexpectedly contains the xHCI halt proof")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD", help="commit with the proof and fix")
    args = parser.parse_args()

    mode = "old" if args.old else "fixed"
    work = ROOT / "out" / "xhci-halt-ack-probe" / f"{args.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        raise SystemExit(f"refusing to overwrite nonempty probe output {work}")
    if args.old:
        verify_baseline()
    tree = Path(tempfile.mkdtemp(prefix="worktree-", dir=work))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), args.tree], check=True)
    try:
        if args.old:
            restore_old(tree)
        make = "gmake" if sys.platform == "darwin" else "make"
        command = [make, "-C", str(tree), f"ARCH={args.arch}", "QEMU_SMP=1"]
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
            proof = proof and re.search(r"SELFTEST: xhci-halt-ack\s+\.\.\. FAIL:", log) is not None
            proof = proof and re.search(r"SELFTEST: PASS \(\d+ tests\)", log, re.M) is None
        else:
            proof = proof and rc == 0 and not failures
            proof = proof and re.search(r"SELFTEST: PASS \(\d+ tests\)$", log, re.M) is not None
            proof = proof and re.search(r"SELFTEST: xhci-halt-ack\s+\.\.\. ok", log) is not None
        print(f"PROBE: {'PASS' if proof else 'FAIL'} arch={args.arch} mode={mode} ({work})")
        if not proof:
            print(f"  make exit={rc}; harness failures={sorted(failures)}; logs={result_path},{serial}")
        return 0 if proof else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
