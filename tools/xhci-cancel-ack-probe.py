#!/usr/bin/env python3
"""Prove a cancelled USB request comes back only after a stop is acknowledged.

Run once per architecture. --old reverse-applies the whole fix commits'
source changes (the repeated-cancel fix of PR #338's review first, then
the original) (xHCI cancel escalation and quarantine, the synchronous
bounce, the class drivers' -EIO handling) in a throwaway worktree and
requires both proofs to fail: xhci-cancel-ack (the old cancel completed
the request after a refused Stop Endpoint) and usb-sync-quarantine (the
old synchronous path handed the caller's buffer to the controller).
Logs stay under out/.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
# Newest first: each is reverse-applied whole, the later fix before the
# fix it edits (review of PR #338 changed lines the first one added).
FIX_SUBJECTS = [
    "fix: keep a quarantined USB request on a repeated cancel",
    "fix: return cancelled USB requests only after an acknowledged stop",
]
SOURCES = [
    "drivers/usb/xhci.c",
    "drivers/usb/usb.c",
    "drivers/usb/usb_storage.c",
    "drivers/usb/usb_hid.c",
    "drivers/usb/usb_hub.c",
    "drivers/include/drivers/usb.h",
]
TESTS = ("xhci-cancel-ack", "usb-sync-quarantine")
OLD_MARKERS = ("XHCI-CANCEL-ACK-SWEEP: FAIL", "USB-SYNC-QUARANTINE: FAIL")
FIXED_MARKERS = ("XHCI-CANCEL-ACK-SWEEP: PASS", "USB-SYNC-QUARANTINE: PASS")
EXPECTED_FAILURES = {
    "kernel reported failure via debug-exit",
    "no 'SELFTEST: PASS' line",
    "missing successful xHCI cancel acknowledgement proof",
    "missing successful synchronous USB quarantine proof",
}


def fix_commits(tree_rev):
    shas = []
    for subject in FIX_SUBJECTS:
        sha = subprocess.check_output(["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings",
                                       f"--grep={subject}", tree_rev], text=True).strip()
        if not sha:
            raise RuntimeError(f"no commit '{subject}' reachable from {tree_rev}")
        shas.append(sha)
    return shas


def restore_old(tree, shas):
    """Each fix whole, reversed: never a hand-picked subset of its hunks."""
    for sha in shas:
        patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--"] + SOURCES)
        if not patch.strip():
            raise RuntimeError(f"fix commit {sha} changes none of {SOURCES}")
        subprocess.run(["git", "-C", str(tree), "apply", "-R", "--index"], input=patch, check=True)
    xhci = (tree / "drivers/usb/xhci.c").read_text()
    if "td->quarantined" in xhci or "ep_stop_and_drain" not in xhci:
        raise RuntimeError("reverse-applied tree does not have the old cancel")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD", help="commit with the proof and fix")
    args = parser.parse_args()

    mode = "old" if args.old else "fixed"
    work = ROOT / "out" / "xhci-cancel-ack-probe" / f"{args.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        raise SystemExit(f"refusing to overwrite nonempty probe output {work}")
    shas = fix_commits(args.tree) if args.old else None
    tree = Path(tempfile.mkdtemp(prefix="worktree-", dir=work))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), args.tree], check=True)
    try:
        if args.old:
            restore_old(tree, shas)
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
        markers = OLD_MARKERS if args.old else FIXED_MARKERS
        proof = all(m in log for m in markers) and "SNAPTEST: PASS" in log and "NETTEST: done " in log and \
            "served 100 of 100" in result
        if args.old:
            forbidden = [f for f in failures if re.fullmatch(
                r"forbidden marker /SELFTEST: FAIL/: SELFTEST: FAIL \(\d+ of \d+\)", f)]
            proof = proof and rc != 0 and len(forbidden) == 1 and failures - set(forbidden) == EXPECTED_FAILURES
            proof = proof and all(re.search(rf"SELFTEST: {t}\s+\.\.\. FAIL:", log) for t in TESTS)
            proof = proof and re.search(r"SELFTEST: FAIL \(2 of \d+\)", log) is not None
        else:
            proof = proof and rc == 0 and not failures
            proof = proof and re.search(r"SELFTEST: PASS \(\d+ tests\)$", log, re.M) is not None
            proof = proof and all(re.search(rf"SELFTEST: {t}\s+\.\.\. ok", log) for t in TESTS)
        print(f"PROBE: {'PASS' if proof else 'FAIL'} arch={args.arch} mode={mode} ({work})")
        if not proof:
            print(f"  make exit={rc}; harness failures={sorted(failures)}; logs={result_path},{serial}")
        return 0 if proof else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
