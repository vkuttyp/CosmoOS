#!/usr/bin/env python3
"""Prove virtio-rng closes its repost gate before device reset.

Run once per architecture. --old removes the stopping check in a
throwaway worktree and requires the deterministic callback assertion to
be the sole boot failure. Logs stay under out/.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BASELINE = "2661385c14c388190d8becfe95388f80c23995d3"
DRIVER = "drivers/virtio/virtio_rng.c"
STOP_CHECK = """    if (r->stopping) {
        spin_unlock_irqrestore(&r->post_lock, s);
        return;
    }
"""
OLD_MARKER = "VRNG-RESET-REPOST: FAIL posts_after_reset=1"
FIXED_MARKER = "VRNG-RESET-REPOST: PASS posts_after_reset=0"
OLD_FAILURE = "missing successful virtio-rng reset/repost proof"


def restore_old(tree):
    path = tree / DRIVER
    source = path.read_text()
    if source.count(STOP_CHECK) != 1:
        raise RuntimeError("virtio-rng stopping-check anchor changed")
    path.write_text(source.replace(STOP_CHECK, "", 1))


def verify_baseline():
    source = subprocess.check_output(
        ["git", "-C", str(ROOT), "show", f"{BASELINE}:{DRIVER}"], text=True)
    if "VRNG-RESET-REPOST" in source:
        raise RuntimeError("pinned main unexpectedly contains the injected proof")
    if "static void vrng_post(" not in source:
        raise RuntimeError("pinned main virtio-rng post anchor is missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD", help="commit with the proof and fix")
    args = parser.parse_args()

    mode = "old" if args.old else "fixed"
    work = ROOT / "out" / "virtio-rng-repost-probe" / f"{args.arch}-{mode}"
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
        proof = marker in log
        proof = proof and re.search(r"^SELFTEST: PASS \(\d+ tests\)$", log, re.M) is not None
        proof = proof and re.search(r"^SELFTEST: net-harness\s+\.\.\. ok", log, re.M) is not None
        proof = proof and "NETTEST: done " in log and "served 100 of 100" in result
        failures = re.findall(r"^  - (.+)$", result, re.M)
        if args.old:
            proof = proof and rc != 0 and failures == [OLD_FAILURE]
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
