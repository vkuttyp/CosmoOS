#!/usr/bin/env python3
"""Prove e1000e retains descriptor DMA after RX/TX disable is unacknowledged.

Run once per architecture. --old restores the old unverified stop path in a
throwaway worktree and requires the deterministic self-test to fail because
the driver frees DMA while an engine remains enabled. Logs stay under out/.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BASELINE = "2661385c14c388190d8becfe95388f80c23995d3"
DRIVER = "drivers/network/e1000e.c"
OLD_MARKER = "E1000E-STOP-ACK-SWEEP: FAIL"
FIXED_MARKER = "E1000E-STOP-ACK-SWEEP: PASS"
EXPECTED_FAILURES = {
    "kernel reported failure via debug-exit",
    "no 'SELFTEST: PASS' line",
    "missing successful e1000e RX/TX disable-acknowledgement proof",
}


def restore_old(tree):
    path = tree / DRIVER
    source = path.read_text()
    start = source.index("static bool hw_stop_rx(struct e1000e *e)")
    end = source.index("static int e1000e_probe(", start)
    source = source[:start] + '''static bool hw_quiesce(struct e1000e *e)
{
    wr32(e, E1000_IMC, 0xffffffffu);
    (void)rd32(e, E1000_ICR);
    wr32(e, E1000_RCTL, 0);
    wr32(e, E1000_TCTL, 0);
    return true;
}

''' + source[end:]
    guard = '''    if (!stopped) {
#if CONFIG_SELFTEST
        g_stop_orphan = e;
#endif
        device_retain_dma(&pdev->dev);   /* no later probe programs this function (U14) */
        pdev->dev.drvdata = NULL;
        kwarn("e1000e: %s: retaining RX/TX DMA after disable was not acknowledged", pdev->dev.name);
        return;
    }
'''
    if source.count(guard) != 1:
        raise RuntimeError("e1000e DMA-retention guard anchor changed")
    remove_start = source.index("static void e1000e_remove(struct pci_device *pdev)")
    remove_end = source.index("#if CONFIG_SELFTEST\n/* Called by the kernel's device self-test", remove_start)
    remove = source[remove_start:remove_end]
    if remove.count(guard) != 1:
        raise RuntimeError("e1000e removal-retention anchor changed")
    remove = remove.replace(guard, "", 1)
    if remove.count("    bool stopped = hw_quiesce(e);\n") != 1:
        raise RuntimeError("e1000e stop acknowledgement call anchor changed")
    remove = remove.replace("    bool stopped = hw_quiesce(e);\n", "    (void)hw_quiesce(e);\n", 1)
    path.write_text(source[:remove_start] + remove + source[remove_end:])


def verify_baseline():
    source = subprocess.check_output(
        ["git", "-C", str(ROOT), "show", f"{BASELINE}:kernel/include/kernel/faultinject.h"], text=True)
    if "FI_E1000E_RX_DISABLE_ACK" in source:
        raise RuntimeError("pinned main unexpectedly contains the e1000e stop proof")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD", help="commit with the proof and fix")
    args = parser.parse_args()

    mode = "old" if args.old else "fixed"
    work = ROOT / "out" / "e1000e-stop-ack-probe" / f"{args.arch}-{mode}"
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
            proof = proof and re.search(r"SELFTEST: e1000e-stop-ack\s+\.\.\. FAIL:", log) is not None
            proof = proof and re.search(r"SELFTEST: PASS \(\d+ tests\)", log, re.M) is None
        else:
            proof = proof and rc == 0 and not failures
            proof = proof and re.search(r"SELFTEST: PASS \(\d+ tests\)$", log, re.M) is not None
            proof = proof and re.search(r"SELFTEST: e1000e-stop-ack\s+\.\.\. ok", log) is not None
        print(f"PROBE: {'PASS' if proof else 'FAIL'} arch={args.arch} mode={mode} ({work})")
        if not proof:
            print(f"  make exit={rc}; harness failures={sorted(failures)}; logs={result_path},{serial}")
        return 0 if proof else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
