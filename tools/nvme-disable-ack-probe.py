#!/usr/bin/env python3
"""Prove NVMe queue DMA is retained when disable is not acknowledged.

Run once per architecture. --old restores unconditional queue reclamation
in a throwaway worktree and requires the deterministic self-test to fail
because it observes the queue allocations being freed. Logs stay under out/.
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
RECLAIM_GUARD = """    if (!stopped) {
        kerror("nvme%u: removal retained controller and queue DMA after unacknowledged disable", c->index);
        return false;
    }
"""
OLD_MARKER = "NVME-DISABLE-ACK: FAIL"
FIXED_MARKER = "NVME-DISABLE-ACK: PASS"
EXPECTED_FAILURES = {
    "kernel reported failure via debug-exit",
    "no 'SELFTEST: PASS' line",
    "missing successful NVMe disable-acknowledgement proof",
}


def restore_old(tree):
    path = tree / DRIVER
    source = path.read_text()
    start = source.index("static bool nvme_remove_queues(struct nvme_ctrl *c, bool stopped)")
    end = source.index("static int nvme_probe(", start)
    helper = source[start:end]
    if helper.count(RECLAIM_GUARD) != 1:
        raise RuntimeError("NVMe removal-reclaim guard anchor changed")
    helper = helper.replace(RECLAIM_GUARD, "    (void)stopped;   /* old behavior reclaimed regardless of the acknowledgement */\n", 1)
    path.write_text(source[:start] + helper + source[end:])


def verify_baseline():
    source = subprocess.check_output(["git", "-C", str(ROOT), "show", f"{BASELINE}:{DRIVER}"], text=True)
    if "nvme_remove_queues" in source or "NVME-DISABLE-ACK" in source:
        raise RuntimeError("pinned main unexpectedly contains the disable proof or fix")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD", help="commit with the proof and fix")
    args = parser.parse_args()

    mode = "old" if args.old else "fixed"
    work = ROOT / "out" / "nvme-disable-ack-probe" / f"{args.arch}-{mode}"
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
            proof = proof and re.search(r"SELFTEST: nvme-disable-ack\s+\.\.\. FAIL:", log) is not None
            proof = proof and re.search(r"SELFTEST: PASS \(\d+ tests\)", log, re.M) is None
        else:
            proof = proof and rc == 0 and not failures
            proof = proof and re.search(r"SELFTEST: PASS \(\d+ tests\)$", log, re.M) is not None
            proof = proof and re.search(r"SELFTEST: nvme-disable-ack\s+\.\.\. ok", log) is not None
        print(f"PROBE: {'PASS' if proof else 'FAIL'} arch={args.arch} mode={mode} ({work})")
        if not proof:
            print(f"  make exit={rc}; harness failures={sorted(failures)}; logs={result_path},{serial}")
        return 0 if proof else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
