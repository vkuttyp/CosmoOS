#!/usr/bin/env python3
"""Prove cosmofs fixture and NVMe worker cleanup on both architectures.

--old restores the two cleanup failures in a throwaway worktree and requires
exactly the two injected self-tests to fail. A successful run requires both
tests, the network harness and the complete boot to pass. Build and serial
logs stay under out/cleanup-path-probe/.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BASELINE = "55b1825ebc28c7f5cdb948477019808dc8ed5517"
COSMOFS = "kernel-services/filesystem/cosmofs/cosmofstest.c"
DEVTEST = "kernel/device/devtest.c"
EXPECTED = {"cosmofs-fixture-cleanup", "nvme-worker-cleanup"}


def restore_old(tree):
    path = tree / COSMOFS
    source = path.read_text()
    # Keep the injected operation boundaries, but restore CHECK's early
    # returns and remove the cleanup label introduced by the fix.
    source, count = re.subn(r"    bool created_dir = false;\n#define ENGINE_CHECK\(cond\).*?    } while \(0\)\n",
                            "", source, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError("engine_mount cleanup macro anchor changed")
    source = source.replace("ENGINE_CHECK(", "CHECK(")
    source = source.replace("    if (mk == 0)\n        created_dir = true;\n", "")
    source = source.replace("#undef ENGINE_CHECK\n", "")
    source, count = re.subn(r"\nfail:\n    if \(created_dir\).*?    return false;\n}", "\n}", source,
                            count=1, flags=re.S)
    if count != 1:
        raise RuntimeError("engine_mount fail-label anchor changed")
    path.write_text(source)

    path = tree / DEVTEST
    source = path.read_text()
    old_release = "        else\n            nvme_worker_free(workers[c].buf);\n"
    if source.count(old_release) != 1:
        raise RuntimeError("NVMe failed-start cleanup anchor changed")
    path.write_text(source.replace(old_release, "", 1))


def verify_old_behavior():
    cosmofs = subprocess.check_output(
        ["git", "-C", str(ROOT), "show", f"{BASELINE}:{COSMOFS}"], text=True)
    engine = re.search(r"static bool engine_mount\(.*?\n}\n", cosmofs, re.S)
    if not engine or "CHECK(cosmofs_format(bd) == 0)" not in engine.group():
        raise RuntimeError("pinned main engine_mount behavior changed")
    if "ramblk_destroy" in engine.group():
        raise RuntimeError("pinned main engine_mount no longer has the reported leak")

    devtest = subprocess.check_output(
        ["git", "-C", str(ROOT), "show", f"{BASELINE}:{DEVTEST}"], text=True)
    if ("workers[c].buf = kmalloc(4096, 0);" not in devtest
            or "if (!cpu_online(c) || threads[c] == NULL)\n            continue;" not in devtest):
        raise RuntimeError("pinned main NVMe worker behavior changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD", help="commit with the proof and fixes")
    args = parser.parse_args()

    mode = "old" if args.old else "fixed"
    work = ROOT / "out" / "cleanup-path-probe" / f"{args.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        raise SystemExit(f"refusing to overwrite nonempty probe output {work}")
    tree = Path(tempfile.mkdtemp(prefix="worktree-", dir=work))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), args.tree], check=True)
    try:
        if args.old:
            verify_old_behavior()
            restore_old(tree)

        make = "gmake" if sys.platform == "darwin" else "make"
        command = [make, "-C", str(tree), f"ARCH={args.arch}"]
        with (work / "build.log").open("w") as output:
            rc = subprocess.run(command + ["-j6", "image"], stdout=output,
                                stderr=subprocess.STDOUT).returncode
        if rc:
            print(f"PROBE: FAIL (build; {work / 'build.log'})")
            return 1

        serial = work / "boot.serial"
        result_path = work / "boot.result"
        with result_path.open("w") as output:
            rc = subprocess.run(command + [f"BOOT_LOG={serial}", "test"], stdout=output,
                                stderr=subprocess.STDOUT).returncode
        log = serial.read_text(errors="replace") if serial.exists() else ""
        for line in log.splitlines():
            if ("selftest: cosmofs-fixture-cleanup:" in line
                    or "selftest: nvme-worker-cleanup:" in line
                    or any(line.startswith(f"SELFTEST: {name} ") for name in EXPECTED)
                    or line.startswith("SELFTEST: net-harness")
                    or line.startswith("SELFTEST: PASS")
                    or line.startswith("SELFTEST: FAIL")):
                print(line)

        failures = set(re.findall(r"^SELFTEST: (\S+)\s+\.\.\. FAIL", log, re.M))
        network_ok = re.search(r"^SELFTEST: net-harness\s+\.\.\. ok", log, re.M) is not None
        if args.old:
            proof = all(re.search(rf"^SELFTEST: {name}\s+\.\.\. FAIL:", log, re.M)
                        for name in EXPECTED)
            proof = proof and failures == EXPECTED and network_ok
            proof = proof and re.search(r"^SELFTEST: FAIL \(2 of \d+\)$", log, re.M)
            proof = proof and "kernel reported failure via debug-exit" in result_path.read_text(errors="replace")
            proof = proof and re.search(r"cosmofs-fixture-cleanup: stage=1 hits=1 released=0", log)
            proof = proof and re.search(r"cosmofs-fixture-cleanup: stage=2 hits=1 released=0", log)
            proof = proof and re.search(r"cosmofs-fixture-cleanup: stage=3 hits=1 released=0", log)
            proof = proof and re.search(r"nvme-worker-cleanup: allocated=1 hits=1 started=0 released=0", log)
            ok = rc != 0 and proof
        else:
            ok = rc == 0 and not failures and network_ok
            ok = ok and all(re.search(rf"^SELFTEST: {name}\s+\.\.\. ok", log, re.M)
                            for name in EXPECTED)
            ok = ok and re.search(r"^SELFTEST: PASS \(\d+ tests\)", log, re.M) is not None
        print(f"PROBE: {'PASS' if ok else 'FAIL'} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
