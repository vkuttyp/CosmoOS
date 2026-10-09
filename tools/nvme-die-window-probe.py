#!/usr/bin/env python3
"""Prove NVMe submit cannot publish after controller death.

Run once per architecture. --old removes the queue-lock dead check in a
throwaway worktree and requires the deterministic interleaving assertion to
be the sole boot failure. The fixed run requires the full boot and network
harness to pass. Build, harness, and serial logs stay under out/.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BASELINE = "2661385c14c388190d8becfe95388f80c23995d3"
DRIVER = "drivers/nvme/nvme.c"
OLD_CHECK = """    if (__atomic_load_n(&c->dead, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&q->lock, s);
        for (unsigned k = 0; k < nsegs; k++)
            dma_unmap(bd->dev, seg_dma[k], seg_len[k], dir);
        return -EIO;
    }
"""
OLD_MARKER = "NVME-INTERLEAVE: FAIL die=1 accepted=1 done=0 inflight=1"
FIXED_MARKER = "NVME-INTERLEAVE: PASS die=1 accepted=0 done=0 inflight=0"


def restore_old(tree):
    path = tree / DRIVER
    source = path.read_text()
    if source.count(OLD_CHECK) != 1:
        raise RuntimeError("NVMe queue-lock dead-check anchor changed")
    source = source.replace(OLD_CHECK, "", 1)
    path.write_text(source)


def verify_baseline():
    source = subprocess.check_output(
        ["git", "-C", str(ROOT), "show", f"{BASELINE}:{DRIVER}"], text=True)
    if "NVME-INTERLEAVE" in source:
        raise RuntimeError("pinned main unexpectedly contains the injected proof")
    if "static int nvme_submit(" not in source:
        raise RuntimeError("pinned main NVMe submit anchor is missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD", help="commit with the proof and fix")
    args = parser.parse_args()

    mode = "old" if args.old else "fixed"
    work = ROOT / "out" / "nvme-die-window-probe" / f"{args.arch}-{mode}"
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
        marker = OLD_MARKER if args.old else FIXED_MARKER
        failures = re.findall(r"^  - (.+)$", result, re.M)
        proof = marker in log
        proof = proof and re.search(r"^SELFTEST: PASS \(\d+ tests\)$", log, re.M) is not None
        proof = proof and re.search(r"^SELFTEST: net-harness\s+\.\.\. ok", log, re.M) is not None
        proof = proof and "NETTEST: done " in log and "served 100 of 100" in result
        if args.old:
            proof = proof and rc != 0
            proof = proof and failures == ["missing successful NVMe submit/death interleaving proof"]
            proof = proof and OLD_MARKER in log and FIXED_MARKER not in log
        else:
            proof = proof and rc == 0 and not failures
            proof = proof and FIXED_MARKER in log and OLD_MARKER not in log
        print(f"PROBE: {'PASS' if proof else 'FAIL'} arch={args.arch} mode={mode} ({work})")
        if not proof:
            print(f"  make exit={rc}; harness failures={failures}; logs={result_path},{serial}")
        return 0 if proof else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
